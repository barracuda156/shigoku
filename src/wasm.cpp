// wasm.cpp — see wasm.hpp. Decode once into a flat instruction vector with
// block/loop/if ends pre-matched, then a label-stack interpreter over it.

#include "wasm.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace shigoku::wasm {

namespace {

constexpr std::size_t kMaxModuleBytes = std::size_t{1} << 20;
constexpr std::uint32_t kMaxFunctions = 256;
constexpr std::uint32_t kMaxLocals = 1024;
constexpr std::size_t kMaxBodyInstrs = std::size_t{1} << 16;
constexpr std::size_t kMaxStack = 4096;
constexpr int kMaxCallDepth = 64;
constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint8_t kI32 = 0x7f;

struct Instr {
  std::uint8_t op = 0;
  std::uint32_t a = 0;  // index / constant / memarg offset / block arity
  std::uint32_t b = 0;  // block/loop/if/else: the matching `end`
  std::uint32_t c = kNone;  // if: its `else`, when present
};

struct FuncType {
  std::uint32_t params = 0;
  std::uint32_t results = 0;
};

struct Function {
  std::uint32_t type = 0;
  std::uint32_t locals = 0;
  std::vector<Instr> code;
};

struct Global {
  std::uint32_t value = 0;
  bool mut = false;
};

struct Export {
  std::string name;
  std::uint8_t kind = 0;
  std::uint32_t index = 0;
};

class Reader {
 public:
  Reader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] bool done() const { return i_ >= n_; }
  [[nodiscard]] std::size_t pos() const { return i_; }
  [[nodiscard]] const std::uint8_t* here() const { return p_ + i_; }

  std::uint8_t byte() {
    if (i_ >= n_) {
      ok_ = false;
      return 0;
    }
    return p_[i_++];
  }

  std::uint32_t u32() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      const std::uint8_t b = byte();
      if (!ok_) return 0;
      v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) {
        if (v > 0xFFFFFFFFull) ok_ = false;
        return static_cast<std::uint32_t>(v);
      }
    }
    ok_ = false;
    return 0;
  }

  // Signed 32-bit LEB, returned as its two's-complement bit pattern.
  std::uint32_t s32() {
    std::int64_t v = 0;
    int shift = 0;
    std::uint8_t b = 0;
    do {
      if (shift >= 35) {
        ok_ = false;
        return 0;
      }
      b = byte();
      if (!ok_) return 0;
      v |= static_cast<std::int64_t>(b & 0x7f) << shift;
      shift += 7;
    } while ((b & 0x80) != 0);
    if (shift < 64 && (b & 0x40) != 0) v |= -(static_cast<std::int64_t>(1) << shift);
    if (v < std::numeric_limits<std::int32_t>::min() ||
        v > std::numeric_limits<std::int32_t>::max()) {
      ok_ = false;
      return 0;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
  }

  bool skip(std::size_t k) {
    if (k > n_ - i_) {
      ok_ = false;
      return false;
    }
    i_ += k;
    return true;
  }

 private:
  const std::uint8_t* p_;
  std::size_t n_;
  std::size_t i_ = 0;
  bool ok_ = true;
};

// `i32.const N end`: the only constant expression the subset accepts.
std::optional<std::uint32_t> const_expr(Reader& r) {
  if (r.byte() != 0x41) return std::nullopt;
  const std::uint32_t v = r.s32();
  if (r.byte() != 0x0b || !r.ok()) return std::nullopt;
  return v;
}

bool block_arity(Reader& r, std::uint32_t& arity) {
  const std::uint8_t t = r.byte();
  if (t == 0x40) {
    arity = 0;
    return r.ok();
  }
  if (t == kI32) {
    arity = 1;
    return r.ok();
  }
  return false;
}

bool is_simple(std::uint8_t op) {
  return op == 0x00 || op == 0x01 || op == 0x0f || op == 0x1a || op == 0x1b ||
         (op >= 0x45 && op <= 0x4f) || (op >= 0x67 && op <= 0x78) || op == 0xc0 || op == 0xc1;
}

bool is_memory_access(std::uint8_t op) {
  return op == 0x28 || op == 0x2c || op == 0x2d || op == 0x2e || op == 0x2f || op == 0x36 ||
         op == 0x3a || op == 0x3b;
}

}  // namespace

