# Phase 1 Results: engine as an in-app thread

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. Module: `vega/hcengine`
(`@openhamclock/hcengine`). This file takes precedence over PHASE0_RESULTS.md where they conflict.

## Verification

On-device self-test (`src/engine/selfTest.ts`, enable `ENGINE_SELFTEST` in `index.js`):
**13/13 passed** in one app session, with no app crash.

| Step | Result |
|:--|:--|
| Start into Setup, stop from Setup | ✅ ~250 ms |
| 3× start with `-k`, then `GET 127.0.0.1:8080/get_sys.txt`, then stop | ✅ REST 200 in ~4 s; stop in 1–2.7 s; REST port closed after stop |
| Engine restart via REST `/restart` (engine's own `ESP.restart`) | ✅ |
| Restart from JS | ✅ |
| LAN on (binds `0.0.0.0`), then LAN off (binds `127.0.0.1`) | ✅ |
| Final stop | ✅ |

## Design (as built)

- **Threads:** `start()` creates a supervisor thread. Each engine run is a thread with an 8 MB stack that runs `hamclock_main`.
  - The supervisor joins it, retires the run's sockets, and loops if a restart is pending.
- **Process-level calls** are contained with linker `--wrap` (`kepler/engine/SysWraps.cpp`):
  - `exit` ends the thread;
  - `fork` returns ENOSYS;
  - `bind` maps INADDR_ANY to loopback unless LAN access is on, and retries EADDRINUSE;
  - `accept`, `close` and `pthread_create` are wrapped for socket tracking and thread ends;
  - `getrusage(SELF)` becomes `RUSAGE_THREAD` on the engine thread.
- **Engine embed hooks** (`ArduinoLib/Arduino.h`, generic; the desktop build is unchanged):
  - `hamclock_stop_requested`: the main loops break, and `delay()` calls `hamclock_stop_hook`.
  - `hamclock_restart_hook`: `ESP.restart()` hands its argv to the host instead of calling `execvp`.
- **Re-run safety in the engine:** the web `Adafruit_RA8875::begin()` is idempotent, so re-runs reuse one canvas and one fbThread.
- **First-run seed:** options `seedCallsign`, `seedLat` and `seedLng` (`NvSeed.cpp`) use the engine's NV API, and only when no callsign is stored.
- **JS API:** `start`, `stop`, `restart`, `setLanAccess`, `getStatus` (JSON with state, ports, events) and `getLogTail`.
- **Always pass `-b`** (default `ohb.hamclock.app:80`).

## Findings (why it is built this way)

1. **No child processes.** In the app sandbox (seccomp, NoNewPrivs), any SDK-built executable segfaults during startup in `__libc_start_main`. Amazon's `/bin/true` runs. Hence the in-app thread design.
2. **`pthread_exit` crashes.** Its forced unwind runs in the system libgcc_s, which calls the C++ personality routine from the libc++abi/libunwind statically linked into our .so (SIGSEGV in `_Unwind_GetGR`).
   - Threads end by throwing `EngineThreadExit` instead, caught at every thread top.
   - The engine has no `catch` blocks. *(This supersedes the Phase 0 `--wrap=exit → pthread_exit` note.)*
3. **Socket fds must be tracked by identity.** ws.cpp closes sockets with `fdopen`+`fclose`, bypassing the `close` wrap, so fd numbers get reused.
   - Tracking is by `{dev, ino}`, and a socket is shut down only if its identity still matches.
   - Retired listeners are shut down but never closed, so their numbers can't be reused while old threads still use them.
4. **Stale server objects survive a re-run.** `webserver.cpp`'s static `restful_server` from the previous run is polled during startup.
   - On the engine thread, `accept()` on a retired listener therefore fails (EINVAL) instead of ending the thread.
   - Helper threads blocked in accept loops still end.
5. **A fresh data dir forces Setup, even with `-k`.** REST 8080 starts only after Setup. `-b` is required but not sufficient.
6. **Screensaver backgrounds the app, which pauses JS timers.** The engine keeps running.
   - `inputd-cli button_press KEY_UP` resets the idle timer for tests; `mouse_move` does not.
   - A product fix is a later phase (see the earlier media-playback keep-awake work: commits `c0e1797`, `e4d54c1`).
7. **Diagnostics.**
   - Native stderr doesn't reach the device log; use `getStatus().events`.
   - HamClock's `makeDiagFile` dup2s fds 1/2 for the whole app process, so RN's stdout also lands in `diagnostic-log.txt`. Follow-up.
   - Crash reports: `vega device copy-logs -a SYSTEM_TOMBSTONE/acr`.
     - The ACR has a full core in `coredump_lz4_base64`.
     - Symbolize pc − `base_of_image` against `build/armv7-debug/lib/libHcEngine.so.debug`.

## Open items

- `curl` on the device (used by the engine's popen paths) is unverified.
- LAN reachability from a laptop with `lanAccess` on is not yet re-checked (the bind to `0.0.0.0` is confirmed).
- `getLogTail` is polluted by the app's stdout (finding 7).
