// See HttpFetch.h.
#include "HttpFetch.h"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace hcengine {

namespace {

// Value after a flag such as -A or -H: "quoted", 'quoted', or up to the next blank.
bool flagValue(const std::string &cmd, const char *flag, std::string &out) {
  size_t pos = 0;
  const size_t flen = strlen(flag);
  while ((pos = cmd.find(flag, pos)) != std::string::npos) {
    // A whole word: start of string or after a blank, followed by a blank.
    bool startOk = pos == 0 || cmd[pos - 1] == ' ' || cmd[pos - 1] == '\t';
    size_t start = pos + flen;
    bool endOk = start < cmd.size() && (cmd[start] == ' ' || cmd[start] == '\t');
    if (!startOk || !endOk) {
      pos = start;
      continue;
    }
    while (start < cmd.size() && (cmd[start] == ' ' || cmd[start] == '\t')) ++start;
    if (start >= cmd.size()) return false;
    char q = cmd[start];
    if (q == '"' || q == '\'') {
      size_t end = cmd.find(q, start + 1);
      if (end == std::string::npos) return false;
      out = cmd.substr(start + 1, end - start - 1);
    } else {
      size_t end = cmd.find_first_of(" \t", start);
      out = end == std::string::npos ? cmd.substr(start) : cmd.substr(start, end - start);
    }
    return true;
  }
  return false;
}

}  // namespace

bool parseCurlCommand(const std::string &cmd, CurlRequest &out) {
  out = CurlRequest{};
  size_t u = cmd.find("https://");
  if (u == std::string::npos) u = cmd.find("http://");
  if (u == std::string::npos) return false;
  size_t e = cmd.find_first_of(" \t\r\n\"'", u);
  out.url = e == std::string::npos ? cmd.substr(u) : cmd.substr(u, e - u);
  flagValue(cmd, "-A", out.userAgent);
  flagValue(cmd, "-H", out.header);
  return true;
}

namespace fetch {

namespace {

using Clock = std::chrono::steady_clock;

struct Pending {
  CurlRequest req;
  int writeFd = -1;
  bool taken = false;
  Clock::time_point deadline;
};

std::mutex mu;
std::map<int, Pending> g_pending;
int g_nextId = 1;
int g_timeoutMs = 0;
bool g_reaper = false;

void jsonStr(std::ostringstream &o, const std::string &s) {
  o << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o << b;
        } else {
          o << c;
        }
    }
  }
  o << '"';
}

// Close expired entries; call under mu.
void sweepLocked() {
  auto now = Clock::now();
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (now >= it->second.deadline) {
      fprintf(stderr, "HcEngine: fetch %d timed out: %s\n", it->first, it->second.req.url.c_str());
      close(it->second.writeFd);
      it = g_pending.erase(it);
    } else {
      ++it;
    }
  }
}

// Write body to fd and close it. Never blocks for good: non-blocking writes, each wait for room
// is bounded, and SIGPIPE (HamClock closed its end) is blocked in this thread only, so it never
// reaches the app.
void writeBody(int fd, std::string body) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  size_t off = 0;
  while (off < body.size()) {
    ssize_t n = write(fd, body.data() + off, body.size() - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p{fd, POLLOUT, 0};
      if (poll(&p, 1, kTimeoutMs) > 0 && !(p.revents & (POLLERR | POLLHUP))) continue;
    }
    break;  // EPIPE, timeout or another error: HamClock isn't reading any more
  }
  close(fd);
}

void startReaperLocked() {
  if (g_reaper) return;
  g_reaper = true;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      sweep();
    }
  }).detach();
}

}  // namespace

bool open(const CurlRequest &req, int *readFd) {
  if (!readFd || req.url.empty()) return false;
  int fds[2];
  if (pipe(fds) != 0) {
    fprintf(stderr, "HcEngine: fetch pipe: %s\n", strerror(errno));
    return false;
  }
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
  std::lock_guard<std::mutex> l(mu);
  sweepLocked();
  int id = g_nextId++;
  Pending p;
  p.req = req;
  p.writeFd = fds[1];
  p.deadline = Clock::now() + std::chrono::milliseconds(g_timeoutMs > 0 ? g_timeoutMs : kTimeoutMs);
  g_pending[id] = std::move(p);
  startReaperLocked();
  *readFd = fds[0];
  return true;
}

std::string take() {
  std::lock_guard<std::mutex> l(mu);
  sweepLocked();
  std::ostringstream o;
  int n = 0;
  for (auto &[id, p] : g_pending) {
    if (p.taken) continue;
    p.taken = true;
    o << (n++ ? "," : "[") << "{\"id\":" << id << ",\"url\":";
    jsonStr(o, p.req.url);
    o << ",\"userAgent\":";
    jsonStr(o, p.req.userAgent);
    o << ",\"header\":";
    jsonStr(o, p.req.header);
    o << "}";
  }
  if (!n) return "";
  o << "]";
  return o.str();
}

void done(int id, bool ok, const std::string &body) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = g_pending.find(id);
    if (it != g_pending.end()) {
      fd = it->second.writeFd;
      g_pending.erase(it);
    }
    sweepLocked();
  }
  if (fd < 0) return;
  if (!ok || body.empty()) {
    close(fd);
    return;
  }
  std::string b = body.size() > kMaxBody ? body.substr(0, kMaxBody) : body;
  std::thread(writeBody, fd, std::move(b)).detach();
}

void closeAll() {
  std::lock_guard<std::mutex> l(mu);
  for (auto &[id, p] : g_pending) close(p.writeFd);
  g_pending.clear();
}

int pending() {
  std::lock_guard<std::mutex> l(mu);
  return (int)g_pending.size();
}

void sweep() {
  std::lock_guard<std::mutex> l(mu);
  sweepLocked();
}

void setTimeoutForTest(int ms) {
  std::lock_guard<std::mutex> l(mu);
  g_timeoutMs = ms;
}

}  // namespace fetch
}  // namespace hcengine

// Called by HamClock's WiFiClient::connectCommand under _IS_VEGA (ESPHamClock/ArduinoLib).
extern "C" bool vega_connect_http(const char *cmd, int *read_fd) {
  if (!cmd || !read_fd) return false;
  hcengine::CurlRequest req;
  if (!hcengine::parseCurlCommand(cmd, req)) {
    fprintf(stderr, "HcEngine: fetch: no URL in command\n");
    return false;
  }
  return hcengine::fetch::open(req, read_fd);
}