struct Module {
  std::vector<FuncType> types;
  std::vector<Function> funcs;
  std::vector<Global> globals;
  std::vector<Export> exports;
  bool has_memory = false;
  std::uint32_t max_pages = kMaxPages;
  std::vector<std::uint8_t> memory;
};

namespace {

bool decode_body(Reader& r, const Module& m, Function& f) {
  const std::uint32_t groups = r.u32();
  std::uint64_t locals = 0;
  for (std::uint32_t g = 0; g < groups && r.ok(); ++g) {
    locals += r.u32();
    if (r.byte() != kI32 || locals > kMaxLocals) return false;
  }
  if (!r.ok()) return false;
  f.locals = static_cast<std::uint32_t>(locals);
  const std::uint64_t frame = static_cast<std::uint64_t>(m.types[f.type].params) + f.locals;

  std::vector<std::uint32_t> open;  // indices of the enclosing block/loop/if
  while (r.ok()) {
    if (f.code.size() >= kMaxBodyInstrs) return false;
    Instr in;
    in.op = r.byte();
    const auto idx = static_cast<std::uint32_t>(f.code.size());
    switch (in.op) {
      case 0x02:
      case 0x03:
      case 0x04:
        if (!block_arity(r, in.a)) return false;
        open.push_back(idx);
        break;
      case 0x05:
        if (open.empty() || f.code[open.back()].op != 0x04 || f.code[open.back()].c != kNone) {
          return false;
        }
        f.code[open.back()].c = idx;
        break;
      case 0x0b:
        if (open.empty()) {
          f.code.push_back(in);
          return r.ok();  // the function's own end
        }
        f.code[open.back()].b = idx;
        if (const std::uint32_t e = f.code[open.back()].c; e != kNone) f.code[e].b = idx;
        open.pop_back();
        break;
      case 0x0c:
      case 0x0d:
        in.a = r.u32();
        if (in.a > open.size()) return false;
        break;
      case 0x10:
        in.a = r.u32();
        if (in.a >= m.funcs.size()) return false;
        break;
      case 0x20:
      case 0x21:
      case 0x22:
        in.a = r.u32();
        if (in.a >= frame) return false;
        break;
      case 0x23:
      case 0x24:
        in.a = r.u32();
        if (in.a >= m.globals.size()) return false;
        if (in.op == 0x24 && !m.globals[in.a].mut) return false;
        break;
      case 0x3f:
      case 0x40:
        if (!m.has_memory || r.byte() != 0x00) return false;
        break;
      case 0x41:
        in.a = r.s32();
        break;
      default:
        if (is_memory_access(in.op)) {
          if (!m.has_memory) return false;
          (void)r.u32();  // alignment hint
          in.a = r.u32();
        } else if (!is_simple(in.op)) {
          return false;
        }
        break;
    }
    f.code.push_back(in);
  }
  return false;
}

std::unique_ptr<Module> decode(const std::uint8_t* bytes, std::size_t len) {
  static constexpr std::uint8_t kHead[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  if (bytes == nullptr || len < 8 || len > kMaxModuleBytes) return nullptr;
  for (int i = 0; i < 8; ++i) {
    if (bytes[i] != kHead[i]) return nullptr;
  }
  auto m = std::make_unique<Module>();
  std::vector<std::uint32_t> func_types;
  struct Segment {
    std::uint32_t offset;
    const std::uint8_t* data;
    std::uint32_t len;
  };
  std::vector<Segment> segments;
  bool saw_code = false;
  std::uint32_t min_pages = 0;

  Reader top(bytes + 8, len - 8);
  std::uint8_t last_id = 0;
  while (!top.done()) {
    const std::uint8_t id = top.byte();
    const std::uint32_t size = top.u32();
    if (!top.ok() || size > len) return nullptr;
    const std::uint8_t* body = top.here();
    if (!top.skip(size)) return nullptr;
    if (id != 0) {
      if (id <= last_id && !(id == 12 && last_id < 10)) return nullptr;
      if (id != 12) last_id = id;
    }
    Reader r(body, size);
    switch (id) {
      case 0:
        break;
      case 1: {
        const std::uint32_t n = r.u32();
        if (n > kMaxFunctions) return nullptr;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          if (r.byte() != 0x60) return nullptr;
          FuncType t;
          t.params = r.u32();
          if (t.params > kMaxLocals) return nullptr;
          for (std::uint32_t k = 0; k < t.params; ++k) {
            if (r.byte() != kI32) return nullptr;
          }
          t.results = r.u32();
          if (t.results > 1) return nullptr;
          for (std::uint32_t k = 0; k < t.results; ++k) {
            if (r.byte() != kI32) return nullptr;
          }
          m->types.push_back(t);
        }
        break;
      }
      case 2:
      case 4:
      case 9:
        if (r.u32() != 0) return nullptr;  // imports, tables, elements: none
        break;
      case 3: {
        const std::uint32_t n = r.u32();
        if (n > kMaxFunctions) return nullptr;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          const std::uint32_t t = r.u32();
          if (t >= m->types.size()) return nullptr;
          func_types.push_back(t);
        }
        break;
      }
      case 5: {
        const std::uint32_t n = r.u32();
        if (n > 1) return nullptr;
        if (n == 1) {
          const std::uint8_t flags = r.byte();
          min_pages = r.u32();
          if (flags == 1) {
            m->max_pages = r.u32();
            if (m->max_pages < min_pages) return nullptr;
            if (m->max_pages > kMaxPages) m->max_pages = kMaxPages;
          } else if (flags != 0) {
            return nullptr;
          }
          if (min_pages > kMaxPages) return nullptr;
          m->has_memory = true;
        }
        break;
      }
      case 6: {
        const std::uint32_t n = r.u32();
        if (n > kMaxFunctions) return nullptr;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          if (r.byte() != kI32) return nullptr;
          const std::uint8_t mut = r.byte();
          if (mut > 1) return nullptr;
          const auto v = const_expr(r);
          if (!v.has_value()) return nullptr;
          m->globals.push_back(Global{*v, mut == 1});
        }
        break;
      }
      case 7: {
        const std::uint32_t n = r.u32();
        if (n > kMaxFunctions) return nullptr;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          const std::uint32_t nl = r.u32();
          if (nl > 256 || !r.ok()) return nullptr;
          Export e;
          const std::uint8_t* name = r.here();
          if (!r.skip(nl)) return nullptr;
          e.name.assign(reinterpret_cast<const char*>(name), nl);
          e.kind = r.byte();
          e.index = r.u32();
          m->exports.push_back(std::move(e));
        }
        break;
      }
      case 10: {
        const std::uint32_t n = r.u32();
        if (n != func_types.size()) return nullptr;
        m->funcs.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) m->funcs[i].type = func_types[i];
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          const std::uint32_t sz = r.u32();
          const std::uint8_t* fb = r.here();
          if (!r.skip(sz)) return nullptr;
          Reader fr(fb, sz);
          if (!decode_body(fr, *m, m->funcs[i]) || !fr.done()) return nullptr;
        }
        saw_code = true;
        break;
      }
      case 11: {
        const std::uint32_t n = r.u32();
        if (n > kMaxFunctions) return nullptr;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
          const std::uint32_t flag = r.u32();
          if (flag == 2 && r.u32() != 0) return nullptr;
          std::optional<std::uint32_t> off;
          if (flag == 0 || flag == 2) {
            off = const_expr(r);
            if (!off.has_value()) return nullptr;
          } else if (flag != 1) {
            return nullptr;
          }
          const std::uint32_t dl = r.u32();
          const std::uint8_t* d = r.here();
          if (!r.skip(dl)) return nullptr;
          if (off.has_value()) segments.push_back(Segment{*off, d, dl});  // passive: never applied
        }
        break;
      }
      case 12:
        (void)r.u32();
        break;
      default:
        return nullptr;  // start function or an unknown section
    }
    if (!r.ok() || (id != 0 && !r.done())) return nullptr;
  }
  if (!func_types.empty() && !saw_code) return nullptr;

  for (const Export& e : m->exports) {
    if (e.kind == 0 && e.index >= m->funcs.size()) return nullptr;
    if (e.kind == 2 && (!m->has_memory || e.index != 0)) return nullptr;
    if (e.kind == 3 && e.index >= m->globals.size()) return nullptr;
    if (e.kind == 1 || e.kind > 3) return nullptr;
  }
  m->memory.assign(static_cast<std::size_t>(min_pages) * kPageSize, 0);
  for (const Segment& s : segments) {
    if (!m->has_memory ||
        static_cast<std::uint64_t>(s.offset) + s.len > m->memory.size()) {
      return nullptr;
    }
    std::copy(s.data, s.data + s.len, m->memory.begin() + s.offset);
  }
  return m;
}

