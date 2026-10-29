/**
 * EngineHost implementation. See EngineHost.h for the model.
 */
#include "EngineHost.h"
#include "FlatJson.h"

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

// Run number of the current hamclock_main call (1, 2, …). HamClock code under _IS_VEGA reads it
// to reset state kept in function statics, because a restart re-enters hamclock_main in the same
// process (e.g. tftMsg's boot-message row/column). Set before the engine thread starts.
unsigned vega_engine_run = 0;

namespace hcengine {

void retireEngineSockets();  // SysWraps.cpp

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
int g_setup = -1;                        // -1 unknown, 0 Setup needed, 1 Setup done
bool g_firstRunLanUsed = false;          // firstRunLanAccess applied (once per process)
VegaSettings g_settings;                 // saved settings; source of truth for LAN and backend
std::string g_settingsDir;               // data dir g_settings were loaded from ('' = not loaded)
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

// ---- saved settings (caller holds mu) ----
void ensureSettingsLoaded(const std::string &dataDir) {
  if (g_settingsDir == dataDir) return;
  mkdirs(dataDir);
  std::string note;
  g_settings = loadSettings(dataDir, note);
  g_settingsDir = dataDir;
  if (!note.empty()) event(note);
}

void persistSettings() {
  std::string err = saveSettings(g_settingsDir, g_settings);
  if (!err.empty()) event(err);
}

/** Options given explicitly override and update the saved settings. Returns '' or an error. */
std::string applyOptionsToSettings(const EngineOptions &o) {
  VegaSettings n = g_settings;
  if (o.lanAccessSet) n.lanAccess = o.lanAccess;
  if (!o.backend.empty()) {
    std::string b = o.backend, e = normalizeBackend(b);
    if (!e.empty()) return "backend: " + e;
    n.backend = b;
  }
  if (!o.mdnsName.empty()) {
    std::string m = o.mdnsName, e = normalizeMdnsName(m);
    if (!e.empty()) return "mdnsName: " + e;
    n.mdnsName = m;
  }
  g_settings = n;
  return "";
}

/** g_opts and g_lan follow the settings. */
void syncOptsFromSettings() {
  g_lan = g_opts.lanAccess = g_settings.lanAccess;
  g_opts.lanAccessSet = true;
  g_opts.backend = g_settings.backend;
  g_opts.mdnsName = g_settings.mdnsName;
}

// openSetup is one-shot: it shaped the args just built (-u); clear it so later restarts from
// settings go straight to the clock. HamClock's own restarts drop -u in ESP::restart. Call under mu.
void takeOpenSetup() {
  if (!g_opts.openSetup) return;
  g_opts.openSetup = false;
  event("next run opens Setup");
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
  std::string err = parseFlatJson(json, [&](const std::string &key, const JsonValue &v) {
    switch (v.type) {
      case JsonValue::String:
        if (key == "dataDir") o.dataDir = v.str;
        else if (key == "backend") o.backend = v.str;
        else if (key == "mdnsName") o.mdnsName = v.str;
        else if (key == "testSeedCallsign") o.testSeedCallsign = v.str;
        break;
      case JsonValue::StringArray:
        if (key == "extraArgs") o.extraArgs = v.arr;
        break;
      case JsonValue::Bool:
        if (key == "lanAccess") { o.lanAccess = v.b; o.lanAccessSet = true; }
        else if (key == "firstRunLanAccess") o.firstRunLanAccess = v.b;
        else if (key == "openSetup") o.openSetup = v.b;
        break;
      case JsonValue::Number:
        if (key == "restPort") o.restPort = (int)v.num;
        else if (key == "liveRwPort") o.liveRwPort = (int)v.num;
        else if (key == "liveRoPort") o.liveRoPort = (int)v.num;
        else if (key == "watchdogSec") o.watchdogSec = (int)v.num;
        break;
      case JsonValue::Null:
        break;
    }
  });
  return err.empty() ? "" : "options: " + err;
}

std::vector<std::string> buildArgs(const EngineOptions &o) {
  std::vector<std::string> a = {"hamclock", "-d", o.dataDir};
  // Always pass -b: without a backend the REST server on 8080 never comes up.
  a.push_back("-b");
  a.push_back(o.backend.empty() ? std::string(kDefaultBackend) : o.backend);
  a.push_back("-e"); a.push_back(std::to_string(o.restPort));
  a.push_back("-w"); a.push_back(std::to_string(o.liveRwPort));
  if (o.liveRoPort != 0) { a.push_back("-r"); a.push_back(std::to_string(o.liveRoPort)); }
  // -k: no Setup offer or Skip waits (HamClock still opens Setup if no callsign is stored).
  // -u: open Setup at once (one run only; ESP::restart drops it).
  a.push_back("-k");
  if (o.openSetup) a.push_back("-u");
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
  // HamClock starts REST only after Setup, so REST listening means Setup is done.
  if (port == g_opts.restPort && g_setup != 1) {
    if (g_setup == 0) event("setup complete");
    g_setup = 1;
  }
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

bool EngineHost::mdnsWanted(int &port, std::string &name) const {
  std::lock_guard<std::mutex> l(mu);
  port = g_opts.liveRwPort;
  name = g_settings.mdnsName;
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
    std::string seeded = testSeedCallsign(o.dataDir, o.testSeedCallsign);
    bool stored = hasStoredCallsign(o.dataDir);
    {
      std::lock_guard<std::mutex> l(mu);
      if (!seeded.empty()) event(seeded);
      g_setup = stored ? 1 : 0;
      if (!stored) {
        event("setup needed: no callsign stored");
        // Before hamclock_main binds anything, so no restart is needed for LAN access.
        if (o.firstRunLanAccess && !g_firstRunLanUsed && !g_lan) {
          g_lan = g_opts.lanAccess = g_settings.lanAccess = true;
          persistSettings();
          event("LAN access on for first-run setup");
        }
      }
      if (o.firstRunLanAccess) g_firstRunLanUsed = true;
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
      vega_engine_run = (unsigned)g_runCount;
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
  ensureSettingsLoaded(opts.dataDir);
  std::string err = applyOptionsToSettings(opts);
  if (!err.empty()) return err;
  persistSettings();
  syncOptsFromSettings();
  g_runArgs = buildArgs(g_opts);
  takeOpenSetup();
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
      VegaSettings prevSettings = g_settings;
      ensureSettingsLoaded(opts->dataDir);
      std::string err = applyOptionsToSettings(*opts);
      if (!err.empty()) { g_settings = prevSettings; return err; }
      g_opts = *opts;
    }
    persistSettings();
    syncOptsFromSettings();
    next = g_opts;
    if (g_supervisorAlive) {
      g_pendingArgs = buildArgs(g_opts);
      takeOpenSetup();
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
  updateSettings(std::string("{\"lanAccess\":") + (on ? "true" : "false") + "}");
}

std::string EngineHost::settings() {
  std::lock_guard<std::mutex> l(mu);
  ensureSettingsLoaded(g_opts.dataDir);
  return settingsJson(g_settings);
}

std::string EngineHost::updateSettings(const std::string &json) {
  bool restartNeeded = false;
  {
    std::lock_guard<std::mutex> l(mu);
    ensureSettingsLoaded(g_opts.dataDir);
    VegaSettings n = g_settings;
    std::string err = mergeSettingsJson(json, n, true);
    if (!err.empty()) return err;
    bool lanChanged = n.lanAccess != g_settings.lanAccess;
    bool backendChanged = n.backend != g_settings.backend;
    bool nameChanged = n.mdnsName != g_settings.mdnsName;
    bool overscanChanged = n.overscan != g_settings.overscan;
    if (!lanChanged && !backendChanged && !nameChanged && !overscanChanged) return "";
    err = saveSettings(g_settingsDir, n);
    if (!err.empty()) { event(err); return err; }
    g_settings = n;
    if (lanChanged) event(std::string("LAN access ") + (n.lanAccess ? "on" : "off"));
    if (backendChanged) event("backend set to " + n.backend);
    if (nameChanged) event("mDNS name set to " + n.mdnsName);
    if (overscanChanged) event("screen margin set to " + std::to_string(n.overscan) + "%");
    syncOptsFromSettings();
    restartNeeded = (lanChanged || backendChanged) && g_supervisorAlive;
  }
  if (restartNeeded) return restart(nullptr);
  return "";
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
  o << ",\"setupComplete\":" << (g_setup < 0 ? "null" : g_setup ? "true" : "false");
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
  o << ",\"mdnsName\":"; jsonStr(o, g_settings.mdnsName);
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
