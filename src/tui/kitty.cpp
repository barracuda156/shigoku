// kitty.cpp — the hand-rolled Kitty graphics emitter (P8, A4). Pure string
// builders; the module never reads or writes a fd. See kitty.hpp for the A4
// wire-subset contract this reproduces.
//
// ENDIANNESS (§3): base64_encode walks the RGBA buffer byte-by-byte; no pixel
// is ever assembled into a wider int, so the emitted APC bytes are identical on
// big- and little-endian targets. The one integer arithmetic here (base64's
// 24-bit accumulator) is value math on `unsigned`, not a reinterpret of memory.

#include "kitty.hpp"

namespace shigoku::tui::kitty {

namespace {

// APC envelope framing (A4): ESC _ G <control-data> [; <payload>] ESC \.
constexpr std::string_view kApcStart = "\x1b_G";  // ESC _ G
constexpr std::string_view kApcEnd = "\x1b\\";    // ESC-backslash (ST).

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64_encode(const std::vector<unsigned char>& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t i = 0;
  const std::size_t n = bytes.size();
  // Full 3-byte groups -> 4 base64 chars. Bit math on `unsigned` is value
  // arithmetic (no memory reinterpret), so it is byte-order independent.
  for (; i + 3 <= n; i += 3) {
    const unsigned b0 = bytes[i], b1 = bytes[i + 1], b2 = bytes[i + 2];
    const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kB64[(triple >> 18) & 0x3F]);
    out.push_back(kB64[(triple >> 12) & 0x3F]);
    out.push_back(kB64[(triple >> 6) & 0x3F]);
    out.push_back(kB64[triple & 0x3F]);
  }
  // Tail: 1 or 2 leftover bytes, '=' padded to a 4-char quantum.
  const std::size_t rem = n - i;
  if (rem == 1) {
    const unsigned b0 = bytes[i];
    const unsigned triple = b0 << 16;
    out.push_back(kB64[(triple >> 18) & 0x3F]);
    out.push_back(kB64[(triple >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const unsigned b0 = bytes[i], b1 = bytes[i + 1];
    const unsigned triple = (b0 << 16) | (b1 << 8);
    out.push_back(kB64[(triple >> 18) & 0x3F]);
    out.push_back(kB64[(triple >> 12) & 0x3F]);
    out.push_back(kB64[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string transmit(const Image& img, std::uint32_t id, int cols, int rows) {
  if (img.w == 0 || img.h == 0 || img.rgba.empty()) {
    return {};  // nothing to draw.
  }
  const std::string payload = base64_encode(img.rgba);

  // Split the base64 payload into 4096-char fragments (A4). kChunkBase64Bytes
  // is a multiple of 4, so no fragment splits a base64 quantum.
  std::string out;
  out.reserve(payload.size() + payload.size() / kChunkBase64Bytes * 32 + 128);

  const std::size_t total = payload.size();
  std::size_t off = 0;
  bool first = true;
  while (true) {
    const std::size_t take =
        (total - off) > kChunkBase64Bytes ? kChunkBase64Bytes : (total - off);
    const bool last = (off + take) >= total;

    out += kApcStart;
    if (first) {
      // The first fragment carries the full control block. m=1 unless this is
      // also the only fragment (then m=0 closes it in one command).
      // s=/v= (source pixel dims) are MANDATORY for raw f=32 data: the terminal
      // validates payload == s*v*4 and cannot rasterize without them (kitty and
      // contour both EINVAL their absence, silently under q=2). An early wire
      // subset omitted them in transcription — sabigoku's ratatui-image emitter
      // always sent them; real hardware caught the gap.
      out += "a=T,f=32,i=";
      out += std::to_string(id);
      out += ",s=";
      out += std::to_string(img.w);
      out += ",v=";
      out += std::to_string(img.h);
      out += ",c=";
      out += std::to_string(cols);
      out += ",r=";
      out += std::to_string(rows);
      out += ",C=1,q=2,m=";
      out += last ? '0' : '1';
    } else {
      // Continuation fragments carry only m= (the terminal keys off the id from
      // the first fragment of the in-flight transmission).
      out += "m=";
      out += last ? '0' : '1';
    }
    out += ';';
    out.append(payload, off, take);
    out += kApcEnd;

    first = false;
    off += take;
    if (last) break;
  }
  return out;
}

std::string delete_image(std::uint32_t id) {
  // a=d delete, d=I (CAPITAL — free stored data + honor the quota, A4), by id.
  std::string out;
  out += kApcStart;
  out += "a=d,d=I,i=";
  out += std::to_string(id);
  out += ",q=2";
  out += kApcEnd;
  return out;
}

std::string query_apc(std::uint32_t id) {
  // Detection probe: transmit a 1x1 transparent RGBA pixel with a=t (store, do
  // NOT display) and let the terminal ACK it. We deliberately do NOT send a=q
  // (query) or q=2 (quiet): contour_ppc — a real Kitty terminal, verified by an
  // `_Gi=…;OK` ack to this exact transmit — ignores a=q entirely and answers
  // NOTHING, so the old a=q,q=2 probe was a false-negative (kitty=false →
  // placeholders on a terminal that draws covers fine). a=t forces a stored
  // image the terminal must acknowledge, and no q= means the OK/EINVAL reply is
  // emitted. The stored 1x1 is inert (never placed); a later real cover uses its
  // own id. probe_reply_is_kitty only needs the `\x1b_G` opener of the ack.
  const std::vector<unsigned char> px = {0, 0, 0, 0};
  const std::string b64 = base64_encode(px);
  std::string out;
  out += kApcStart;
  out += "i=";
  out += std::to_string(id);
  out += ",a=t,f=32,s=1,v=1;";
  out += b64;
  out += kApcEnd;
  return out;
}

std::string detection_probe(std::uint32_t id) {
  std::string out = query_apc(id);
  out += "\x1b[c";  // DA1 request (CSI c): its answer bounds the probe window.
  return out;
}

bool probe_reply_is_kitty(std::string_view reply) {
  // A Kitty graphics response is an APC: ESC _ G … ESC \. Its presence anywhere
  // in the captured reply (before or interleaved with the DA1 answer) means the
  // terminal speaks the protocol (A4). We only need the opener — contour/kitty
  // always frame the ack as ESC _ G.
  return reply.find("\x1b_G") != std::string_view::npos;
}

}  // namespace shigoku::tui::kitty
