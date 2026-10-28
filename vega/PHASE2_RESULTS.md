# Phase 2 Results: WebView UI

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. This file takes precedence over PHASE0/PHASE1_RESULTS.md where they conflict.

## Verification (on device, by hand)

| Check | Result |
|:--|:--|
| Engine starts, WebView shows the engine's `live.html` (Setup on a fresh data dir) | ✅ |
| D-pad moves live.html's virtual cursor (first press shows it) | ✅ (no acceleration on hold yet; see open items) |
| OK taps at the cursor | ✅ |
| Back opens the native settings panel; Back or "Resume clock" closes it | ✅ |

## Design (as built)

- `src/App.tsx` is a shell (~390 lines, was ~1020):
  - polls `HcEngine.getStatus()`: every 500 ms while starting, every 3 s once ready;
  - mounts `<WebView>` on `http://127.0.0.1:<liveRwPort>/live.html` once that port is listening;
  - shows a status card while starting, restarting or after an error, and retries the page after a load error;
  - reloads the page when a new engine run starts (`runCount` changes).
- Settings panel: Resume, Reload display, LAN access on/off (restarts the engine), Restart HamClock, Exit app.
- Removed: the RN frame renderer, base64 decoder, live-socket client and RN cursor.
- Key routing: `KEY_ROUTER_JS` is injected with `injectedJavaScriptBeforeContentLoaded` and adds a capture-phase `keydown` listener in the page.
  - Arrows call `handleVirtualCursorMove`; Enter/Select calls `handleVirtualCursorClick`.
  - Back (`GoBack`, keyCode 27) calls `ReactNativeWebView.postMessage('back')`, which toggles the panel. If live.html's embed overlay is visible, Back is left to the page.
  - Every key is also posted as `key:<key>:<code>[:h]` and logged as `[HCWEB]` (diagnostics).

## Findings

1. **live.html doesn't call its own cursor code.** It defines `handleVirtualCursorMove`/`handleVirtualCursorClick`, but the host must call them; the Android app does this from native key handling. Without that, arrows go to HamClock as keys and move focus between Setup fields.
2. **`allowSystemKeyEvents` sends Back only to the page** (`key=GoBack`, `keyCode=27`), so RN's `BackHandler` never fires. Hence the page posts Back to RN.
   - While the panel is open, `allowSystemKeyEvents` is false, so RN focus and `BackHandler` work.
3. **The embed overlay is hidden by CSS**, so its inline `style.display` is `''`. Use `getComputedStyle`.
4. **Menu opens the RN dev menu in Debug builds.** Don't use Menu for app features while testing Debug builds.
5. The `@amazon-devices/webview` dependency was missing from package.json (the manifest entries were already there).

## Open items

- **D-pad acceleration on hold.** Wanted: roughly the Android app's feel. live.html only speeds up when presses are under 200 ms apart, so check whether Vega key repeat arrives at all, and at what rate, while a button is held. The Android app may add its own repeat on hold.
  - Branch `feat/vega-support` (earlier attempt) found key-timing issues and worked around them. It wasn't as smooth as Android: don't copy it. Review it only if it saves time.
- No callsign seed on first run (the user does Setup in HamClock); decide whether to keep it that way.
- LAN setting is not persisted across launches; TV IP not shown yet (Phase 3).
- Carried over from Phase 1: screensaver backgrounds the app; `getLogTail` noise; curl in sandbox unverified.
