/**
 * Host-built tests for engine code that doesn't need the TV (PLAN.md 7.5):
 *   - LanFilter.cpp: peerAllowed (which peers may connect);
 *   - MdnsResponder.cpp: name encoding/decoding, records, answers and conflict detection;
 *   - Settings.cpp: mDNS name and backend validation.
 * Build and run: make -C vega/hcengine/test/host
 *
 * MdnsResponder.cpp is included directly so its file-local (anonymous namespace) helpers and the
 * Responder class can be reached; "private" is opened up for that include only. EngineHost is
 * stubbed below (the responder only calls instance(), noteEvent() and mdnsWanted()).
 */
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "EngineHost.h"

#define private public
#include "MdnsResponder.cpp"
#undef private

// ---- EngineHost stubs ----
namespace hcengine {
static std::vector<std::string> g_events;
EngineHost &EngineHost::instance() {
  static EngineHost h;
  return h;
}
void EngineHost::noteEvent(const std::string &msg) { g_events.push_back(msg); }
bool EngineHost::mdnsWanted(int &, std::string &) const { return false; }
}  // namespace hcengine

using namespace hcengine;

// ---- tiny test harness ----
static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_fail;                                                           \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
  } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---- helpers ----
static bool allowed4(const char *ip, bool lan) {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  inet_pton(AF_INET, ip, &sa.sin_addr);
  return peerAllowed(reinterpret_cast<sockaddr *>(&sa), lan);
}
static bool allowed6(const char *ip, bool lan) {
  sockaddr_in6 sa{};
  sa.sin6_family = AF_INET6;
  inet_pton(AF_INET6, ip, &sa.sin6_addr);
  return peerAllowed(reinterpret_cast<sockaddr *>(&sa), lan);
}
static uint32_t ip4(const char *s) {
  uint32_t a = 0;
  inet_pton(AF_INET, s, &a);
  return a;  // network order
}
static const uint8_t *u8(const std::string &s) { return reinterpret_cast<const uint8_t *>(s.data()); }
static uint16_t get16(const std::string &s, size_t off) {
  return (uint16_t)(((uint8_t)s[off] << 8) | (uint8_t)s[off + 1]);
}

// ---- peerAllowed ----
static void testPeerAllowed() {
  // loopback: always
  CHECK(allowed4("127.0.0.1", false));
  CHECK(allowed4("127.1.2.3", false));
  CHECK(allowed6("::1", false));
  CHECK(allowed6("::ffff:127.0.0.1", false));
  // private ranges: only with LAN access
  for (const char *ip : {"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.68", "169.254.10.20"}) {
    CHECK(allowed4(ip, true));
    CHECK(!allowed4(ip, false));
  }
  // just outside 172.16.0.0/12, and public addresses: never
  for (const char *ip : {"172.15.255.255", "172.32.0.1", "8.8.8.8", "192.169.0.1", "11.0.0.1"}) {
    CHECK(!allowed4(ip, true));
  }
  // IPv6: link-local and ULA with LAN; v4-mapped follows the v4 rules; global never
  CHECK(allowed6("fe80::1", true));
  CHECK(!allowed6("fe80::1", false));
  CHECK(allowed6("fd12:3456::1", true));
  CHECK(allowed6("fc00::1", true));
  CHECK(allowed6("::ffff:192.168.1.5", true));
  CHECK(!allowed6("::ffff:192.168.1.5", false));
  CHECK(!allowed6("::ffff:8.8.8.8", true));
  CHECK(!allowed6("2001:db8::1", true));
  CHECK(!allowed6("fec0::1", true));  // site-local (deprecated): not fe80::/10
  // other families / null
  CHECK(!peerAllowed(nullptr, true));
  sockaddr_un un{};
  un.sun_family = AF_UNIX;
  CHECK(!peerAllowed(reinterpret_cast<sockaddr *>(&un), true));
  // peerString
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  inet_pton(AF_INET, "192.168.1.237", &sa.sin_addr);
  CHECK_EQ(peerString(reinterpret_cast<sockaddr *>(&sa)), "192.168.1.237");
}

// ---- DNS names ----
static void testNames() {
  std::string o;
  putName(o, "hamclock.local");
  CHECK_EQ(o, std::string("\x08hamclock\x05local\x00", 16));

  size_t off = 0;
  std::string out;
  CHECK(readName(u8(o), o.size(), off, out));
  CHECK_EQ(out, "hamclock.local");
  CHECK_EQ(off, o.size());

  // compression: "_http._tcp.local" at 0, then "HamClock" + pointer to 0
  std::string p;
  putName(p, "_http._tcp.local");
  size_t second = p.size();
  p += "\x08HamClock";
  p += "\xc0";
  p += '\0';
  p += "X";  // trailing byte: off must stop after the pointer
  off = second;
  CHECK(readName(u8(p), p.size(), off, out));
  CHECK_EQ(out, "HamClock._http._tcp.local");
  CHECK_EQ(off, second + 9 + 2);

  // pointer loop: fails instead of spinning
  std::string loop("\xc0\x00", 2);
  off = 0;
  CHECK(!readName(u8(loop), loop.size(), off, out));

  // truncated label / pointer
  std::string trunc("\x08hamc", 5);
  off = 0;
  CHECK(!readName(u8(trunc), trunc.size(), off, out));
  std::string halfPtr("\xc0", 1);
  off = 0;
  CHECK(!readName(u8(halfPtr), halfPtr.size(), off, out));

  // labels over 63 bytes are cut to 63
  std::string longName(70, 'a');
  std::string lo;
  putName(lo, longName);
  CHECK_EQ((uint8_t)lo[0], 63);
}

