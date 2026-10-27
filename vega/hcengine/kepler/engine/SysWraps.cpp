/**
 * Linker wraps (-Wl,--wrap=<sym>) that adapt HamClock's process-level system calls to life on a
 * thread inside the app. Only calls made from code linked into libHcEngine.so are affected.
 *
 *   exit       engine thread: end the thread (the host decides what happens next);
 *              other engine helper threads: end just that thread. Never terminates the app.
 *   fork       fails with ENOSYS. Forking a multi-threaded app process is unsafe, and the paths
 *              that use it (OTA update, CPU temperature, brightness) don't apply on a TV.
 *              popen()/system() are left alone: glibc implements them with posix_spawn, and
 *              some features shell out to curl.
 *   bind       TCP listeners go to 127.0.0.1 unless LAN access is on; retries while the port is
 *              still held (e.g. by the previous app process during a relaunch).
 *   accept     tracks connections; a thread accepting on a retired listener ends.
 *   close      keeps the tracking sets accurate so we never touch a reused descriptor.
 *   getrusage  RUSAGE_SELF on the engine thread becomes RUSAGE_THREAD, so the engine's CPU
 *              throttle measures itself rather than the whole app (JS, WebView, ...).
 *   pthread_create  starts engine threads through a trampoline that catches EngineThreadExit.
 *
 * Threads are ended by throwing hcengine::EngineThreadExit, never pthread_exit (see EngineHost.h).
 */
#include "EngineHost.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_accept(int fd, struct sockaddr *addr, socklen_t *len);
int __real_close(int fd);
int __real_getrusage(int who, struct rusage *usage);
int __real_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
}

namespace hcengine {
namespace {

constexpr int kBindRetries = 40;       // x 250 ms = 10 s
constexpr useconds_t kBindRetryUs = 250 * 1000;

std::mutex smu;

// A tracked socket. The inode lets us tell whether an fd number still refers to the same socket:
// some engine paths close sockets inside libc (e.g. ws.cpp uses fdopen()+fclose()), bypassing the
// close wrap, after which the app may reuse the number for something of its own.
struct SockId {
  dev_t dev;
  ino_t ino;
};
std::map<int, SockId> listenFds;  // engine TCP listeners currently open
std::map<int, SockId> clientFds;  // connections accepted by engine listeners
std::map<int, SockId> retiredFds; // listeners from a finished run; kept open so the fd isn't reused

bool sockId(int fd, SockId &id) {
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;
  id = {st.st_dev, st.st_ino};
  return true;
}

bool stillSame(int fd, const SockId &id) {
  SockId now{};
  return sockId(fd, now) && now.dev == id.dev && now.ino == id.ino;
}

bool isRetired(int fd) {  // caller holds smu
  auto it = retiredFds.find(fd);
  if (it == retiredFds.end()) return false;
  if (stillSame(fd, it->second)) return true;
  retiredFds.erase(it);  // number now belongs to another socket
  return false;
}

bool isTcp(int fd) {
  int type = 0;
  socklen_t l = sizeof type;
  return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) == 0 && type == SOCK_STREAM;
}

}  // namespace

/** Called by the supervisor after an engine run ends, before the next run binds. */
void retireEngineSockets() {
  std::lock_guard<std::mutex> l(smu);
  int nc = 0, nl = 0;
  for (auto &[fd, id] : clientFds) {
    if (stillSame(fd, id)) { shutdown(fd, SHUT_RDWR); ++nc; }  // wakes ws client threads
  }
  clientFds.clear();
  for (auto &[fd, id] : listenFds) {
    if (!stillSame(fd, id)) continue;
    shutdown(fd, SHUT_RDWR);  // wakes accept(); the port can be bound again
    retiredFds[fd] = id;
    ++nl;
  }
  listenFds.clear();
  fprintf(stderr, "HcEngine: retired %d listeners, %d connections\n", nl, nc);
}

}  // namespace hcengine

using hcengine::EngineHost;

