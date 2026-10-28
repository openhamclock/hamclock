/**
 * Minimal mDNS responder (RFC 6762) and DNS-SD advertisement (RFC 6763) for LAN access mode
 * (ARCHITECTURE.md §3.3). While LAN access is on and the engine is running, it:
 *   - claims <base>.local, probing first; on a conflict it tries <base>-2.local, -3, ...;
 *   - answers A queries for that name with the TV's LAN IPv4 address;
 *   - advertises an _http._tcp service "HamClock" (same suffix as the host name on conflict)
 *     pointing at the live web port, with TXT path=/live.html;
 *   - sends goodbyes (TTL 0) when LAN access is turned off or the engine stops.
 *
 * The TV already runs avahi-daemon on 0.0.0.0:5353 and apps can't reach its D-Bus API. This
 * socket binds 224.0.0.251:5353 with SO_REUSEADDR, so it shares multicast traffic with avahi and
 * never receives unicast datagrams meant for avahi.
 *
 * Not implemented (optional in the RFC or not needed here): known-answer suppression, response
 * delays for shared records, probe tie-breaking, AAAA/NSEC.
 */
#include "EngineHost.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace hcengine {

namespace {

constexpr uint16_t T_A = 1, T_PTR = 12, T_TXT = 16, T_SRV = 33, T_ANY = 255;
constexpr uint16_t C_IN = 1, C_FLUSH = 0x8000, Q_UNICAST = 0x8000;
constexpr int kPort = 5353;
constexpr const char *kGroup = "224.0.0.251";
constexpr const char *kBase = "hamclock";
constexpr const char *kInstanceBase = "HamClock";
constexpr const char *kService = "_http._tcp.local";
constexpr const char *kServices = "_services._dns-sd._udp.local";
constexpr int kMaxSuffix = 20;

std::mutex statusMu;
std::string g_mdnsState = "off";  // off | probing | announced | error: ...
std::string g_mdnsHost;           // e.g. hamclock-2.local once announced

void setStatus(const std::string &state, const std::string &host) {
  std::lock_guard<std::mutex> l(statusMu);
  g_mdnsState = state;
  g_mdnsHost = host;
}

long nowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// ---- wire helpers ----
void put16(std::string &o, uint16_t v) { o += (char)(v >> 8); o += (char)(v & 0xff); }
void put32(std::string &o, uint32_t v) { put16(o, (uint16_t)(v >> 16)); put16(o, (uint16_t)v); }

void putName(std::string &o, const std::string &name) {
  size_t i = 0;
  while (i < name.size()) {
    size_t j = name.find('.', i);
    if (j == std::string::npos) j = name.size();
    size_t len = j - i;
    if (len > 63) len = 63;
    o += (char)len;
    o.append(name, i, len);
    i = j + 1;
  }
  o += '\0';
}

bool readName(const uint8_t *p, size_t n, size_t &off, std::string &out) {
  out.clear();
  size_t pos = off;
  bool jumped = false;
  for (int hops = 0; hops < 32; ++hops) {
    if (pos >= n) return false;
    uint8_t len = p[pos];
    if (len == 0) {
      if (!jumped) off = pos + 1;
      return true;
    }
    if ((len & 0xc0) == 0xc0) {
      if (pos + 1 >= n) return false;
      size_t ptr = ((len & 0x3f) << 8) | p[pos + 1];
      if (!jumped) off = pos + 2;
      jumped = true;
      pos = ptr;
      continue;
    }
    if (pos + 1 + len > n) return false;
    if (!out.empty()) out += '.';
    out.append(reinterpret_cast<const char *>(p + pos + 1), len);
    pos += 1 + len;
  }
  return false;
}

struct Rec {
  std::string name;
  uint16_t type;
  bool unique;  // cache-flush bit
  uint32_t ttl;
  std::string rdata;
};

struct Names {
  int suffix = 1;
  std::string host() const { return std::string(kBase) + (suffix > 1 ? "-" + std::to_string(suffix) : "") + ".local"; }
  std::string instance() const {
    return std::string(kInstanceBase) + (suffix > 1 ? " (" + std::to_string(suffix) + ")" : "") + "." + kService;
  }
};

std::vector<Rec> buildRecords(const Names &nm, uint32_t ip /* network order */, int port) {
  std::vector<Rec> r;
  std::string a(reinterpret_cast<const char *>(&ip), 4);
  r.push_back({nm.host(), T_A, true, 120, a});
  std::string srv;
  put16(srv, 0); put16(srv, 0); put16(srv, (uint16_t)port);
  putName(srv, nm.host());
  r.push_back({nm.instance(), T_SRV, true, 120, srv});
  std::string txt = "path=/live.html";
  r.push_back({nm.instance(), T_TXT, true, 4500, std::string(1, (char)txt.size()) + txt});
  std::string ptr;
  putName(ptr, nm.instance());
  r.push_back({kService, T_PTR, false, 4500, ptr});
  std::string sptr;
  putName(sptr, kService);
  r.push_back({kServices, T_PTR, false, 4500, sptr});
  return r;
}

void putRec(std::string &o, const Rec &r, bool legacy, bool goodbye) {
  putName(o, r.name);
  put16(o, r.type);
  put16(o, (uint16_t)(C_IN | (r.unique && !legacy ? C_FLUSH : 0)));
  uint32_t ttl = goodbye ? 0 : legacy && r.ttl > 10 ? 10 : r.ttl;
  put32(o, ttl);
  put16(o, (uint16_t)r.rdata.size());
  o += r.rdata;
}

struct Question {
  std::string name;  // lower case
  uint16_t type;
};

class Responder {
public:
  void run() {
    for (;;) {
      int port = 0;
      bool want = EngineHost::instance().mdnsWanted(port);
      long now = nowMs();
      if (now - addrCheckedMs_ > 5000 || (want && ip_ == 0)) {
        addrCheckedMs_ = now;
        auto addrs = lanIPv4Addresses();
        uint32_t ip = 0;
        if (!addrs.empty()) inet_pton(AF_INET, addrs.front().c_str(), &ip);
        if (ip != ip_ && fd_ >= 0) {
          EngineHost::instance().noteEvent("mDNS: address changed; re-announcing");
          shutdownSocket(true);
        }
        ip_ = ip;
      }
      if (want && port != port_ && fd_ >= 0) shutdownSocket(true);
      port_ = port;

      if (!want || ip_ == 0) {
        if (fd_ >= 0) shutdownSocket(true);
        std::string idle = want ? "error: no LAN address" : "off";
        if (idle != idleStatus_) { setStatus(idle, ""); idleStatus_ = idle; }
        usleep(250 * 1000);
        continue;
      }

      if (fd_ < 0) {
        if (now < retryAtMs_) { usleep(250 * 1000); continue; }
        idleStatus_.clear();
        if (!openSocket()) { retryAtMs_ = now + 10000; continue; }
        names_.suffix = 1;
        startProbing();
      }

      step(nowMs());
      pollOnce(100);
    }
  }

private:
  enum class St { Off, Probing, Announcing, Running };
  St state_ = St::Off;
  int fd_ = -1;
  uint32_t ip_ = 0;
  int port_ = 0;
  long addrCheckedMs_ = 0, retryAtMs_ = 0, nextAtMs_ = 0;
  int sent_ = 0;
  bool announced_ = false;
  Names names_;
  std::vector<Rec> recs_;
  std::string idleStatus_;  // last status published while not advertising

