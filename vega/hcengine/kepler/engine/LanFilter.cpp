/**
 * LAN access policy helpers (ARCHITECTURE.md §3.3):
 *   - which peers may connect to the engine's servers;
 *   - the TV's own LAN addresses, shown on the settings panel so a laptop user knows where to browse.
 */
#include "EngineHost.h"

#include <arpa/inet.h>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hcengine {

namespace {

bool v4Allowed(uint32_t a /* host order */, bool lan) {
  if ((a >> 24) == 127) return true;  // 127.0.0.0/8
  if (!lan) return false;
  return (a >> 24) == 10                // 10.0.0.0/8
         || (a >> 20) == 0xAC1         // 172.16.0.0/12
         || (a >> 16) == 0xC0A8        // 192.168.0.0/16
         || (a >> 16) == 0xA9FE;       // 169.254.0.0/16 (link-local)
}

}  // namespace

bool peerAllowed(const sockaddr *sa, bool lan) {
  if (!sa) return false;
  if (sa->sa_family == AF_INET) {
    return v4Allowed(ntohl(reinterpret_cast<const sockaddr_in *>(sa)->sin_addr.s_addr), lan);
  }
  if (sa->sa_family == AF_INET6) {
    const in6_addr &a = reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&a)) {
      uint32_t v4;
      memcpy(&v4, a.s6_addr + 12, 4);
      return v4Allowed(ntohl(v4), lan);
    }
    if (IN6_IS_ADDR_LOOPBACK(&a)) return true;
    if (!lan) return false;
    return (a.s6_addr[0] == 0xfe && (a.s6_addr[1] & 0xc0) == 0x80)  // fe80::/10
           || (a.s6_addr[0] & 0xfe) == 0xfc;                       // fc00::/7 (ULA)
  }
  return false;
}

std::string peerString(const sockaddr *sa) {
  char buf[INET6_ADDRSTRLEN] = "?";
  if (sa && sa->sa_family == AF_INET)
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(sa)->sin_addr, buf, sizeof buf);
  else if (sa && sa->sa_family == AF_INET6)
    inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_addr, buf, sizeof buf);
  return buf;
}

std::vector<std::string> lanIPv4Addresses() {
  std::vector<std::string> out;
  ifaddrs *ifs = nullptr;
  if (getifaddrs(&ifs) == 0) {
    for (ifaddrs *i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
      if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
      // Only addresses a LAN peer could reach us on under the filter above.
      if (!peerAllowed(i->ifa_addr, true)) continue;
      out.push_back(peerString(i->ifa_addr));
    }
    freeifaddrs(ifs);
  }
  if (!out.empty()) return out;

  // Fallback: ask the kernel which source address it would use for an outside destination.
  // connect() on UDP sends nothing.
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return out;
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(53);
  inet_pton(AF_INET, "192.0.2.1", &dst.sin_addr);  // TEST-NET-1; never contacted
  sockaddr_in me{};
  socklen_t ml = sizeof me;
  if (connect(s, reinterpret_cast<sockaddr *>(&dst), sizeof dst) == 0 &&
      getsockname(s, reinterpret_cast<sockaddr *>(&me), &ml) == 0 &&
      peerAllowed(reinterpret_cast<sockaddr *>(&me), true) &&
      (ntohl(me.sin_addr.s_addr) >> 24) != 127) {
    out.push_back(peerString(reinterpret_cast<sockaddr *>(&me)));
  }
  close(s);
  return out;
}

}  // namespace hcengine
