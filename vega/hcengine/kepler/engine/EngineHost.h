/**
 * EngineHost: runs HamClock's hamclock_main() on a worker thread inside the Vega app process.
 *
 * HamClock was written to own a whole process: it calls exit(), restarts itself with execvp(),
 * and binds its servers to INADDR_ANY. Inside an app, those would kill or replace the app, or
 * expose the servers to the LAN. EngineHost contains them:
 *   - exit()            -> ends the engine thread (linker --wrap, see SysWraps.cpp)
 *   - ESP.restart()     -> engine thread ends, supervisor starts it again with the new args
 *   - bind()            -> loopback unless LAN access is on; retries while the port is busy
 *   - accept()          -> peers outside the allowed ranges are dropped (LanFilter.cpp)
 *   - fork()            -> fails with ENOSYS (unsafe in a multi-threaded app)
 *   - sockets           -> tracked, and retired on stop so old server threads end cleanly
 *
 * The LAN access choice is saved in <dataDir>/vega-lan-access and used when start() is called
 * without an explicit lanAccess.
 *
 * Setup state (status "setupComplete"): before each run, on the engine thread and before
 * hamclock_main, the host reads the stored callsign through HamClock's NV API (NvSeed.cpp).
 * No callsign -> false (HamClock forces Setup, even with -k). It turns true when the REST port
 * starts listening, because HamClock starts REST only after Setup is done. null until the
 * first check. Nothing reads NV while the engine runs.
 *
 * Thread model: one supervisor thread per start(); it creates an engine thread per run and joins it.
 */
#pragma once

#include <string>
#include <sys/socket.h>
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
  bool lanAccessSet = false;                     // lanAccess given; else the saved choice is used
  std::vector<std::string> extraArgs;            // appended verbatim
  int watchdogSec = 0;                           // diagnostics only: abort() if JS stops polling
  // Turn LAN access on (and save it) if Setup is needed, before the engine binds. Applied at most
  // once per app process, so a user who turns LAN off again isn't overridden by a restart.
  bool firstRunLanAccess = false;
  // Tests only (selfTest.ts): store this callsign if none is stored, so REST comes up without
  // Setup. The app never seeds; the user enters their own callsign in HamClock's Setup.
  std::string testSeedCallsign;                  // '' = don't seed
};

enum class EngineState { Idle, Starting, Running, Stopping, Stopped, Exited, Error };

const char *stateName(EngineState s);

/** Parse EngineOptions JSON over `base`. Unknown keys are ignored. Returns '' or an error. */
std::string parseOptions(const std::string &json, EngineOptions &base);

/** Build argv (including argv[0]) for hamclock_main from options. */
std::vector<std::string> buildArgs(const EngineOptions &o);

// ---- LanFilter.cpp ----
/** Loopback always; with lan also RFC 1918, 169.254/16, fe80::/10, fc00::/7 (ARCHITECTURE §3.3). */
bool peerAllowed(const sockaddr *sa, bool lan);
std::string peerString(const sockaddr *sa);
/** The TV's IPv4 addresses that LAN peers can use (private ranges, interface up). */
std::vector<std::string> lanIPv4Addresses();

// ---- NvSeed.cpp (engine thread only, before hamclock_main) ----
/** True if a non-empty callsign is stored in dataDir's eeprom file. */
bool hasStoredCallsign(const std::string &dataDir);
/** Tests only: store call if no callsign is stored. Returns what was written, or ''. */
std::string testSeedCallsign(const std::string &dataDir, const std::string &call);

// ---- QrMatrix.cpp ----
/** QR code for text: rows of '0'/'1' joined by '\n', no quiet zone ('' if it doesn't fit). */
std::string qrMatrix(const std::string &text);

// ---- MdnsResponder.cpp ----
/** Start the responder thread once; it advertises while mdnsWanted() is true. */
void startMdnsResponder();
/** state: off | probing | announced | error: ...; host: e.g. hamclock-2.local ('' if none). */
void mdnsStatus(std::string &state, std::string &host);

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
  void noteRejected(const std::string &peer);
  void noteEvent(const std::string &msg);
  /** True while LAN access is on and the engine supervisor is alive; port = live web port. */
  bool mdnsWanted(int &port) const;
  /** Called on the engine thread by exit()/ESP.restart()/stop hook. Ends the thread. */
  [[noreturn]] void endEngineThread(int exitCode, std::vector<std::string> *restartArgs);

private:
  EngineHost() = default;
  void supervisorMain();
  static void *engineThreadMain(void *);
  static void *supervisorThreadMain(void *);
};

}  // namespace hcengine