std::uint32_t clz32(std::uint32_t v) {
  if (v == 0) return 32;
  std::uint32_t n = 0;
  while ((v & 0x80000000u) == 0) {
    v <<= 1;
    ++n;
  }
  return n;
}

std::uint32_t ctz32(std::uint32_t v) {
  if (v == 0) return 32;
  std::uint32_t n = 0;
  while ((v & 1u) == 0) {
    v >>= 1;
    ++n;
  }
  return n;
}

std::uint32_t popcnt32(std::uint32_t v) {
  std::uint32_t n = 0;
  for (; v != 0; v &= v - 1) ++n;
  return n;
}

std::int32_t as_signed(std::uint32_t v) { return static_cast<std::int32_t>(v); }

// The binary i32 ops; false on a trap (division by zero, signed overflow).
bool binary(std::uint8_t op, std::uint32_t a, std::uint32_t b, std::uint32_t& out) {
  const std::uint32_t k = b & 31u;
  switch (op) {
    case 0x46: out = a == b; return true;
    case 0x47: out = a != b; return true;
    case 0x48: out = as_signed(a) < as_signed(b); return true;
    case 0x49: out = a < b; return true;
    case 0x4a: out = as_signed(a) > as_signed(b); return true;
    case 0x4b: out = a > b; return true;
    case 0x4c: out = as_signed(a) <= as_signed(b); return true;
    case 0x4d: out = a <= b; return true;
    case 0x4e: out = as_signed(a) >= as_signed(b); return true;
    case 0x4f: out = a >= b; return true;
    case 0x6a: out = a + b; return true;
    case 0x6b: out = a - b; return true;
    case 0x6c: out = a * b; return true;
    case 0x6d:
      if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return false;
      out = static_cast<std::uint32_t>(as_signed(a) / as_signed(b));
      return true;
    case 0x6e:
      if (b == 0) return false;
      out = a / b;
      return true;
    case 0x6f:
      if (b == 0) return false;
      out = (a == 0x80000000u && b == 0xFFFFFFFFu)
                ? 0
                : static_cast<std::uint32_t>(as_signed(a) % as_signed(b));
      return true;
    case 0x70:
      if (b == 0) return false;
      out = a % b;
      return true;
    case 0x71: out = a & b; return true;
    case 0x72: out = a | b; return true;
    case 0x73: out = a ^ b; return true;
    case 0x74: out = a << k; return true;
    case 0x75: out = static_cast<std::uint32_t>(as_signed(a) >> k); return true;
    case 0x76: out = a >> k; return true;
    case 0x77: out = k == 0 ? a : (a << k) | (a >> (32 - k)); return true;
    case 0x78: out = k == 0 ? a : (a >> k) | (a << (32 - k)); return true;
    default: return false;
  }
}

