// offer.hpp — what a resolved stream offers to download: its variants as the
// rows a picker lists, and the few pure helpers that word them. No network
// here: download::probe fills a StreamOffer, the surfaces show it.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain.hpp"

namespace shigoku {

// One thing the stream can deliver. `rung` is what taking the row asks the
// transfer for; the rest is what the playlist said about it.
struct OfferRow {
  Rung rung;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  std::optional<std::uint64_t> bandwidth;  // bits per second, as claimed.
  std::string codecs;
  // The row's audio is a separate rendition, fetched beside the video and
  // muxed into the file. `audio` is that rendition's name.
  bool separate_audio = false;
  std::string audio;

  friend bool operator==(const OfferRow&, const OfferRow&) = default;
};

struct StreamOffer {
  enum class Shape {
    File,    // one file: nothing to choose.
    Single,  // one playlist of segments: the source serves this stream only.
    Ladder,  // a master playlist: every row is a variant that can be taken.
    Unread,  // a playlist that could not be read now; it transfers as served.
  };
  Shape shape = Shape::File;
  // Best first. Never empty: the shapes without a choice carry their one row.
  std::vector<OfferRow> rows;

  [[nodiscard]] bool has_choice() const { return shape == Shape::Ladder && rows.size() > 1; }

  friend bool operator==(const StreamOffer&, const StreamOffer&) = default;
};

// Why a single-row offer has nothing to choose, for the line under it. ""
// for a ladder.
[[nodiscard]] inline std::string offer_shape_note(StreamOffer::Shape shape) {
  switch (shape) {
    case StreamOffer::Shape::File:
      return "a single file: the source serves one quality";
    case StreamOffer::Shape::Single:
      return "one stream: the source serves one quality here";
    case StreamOffer::Shape::Unread:
      return "its list of qualities could not be read: it downloads as served";
    case StreamOffer::Shape::Ladder:
      return {};
  }
  return {};
}

// "412 MB", "1.3 GB", "0.4 MB" — binary units, one decimal under 100, cut
// (never rounded up: a size is not overstated).
[[nodiscard]] inline std::string human_bytes(std::uint64_t bytes) {
  const std::uint64_t kMb = 1024ull * 1024ull;
  auto one_decimal = [](std::uint64_t value, std::uint64_t unit) {
    const std::uint64_t tenths = value / unit * 10ull + value % unit * 10ull / unit;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
  };
  if (bytes >= 1000ull * kMb) return one_decimal(bytes, 1024ull * kMb) + " GB";
  if (bytes >= 100ull * kMb) return std::to_string(bytes / kMb) + " MB";
  return one_decimal(bytes, kMb) + " MB";
}

// "1920×1080", "1080p" when only the height is known, "2.4 Mbit/s" when only
// the bitrate is, else "as served".
[[nodiscard]] inline std::string offer_row_size(const OfferRow& row) {
  if (row.height.has_value()) {
    if (row.width.has_value()) {
      return std::to_string(*row.width) + "\xC3\x97" + std::to_string(*row.height);  // ×
    }
    return std::to_string(*row.height) + "p";
  }
  if (row.bandwidth.has_value() && *row.bandwidth > 0) {
    const std::uint64_t tenths = *row.bandwidth / 100000ull;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " Mbit/s";
  }
  return "as served";
}

// The row's size on disk as far as the playlist lets it be guessed: its
// claimed bitrate over the episode's length. nullopt when either is unknown
// or the bitrate is not a believable one (some sources write it in other
// units; under 64 kbit/s is taken as that).
[[nodiscard]] inline std::optional<std::uint64_t> offer_row_bytes(const OfferRow& row,
                                                                  std::uint32_t duration_secs) {
  if (duration_secs == 0 || !row.bandwidth.has_value() || *row.bandwidth < 64000ull) {
    return std::nullopt;
  }
  return *row.bandwidth / 8ull * duration_secs;
}

// A row in words: "1920×1080 · ~1.3 GB · avc1.64001f,mp4a.40.2 · audio:
// Native" — its size, then each part that is known: the estimate over
// `duration_secs`, the codecs, the separate audio rendition's name. The
// playlist's own strings are stripped of control bytes.
[[nodiscard]] inline std::string offer_row_line(const OfferRow& row, std::uint32_t duration_secs) {
  std::string out = offer_row_size(row);
  auto add = [&out](const std::string& part) {
    out += " \xC2\xB7 ";  // ·
    out += part;
  };
  if (const auto bytes = offer_row_bytes(row, duration_secs)) add("~" + human_bytes(*bytes));
  if (const std::string codecs = strip_controls(row.codecs); !codecs.empty()) add(codecs);
  if (const std::string name = strip_controls(row.audio); !name.empty()) {
    add("audio: " + name);
  } else if (row.separate_audio) {
    add("separate audio");
  }
  return out;
}

}  // namespace shigoku