extern "C" {

[[noreturn]] void __wrap_exit(int code) {
  if (EngineHost::instance().isEngineThread()) EngineHost::instance().endEngineThread(code, nullptr);
  fprintf(stderr, "HcEngine: exit(%d) on engine helper thread; ending that thread\n", code);
  throw hcengine::EngineThreadExit{"exit on helper thread"};
}

pid_t __wrap_fork(void) {
  errno = ENOSYS;
  return -1;
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  if (!addr || addr->sa_family != AF_INET || len < (socklen_t)sizeof(sockaddr_in) || !hcengine::isTcp(fd))
    return __real_bind(fd, addr, len);

  sockaddr_in sin = *reinterpret_cast<const sockaddr_in *>(addr);
  if (sin.sin_addr.s_addr == htonl(INADDR_ANY) && !EngineHost::instance().lanAccess())
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int rc = -1;
  for (int i = 0; i < hcengine::kBindRetries; ++i) {
    rc = __real_bind(fd, reinterpret_cast<sockaddr *>(&sin), sizeof sin);
    if (rc == 0 || errno != EADDRINUSE) break;
    usleep(hcengine::kBindRetryUs);
  }
  if (rc == 0) {
    hcengine::SockId id{};
    if (hcengine::sockId(fd, id)) {
      std::lock_guard<std::mutex> l(hcengine::smu);
      hcengine::retiredFds.erase(fd);
      hcengine::listenFds[fd] = id;
    }
    EngineHost::instance().noteListening(ntohs(sin.sin_port));
  }
  return rc;
}

int __wrap_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  std::unique_lock<std::mutex> l(hcengine::smu);
  if (hcengine::isRetired(fd)) {
    l.unlock();
    // The engine thread polls with select()+accept() (WiFiServer::available()). The previous run's
    // restful_server pointer is still polled until initWebServer() replaces it, so just fail.
    // Helper threads block in accept loops and must end instead.
    if (EngineHost::instance().isEngineThread()) {
      errno = EINVAL;
      return -1;
    }
    throw hcengine::EngineThreadExit{"accept on retired listener"};
  }
  l.unlock();
  int c = __real_accept(fd, addr, len);
  int err = errno;
  hcengine::SockId cid{};
  bool haveId = c >= 0 && hcengine::sockId(c, cid);
  l.lock();
  if (hcengine::isRetired(fd)) {
    if (c >= 0) __real_close(c);
    l.unlock();
    if (EngineHost::instance().isEngineThread()) {
      errno = EINVAL;
      return -1;
    }
    throw hcengine::EngineThreadExit{"listener retired during accept"};
  }
  if (haveId) hcengine::clientFds[c] = cid;
  errno = err;
  return c;
}

int __wrap_close(int fd) {
  {
    std::lock_guard<std::mutex> l(hcengine::smu);
    hcengine::clientFds.erase(fd);
    hcengine::listenFds.erase(fd);
    hcengine::retiredFds.erase(fd);
  }
  return __real_close(fd);
}

// Every thread the engine creates starts here, so EngineThreadExit (or any stray exception)
// ends just that thread instead of reaching std::terminate and killing the app.
namespace {
struct ThreadStart {
  void *(*fn)(void *);
  void *arg;
};
void *threadTrampoline(void *p) {
  ThreadStart ts = *static_cast<ThreadStart *>(p);
  delete static_cast<ThreadStart *>(p);
  try {
    return ts.fn(ts.arg);
  } catch (const hcengine::EngineThreadExit &) {
  } catch (...) {
    fprintf(stderr, "HcEngine: engine helper thread threw; ending that thread\n");
  }
  return nullptr;
}
}  // namespace

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
  auto *ts = new ThreadStart{fn, arg};
  int e = __real_pthread_create(t, a, threadTrampoline, ts);
  if (e != 0) delete ts;
  return e;
}

int __wrap_getrusage(int who, struct rusage *usage) {
  if (who == RUSAGE_SELF && EngineHost::instance().isEngineThread()) who = RUSAGE_THREAD;
  return __real_getrusage(who, usage);
}

}  // extern "C"