struct Label {
  std::uint32_t cont;    // pc a branch lands on
  std::size_t height;    // value-stack height at entry
  std::uint32_t br_arity;
  std::uint32_t end_arity;
  bool loop;
};

// Keep the top `arity` values, drop everything above `height` beneath them.
bool unwind(std::vector<std::uint32_t>& stack, std::size_t height, std::uint32_t arity) {
  if (stack.size() < height + arity) return false;
  std::copy(stack.end() - arity, stack.end(), stack.begin() + static_cast<std::ptrdiff_t>(height));
  stack.resize(height + arity);
  return true;
}

bool exec(Module& m, std::uint32_t fidx, std::vector<std::uint32_t>& stack, int depth,
          std::uint64_t& steps) {
  if (depth > kMaxCallDepth) return false;
  const Function& f = m.funcs[fidx];
  const FuncType& t = m.types[f.type];
  if (stack.size() < t.params) return false;
  std::vector<std::uint32_t> locals(static_cast<std::size_t>(t.params) + f.locals, 0);
  for (std::uint32_t i = t.params; i-- > 0;) {
    locals[i] = stack.back();
    stack.pop_back();
  }
  const std::size_t base = stack.size();
  std::vector<Label> labels;
  labels.push_back(Label{static_cast<std::uint32_t>(f.code.size()), base, t.results, t.results,
                         false});

  auto pop = [&](std::uint32_t& v) {
    if (stack.size() <= base) return false;
    v = stack.back();
    stack.pop_back();
    return true;
  };
  auto mem_ok = [&](std::uint64_t addr, std::uint64_t width) {
    return addr + width <= m.memory.size();
  };

  std::size_t pc = 0;
  while (pc < f.code.size()) {
    if (++steps > kStepBudget || stack.size() > kMaxStack) return false;
    const Instr& in = f.code[pc];
    switch (in.op) {
      case 0x00:
        return false;
      case 0x01:
        break;
      case 0x02:
        labels.push_back(Label{in.b + 1, stack.size(), in.a, in.a, false});
        break;
      case 0x03:
        labels.push_back(Label{static_cast<std::uint32_t>(pc + 1), stack.size(), 0, in.a, true});
        break;
      case 0x04: {
        std::uint32_t cond = 0;
        if (!pop(cond)) return false;
        if (cond != 0) {
          labels.push_back(Label{in.b + 1, stack.size(), in.a, in.a, false});
        } else if (in.c != kNone) {
          labels.push_back(Label{in.b + 1, stack.size(), in.a, in.a, false});
          pc = in.c + 1;
          continue;
        } else {
          pc = in.b + 1;
          continue;
        }
        break;
      }
      case 0x05:
      case 0x0b: {
        if (labels.size() == 1) {
          if (!unwind(stack, base, t.results)) return false;
          return true;
        }
        const Label l = labels.back();
        labels.pop_back();
        if (!unwind(stack, l.height, l.end_arity)) return false;
        if (in.op == 0x05) {
          pc = in.b + 1;
          continue;
        }
        break;
      }
      case 0x0c:
      case 0x0d: {
        if (in.op == 0x0d) {
          std::uint32_t cond = 0;
          if (!pop(cond)) return false;
          if (cond == 0) break;
        }
        const std::size_t target = labels.size() - 1 - in.a;
        const Label l = labels[target];
        if (!unwind(stack, l.height, l.br_arity)) return false;
        if (target == 0) return true;  // a branch to the function body returns
        labels.resize(l.loop ? target + 1 : target);
        pc = l.cont;
        continue;
      }
      case 0x0f:
        return unwind(stack, base, t.results);
      case 0x10:
        if (!exec(m, in.a, stack, depth + 1, steps)) return false;
        break;
      case 0x1a: {
        std::uint32_t v = 0;
        if (!pop(v)) return false;
        break;
      }
      case 0x1b: {
        std::uint32_t cond = 0, b = 0, a = 0;
        if (!pop(cond) || !pop(b) || !pop(a)) return false;
        stack.push_back(cond != 0 ? a : b);
        break;
      }
      case 0x20:
        stack.push_back(locals[in.a]);
        break;
      case 0x21:
        if (!pop(locals[in.a])) return false;
        break;
      case 0x22:
        if (stack.size() <= base) return false;
        locals[in.a] = stack.back();
        break;
      case 0x23:
        stack.push_back(m.globals[in.a].value);
        break;
      case 0x24:
        if (!pop(m.globals[in.a].value)) return false;
        break;
      case 0x28:
      case 0x2c:
      case 0x2d:
      case 0x2e:
      case 0x2f: {
        std::uint32_t base_addr = 0;
        if (!pop(base_addr)) return false;
        const std::uint64_t addr = static_cast<std::uint64_t>(base_addr) + in.a;
        const std::uint64_t width = in.op == 0x28 ? 4 : (in.op >= 0x2e ? 2 : 1);
        if (!mem_ok(addr, width)) return false;
        const std::uint8_t* p = m.memory.data() + addr;
        std::uint32_t v = 0;
        for (std::uint64_t i = width; i-- > 0;) v = (v << 8) | p[i];
        if (in.op == 0x2c) v = static_cast<std::uint32_t>(static_cast<std::int8_t>(v & 0xffu));
        if (in.op == 0x2e) v = static_cast<std::uint32_t>(static_cast<std::int16_t>(v & 0xffffu));
        stack.push_back(v);
        break;
      }
      case 0x36:
      case 0x3a:
      case 0x3b: {
        std::uint32_t v = 0, base_addr = 0;
        if (!pop(v) || !pop(base_addr)) return false;
        const std::uint64_t addr = static_cast<std::uint64_t>(base_addr) + in.a;
        const std::uint64_t width = in.op == 0x36 ? 4 : (in.op == 0x3b ? 2 : 1);
        if (!mem_ok(addr, width)) return false;
        std::uint8_t* p = m.memory.data() + addr;
        for (std::uint64_t i = 0; i < width; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
        break;
      }
      case 0x3f:
        stack.push_back(static_cast<std::uint32_t>(m.memory.size() / kPageSize));
        break;
      case 0x40: {
        std::uint32_t n = 0;
        if (!pop(n)) return false;
        const auto old = static_cast<std::uint32_t>(m.memory.size() / kPageSize);
        if (static_cast<std::uint64_t>(old) + n > m.max_pages) {
          stack.push_back(0xFFFFFFFFu);
        } else {
          m.memory.resize(static_cast<std::size_t>(old + n) * kPageSize, 0);
          stack.push_back(old);
        }
        break;
      }
      case 0x41:
        stack.push_back(in.a);
        break;
      case 0x45: {
        std::uint32_t v = 0;
        if (!pop(v)) return false;
        stack.push_back(v == 0);
        break;
      }
      case 0x67:
      case 0x68:
      case 0x69:
      case 0xc0:
      case 0xc1: {
        std::uint32_t v = 0;
        if (!pop(v)) return false;
        if (in.op == 0x67) v = clz32(v);
        if (in.op == 0x68) v = ctz32(v);
        if (in.op == 0x69) v = popcnt32(v);
        if (in.op == 0xc0) v = static_cast<std::uint32_t>(static_cast<std::int8_t>(v & 0xffu));
        if (in.op == 0xc1) v = static_cast<std::uint32_t>(static_cast<std::int16_t>(v & 0xffffu));
        stack.push_back(v);
        break;
      }
      default: {
        std::uint32_t b = 0, a = 0, out = 0;
        if (!pop(b) || !pop(a) || !binary(in.op, a, b, out)) return false;
        stack.push_back(out);
        break;
      }
    }
    ++pc;
  }
  return false;  // decode guarantees a final `end`; running past it is a bug
}

}  // namespace

