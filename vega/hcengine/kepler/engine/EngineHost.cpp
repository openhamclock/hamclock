/**
 * EngineHost implementation. See EngineHost.h for the model.
 */
#include "EngineHost.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>

// Engine entry point and embed hooks (ESPHamClock/ArduinoLib/Arduino.h).
extern int hamclock_main(int ac, char *av[]);
extern volatile bool hamclock_stop_requested;
extern void (*hamclock_restart_hook)(char *argv[]);
extern void (*hamclock_stop_hook)(void);
extern const char *hc_version;

namespace hcengine {

void retireEngineSockets();  // SysWraps.cpp
std::string seedFirstRun(const std::string &dataDir, const std::string &call, double lat,
                         double lng);  // NvSeed.cpp

namespace {

constexpr size_t kEngineStack = 8 * 1024 * 1024;
constexpr size_t kMaxEvents = 30;

std::mutex mu;
EngineState g_state = EngineState::Idle;
EngineOptions g_opts;
bool g_lan = false;
bool g_supervisorAlive = false;
bool g_stopRequested = false;
bool g_restartPending = false;
std::vector<std::string> g_runArgs;      // args for the next/current run
std::vector<std::string> g_pendingArgs;  // args for a requested restart
int g_runCount = 0;
int g_exitCode = INT_MIN;                // INT_MIN = none
std::string g_lastError;
std::set<int> g_listening;
std::deque<std::string> g_events;
int g_rejected = 0;                      // connections dropped by the LAN filter (this session)
std::string g_lastRejected;
thread_local bool t_isEngine = false;

// argv storage for the current run; hamclock keeps a pointer to it (our_argv).
std::vector<std::string> g_argvStore;
std::vector<char *> g_argv;

// Diagnostic watchdog (EngineOptions.watchdogSec): abort() if getStatus() isn't called for that
// long, so the crash report captures every thread's stack. Off (0) in production.
std::atomic<long> g_lastStatusMs{0};
std::atomic<int> g_watchdogSec{0};

long nowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void *watchdogMain(void *) {
  for (;;) {
    sleep(1);
    int lim = g_watchdogSec.load();
    if (lim > 0 && nowMs() - g_lastStatusMs.load() > lim * 1000L) {
      fprintf(stderr, "HcEngine: watchdog: getStatus not called for %d s; aborting\n", lim);
      abort();
    }
  }
  return nullptr;
}

void event(const std::string &msg) {  // caller holds mu
  fprintf(stderr, "HcEngine: %s\n", msg.c_str());
  g_events.push_back(msg);
  if (g_events.size() > kMaxEvents) g_events.pop_front();
}

std::string joinArgs(const std::vector<std::string> &a) {
  std::string s;
  for (auto &x : a) s += (s.empty() ? "" : " ") + x;
  return s;
}

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
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o << b; }
        else o << c;
    }
  }
  o << '"';
}

void mkdirs(const std::string &path) {
  std::string cur;
  std::istringstream ss(path);
  for (std::string part; std::getline(ss, part, '/');) {
    if (part.empty()) { if (cur.empty()) cur = "/"; continue; }
    cur += (cur.empty() || cur.back() == '/' ? "" : "/") + part;
    mkdir(cur.c_str(), 0755);
  }
}

// ---- saved LAN access choice ----
std::string lanFile(const std::string &dataDir) { return dataDir + "/vega-lan-access"; }

bool readSavedLan(const std::string &dataDir) {
  FILE *f = fopen(lanFile(dataDir).c_str(), "r");
  if (!f) return false;
  int c = fgetc(f);
  fclose(f);
  return c == '1';
}

void saveLan(const std::string &dataDir, bool on) {  // caller holds mu
  if (dataDir.empty()) return;
  std::string path = lanFile(dataDir), tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "w");
  if (!f) { event("can't save LAN setting: " + std::string(strerror(errno))); return; }
  fputs(on ? "1\n" : "0\n", f);
  fclose(f);
  rename(tmp.c_str(), path.c_str());
}

// ---- engine hooks (run on the engine thread) ----
void restartHook(char *argv[]) {
  std::vector<std::string> args;
  for (int i = 0; argv && argv[i]; ++i) args.emplace_back(argv[i]);
  EngineHost::instance().endEngineThread(0, &args);
}

void stopHook() {
  if (EngineHost::instance().isEngineThread()) EngineHost::instance().endEngineThread(0, nullptr);
}

