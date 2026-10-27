/**
 * EngineHost: runs HamClock's hamclock_main() on a worker thread inside the Vega app process.
 *
 * HamClock was written to own a whole process: it calls exit(), restarts itself with execvp(),
 * and binds its servers to INADDR_ANY. Inside an app, those would kill or replace the app, or
 * expose the servers to the LAN. EngineHost contains them:
 *   - exit()            -> ends the engine thread (linker --wrap, see SysWraps.cpp)
 *   - ESP.restart()     -> engine thread ends, supervisor starts it again with the new args
 *   - bind()            -> loopback unless LAN access is on; retries while the port is busy
 *   - fork()            -> fails with ENOSYS (unsafe in a multi-threaded app)
 *   - sockets           -> tracked, and retired on stop so old server threads end cleanly
 *
 * Thread model: one supervisor thread per start(); it creates an engine thread per run and joins it.
 */
#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace hcengine {

/**
 * Thrown to end an engine thread (exit(), restart, stop, retired listener). Caught at the top of
 * the engine thread and of every thread the engine creates (pthread_create is wrapped).
 * N.B. pthread_exit() can't be used: its forced unwind runs in the system libgcc_s, which crashes
 * calling the C++ personality routine from the libc++abi/libunwind statically linked into this .so.
 */
struct EngineThreadExit {
  const char *why = "";  // static string; reported in the status events
};

struct EngineOptions {
  std::string dataDir = "/data/hamclock";
  std::string backend = "ohb.hamclock.app:80";  // -b host:port
  int restPort = 8080;                           // -e; -1 disables
  int liveRwPort = 8081;                         // -w; serves live.html
  int liveRoPort = 0;                            // -r; 0 = engine default, -1 disables
  bool lanAccess = false;                        // bind 0.0.0.0 instead of 127.0.0.1
  std::vector<std::string> extraArgs;            // appended verbatim
  int watchdogSec = 0;                           // diagnostics only: abort() if JS stops polling
  // First-run seed, written only if no callsign is stored yet. Without a callsign HamClock forces
  // Setup (even with -k) and the RESTful server doesn't start until Setup is done.
  std::string seedCallsign;                      // '' = don't seed
  double seedLat = NAN, seedLng = NAN;           // DE location; NaN = don't seed
};

enum class EngineState { Idle, Starting, Running, Stopping, Stopped, Exited, Error };

const char *stateName(EngineState s);

/** Parse EngineOptions JSON over `base`. Unknown keys are ignored. Returns '' or an error. */
std::string parseOptions(const std::string &json, EngineOptions &base);

/** Build argv (including argv[0]) for hamclock_main from options. */
std::vector<std::string> buildArgs(const EngineOptions &o);

class EngineHost {
public:
  static EngineHost &instance();

  /** Start with options. Returns '' or an error (e.g. already running). */
  std::string start(const EngineOptions &opts);
  /** Request stop; returns at once. */
  void stop();
  /** Restart with new options (or current ones if null). Starts if not running. */
  std::string restart(const EngineOptions *opts);
  /** Change bind policy; restarts if running. */
  void setLanAccess(bool on);

  std::string statusJson();
  std::string logTail(size_t maxBytes);

  // ---- called from SysWraps / engine hooks; not for JS ----
  bool lanAccess() const;
  bool isEngineThread() const;
  void noteListening(int port);
  /** Called on the engine thread by exit()/ESP.restart()/stop hook. Ends the thread. */
  [[noreturn]] void endEngineThread(int exitCode, std::vector<std::string> *restartArgs);

private:
  EngineHost() = default;
  void supervisorMain();
  static void *engineThreadMain(void *);
  static void *supervisorThreadMain(void *);
};

}  // namespace hcengine
