# Phase 6 Results: Remote, screensaver, lifecycle

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. Package `org.openhamclock.hamclock`. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Verification (on device)

Checked by eye on the TV, plus the device log (`[HCEV]`, `[HCAPP]`, `[HCWEB]`, lcm lines). Runner: `tmp/vega-spike/run-p6.sh` (same options as run-p5.sh; also greps `HCEV`, `HCAPP`).

| Check | Result |
|:--|:--|
| FRESH install: Setup shows DE from geoIP (not HamClock's 40N 99W default) and NTP = Computer | ✅ (user) |
| DE edited in Setup, app relaunched: no new geoIP lookup, the edit stays | ✅ (user) |
| Backend Done, then name Done: no restart; Back: one restart with the new `-b` | ✅ `run 2: … -b ohb.hamclock.app:8012` right after `leaving Settings`; nothing at the Done presses |
| Backend cleared, Back: one restart back to the default | ✅ `run 3: … -b ohb.hamclock.app:80` |
| `diagnostic-log.txt` holds only engine output | ✅ HamClock lines plus `HcEngine:` lines; before, the tail was ~145 lines of the Vega runtime's `Key not found` |
| Screensaver stays away while HamClock is on screen | ✅ no `com.amazon.idle` idle event for 7+ min (screensaver timeout 5 min); without the tone it started at exactly 5:00 |
| Keep-awake tone inaudible | ✅ (user, volume full: only the TV's own 60 Hz hum) |
| Home → back after 10 s: live clock | ✅ |
| Home → 5+ min (TV screensaver comes on, tone paused) → back: live clock | ✅ screensaver started 5:00 after Home; clock live on return, no page reload needed |
| Holding an arrow speeds the cursor up; it stops on release; single presses and quick double taps are small steps | ✅ (user: "feels great") |

## Design (as built)

### 6.1 D-pad speed-up on hold

- Measured: Vega repeats a held key after ~520 ms, then every ~200 ms (5/s). `e.repeat` is never set, and the repeats have no keyup; one keyup arrives at release.
- live.html speeds up on presses < 200 ms apart (counter thresholds 8/18/30, tuned for Android's ~50 ms repeat), so at 200 ms it rarely triggered.
- `pageScript` (the injected key router): an arrow keydown moves once. A second keydown of the same arrow without a keyup in between (within 700 ms), i.e. Vega's first repeat, starts our own repeat: `handleVirtualCursorMove` every 50 ms, so live.html's speed-up works as on Android. Stops on keyup, on any other key, or when Vega's repeats stop for 350 ms (fallback if a keyup is lost). Later Vega repeats only keep it alive.

### 6.2 Screensaver

- `TimeoutManager` (`useSetTimeoutCallback` / `useSetLifespanCallback`, lifespan PERMANENT, timeout 0) exists in the current SDK but does nothing for the screensaver: `lcm-idle-handler` still launched `com.amazon.screensaver.main` at 5:00. It only governs how the lifecycle manager treats an inactive app in the backstack, so it was removed.
- Vega holds the screensaver off only while media plays. As in the earlier branch (`RemoteNav.ts`), the page runs a WebAudio oscillator: 30 Hz sine at gain 0.0001 (inaudible), started 1.5 s after load and checked every 30 s (resumed if suspended).
- WebView `mediaPlaybackRequiresUserAction={false}`; `manifest.toml` wants `com.amazon.audio.stream`, `com.amazon.audio.control`, `com.amazon.mediatransform.service` (the WebView asked for the last two before and was refused: "missing wants declaration").
- State is posted as `[HCWEB] audio:<running|suspended|…>`.

### 6.3 Home and background

- The tone plays only while HamClock is on screen; otherwise it would keep the screensaver away in other apps. Paused on the page's `visibilitychange` to hidden, and by RN on `AppState` change (`window.__hcAudio(on)`); resumed on return. Both fire on Home (log: `[HCAPP] state background`, `[HCWEB] visibility:hidden`, `audio:suspended`).
- No reload on return was needed, after 15 s or 5+ min in the background (with the TV screensaver up meanwhile). live.html also reloads itself if its websocket closes (`reloadThisPage`). Status polling (a `setTimeout` chain) resumes with JS.
- The Vega WebView has no renderer-gone callback and no `reload()` (only `injectJavaScript`), so Phase 5 finding 2 (remount on engine restart) stays as is.

### 6.4 Diagnostics: engine log separate from the app's stdout

- `kepler/engine/EngineLog.cpp`: linker wraps on `libHcEngine.so` only (`dup2`, `printf`, `vprintf`, `fprintf`, `vfprintf`, `fputs`, `puts`, `putchar`, `fwrite`, `fflush`, `write`). `nm` showed these are the stdio calls the library imports.
  - `dup2(fd, 1|2)` (HamClock's `makeDiagFile`) keeps a copy of fd as the engine log and leaves the process's fds 1 and 2 alone.
  - Calls in the library aimed at stdout/stderr go to that file (line-buffered); other FILEs pass through. Routing is by call site, so HamClock's helper threads and the host's own `HcEngine: …` lines are included, and the rest of the process isn't.
- No upstream change. `getLogTail` reads `diagnostic-log.txt`, now clean. HamClock's `postDiags` uploads the same files.
- Native stderr still doesn't reach the device log, so the app mirrors new `getStatus().events` as `[HCEV] …` (each once; the events list is a ring of 30).

### 6.5 curl in the sandbox: not possible; disabled

- Probes from the engine: `popen` of anything (even `echo`) exits 127. `/bin/sh` and `/usr/bin/curl` exist and are executable, but `posix_spawn` of either fails at load: `symbol lookup error: undefined symbol: UP` (sh), `curl_easy_perform` (curl), with or without `LD_LIBRARY_PATH` (`/mnt:/usr/lib/keplerscript-4`). The sandbox's view of the system libraries doesn't fit those binaries. HamClock's own `can not run date??` / `df??` in the diag log is the same.
- Under `_IS_VEGA` (upstream, minimal):
  - `WiFiClient::connectCommand` returns false at once (`ArduinoLib/WiFiClient.cpp`).
  - `plotChoiceIsAvailable` hides `PLOT_CH_SATACT` (hams.at) and `PLOT_CH_MESHTASTIC`, the panes that need it (`plotmgmnt.cpp`).
- The HTTPS version check (`connecthttpsHCGET`, only with a backend other than the software host) now just finds no new version. Updates come from the Appstore (`NO_UPGRADE`) anyway.
- To bring those panes back, the engine would need an in-process HTTPS client (libcurl/TLS in the app), like Android's JNI fetch pipe. Not scheduled.

### 6.6 Backend: Done saves; restart on leaving Settings

- Engine: `updateSettings` restarts at once only for a LAN change. A backend change is saved (`backend set to … (applies at the next restart)`); any restart picks it up.
- Status: `backend` is now the backend of the current run (the run's `-b`, `g_runBackend`); new `restartNeeded` = saved backend ≠ running one.
- App: when the Settings page is left (Back button or remote Back), a fresh `getStatus()`; if `restartNeeded`, one `restart(DEFAULT_ENGINE_OPTIONS)` (no new TurboModule method). The panel closes and the clock reloads. Field help: "HamClock restarts when you leave this page."

### 6.7 First-run presets

- `kepler/engine/FirstRunPresets.cpp` (built in the `hamclock_engine` object library), called on the engine thread before `hamclock_main` only when no callsign is stored. Each preset is written only if its key doesn't exist yet:
  - `NV_NTPSET` = 2 (Computer; setup.cpp's `NTPSC_*` list is file-local, so the value is documented there).
  - DE: `GET /ham/HamClock/fetchIPGeoloc.pl` from the configured backend (the same request as `wifi.cpp` `geolocateIP`; `LAT=`/`LNG=`/`IP=`/`CREDIT=`), 5 s connect/read timeouts, UA `HamClock-vega/<version>`. Writes `NV_DE_LAT`, `NV_DE_LNG` and `NV_DE_GRID` (setup.cpp resets all three unless all three exist). TZ is left to HamClock (auto when missing).
- Why not `-g`: the plan's note was wrong. The geoIP flag is `-g` (`-i <ip>` geolocates a given IP), and `-g requires -k` (a usage check; `-k` doesn't disable it). But `-g` geolocates in `initSys()`, after `clockSetup()`, so Setup wouldn't show it and it would overwrite what the user entered. Setup's "IP geolocate" option isn't saved at all (reset at start).
- HamClock writes its own defaults on its first start, so the presets apply once per install and never overwrite a later edit (TVs rarely move). If geoIP fails then, HamClock's default stays. Events: `first-run preset: NTP = Computer`, `first-run preset: DE <lat> <lng> from geoIP (<credit>)` or `… geoIP failed (…)`.

## Findings

1. **Vega key repeat is slow** (~520 ms, then ~200 ms) and unmarked (`e.repeat` false, no keyups between repeats). See 6.1.
2. **The screensaver ignores `TimeoutManager`**; only media playback holds it off. See 6.2.
3. **No subprocesses in the app sandbox** (sh/curl fail at symbol lookup). Anything in HamClock that shells out (`date`, `df`, curl panes, `cputemp`) does nothing on Vega. See 6.5.
4. **The Vega runtime floods stdout** (`Key not found`), which HamClock's process-wide dup2 used to capture. See 6.4.
5. At launch the app reports `AppState` `background` then `active` within ~0.3 s; harmless (the tone starts later anyway).
6. Device log stream: one FRESH run had no JS lines at all (system lines only); later runs were fine.

## Open items

- Phase 7: trim debug logging: `[HCWEB]` keys/keyups/visibility/audio, `[HCEV]`, `[HCAPP]`, plus the earlier `[HCLAN]`, `[HCSET]`, `[HCFIELD]`.
- Not checked: SATACT/Meshtastic actually missing from the pane menu (needs Setup done; logic is a plain `return false`).
- Optional later: in-process HTTPS for the curl panes (6.5); audio focus with other apps' playback (not seen: the tone is paused whenever HamClock isn't in front).