// ---- tiny JSON reader for flat EngineOptions objects ----
struct Json {
  const std::string &s;
  size_t i = 0;
  std::string err;
  explicit Json(const std::string &str) : s(str) {}
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
  bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
  bool str(std::string &out) {
    ws();
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'u': i += 4; out += '?'; break;  // not needed for options
          default: out += e;
        }
      } else out += c;
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool fail(const char *m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
};

}  // namespace

const char *stateName(EngineState s) {
  switch (s) {
    case EngineState::Idle: return "idle";
    case EngineState::Starting: return "starting";
    case EngineState::Running: return "running";
    case EngineState::Stopping: return "stopping";
    case EngineState::Stopped: return "stopped";
    case EngineState::Exited: return "exited";
    case EngineState::Error: return "error";
  }
  return "?";
}

std::string parseOptions(const std::string &json, EngineOptions &o) {
  if (json.empty()) return "";
  Json j(json);
  if (!j.eat('{')) return "options: expected object";
  if (j.eat('}')) return "";
  do {
    std::string key;
    if (!j.str(key) || !j.eat(':')) return "options: " + (j.err.empty() ? "expected ':'" : j.err);
    j.ws();
    char c = j.i < json.size() ? json[j.i] : 0;
    if (c == '"') {
      std::string v;
      if (!j.str(v)) return "options: " + j.err;
      if (key == "dataDir") o.dataDir = v;
      else if (key == "backend") o.backend = v;
      else if (key == "seedCallsign") o.seedCallsign = v;
    } else if (c == '[') {
      ++j.i;
      std::vector<std::string> arr;
      if (!j.eat(']')) {
        do {
          std::string v;
          if (!j.str(v)) return "options: " + j.err;
          arr.push_back(v);
        } while (j.eat(','));
        if (!j.eat(']')) return "options: expected ']'";
      }
      if (key == "extraArgs") o.extraArgs = arr;
    } else if (json.compare(j.i, 4, "true") == 0 || json.compare(j.i, 5, "false") == 0) {
      bool v = json[j.i] == 't';
      j.i += v ? 4 : 5;
      if (key == "lanAccess") { o.lanAccess = v; o.lanAccessSet = true; }
    } else if (json.compare(j.i, 4, "null") == 0) {
      j.i += 4;
    } else {
      char *end = nullptr;
      double v = strtod(json.c_str() + j.i, &end);
      if (end == json.c_str() + j.i) return "options: bad value for " + key;
      j.i = end - json.c_str();
      if (key == "restPort") o.restPort = (int)v;
      else if (key == "liveRwPort") o.liveRwPort = (int)v;
      else if (key == "liveRoPort") o.liveRoPort = (int)v;
      else if (key == "watchdogSec") o.watchdogSec = (int)v;
      else if (key == "seedLat") o.seedLat = v;
      else if (key == "seedLng") o.seedLng = v;
    }
  } while (j.eat(','));
  if (!j.eat('}')) return "options: expected '}'";
  return "";
}

std::vector<std::string> buildArgs(const EngineOptions &o) {
  std::vector<std::string> a = {"hamclock", "-d", o.dataDir};
  // Always pass -b: without a backend the REST server on 8080 never comes up.
  a.push_back("-b");
  a.push_back(o.backend.empty() ? std::string("ohb.hamclock.app:80") : o.backend);
  a.push_back("-e"); a.push_back(std::to_string(o.restPort));
  a.push_back("-w"); a.push_back(std::to_string(o.liveRwPort));
  if (o.liveRoPort != 0) { a.push_back("-r"); a.push_back(std::to_string(o.liveRoPort)); }
  a.insert(a.end(), o.extraArgs.begin(), o.extraArgs.end());
  return a;
}

EngineHost &EngineHost::instance() {
  static EngineHost h;
  return h;
}

bool EngineHost::lanAccess() const {
  std::lock_guard<std::mutex> l(mu);
  return g_lan;
}

bool EngineHost::isEngineThread() const { return t_isEngine; }

void EngineHost::noteListening(int port) {
  std::lock_guard<std::mutex> l(mu);
  g_listening.insert(port);
  event("listening on " + std::string(g_lan ? "0.0.0.0" : "127.0.0.1") + ":" + std::to_string(port));
  if (port == g_opts.liveRwPort && g_state == EngineState::Starting) g_state = EngineState::Running;
}

void EngineHost::noteRejected(const std::string &peer) {
  std::lock_guard<std::mutex> l(mu);
  ++g_rejected;
  // One event per new address, so a scanner can't flush the event ring.
  if (peer != g_lastRejected) event("refused connection from " + peer + (g_lan ? " (not a LAN address)" : " (LAN access off)"));
  g_lastRejected = peer;
}

