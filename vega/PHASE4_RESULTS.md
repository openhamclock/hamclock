# Phase 4 Results: First-run setup

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. Package `org.openhamclock.hamclock`. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Verification (on device)

"Fresh" = `vega device uninstall-app` first, which clears the app's `/data` (runner: `tmp/vega-spike/run-p4.sh` with `FRESH=1`).

| Check | Result |
|:--|:--|
| Fresh install: first-run screen appears (title, 3 steps, URLs, QR, "Waiting for setup…" with pulsing dot) | ✅ (by eye) |
| Fresh install: LAN access is on without a restart (`live.html` answers on `192.168.1.237:8081` from the laptop; REST `:8080` down) | ✅ |
| mDNS announces `hamclock-2.local`; the QR switches to it (to be changed: QR always uses the IP; see Open items) | ✅ (`avahi-browse`, `[HCLAN]`) |
| Phone scans the QR and opens live.html | ✅ |
| Focus starts on "Set up on this TV instead"; Left/Right moves between buttons | ✅ |
| Back on the first-run screen opens the settings panel; Back again returns to the first-run screen | ✅ |
| Setup finished on the phone → TV switches to the clock with no key press; D-pad works after | ✅ (REST up ~60 s after the screen appeared; `[HCSETUP] setupComplete=true`) |
| Relaunch after Setup: straight to the clock, no first-run screen; LAN still on | ✅ |
| "Set up on this TV instead" → Setup with the D-pad → clock | ✅ (fresh install; `[HCSETUP]` false → onTv=true → setupComplete=true; 484 page keys; REST up with the entered callsign) |
| Quit mid-setup → first-run screen again at next launch | ✅ (relaunched twice before Setup; screen came back each time) |

## Design (as built)

### 4.1 Seeding removed

- `EngineOptions` no longer has `seedCallsign`/`seedLat`/`seedLng`; no app path seeds.
- `NvSeed.cpp` is now `hasStoredCallsign(dataDir)` (read-only, HamClock's NV API, engine thread, before `hamclock_main`).
- Test-only: `testSeedCallsign` (callsign only, no location), used only by `selfTest.ts`. It writes only if no callsign is stored.

### 4.2 Setup state

- `getStatus().setupComplete`: `null` until the first check, `false` if no callsign was stored when the run started, `true` once the REST port listens (HamClock starts REST only after Setup).
- Set per run in `engineThreadMain` (no NV reads while the engine runs). Events: `setup needed: no callsign stored`, `setup complete`.

### 4.3 QR code

- `HcEngine.qrMatrix(text)` (native, `kepler/engine/QrMatrix.cpp`) uses HamClock's bundled `qrcodegen` (ECC Medium, boosted). Returns rows of `0`/`1`; the TS facade turns them into `boolean[][]`.
- `src/QrCode.tsx` draws it with Views: one View per row, runs of the same color merged, module size snapped to whole device pixels, 4-module quiet zone. It sits on a white rounded card about 560 dp across.
- `generated/HcEngineSpec.*` and `HcEngine.h` were regenerated with `npm run kepler-codegen`. The codegen overwrites `HcEngine.cpp` with a template, so restore it from git afterwards.

### 4.4 First-run screen (`App.tsx` `FirstRunScreen`)

- Shown when `setupComplete === false`, the user hasn't chosen "Set up on this TV instead" (this launch), and the engine isn't stopped/errored. It covers the WebView, which keeps loading underneath, so choosing the TV path is instant.
- Left: "Welcome to HamClock on TV", a one-line lead, 3 numbered steps (wording after the Android app), the URLs (`.local` once announced, then IP), a status line with a pulsing dot. Buttons: "Set up on this TV instead" (preferred focus) and "Exit app". If LAN access is off, it says so and adds "Turn on LAN access".
- Right: the QR for the `.local` URL once announced, else the IP URL. (Superseded by decision: always the IP URL; see Open items.)
- Keys: while the screen (or the settings panel) is up, the WebView gets `allowSystemKeyEvents=false` and no preferred focus, so Back reaches RN's `BackHandler` and opens the settings panel. The buttons sit in a `TVFocusGuideView` with focus trapped, so focus can't move into the WebView.
- It closes by itself when `setupComplete` turns true; focus goes back to the WebView.
- Diagnostics: `[HCSETUP] setupComplete=… firstRun=… onTv=…` on change.

### 4.5 LAN access on first run (deviation from plan, same effect)

- The plan had RN turn LAN on when the screen first appears. That would restart the engine and reload the page. Instead, `DEFAULT_ENGINE_OPTIONS.firstRunLanAccess = true`: the engine thread turns LAN on (and saves it) right after the callsign check, **before `hamclock_main` binds**, so no restart is needed. Event: `LAN access on for first-run setup`.
- It is applied at most once per app process, so if the user turns LAN off in the panel mid-setup, the restart doesn't turn it back on. The next launch with Setup still needed turns it on again, matching "quitting mid-setup shows the screen again".
- After Setup, LAN stays on (decision 1); the user can turn it off in the settings panel, and that choice is saved.

## Findings

1. **A WebView renderer death kills the whole app.** Chromium logs `RendererService died, killing browser process` and the app aborts (signal 5). On this run the cause was memory: a `screenshooter` process I had started (it can't capture as `app_user`, "creating a buffer file … Permission denied", but stayed alive holding 122 MB) filled swap (1023/1023 MB), and the low-memory manager killed the renderer service ~2 s after each WebView mount. After `pkill screenshooter`, swap went to 116 MB used and the app ran normally.
   - Don't run `screenshooter` on the TV; there is no working screenshot tool for us.
   - The Phase 3 "Reload display" crash may be the same failure (renderer restart). Phase 6/7 should look at whether the app can survive a renderer death (e.g. `onRenderProcessGone`-style handling, if the Vega WebView offers one).
2. **The screensaver can come up over the first-run screen** and stays until a key press (known Phase 1 finding 6; fix is Phase 6.2). Setup from the phone still worked once the user woke the TV.
3. **Device log stream** stalls on journal rotation now and then (`Journal header limits reached … rotating`). Restarting the stream helps; checking from the laptop (curl, `avahi-browse`) was more reliable.

## Open items

- **Decided (to do in Phase 5): the QR code always uses the IP URL.** The mDNS name is for convenience when someone has to remember or type the address; a QR code can go straight to the IP, which also avoids phones that can't resolve `.local`. Change `qrUrl` in `FirstRunScreen` to `ipUrls[0]` only, and use the same rule for the settings-panel QR (Phase 5.5). The `.local` URL stays as text above the IP URL.
- Carried over: D-pad acceleration on hold; screensaver; `getLogTail` noise; curl in sandbox; editable mDNS name (Phase 5).