// ---- records ----
static void testRecords() {
  Names nm;
  CHECK_EQ(nm.host(), "hamclock.local");
  CHECK_EQ(nm.instance(), "HamClock._http._tcp.local");
  nm.suffix = 2;
  CHECK_EQ(nm.host(), "hamclock-2.local");
  CHECK_EQ(nm.instance(), "HamClock (2)._http._tcp.local");
  Names shack;
  shack.base = "shack";
  CHECK_EQ(shack.host(), "shack.local");
  CHECK_EQ(shack.instance(), "HamClock shack._http._tcp.local");

  Names def;
  auto r = buildRecords(def, ip4("192.168.1.237"), 8081);
  CHECK_EQ(r.size(), 5u);
  CHECK(r[0].name == "hamclock.local" && r[0].type == T_A && r[0].unique);
  CHECK_EQ(r[0].rdata, std::string("\xc0\xa8\x01\xed", 4));
  CHECK(r[1].type == T_SRV && r[1].name == def.instance());
  CHECK_EQ(get16(r[1].rdata, 4), 8081);  // priority, weight, port
  size_t off = 6;
  std::string target;
  CHECK(readName(u8(r[1].rdata), r[1].rdata.size(), off, target));
  CHECK_EQ(target, "hamclock.local");
  CHECK(r[2].type == T_TXT);
  CHECK_EQ(r[2].rdata, std::string("\x0fpath=/live.html"));
  CHECK(r[3].type == T_PTR && r[3].name == kService && !r[3].unique);
  CHECK(r[4].type == T_PTR && r[4].name == kServices);

  // putRec: cache-flush on unique records, TTL rules
  std::string o;
  putRec(o, r[0], false, false);
  size_t p = 0;
  std::string n;
  CHECK(readName(u8(o), o.size(), p, n));
  CHECK_EQ(get16(o, p), T_A);
  CHECK_EQ(get16(o, p + 2), C_IN | C_FLUSH);
  CHECK_EQ(((uint32_t)get16(o, p + 4) << 16) | get16(o, p + 6), 120u);
  CHECK_EQ(get16(o, p + 8), 4);

  std::string legacy;
  putRec(legacy, r[2], true, false);  // legacy unicast: no flush bit, TTL capped at 10
  size_t q = 0;
  CHECK(readName(u8(legacy), legacy.size(), q, n));
  CHECK_EQ(get16(legacy, q + 2), C_IN);
  CHECK_EQ(((uint32_t)get16(legacy, q + 4) << 16) | get16(legacy, q + 6), 10u);

  std::string bye;
  putRec(bye, r[3], false, true);  // goodbye: TTL 0
  q = 0;
  CHECK(readName(u8(bye), bye.size(), q, n));
  CHECK_EQ(((uint32_t)get16(bye, q + 4) << 16) | get16(bye, q + 6), 0u);
}

// ---- responder: answers (over a loopback UDP socket) and conflicts ----
static std::string query(uint16_t id, const std::string &name, uint16_t type) {
  std::string o;
  put16(o, id); put16(o, 0);
  put16(o, 1); put16(o, 0); put16(o, 0); put16(o, 0);
  putName(o, name); put16(o, type); put16(o, C_IN);
  return o;
}

static std::string response(const std::vector<Rec> &rs) {
  std::string o;
  put16(o, 0); put16(o, 0x8400);
  put16(o, 0); put16(o, (uint16_t)rs.size()); put16(o, 0); put16(o, 0);
  for (auto &r : rs) putRec(o, r, false, false);
  return o;
}