void EngineHost::noteEvent(const std::string &msg) {
  std::lock_guard<std::mutex> l(mu);
  event(msg);
}

bool EngineHost::mdnsWanted(int &port) const {
  std::lock_guard<std::mutex> l(mu);
  port = g_opts.liveRwPort;
  return g_lan && g_supervisorAlive;
}

void EngineHost::endEngineThread(int exitCode, std::vector<std::string> *restartArgs) {
  {
    std::lock_guard<std::mutex> l(mu);
    if (restartArgs) {
      g_pendingArgs = *restartArgs;
      g_restartPending = true;
      event("engine requested restart: " + joinArgs(g_pendingArgs));
    } else if (!hamclock_stop_requested) {
      g_exitCode = exitCode;
      event("engine called exit(" + std::to_string(exitCode) + ")");
    }
  }
  throw EngineThreadExit{restartArgs ? "restart" : hamclock_stop_requested ? "stop" : "exit"};
}

void *EngineHost::engineThreadMain(void *) {
  t_isEngine = true;
  std::string how;
  try {
    EngineOptions o;
    {
      std::lock_guard<std::mutex> l(mu);
      o = g_opts;
    }
    std::string seeded = seedFirstRun(o.dataDir, o.seedCallsign, o.seedLat, o.seedLng);
    if (!seeded.empty()) {
      std::lock_guard<std::mutex> l(mu);
      event(seeded);
    }
    int rc = hamclock_main((int)g_argv.size() - 1, g_argv.data());
    how = "hamclock_main returned " + std::to_string(rc);
  } catch (const EngineThreadExit &x) {
    how = std::string("engine thread ended: ") + x.why;
  } catch (const std::exception &e) {
    how = std::string("engine threw: ") + e.what();
  } catch (...) {
    how = "engine threw an unknown exception";
  }
  std::lock_guard<std::mutex> l(mu);
  if (how.compare(0, 12, "engine threw") == 0) g_lastError = how;
  event(how);
  return nullptr;
}

void *EngineHost::supervisorThreadMain(void *) {
  instance().supervisorMain();
  return nullptr;
}

void EngineHost::supervisorMain() {
  for (;;) {
    {
      std::lock_guard<std::mutex> l(mu);
      g_argvStore = g_runArgs;
      g_argv.clear();
      for (auto &s : g_argvStore) g_argv.push_back(s.data());
      g_argv.push_back(nullptr);
      g_listening.clear();
      g_exitCode = INT_MIN;
      g_state = EngineState::Starting;
      ++g_runCount;
      hamclock_stop_requested = false;
      event("run " + std::to_string(g_runCount) + ": " + joinArgs(g_argvStore));
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kEngineStack);
    pthread_t tid;
    int e = pthread_create(&tid, &attr, engineThreadMain, nullptr);
    pthread_attr_destroy(&attr);
    if (e != 0) {
      std::lock_guard<std::mutex> l(mu);
      g_lastError = std::string("pthread_create: ") + strerror(e);
      event(g_lastError);
      g_state = EngineState::Error;
      g_supervisorAlive = false;
      return;
    }
    pthread_join(tid, nullptr);

    // Old server threads may still hold the ports; retire them before the next run binds.
    retireEngineSockets();

    std::lock_guard<std::mutex> l(mu);
    if (g_restartPending && !g_stopRequested) {
      g_runArgs = g_pendingArgs;
      g_restartPending = false;
      continue;
    }
    g_restartPending = false;
    g_state = g_stopRequested ? EngineState::Stopped : EngineState::Exited;
    event(std::string("engine ") + stateName(g_state));
    g_supervisorAlive = false;
    return;
  }
}

