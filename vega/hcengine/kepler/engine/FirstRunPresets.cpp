/**
 * First-run presets, written through HamClock's NV API before HamClock's first Setup.
 *
 * Built as part of the hamclock_engine object library (needs HamClock.h). Called on the engine
 * thread before hamclock_main(), only while no callsign is stored, so NV is never touched while
 * the engine runs.
 *
 * A TV has no GPS and HamClock can't set its clock, so on a fresh install:
 *   - DE location from IP geolocation (the backend's fetchIPGeoloc.pl, the same request as
 *     wifi.cpp geolocateIP). HamClock's own -g lookup runs in initSys(), after Setup, so Setup
 *     wouldn't show it, and it would overwrite what the user entered there. Done here, the
 *     location is just HamClock's stored DE: Setup shows it and the user can edit it.
 *   - NTP = "Computer" (use the system time).
 *
 * Each preset is written only if its key doesn't exist yet. HamClock writes its own defaults on
 * its first start, so the presets apply once per install and never overwrite a later edit (TVs
 * rarely move). If the geoIP lookup fails on that first start, HamClock's default location stays.
 */
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "HamClock.h"

namespace hcengine {

namespace {

// setup.cpp's NTPSC_* list is local to that file: No=0, Built-in=1, Computer=2, host=3.
constexpr uint8_t kNtpComputer = 2;
constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxReply = 8192;

/** connect() with a timeout. Returns the socket or -1. */
int connectTimeout(const addrinfo *ai) {
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) return -1;
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
  if (rc < 0 && errno == EINPROGRESS) {
    pollfd p{fd, POLLOUT, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (poll(&p, 1, kTimeoutMs) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
      rc = 0;
  }
  if (rc != 0) { close(fd); return -1; }
  fcntl(fd, F_SETFL, fl);
  timeval tv{kTimeoutMs / 1000, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

/** GET a /ham/HamClock page from backend ("host:port"). Returns the body, or '' and err. */
std::string backendGet(const std::string &backend, const char *page, std::string &err) {
  std::string host = backend, port = "80";
  size_t colon = backend.rfind(':');
  if (colon != std::string::npos) { host = backend.substr(0, colon); port = backend.substr(colon + 1); }

  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int g = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (g != 0) { err = std::string("lookup ") + host + ": " + gai_strerror(g); return ""; }
  int fd = -1;
  for (addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) fd = connectTimeout(ai);
  freeaddrinfo(res);
  if (fd < 0) { err = "can't connect to " + backend; return ""; }

  std::string req = std::string("GET /ham/HamClock") + page + " HTTP/1.0\r\nHost: " + host +
                    "\r\nUser-Agent: " + platform + "/" + hc_version + "\r\nConnection: close\r\n\r\n";
  std::string reply;
  if (send(fd, req.data(), req.size(), 0) == (ssize_t)req.size()) {
    char buf[1024];
    ssize_t n;
    while (reply.size() < kMaxReply && (n = recv(fd, buf, sizeof buf, 0)) > 0) reply.append(buf, (size_t)n);
  }
  close(fd);

  size_t body = reply.find("\r\n\r\n");
  if (reply.compare(0, 5, "HTTP/") != 0 || body == std::string::npos) { err = "no reply from " + backend; return ""; }
  size_t sp = reply.find(' ');
  int status = sp != std::string::npos ? atoi(reply.c_str() + sp + 1) : 0;
  if (status != 200) { err = "HTTP " + std::to_string(status) + " from " + backend; return ""; }
  return reply.substr(body + 4);
}

/** "KEY=value" lines from body. */
std::string field(const std::string &body, const char *key) {
  std::string k = std::string(key) + "=";
  for (size_t at = 0; at < body.size();) {
    size_t nl = body.find('\n', at);
    std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  return "";
}

std::string presetLocation(const std::string &backend) {
  std::string err;
  std::string body = backendGet(backend, "/fetchIPGeoloc.pl", err);
  if (body.empty() && err.empty()) err = "empty reply";
  std::string lat = field(body, "LAT"), lng = field(body, "LNG");
  if (err.empty() && (lat.empty() || lng.empty())) {
    // Anything else is an error message on the first line (see geolocateIP).
    err = body.substr(0, body.find('\n'));
  }
  if (!err.empty()) return "first-run preset: geoIP failed (" + err + "); HamClock's default location stays";

  LatLong ll;
  ll.lat_d = (float)atof(lat.c_str());
  ll.lng_d = (float)atof(lng.c_str());
  ll.normalize();
  NVWriteFloat(NV_DE_LAT, ll.lat_d);
  NVWriteFloat(NV_DE_LNG, ll.lng_d);
  setNVMaidenhead(NV_DE_GRID, ll);  // setup.cpp resets all three unless all three exist
  char msg[160];
  snprintf(msg, sizeof msg, "first-run preset: DE %.2f %.2f from geoIP (%s)", ll.lat_d, ll.lng_d,
           field(body, "CREDIT").c_str());
  return msg;
}

}  // namespace

std::vector<std::string> firstRunPresets(const std::string &dataDir, const std::string &backend) {
  // Same value hamclock_main derives from -d; the NV code needs it to find the eeprom file.
  our_dir = dataDir;
  if (our_dir.empty() || our_dir.back() != '/') our_dir += '/';

  std::vector<std::string> notes;
  uint8_t ntp;
  if (!NVReadUInt8(NV_NTPSET, &ntp)) {
    NVWriteUInt8(NV_NTPSET, kNtpComputer);
    notes.push_back("first-run preset: NTP = Computer");
  }
  float f;
  if (!NVReadFloat(NV_DE_LAT, &f)) notes.push_back(presetLocation(backend));
  return notes;
}

}  // namespace hcengine