Instance::Instance(std::unique_ptr<Module> m) : m_(std::move(m)) {}
Instance::Instance(Instance&&) noexcept = default;
Instance& Instance::operator=(Instance&&) noexcept = default;
Instance::~Instance() = default;

std::optional<Instance> Instance::load(const std::uint8_t* bytes, std::size_t len) {
  auto m = decode(bytes, len);
  if (m == nullptr) return std::nullopt;
  return Instance(std::move(m));
}

std::optional<std::vector<std::uint32_t>> Instance::call(std::string_view name,
                                                         const std::vector<std::uint32_t>& args) {
  for (const Export& e : m_->exports) {
    if (e.kind != 0 || e.name != name) continue;
    const FuncType& t = m_->types[m_->funcs[e.index].type];
    if (args.size() != t.params) return std::nullopt;
    std::vector<std::uint32_t> stack(args);
    std::uint64_t steps = 0;
    if (!exec(*m_, e.index, stack, 0, steps) || stack.size() != t.results) return std::nullopt;
    return stack;
  }
  return std::nullopt;
}

bool Instance::ensure_pages(std::uint32_t pages) {
  if (!m_->has_memory || pages > m_->max_pages) return false;
  const std::size_t want = static_cast<std::size_t>(pages) * kPageSize;
  if (m_->memory.size() < want) m_->memory.resize(want, 0);
  return true;
}

bool Instance::write(std::uint32_t addr, const std::uint8_t* data, std::size_t len) {
  if (static_cast<std::uint64_t>(addr) + len > m_->memory.size()) return false;
  std::copy(data, data + len, m_->memory.begin() + addr);
  return true;
}

std::optional<std::vector<std::uint8_t>> Instance::read(std::uint32_t addr,
                                                        std::size_t len) const {
  if (static_cast<std::uint64_t>(addr) + len > m_->memory.size()) return std::nullopt;
  return std::vector<std::uint8_t>(m_->memory.begin() + addr, m_->memory.begin() + addr + len);
}

}  // namespace shigoku::wasm