std::string EngineHost::start(const EngineOptions &opts) {
  std::lock_guard<std::mutex> l(mu);
  if (g_supervisorAlive) return "already running";
  hamclock_restart_hook = restartHook;
  hamclock_stop_hook = stopHook;
  g_opts = opts;
  mkdirs(opts.dataDir);
  if (!opts.lanAccessSet) g_opts.lanAccess = readSavedLan(opts.dataDir);
  g_lan = g_opts.lanAccess;
  saveLan(g_opts.dataDir, g_lan);
  g_runArgs = buildArgs(g_opts);
  g_stopRequested = false;
  g_restartPending = false;
  g_lastError.clear();

  if (opts.watchdogSec > 0) {
    g_lastStatusMs = nowMs();
    if (g_watchdogSec.exchange(opts.watchdogSec) == 0) {
      pthread_t wt;
      if (pthread_create(&wt, nullptr, watchdogMain, nullptr) == 0) pthread_detach(wt);
    }
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t tid;
  int e = pthread_create(&tid, &attr, supervisorThreadMain, nullptr);
  pthread_attr_destroy(&attr);
  if (e != 0) {
    g_lastError = std::string("pthread_create: ") + strerror(e);
    g_state = EngineState::Error;
    return g_lastError;
  }
  g_supervisorAlive = true;
  g_state = EngineState::Starting;
  startMdnsResponder();
  return "";
}

void EngineHost::stop() {
  std::lock_guard<std::mutex> l(mu);
  if (!g_supervisorAlive) return;
  g_stopRequested = true;
  g_restartPending = false;
  g_state = EngineState::Stopping;
  hamclock_stop_requested = true;
  event("stop requested");
}

std::string EngineHost::restart(const EngineOptions *opts) {
  EngineOptions next;
  {
    std::lock_guard<std::mutex> l(mu);
    if (opts) {
      bool keepLan = g_opts.lanAccess;
      g_opts = *opts;
      if (!opts->lanAccessSet) g_opts.lanAccess = keepLan;
      g_opts.lanAccessSet = true;
    }
    g_lan = g_opts.lanAccess;
    saveLan(g_opts.dataDir, g_lan);
    next = g_opts;
    if (g_supervisorAlive) {
      g_pendingArgs = buildArgs(g_opts);
      g_restartPending = true;
      g_stopRequested = false;
      g_state = EngineState::Stopping;
      hamclock_stop_requested = true;
      event("restart requested");
      return "";
    }
  }
  return start(next);
}

void EngineHost::setLanAccess(bool on) {
  bool alive;
  {
    std::lock_guard<std::mutex> l(mu);
    if (g_opts.lanAccess == on && g_lan == on) return;
    g_opts.lanAccess = on;
    g_lan = on;
    saveLan(g_opts.dataDir, on);
    alive = g_supervisorAlive;
    event(std::string("LAN access ") + (on ? "on" : "off"));
  }
  if (alive) restart(nullptr);
}

std::string EngineHost::statusJson() {
  g_lastStatusMs = nowMs();
  std::vector<std::string> addrs = lanIPv4Addresses();  // outside mu: may do socket calls
  std::string mState, mHost;
  mdnsStatus(mState, mHost);
  std::lock_guard<std::mutex> l(mu);
  std::ostringstream o;
  o << "{\"state\":\"" << stateName(g_state) << "\"";
  o << ",\"runCount\":" << g_runCount;
  o << ",\"exitCode\":";
  if (g_exitCode == INT_MIN) o << "null"; else o << g_exitCode;
  o << ",\"lastError\":"; jsonStr(o, g_lastError);
  o << ",\"lanAccess\":" << (g_lan ? "true" : "false");
  o << ",\"restPort\":" << g_opts.restPort;
  o << ",\"liveRwPort\":" << g_opts.liveRwPort;
  o << ",\"liveRoPort\":" << g_opts.liveRoPort;
  o << ",\"listening\":[";
  bool first = true;
  for (int p : g_listening) { o << (first ? "" : ",") << p; first = false; }
  o << "],\"lanAddresses\":[";
  first = true;
  for (auto &a : addrs) { o << (first ? "" : ","); jsonStr(o, a); first = false; }
  o << "],\"rejectedConnections\":" << g_rejected;
  o << ",\"mdnsState\":"; jsonStr(o, mState);
  o << ",\"mdnsHost\":"; jsonStr(o, mHost);
  o << ",\"dataDir\":"; jsonStr(o, g_opts.dataDir);
  o << ",\"backend\":"; jsonStr(o, g_opts.backend);
  o << ",\"version\":"; jsonStr(o, hc_version ? hc_version : "");
  o << ",\"events\":[";
  first = true;
  for (auto &ev : g_events) { o << (first ? "" : ","); jsonStr(o, ev); first = false; }
  o << "]}";
  return o.str();
}

std::string EngineHost::logTail(size_t maxBytes) {
  std::string path;
  {
    std::lock_guard<std::mutex> l(mu);
    path = g_opts.dataDir + "/diagnostic-log.txt";
  }
  FILE *f = fopen(path.c_str(), "r");
  if (!f) return "";
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long from = size > (long)maxBytes ? size - (long)maxBytes : 0;
  fseek(f, from, SEEK_SET);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

}  // namespace hcengine
