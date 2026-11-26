// uneval.cpp — see uneval.hpp.

#include "uneval.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace shigoku::uneval {
namespace {

using json = nlohmann::json;

// A hoisted parameter's stand-in inside the tree until the call's arguments
// are read: an object holding this one key and the parameter's position.
constexpr const char* kRefKey = "\x01ref";
constexpr std::size_t kMaxIndex = 65536;
constexpr std::size_t kMaxPrefill = 4096;
// How deep a value may nest once its references are replaced.
constexpr int kMaxResolved = 4 * kMaxDepth;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
bool ident_char(char c) { return ident_start(c) || is_digit(c); }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// One accessor of an assignment target: `[3]` or `.key` / `["key"]`.
struct Step {
  bool index = false;
  std::size_t at = 0;
  std::string key;
};

struct Assign {
  std::size_t param = 0;
  std::vector<Step> path;
  json value;
};

enum class Mark : std::uint8_t { Fresh, Open, Done };

struct Scope {
  std::vector<std::string> names;
  std::vector<json> values;
  std::vector<Mark> marks;
  std::vector<std::size_t> sizes;
  std::vector<int> heights;
};

std::optional<std::size_t> ref_index(const json& v) {
  if (!v.is_object() || v.size() != 1) return std::nullopt;
  const auto it = v.find(kRefKey);
  if (it == v.end() || !it->is_number_unsigned()) return std::nullopt;
  return it->get<std::size_t>();
}

// Counts the nodes of `v` and how deep it nests.
void measure(const json& v, int depth, std::size_t& nodes, int& height) {
  ++nodes;
  if (depth > height) height = depth;
  if (v.is_structured()) {
    for (const auto& child : v) measure(child, depth + 1, nodes, height);
  }
}

class Reader {
 public:
  explicit Reader(std::string_view s) : s_(s) {}

  bool value(json& out, int depth) {
    if (depth > kMaxDepth || !charge(1)) return false;
    ws();
    if (eof()) return false;
    const char c = s_[i_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"' || c == '\'') {
      std::string str;
      if (!string(str)) return false;
      out = std::move(str);
      return true;
    }
    if (c == '(') return paren(out, depth);
    if (c == '-' || c == '+') {
      const char next = i_ + 1 < s_.size() ? s_[i_ + 1] : '\0';
      if (is_digit(next) || next == '.') return number(out);
      ++i_;
      json inner;
      if (!value(inner, depth + 1)) return false;
      out = nullptr;  // -Infinity and the like.
      return true;
    }
    if (is_digit(c) || c == '.') return number(out);
    if (ident_start(c)) return word(out, depth);
    return false;
  }

 private:
  bool eof() const { return i_ >= s_.size(); }
  char peek() const { return eof() ? '\0' : s_[i_]; }

  void ws() {
    while (!eof()) {
      const char c = s_[i_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++i_;
    }
  }

  bool expect(char c) {
    ws();
    if (peek() != c) return false;
    ++i_;
    return true;
  }

  bool charge(std::size_t n) {
    if (n > kMaxNodes - nodes_) return false;
    nodes_ += n;
    return true;
  }

  bool ident(std::string& out) {
    if (eof() || !ident_start(s_[i_])) return false;
    const std::size_t start = i_;
    while (!eof() && ident_char(s_[i_])) ++i_;
    out.assign(s_.substr(start, i_ - start));
    return true;
  }

  bool keyword(std::string_view w) const {
    if (s_.substr(i_, w.size()) != w) return false;
    const std::size_t after = i_ + w.size();
    return after >= s_.size() || !ident_char(s_[after]);
  }

  bool hex4(std::uint32_t& out) {
    if (i_ + 4 > s_.size()) return false;
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const int h = hex_value(s_[i_ + static_cast<std::size_t>(k)]);
      if (h < 0) return false;
      v = (v << 4) | static_cast<std::uint32_t>(h);
    }
    i_ += 4;
    out = v;
    return true;
  }

