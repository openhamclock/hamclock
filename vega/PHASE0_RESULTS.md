# Phase 0 Results — HamClock engine embedded in a Vega app

Device: Fire TV Stick "calypso", Vega OS 2.0, armv7l, `192.168.1.237`.
Spike: `../tmp/vega-spike/` (throwaway; package `com.openhamclock.hcspike`).

## Feasibility checks

| # | Check | Result | Evidence |
|:--|:--|:--|:--|
| 1 | Cross-compile engine with Vega toolchain | ✅ PASS | x86_64 + armv7, no source changes, static libc++ |
| 2 | WebView loads `live.html` over loopback | ✅ PASS | `WEBVIEW onLoad OK` from `http://127.0.0.1:8081` |
| 3 | LAN reachability from app sandbox | ✅ PASS | laptop → `:8081/live.html` = HTTP 200 |
| 4 | Persistent storage | ✅ PASS | app-private `/data`; survives terminate + app update |
| 5 | Lifecycle (Home / relaunch) | ✅ PASS* | Home: engine keeps serving. Relaunch: new process, engine rebinds OK |
| — | Engine `exit()` must not kill app | ✅ fixed | `--wrap=exit` → `pthread_exit`; no more SIGABRT |
| — | D-Pad into `live.html` | ❌ FAIL | D-Pad jumps between form fields; OK hits Done. Needs a virtual cursor or key mapping (Phase 2) |
| — | Setup via LAN browser | ✅ PASS | Callsign set from laptop on :8081; engine left Setup. 8080 should now listen (not yet verified) |

\* Relaunch stops the old process and starts a new one, so there's a short window
where the old sockets may still be held. Phase 1 should set `SO_REUSEADDR` and
retry the bind.

## Sandbox facts learned

- The app runs as uid 5000 in its own PID namespace (`pid=2`), with cwd `/` and no `HOME`.
- Writable locations: `/data` (private and persistent) and `/tmp`. `/` is read-only.
- Plain `http` needs `[network-traffic-policy.cleartext] allowed-domains = ["127.0.0.1","localhost"]`.
- The WebView needs `com.amazon.webview.renderer_service` in `[wants]`.
- A local `file:` native module needs Metro `watchFolders`.
- The device has no libc++, so the C++ runtime must be static (the toolchain already does this).

## Android settings → Vega mapping

| Android item | Vega plan |
|:--|:--|
| First-run guide (Setup / Exit) | ✅ Same, in the RN shell |
| Allow local network access | ✅ Bind `127.0.0.1` vs `0.0.0.0` (WiFiServer.cpp, ws.cpp) |
| mDNS name (`hamclock.local`) | ⚠️ Unverified; Vega may not let apps publish mDNS. Fallback: show IP URL |
| Local URL + endpoint tabs (8081 / 8080) | ✅ |
| Copy URL | ⚠️ No clipboard use on TV; replace with QR + big URL text |
| QR code | ✅ (JS QR lib) |
| Ports info / REST conflict warnings + fallback ladder | ✅ Same logic in shim |
| Backend host:port | ✅ `-b` |
| Start on device boot | ❌ Not available to Vega apps (engine runs only while app open — agreed) |
| Keep running in background | ⚠️ Partially happens anyway (Home keeps it alive until the system reclaims it); no guarantee. Hide or label "best effort" |
| Check for updates automatically | ❌ Updates come from the Amazon Appstore |
| Screen safe margin 0/3/5% | ✅ Padding around the WebView |
| Save / Save & Restart / Cancel | ✅ Restart = stop and start the engine thread |
| Error screen + Clean Restart | ✅ |
| "Press Back again to exit" | ✅ |
| Update-available dialogs | ❌ N/A (Appstore) |

## Proposed Phase 1 (production native module in `vega/`)

1. Add a `vega/hcengine` Turbo Module. Its CMake builds the engine sources directly (no prebuilt `.a`).
2. Exit/restart shim:
   - `--wrap=exit` becomes a thread exit with status;
   - replace the `execvp` restart with a thread restart;
   - stub out the `fork`/`exec` paths under `_VEGA`;
   - use `RUSAGE_THREAD`.
3. Add a bind-address setting (LAN toggle), plus `SO_REUSEADDR` and a bind retry.
4. JS API: `start(opts)`, `stop()`, `getStatus()`, `setLanAccess(bool)`.
5. Manifest: cleartext policy and WebView renderer service; data dir `/data/hamclock`.

## Open items for next session

- Review the "❌ / ⚠️" settings items above again with the user.
- D-Pad: design a remote-control cursor/key mapping for `live.html`.
- Verify REST 8080 listens after setup. Likely cause it didn't: the spike ran
  without `-b`, so the engine used the dead default backend
  `clearskyinstitute.com:80` and hung on a long timeout. Always pass
  `-b ohb.hamclock.app:80` (the default, but user-configurable in settings).
