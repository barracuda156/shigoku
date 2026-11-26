// uneval.hpp — reads the JavaScript value expressions a SvelteKit page ships
// its load data as (devalue's `uneval` output) into JSON. Not a JavaScript
// parser: the grammar is what that one serializer writes.
//
//   object   {key:value,"quoted key":value}      array    [a,,b]
//   string   "…" with the usual escapes          number   12, -0, .5, 1e21, 5n
//   words    true false null, and `void 0`, undefined, NaN, Infinity as null
//   call     Name(args), new Name(args), a.b(args) -> the last argument, null
//            with none (a Date is its stamp, a Set or Map its entries,
//            a3ec deferred promise its slot); Array(n) and {} start empty
//   shared   (function(a,b){a.x=1;b[0]=a;return [a,b]}({},Array(1)))
//            values referenced twice are hoisted into parameters; the result
//            has every reference replaced by its value
//
// A reference cycle reads as null where it closes. The page is untrusted:
// nesting past kMaxDepth as written (four times that once references are
// replaced) or an expansion past kMaxNodes fails the read.

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace shigoku::uneval {

inline constexpr int kMaxDepth = 64;
inline constexpr std::size_t kMaxNodes = 100000;

// The value expression at the start of `text` (leading whitespace skipped);
// whatever follows it is ignored. nullopt on malformed input or a limit.
[[nodiscard]] std::optional<nlohmann::json> parse(std::string_view text);

}  // namespace shigoku::uneval