  bool string(std::string& out) {
    const char quote = s_[i_++];
    for (;;) {
      if (eof()) return false;
      const char c = s_[i_++];
      if (c == quote) return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (eof()) return false;
      const char e = s_[i_++];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'v': out.push_back('\v'); break;
        case '0': out.push_back('\0'); break;
        case '\n': break;  // line continuation.
        case 'x': {
          if (i_ + 2 > s_.size()) return false;
          const int hi = hex_value(s_[i_]);
          const int lo = hex_value(s_[i_ + 1]);
          if (hi < 0 || lo < 0) return false;
          i_ += 2;
          append_utf8(out, static_cast<std::uint32_t>(hi * 16 + lo));
          break;
        }
        case 'u': {
          std::uint32_t cp = 0;
          if (peek() == '{') {
            ++i_;
            int digits = 0;
            while (!eof() && s_[i_] != '}') {
              const int h = hex_value(s_[i_++]);
              if (h < 0 || ++digits > 6) return false;
              cp = (cp << 4) | static_cast<std::uint32_t>(h);
            }
            if (eof() || digits == 0) return false;
            ++i_;
          } else {
            if (!hex4(cp)) return false;
            // A surrogate pair is one code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(i_, 2) == "\\u") {
              const std::size_t back = i_;
              i_ += 2;
              std::uint32_t low = 0;
              if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                i_ = back;
              }
            }
          }
          append_utf8(out, cp);
          break;
        }
        default: out.push_back(e); break;
      }
    }
  }

  bool number(json& out) {
    bool negative = false;
    if (peek() == '-' || peek() == '+') negative = s_[i_++] == '-';
    std::uint64_t whole = 0;
    bool whole_fits = true;
    double mantissa = 0.0;
    int digits = 0;
    int fraction = 0;
    bool integral = true;
    while (!eof() && is_digit(s_[i_])) {
      const std::uint64_t d = static_cast<std::uint64_t>(s_[i_++] - '0');
      if (whole > (UINT64_MAX - d) / 10) whole_fits = false;
      if (whole_fits) whole = whole * 10 + d;
      mantissa = mantissa * 10.0 + static_cast<double>(d);
      ++digits;
    }
    if (peek() == '.') {
      ++i_;
      integral = false;
      while (!eof() && is_digit(s_[i_])) {
        mantissa = mantissa * 10.0 + static_cast<double>(s_[i_++] - '0');
        ++digits;
        if (fraction < 400) ++fraction;
      }
    }
    if (digits == 0) return false;
    int exponent = 0;
    if (peek() == 'e' || peek() == 'E') {
      const std::size_t back = i_;
      ++i_;
      bool exp_negative = false;
      if (peek() == '-' || peek() == '+') exp_negative = s_[i_++] == '-';
      int exp_digits = 0;
      while (!eof() && is_digit(s_[i_])) {
        if (exponent < 1000) exponent = exponent * 10 + (s_[i_] - '0');
        ++i_;
        ++exp_digits;
      }
      if (exp_digits == 0) {
        i_ = back;
        exponent = 0;
      } else {
        integral = false;
        if (exp_negative) exponent = -exponent;
      }
    }
    if (peek() == 'n') ++i_;  // BigInt.
    if (!eof() && ident_char(s_[i_])) return false;

    if (integral && whole_fits) {
      if (!negative && whole <= static_cast<std::uint64_t>(INT64_MAX)) {
        out = static_cast<std::int64_t>(whole);
        return true;
      }
      if (negative && whole <= static_cast<std::uint64_t>(INT64_MAX)) {
        out = -static_cast<std::int64_t>(whole);
        return true;
      }
    }
    // Powers of ten up to 1e22 are exact, so a short decimal divides or
    // multiplies once and rounds once ("0.3" is 3 / 10, not 3 * 0.1).
    const int scale = exponent - fraction;
    double v = mantissa;
    if (scale >= -22 && scale <= 22) {
      double p = 1.0;
      for (int k = 0; k < (scale < 0 ? -scale : scale); ++k) p *= 10.0;
      v = scale < 0 ? mantissa / p : mantissa * p;
    } else {
      v = mantissa * std::pow(10.0, static_cast<double>(scale));
    }
    if (!std::isfinite(v)) {
      out = nullptr;
      return true;
    }
    out = negative ? -v : v;
    return true;
  }

  bool object(json& out, int depth) {
    ++i_;
    out = json::object();
    ws();
    if (peek() == '}') {
      ++i_;
      return true;
    }
    for (;;) {
      ws();
      if (eof()) return false;
      std::string key;
      const char c = s_[i_];
      if (c == '"' || c == '\'') {
        if (!string(key)) return false;
      } else if (ident_start(c)) {
        if (!ident(key)) return false;
      } else if (is_digit(c)) {
        const std::size_t start = i_;
        while (!eof() && (is_digit(s_[i_]) || s_[i_] == '.')) ++i_;
        key.assign(s_.substr(start, i_ - start));
      } else {
        return false;
      }
      if (!expect(':')) return false;
      json v;
      if (!value(v, depth + 1)) return false;
      out[key] = std::move(v);
      ws();
      if (peek() == ',') {
        ++i_;
        ws();
        if (peek() == '}') {
          ++i_;
          return true;
        }
        continue;
      }
      if (peek() == '}') {
        ++i_;
        return true;
      }
      return false;
    }
  }

  bool array(json& out, int depth) {
    ++i_;
    out = json::array();
    for (;;) {
      ws();
      if (eof()) return false;
      const char c = s_[i_];
      if (c == ']') {
        ++i_;
        return true;
      }
      if (c == ',') {  // a hole.
        ++i_;
        if (!charge(1)) return false;
        out.push_back(nullptr);
        continue;
      }
      json v;
      if (!value(v, depth + 1)) return false;
      out.push_back(std::move(v));
      ws();
      if (peek() == ',') {
        ++i_;
        continue;
      }
      if (peek() == ']') {
        ++i_;
        return true;
      }
      return false;
    }
  }

  // A parenthesised value, or the hoisting function and its call.
  bool paren(json& out, int depth) {
    ++i_;
    ws();
    if (keyword("function")) return hoisted(out, depth);
    if (!value(out, depth + 1)) return false;
    return expect(')');
  }

  bool word(json& out, int depth) {
    std::string w;
    if (!ident(w)) return false;
    if (w == "true") {
      out = true;
      return true;
    }
    if (w == "false") {
      out = false;
      return true;
    }
    if (w == "null" || w == "undefined" || w == "NaN" || w == "Infinity") {
      out = nullptr;
      return true;
    }
    if (w == "void") {
      json ignored;
      if (!value(ignored, depth + 1)) return false;
      out = nullptr;
      return true;
    }
    if (w == "new") {
      ws();
      if (!ident(w)) return false;
      return chain(out, depth, std::move(w));
    }
    if (w == "function") return false;
    if (!scopes_.empty()) {
      const Scope& sc = scopes_.back();
      for (std::size_t k = 0; k < sc.names.size(); ++k) {
        if (sc.names[k] != w) continue;
        const std::size_t back = i_;
        ws();
        const char next = peek();
        i_ = back;
        if (next == '(' || next == '.' || next == '[') break;
        out = json::object();
        out[kRefKey] = k;
        return true;
      }
    }
    return chain(out, depth, std::move(w));
  }

  // Name, then members and calls: the last call's last argument.
  bool chain(json& out, int depth, std::string callee) {
    bool called = false;
    json last = nullptr;
    for (;;) {
      ws();
      const char c = peek();
      if (c == '.') {
        ++i_;
        ws();
        std::string member;
        if (!ident(member)) return false;
        callee.push_back('.');
        callee += member;
        continue;
      }
      if (c == '[') {
        ++i_;
        json ignored;
        if (!value(ignored, depth + 1) || !expect(']')) return false;
        continue;
      }
      if (c != '(') break;
      ++i_;
      last = nullptr;
      std::size_t argc = 0;
      for (;;) {
        ws();
        if (peek() == ')') {
          ++i_;
          break;
        }
        json v;
        if (!value(v, depth + 1)) return false;
        last = std::move(v);
        ++argc;
        ws();
        if (peek() == ',') {
          ++i_;
          continue;
        }
        if (peek() == ')') {
          ++i_;
          break;
        }
        return false;
      }
      if (callee == "Array") {
        std::size_t n = 0;
        if (argc == 1 && last.is_number_integer() && last.get<std::int64_t>() > 0 &&
            last.get<std::int64_t>() <= static_cast<std::int64_t>(kMaxPrefill)) {
          n = static_cast<std::size_t>(last.get<std::int64_t>());
        }
        if (!charge(n)) return false;
        last = json::array();
        for (std::size_t k = 0; k < n; ++k) last.push_back(nullptr);
      } else if (callee == "Object.create") {
        last = json::object();
      }
      called = true;
    }
    out = called ? std::move(last) : json(nullptr);
    return true;
  }

  // Skip one statement this reader has no use for, through its `;`.
  bool skip_statement() {
    int nest = 0;
    while (!eof()) {
      const char c = s_[i_];
      if (c == '"' || c == '\'') {
        std::string ignored;
        if (!string(ignored)) return false;
        continue;
      }
      ++i_;
      if (c == '(' || c == '[' || c == '{') {
        ++nest;
      } else if (c == ')' || c == ']' || c == '}') {
        if (--nest < 0) return false;
      } else if (c == ';' && nest == 0) {
        return true;
      }
    }
    return false;
  }

  // `name[3]=value;` / `name.key=value;` on a parameter; anything else skips.
  bool statement(std::vector<Assign>& assigns, int depth) {
    const std::size_t start = i_;
    std::string name;
    if (!ident(name)) return skip_statement();
    const Scope& sc = scopes_.back();
    std::size_t param = sc.names.size();
    for (std::size_t k = 0; k < sc.names.size(); ++k) {
      if (sc.names[k] == name) param = k;
    }
    if (param == sc.names.size()) {
      i_ = start;
      return skip_statement();
    }
    Assign a;
    a.param = param;
    for (;;) {
      ws();
      if (peek() == '.') {
        ++i_;
        ws();
        Step st;
        if (!ident(st.key)) return false;
        a.path.push_back(std::move(st));
        continue;
      }
      if (peek() == '[') {
        ++i_;
        ws();
        Step st;
        if (peek() == '"' || peek() == '\'') {
          if (!string(st.key)) return false;
        } else {
          std::size_t at = 0;
          int digits = 0;
          while (!eof() && is_digit(s_[i_])) {
            if (at <= kMaxIndex) at = at * 10 + static_cast<std::size_t>(s_[i_] - '0');
            ++i_;
            ++digits;
          }
          if (digits == 0 || at > kMaxIndex) return false;
          st.index = true;
          st.at = at;
        }
        if (!expect(']')) return false;
        a.path.push_back(std::move(st));
        continue;
      }
      break;
    }
    ws();
    const bool assigns_here =
        peek() == '=' && !(i_ + 1 < s_.size() && s_[i_ + 1] == '=') && !a.path.empty();
    if (!assigns_here) {
      i_ = start;
      return skip_statement();
    }
    ++i_;
    if (!value(a.value, depth + 1)) return false;
    if (!expect(';')) return false;
    assigns.push_back(std::move(a));
    return true;
  }

  bool call_arguments(std::vector<json>& values, int depth) {
    if (!expect('(')) return false;
    for (;;) {
      ws();
      if (peek() == ')') {
        ++i_;
        return true;
      }
      json v;
      if (!value(v, depth + 1)) return false;
      values.push_back(std::move(v));
      ws();
      if (peek() == ',') {
        ++i_;
        continue;
      }
      if (peek() == ')') {
        ++i_;
        return true;
      }
      return false;
    }
  }

  bool apply(Scope& sc, Assign& a) {
    json* node = &sc.values[a.param];
    for (const Step& st : a.path) {
      if (ref_index(*node).has_value()) return true;  // not a container of ours.
      if (st.index) {
        if (!node->is_array()) *node = json::array();
        if (node->size() <= st.at) {
          if (!charge(st.at + 1 - node->size())) return false;
          while (node->size() <= st.at) node->push_back(nullptr);
        }
        node = &(*node)[st.at];
      } else {
        if (!node->is_object()) *node = json::object();
        node = &(*node)[st.key];
      }
    }
    *node = std::move(a.value);
    return true;
  }

  bool resolve(json& v, Scope& sc, int depth) {
    if (depth > kMaxResolved) return false;
    if (const auto ix = ref_index(v); ix.has_value()) {
      if (*ix >= sc.values.size() || sc.marks[*ix] == Mark::Open) {
        v = nullptr;
        return true;
      }
      if (sc.marks[*ix] == Mark::Fresh) {
        sc.marks[*ix] = Mark::Open;
        if (!resolve(sc.values[*ix], sc, depth + 1)) return false;
        sc.marks[*ix] = Mark::Done;
        measure(sc.values[*ix], 0, sc.sizes[*ix], sc.heights[*ix]);
      }
      // A value resolved earlier is copied in whole: it must fit under the
      // nesting limit where it lands, or chained parameters nest without end.
      if (depth + sc.heights[*ix] > kMaxResolved) return false;
      if (!charge(sc.sizes[*ix])) return false;
      v = sc.values[*ix];
      return true;
    }
    if (v.is_structured()) {
      for (auto& child : v) {
        if (!resolve(child, sc, depth + 1)) return false;
      }
    }
    return true;
  }

  // At `function`: (function(a,b){a.x=1;return [a,b]}({},{})) and the
  // (function(){…})(…) spelling of the same.
  bool hoisted(json& out, int depth) {
    i_ += 8;  // "function"
    ws();
    std::string name;
    (void)ident(name);
    if (!expect('(')) return false;
    Scope sc;
    for (;;) {
      ws();
      if (peek() == ')') {
        ++i_;
        break;
      }
      std::string p;
      if (!ident(p)) return false;
      sc.names.push_back(std::move(p));
      ws();
      if (peek() == ',') ++i_;
    }
    if (!expect('{')) return false;
    scopes_.push_back(std::move(sc));
    std::vector<Assign> assigns;
    json result;
    bool ok = false;
    for (;;) {
      ws();
      if (eof()) break;
      if (keyword("return")) {
        i_ += 6;
        if (!value(result, depth + 1)) break;
        ws();
        if (peek() == ';') ++i_;
        ok = expect('}');
        break;
      }
      if (!statement(assigns, depth)) break;
    }
    Scope scope = std::move(scopes_.back());
    scopes_.pop_back();
    if (!ok) return false;

    ws();
    if (peek() == ')') {
      ++i_;
      if (!call_arguments(scope.values, depth)) return false;
    } else {
      if (!call_arguments(scope.values, depth)) return false;
      if (!expect(')')) return false;
    }
    scope.values.resize(scope.names.size());
    scope.marks.assign(scope.values.size(), Mark::Fresh);
    scope.sizes.assign(scope.values.size(), 0);
    scope.heights.assign(scope.values.size(), 0);
    for (Assign& a : assigns) {
      if (!apply(scope, a)) return false;
    }
    if (!resolve(result, scope, 0)) return false;
    out = std::move(result);
    return true;
  }

  std::string_view s_;
  std::size_t i_ = 0;
  std::size_t nodes_ = 0;
  std::vector<Scope> scopes_;
};

}  // namespace

std::optional<nlohmann::json> parse(std::string_view text) {
  Reader reader(text);
  json out;
  if (!reader.value(out, 0)) return std::nullopt;
  return out;
}

}  // namespace shigoku::uneval