  bool openSocket() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return fail("socket", errno);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(kPort);
    inet_pton(AF_INET, kGroup, &sa.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) { int e = errno; close(s); return fail("bind 224.0.0.251:5353", e); }
    ip_mreq mr{};
    mr.imr_multiaddr = sa.sin_addr;
    mr.imr_interface.s_addr = ip_;
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) != 0) { int e = errno; close(s); return fail("join 224.0.0.251", e); }
    in_addr ifa{};
    ifa.s_addr = ip_;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    unsigned char ttl = 255, loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    fd_ = s;
    return true;
  }

  bool fail(const char *what, int e) {
    std::string msg = std::string("error: ") + what + ": " + strerror(e);
    EngineHost::instance().noteEvent("mDNS " + msg);
    setStatus(msg, "");
    return false;
  }

  void shutdownSocket(bool goodbye) {
    if (goodbye && announced_) sendRecords(recs_, true);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    announced_ = false;
    state_ = St::Off;
  }

  void startProbing() {
    recs_ = buildRecords(names_, ip_, port_);
    state_ = St::Probing;
    sent_ = 0;
    nextAtMs_ = nowMs() + 20;  // RFC: random 0-250 ms; a small fixed delay is fine for one host
    setStatus("probing", names_.host());
  }

  void conflict() {
    std::string old = names_.host();
    if (announced_) sendRecords(recs_, true);
    announced_ = false;
    if (++names_.suffix > kMaxSuffix) names_.suffix = 1;
    EngineHost::instance().noteEvent("mDNS: " + old + " is taken; trying " + names_.host());
    startProbing();
    nextAtMs_ = nowMs() + 1000;
  }

  void step(long now) {
    if (now < nextAtMs_) return;
    switch (state_) {
      case St::Probing:
        if (sent_ < 3) { sendProbe(); ++sent_; nextAtMs_ = now + 250; }
        else { state_ = St::Announcing; sent_ = 0; nextAtMs_ = now; }
        break;
      case St::Announcing:
        sendRecords(recs_, false);
        announced_ = true;
        if (++sent_ == 1) {
          setStatus("announced", names_.host());
          EngineHost::instance().noteEvent("mDNS: announced " + names_.host() + " and service \"" +
                                           names_.instance() + "\"");
        }
        if (sent_ >= 2) state_ = St::Running;
        nextAtMs_ = now + 1000;
        break;
      default:
        break;
    }
  }

  void sendTo(const std::string &pkt, const sockaddr_in *dst) {
    sockaddr_in g{};
    if (!dst) {
      g.sin_family = AF_INET;
      g.sin_port = htons(kPort);
      inet_pton(AF_INET, kGroup, &g.sin_addr);
      dst = &g;
    }
    sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr *>(dst), sizeof *dst);
  }

  void sendProbe() {
    std::string o;
    put16(o, 0); put16(o, 0);              // id, flags (query)
    put16(o, 3); put16(o, 0);              // qd, an
    put16(o, 2); put16(o, 0);              // ns, ar
    // ANY per RFC 6762; also A, because simple responders (e.g. the Android app) only answer A.
    putName(o, names_.host()); put16(o, T_ANY); put16(o, C_IN | Q_UNICAST);
    putName(o, names_.host()); put16(o, T_A); put16(o, C_IN | Q_UNICAST);
    putName(o, names_.instance()); put16(o, T_ANY); put16(o, C_IN | Q_UNICAST);
    putRec(o, recs_[0], false, false);     // proposed A
    putRec(o, recs_[1], false, false);     // proposed SRV
    sendTo(o, nullptr);
  }

  void sendRecords(const std::vector<Rec> &rs, bool goodbye) {
    std::string o;
    put16(o, 0); put16(o, 0x8400);
    put16(o, 0); put16(o, (uint16_t)rs.size());
    put16(o, 0); put16(o, 0);
    for (auto &r : rs) putRec(o, r, false, goodbye);
    sendTo(o, nullptr);
  }

  void pollOnce(int ms) {
    pollfd pf{fd_, POLLIN, 0};
    if (poll(&pf, 1, ms) <= 0 || !(pf.revents & POLLIN)) return;
    uint8_t buf[9000];
    sockaddr_in src{};
    socklen_t sl = sizeof src;
    ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&src), &sl);
    if (n < 12) return;
    handle(buf, (size_t)n, src);
  }

  void handle(const uint8_t *p, size_t n, const sockaddr_in &src) {
    uint16_t id = (p[0] << 8) | p[1];
    uint16_t flags = (p[2] << 8) | p[3];
    uint16_t qd = (p[4] << 8) | p[5], an = (p[6] << 8) | p[7];
    uint16_t ns = (p[8] << 8) | p[9], ar = (p[10] << 8) | p[11];
    size_t off = 12;
    std::vector<Question> qs;
    for (int i = 0; i < qd; ++i) {
      Question q;
      if (!readName(p, n, off, q.name) || off + 4 > n) return;
      q.type = (p[off] << 8) | p[off + 1];
      off += 4;
      q.name = lower(q.name);
      qs.push_back(q);
    }
    if (flags & 0x8000) {  // response: look for someone else claiming our names
      checkConflicts(p, n, off, an + ns + ar);
      return;
    }
    if (state_ != St::Announcing && state_ != St::Running) return;
    answer(qs, id, src);
  }

  void checkConflicts(const uint8_t *p, size_t n, size_t off, int count) {
    std::string host = lower(names_.host()), inst = lower(names_.instance());
    for (int i = 0; i < count; ++i) {
      std::string name;
      if (!readName(p, n, off, name) || off + 10 > n) return;
      uint16_t type = (p[off] << 8) | p[off + 1];
      uint16_t rdlen = (p[off + 8] << 8) | p[off + 9];
      off += 10;
      if (off + rdlen > n) return;
      name = lower(name);
      bool mine = true;
      if (name == host && type == T_A) {
        mine = rdlen == 4 && memcmp(p + off, &ip_, 4) == 0;
      } else if (name == inst && type == T_SRV) {
        // Ours if it points at our port and host name.
        size_t t = off + 6;
        std::string target;
        mine = rdlen >= 7 && ((p[off + 4] << 8) | p[off + 5]) == port_ && readName(p, n, t, target) &&
               lower(target) == host;
      } else if (name == inst && type == T_TXT) {
        mine = true;  // TXT contents can differ harmlessly; SRV decides
      } else if (name == host || name == inst) {
        mine = state_ != St::Probing;  // while probing, any record for our names means taken
      }
      if (!mine) { conflict(); return; }
      off += rdlen;
    }
  }

  void answer(const std::vector<Question> &qs, uint16_t id, const sockaddr_in &src) {
    std::string host = lower(names_.host()), inst = lower(names_.instance());
    std::string svc = kService, svcs = kServices;
    std::set<size_t> ans, add;
    for (auto &q : qs) {
      bool any = q.type == T_ANY;
      if (q.name == host && (any || q.type == T_A)) ans.insert(0);
      if (q.name == inst) {
        if (any || q.type == T_SRV) { ans.insert(1); add.insert(0); }
        if (any || q.type == T_TXT) ans.insert(2);
      }
      if (q.name == svc && (any || q.type == T_PTR)) { ans.insert(3); add.insert(1); add.insert(2); add.insert(0); }
      if (q.name == svcs && (any || q.type == T_PTR)) ans.insert(4);
    }
    if (ans.empty()) return;
    for (size_t i : ans) add.erase(i);

    bool legacy = ntohs(src.sin_port) != kPort;  // one-shot resolver: unicast reply, echo question
    std::string o;
    put16(o, legacy ? id : 0); put16(o, 0x8400);
    put16(o, legacy ? (uint16_t)qs.size() : 0); put16(o, (uint16_t)ans.size());
    put16(o, 0); put16(o, (uint16_t)add.size());
    if (legacy)
      for (auto &q : qs) { putName(o, q.name); put16(o, q.type); put16(o, C_IN); }
    for (size_t i : ans) putRec(o, recs_[i], legacy, false);
    for (size_t i : add) putRec(o, recs_[i], legacy, false);
    sendTo(o, legacy ? &src : nullptr);
  }
};

void *responderMain(void *) {
  Responder r;
  r.run();
  return nullptr;
}

}  // namespace

void startMdnsResponder() {
  static std::once_flag once;
  std::call_once(once, [] {
    pthread_t t;
    if (pthread_create(&t, nullptr, responderMain, nullptr) == 0) pthread_detach(t);
  });
}

void mdnsStatus(std::string &state, std::string &host) {
  std::lock_guard<std::mutex> l(statusMu);
  state = g_mdnsState;
  host = g_mdnsHost;
}

}  // namespace hcengine