static void testResponder() {
  // A socket on 127.0.0.1:<ephemeral>; replies to a "legacy" (non-5353) source go back to it.
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(s >= 0);
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(s, reinterpret_cast<sockaddr *>(&me), sizeof me) == 0);
  socklen_t ml = sizeof me;
  getsockname(s, reinterpret_cast<sockaddr *>(&me), &ml);

  Responder r;
  r.fd_ = s;
  r.ip_ = ip4("192.168.1.237");
  r.port_ = 8081;
  r.recs_ = buildRecords(r.names_, r.ip_, r.port_);
  r.state_ = Responder::St::Running;

  auto recvReply = [&](std::string &out) {
    pollfd pf{s, POLLIN, 0};
    if (poll(&pf, 1, 500) <= 0) return false;
    char buf[2048];
    ssize_t n = recv(s, buf, sizeof buf, 0);
    if (n <= 0) return false;
    out.assign(buf, (size_t)n);
    return true;
  };

  // A query for our host name (case-insensitive): unicast reply, id echoed, one A answer
  std::string q = query(0x1234, "HamClock.Local", T_A);
  r.handle(u8(q), q.size(), me);
  std::string rep;
  CHECK(recvReply(rep));
  if (rep.size() >= 12) {
    CHECK_EQ(get16(rep, 0), 0x1234);
    CHECK_EQ(get16(rep, 2), 0x8400);
    CHECK_EQ(get16(rep, 4), 1);  // question echoed (legacy)
    CHECK_EQ(get16(rep, 6), 1);  // one answer
    CHECK_EQ(rep.substr(rep.size() - 4), std::string("\xc0\xa8\x01\xed", 4));
  }

  // PTR for the service: PTR answer plus SRV, TXT and A as additionals
  q = query(7, "_http._tcp.local", T_PTR);
  r.handle(u8(q), q.size(), me);
  CHECK(recvReply(rep));
  if (rep.size() >= 12) {
    CHECK_EQ(get16(rep, 6), 1);
    CHECK_EQ(get16(rep, 10), 3);
  }

  // Someone else's name: no reply
  q = query(8, "other.local", T_A);
  r.handle(u8(q), q.size(), me);
  CHECK(!recvReply(rep));

  // Not answering while probing
  r.state_ = Responder::St::Probing;
  q = query(9, "hamclock.local", T_A);
  r.handle(u8(q), q.size(), me);
  CHECK(!recvReply(rep));

  // Conflicts: our own A record (same IP) is not a conflict...
  r.state_ = Responder::St::Running;
  r.announced_ = false;
  std::string resp = response({r.recs_[0]});
  r.handle(u8(resp), resp.size(), me);
  CHECK_EQ(r.names_.suffix, 1);

  // ...another host's A record for our name is: move to hamclock-2.local and probe again
  Rec other = r.recs_[0];
  other.rdata = std::string("\xc0\xa8\x01\x44", 4);  // 192.168.1.68
  resp = response({other});
  g_events.clear();
  r.handle(u8(resp), resp.size(), me);
  CHECK_EQ(r.names_.suffix, 2);
  CHECK(r.state_ == Responder::St::Probing);
  CHECK(!g_events.empty() && g_events.back().find("hamclock.local is taken") != std::string::npos);

  // While probing, any record for our (new) name means it's taken
  Rec srvOther = buildRecords(r.names_, ip4("192.168.1.68"), 8081)[0];
  resp = response({srvOther});
  r.handle(u8(resp), resp.size(), me);
  CHECK_EQ(r.names_.suffix, 3);

  // An SRV for our instance pointing at another port is a conflict once running
  r.state_ = Responder::St::Running;
  r.recs_ = buildRecords(r.names_, r.ip_, r.port_);
  auto theirs = buildRecords(r.names_, r.ip_, 9999);
  resp = response({theirs[1]});
  r.handle(u8(resp), resp.size(), me);
  CHECK_EQ(r.names_.suffix, 4);

  // Garbage doesn't crash
  std::string junk("\x00\x00\x84\x00\x00\x00\x00\x05\x00\x00\x00\x00\xc0", 13);
  r.handle(u8(junk), junk.size(), me);

  r.fd_ = -1;
  close(s);
}

// ---- Settings.cpp validators ----
static void testSettings() {
  std::string n = "  Shack-TV.local ";
  CHECK_EQ(normalizeMdnsName(n), "");
  CHECK_EQ(n, "shack-tv");
  n = "";
  CHECK_EQ(normalizeMdnsName(n), "");
  CHECK_EQ(n, "hamclock");
  n = "-bad";
  CHECK(!normalizeMdnsName(n).empty());
  n = "no_underscores";
  CHECK(!normalizeMdnsName(n).empty());
  n = std::string(41, 'a');
  CHECK(!normalizeMdnsName(n).empty());

  std::string b = " OHB.hamclock.app ";
  CHECK_EQ(normalizeBackend(b), "");
  CHECK_EQ(b, "ohb.hamclock.app:80");
  b = "";
  CHECK_EQ(normalizeBackend(b), "");
  CHECK_EQ(b, "ohb.hamclock.app:80");
  b = "host:8012";
  CHECK_EQ(normalizeBackend(b), "");
  CHECK_EQ(b, "host:8012");
  for (const char *bad : {"host:0", "host:65536", "host:", "ho st:80", ".host:80", "host:8x"}) {
    std::string x = bad;
    CHECK(!normalizeBackend(x).empty());
  }
  CHECK(validOverscan(0) && validOverscan(3) && validOverscan(5) && !validOverscan(4));
}

int main() {
  testPeerAllowed();
  testNames();
  testRecords();
  testResponder();
  testSettings();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
