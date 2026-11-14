// wasm.hpp — a bounded interpreter for small, import-free WebAssembly modules
// whose code is integer arithmetic over linear memory (flixcloud's per-request
// key mixer, reanime.hpp). i32 is the only value type: a module declaring any
// other type, an import, a table, or an opcode outside the integer/memory/
// control subset fails load() and never runs.
//
// Every run is bounded: a step budget per call, a call-depth cap, a value-
// stack cap, and every memory access range-checked (a trap, never UB).
// Little-endian loads/stores are assembled byte by byte, so results are the
// same on big-endian PPC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::wasm {

inline constexpr std::size_t kPageSize = 65536;
// Linear memory ceiling, pages (1 MiB). The mixer uses one.
inline constexpr std::uint32_t kMaxPages = 16;
// Instructions one call() may execute before it traps.
inline constexpr std::uint64_t kStepBudget = 4'000'000;

struct Module;  // decoded sections; defined in wasm.cpp

class Instance {
 public:
  // Decode and instantiate (memory sized, globals and data segments applied).
  // nullopt on malformed bytes or anything outside the supported subset.
  [[nodiscard]] static std::optional<Instance> load(const std::uint8_t* bytes, std::size_t len);

  // Run export `name` with i32 arguments. nullopt on an unknown export, an
  // arity mismatch, a trap, or the step budget running out.
  [[nodiscard]] std::optional<std::vector<std::uint32_t>> call(
      std::string_view name, const std::vector<std::uint32_t>& args);

  // Grow memory to at least `pages` (the page's JS grows an empty memory by
  // one before writing). false past kMaxPages or the declared maximum.
  [[nodiscard]] bool ensure_pages(std::uint32_t pages);

  [[nodiscard]] bool write(std::uint32_t addr, const std::uint8_t* data, std::size_t len);
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(std::uint32_t addr,
                                                              std::size_t len) const;

  Instance(Instance&&) noexcept;
  Instance& operator=(Instance&&) noexcept;
  ~Instance();

 private:
  explicit Instance(std::unique_ptr<Module> m);
  std::unique_ptr<Module> m_;
};

}  // namespace shigoku::wasm
