#include "senshi_oct.hpp"

#include <nlohmann/json.hpp>

#include "subprocess.hpp"

namespace shigoku::senshi::oct {

using json = nlohmann::json;

// Browser surface only as far as vendor.js reads it: the origin it hashes,
// the UA it sends, a current-script dataset (its gateway override), inert
// DOM/storage stubs, and WebCrypto. Nothing from npm; Node's own fetch,
// vm, WebAssembly and WebCrypto do the rest. Globals go in through
// defineProperty because newer Node ships read-only `navigator`/`crypto`
// accessors that plain assignment cannot replace.
const char* const kShim = R"JS(
'use strict';
const vm = require('vm');
const [, , id, ua] = process.argv;
const UA = ua || 'Mozilla/5.0';
const ORIGIN = 'https://senshi.to';
const HDRS = { 'User-Agent': UA, Origin: ORIGIN, Referer: ORIGIN + '/', Accept: '*/*' };
const fail = (msg, code) => { process.stdout.write(JSON.stringify({ error: String(msg) })); process.exit(code); };
const realFetch = globalThis.fetch;
if (typeof realFetch !== 'function') fail('node 18 or newer is needed (no global fetch)', 3);
const def = (name, value) => Object.defineProperty(globalThis, name, { value, configurable: true, writable: true });
const el = () => ({ style: {}, dataset: {}, setAttribute() {}, getAttribute: () => null, appendChild() {}, remove() {}, addEventListener() {}, removeEventListener() {} });
def('fetch', (u, init = {}) => realFetch(u, { ...init, headers: { ...HDRS, ...(init.headers || {}) } }));
def('window', globalThis); def('self', globalThis); def('top', globalThis); def('parent', globalThis);
def('location', { href: ORIGIN + '/', hostname: 'senshi.to', host: 'senshi.to', origin: ORIGIN, protocol: 'https:', pathname: '/', search: '', hash: '' });
def('navigator', { userAgent: UA, language: 'en-US', languages: ['en-US'], platform: 'MacIntel', hardwareConcurrency: 4, webdriver: false, onLine: true });
def('document', { currentScript: { src: 'https://cdn.vidcloud.se/vjs/vendor.js', dataset: {}, getAttribute: () => null }, referrer: ORIGIN + '/', cookie: '', createElement: el, head: el(), body: el(), documentElement: el(), querySelector: () => null, querySelectorAll: () => [], addEventListener() {}, removeEventListener() {}, hidden: false, visibilityState: 'visible', readyState: 'complete' });
const storage = () => ({ getItem: () => null, setItem() {}, removeItem() {}, clear() {} });
def('localStorage', storage()); def('sessionStorage', storage());
def('screen', { width: 1920, height: 1080, availWidth: 1920, availHeight: 1080 });
def('innerWidth', 1920); def('innerHeight', 1080); def('devicePixelRatio', 1);
def('addEventListener', () => {}); def('removeEventListener', () => {}); def('matchMedia', () => ({ matches: false }));
def('requestAnimationFrame', (f) => setTimeout(f, 16)); def('cancelAnimationFrame', clearTimeout);
def('MediaSource', Object.assign(function () {}, { isTypeSupported: () => true }));
if (!globalThis.crypto) def('crypto', require('crypto').webcrypto);
(async () => {
  if (!/^\d+$/.test(id || '')) throw new Error('usage: node - <remote_source_id> <user_agent>');
  const res = await realFetch('https://cdn.vidcloud.se/vjs/vendor.js', { headers: HDRS });
  if (!res.ok) throw new Error('vendor.js: HTTP ' + res.status);
  vm.runInThisContext(await res.text(), { filename: 'vendor.js' });
  if (!globalThis.__oct || typeof globalThis.__oct.open !== 'function') throw new Error('the runtime did not define __oct.open');
  const r = await globalThis.__oct.open(Number(id));
  process.stdout.write(JSON.stringify(r));
  process.exit(0);
})().catch((e) => fail((e && e.message) || e, 1));
)JS";

Result<std::string, ProviderError> open(const std::vector<std::string>& node_argv,
                                        std::int64_t remote_source_id,
                                        std::string_view user_agent) {
  if (node_argv.empty() || remote_source_id <= 0) return err(ProviderError::unsupported());
  std::vector<std::string> argv = node_argv;
  argv.emplace_back("-");
  argv.push_back(std::to_string(remote_source_id));
  argv.emplace_back(user_agent);
  auto ran = subprocess::run(argv, kShim, kTimeoutMs);
  if (!ran.has_value()) {
    if (ran.error().kind == subprocess::Failure::Kind::NotFound) {
      ProviderError e = ProviderError::unsupported();
      e.detail = "node not found (" + ran.error().detail + ")";
      return err(std::move(e));
    }
    return err(ProviderError::decode("senshi runtime: " + ran.error().detail));
  }
  if (ran->exit_code != 0) {
    // The shim reports its own failure as {"error": ...} on stdout.
    std::string why = "exit " + std::to_string(ran->exit_code);
    try {
      const json j = json::parse(ran->out);
      if (j.is_object() && j.contains("error") && j.at("error").is_string()) {
        why += ": " + j.at("error").get<std::string>();
      }
    } catch (const json::parse_error&) {
    }
    return err(ProviderError::decode("senshi runtime: " + why));
  }
  return ran->out;
}

}  // namespace shigoku::senshi::oct
