# Phase 5 Results: Settings and TV polish

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. Package `org.openhamclock.hamclock`. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Verification (on device)

Checked by eye on the TV, plus the device log and checks from the laptop (`curl`, `avahi-browse`).

| Check | Result |
|:--|:--|
| Install over Phase 4: the `vega-lan-access` file (on) migrates into `vega-settings.json` and is deleted | ✅ `{"lanAccess":true,"mdnsName":"hamclock",…}` |
| Each setting survives a relaunch (name, backend, overscan) | ✅ (`mdnsName foo` → `foo.local`, `backend ohb.hamclock.app:8012`, `overscan 5` after relaunch) |
| A name change shows up in `avahi-browse` | ✅ `HamClock foobar` → `foobar.local` at `192.168.1.237:8081`, `path=/live.html` |
| A backend change appears in the run args (`-b`) and restarts the engine | ✅ `get_sys.txt`: `BEHost ohb.hamclock.app`, `BEPort 80`, uptime reset at the change |
| Name and backend set back to defaults by clearing the field | ✅ `{"mdnsName":"hamclock","backend":"ohb.hamclock.app:80"}`; mDNS `hamclock-2.local` (the Android Fire Stick on the LAN holds `hamclock.local`) |
| Invalid input shows the error text | ✅ `-name` → "Use only letters, digits and hyphens." (red, under the field; nothing saved) |
| Overscan None / 3% / 5% visible and stays; None fills the screen top to bottom | ✅ |
| Overscan survives a relaunch (first frame already fitted) | ✅ (`overscan 5` relaunch) |
| Text fields: D-pad moves over them without opening the keyboard; OK opens it | ✅ |
| Done saves; focus stays on the field and the D-pad moves on | ✅ |
| One Back while typing cancels the edit; focus returns to the field | ✅ |
| Panel QR (main page) scans to the IP URL | ✅ |
| FRESH install: first-run QR encodes the IP URL; LAN on at once (live.html `200` from the laptop, REST down) | ✅ |
| HamClock's boot screen (settings summary) after an engine restart: two columns, not overlapping | ✅ (after the `tftMsg` fix, finding 1; engine restart from the panel) |
| Normal launch (callsign stored) goes straight to the clock: no "Click anywhere to enter Setup" countdown | ✅ (`-k`) |
| "Setup HamClock" opens HamClock's Setup page at once; Done returns to the clock without looping into Setup | ✅ (`-k -u`, `-u` dropped by `ESP::restart`) |

## Design (as built)

### 5.1 One settings store

- `<dataDir>/vega-settings.json` = `{lanAccess, mdnsName, backend, overscan}`, read and written natively (`kepler/engine/Settings.cpp`, atomic tmp + rename).
- Validation: `normalizeMdnsName` (DNS label, 1–40 chars, lowercased, `.local` stripped, empty = `hamclock`); `normalizeBackend` (`host[:port]`, `:80` added, empty = default). Bad input returns a message for the user; nothing is saved.
- Migration of the Phase 3 `vega-lan-access` file (then deleted). `FlatJson.h` is a shared flat-JSON reader/writer.
- `EngineHost`: `g_settings` (under `mu`) is the source of truth. Options passed to `start`/`restart` override and are saved; omitted ones use the saved values (`DEFAULT_ENGINE_OPTIONS` no longer sets `backend`). `settings()` / `updateSettings(json)`: a LAN or backend change restarts a running engine; a name change makes the responder re-probe. Status adds `mdnsName`.
- TurboModule: `getSettings()`, `updateSettings(json)`. Codegen overwrites `HcEngine.cpp/.h`; they were restored from git and the two methods added by hand. Log: `[HCSET] <settings json>` on change.

### 5.2 mDNS name

- `MdnsResponder` takes the base name (`mdnsWanted(port, name)`); on change it sends goodbyes and probes the new name. Instance name: `HamClock` for the default name, else `HamClock <name>`, plus ` (N)` on conflict.
- The settings page says when the name is taken and which one the TV uses instead.

### 5.3 Backend host

- Text field "Backend server (host:port)"; empty = `ohb.hamclock.app:80`. Saving restarts the engine (the panel closes).

### Text fields on a TV (`SettingField`)

- A focused `TextInput` opens the on-screen keyboard at once, which traps D-pad navigation. The field is a focusable button showing the value ("Press OK to edit."); OK mounts the `TextInput` with `autoFocus`.
- Done saves. Back cancels: the TV keyboard takes the first Back itself and only hides, so `keyboardDidHide` (and a blur without Done) means cancel, after 200 ms so a Done arriving just after still wins.
- After an edit the panel's `TVFocusGuideView` is remounted (key = page + counter) with focus on the edited field. Removing the focused `TextInput` left Vega's focus engine stuck on the field: keys arrived in RN, but focus never moved. A fresh guide, as on a page switch, works.
- The error / "Saved." note (and the rejected text) is kept in App state, so it survives the remount.
- The earlier `feat/vega-support` branch's `TvEditableField` was a plain `TextInput`, so there was nothing to reuse.
- `manifest.toml`: `com.amazon.inputmethod.service` (needed for the keyboard).

### 5.4 Screen safe margin (overscan), done inside the page

- Padding around the WebView in RN didn't hold: the margin flashed and reverted, and the Vega WebView surface doesn't follow the RN layout change. Also, 0% wasn't full screen, because live.html's `initCanvas()` only shrinks a clock larger than the window; a smaller one (BuildSz 1600×960) is centered at its own size. It runs again on every full frame.
- Now (approach from `feat/vega-support`'s `RemoteNav.ts`): the script injected before load wraps `initCanvas` on `DOMContentLoaded` and always scales the canvas to fit the window minus `2 × margin`, keeping the aspect ratio and centered, and sets `app_scale` so the virtual cursor and touches line up.
- The margin at WebView mount goes into the injected script (first frame already fitted); changes are pushed with `window.__hcSetOverscan(pct)`, also on `onLoad`.

### 5.5 QR codes

- Always the IP URL (decision 4), first-run screen and panel; the `.local` URL is text.

### 5.6 Panel layout

- Main page: left column (title, status, Resume / LAN access / Settings… / Setup HamClock / Exit, 440 dp), right column `LanInfo` (QR, `.local` URL, IP URL, refused count; takes the rest).
- Settings page: Network name, Backend, Screen safe margin (None / Normal 3% / Large 5%), Back. Back on the settings page → main page.

### 5.7 Startup: `-k` always, `-u` for "Setup HamClock"

- Every engine run gets `-k` (`buildArgs`): no 10 s Setup offer, no Skip waits. HamClock still opens Setup by itself when no callsign is stored, so first run is unchanged.
- New HamClock option `-u` (all platforms, no ifdef): open Setup at once, without the countdown; overrides `-k`.
  - `Arduino.cpp` parses it into `setup_now` (`ESPHamClock.cpp`, `ArduinoLib.h`); `setup.cpp` `clockSetup()` uses it and clears it (one-shot even where globals outlive a restart, as on Vega).
  - `ESP::restart()` always drops `-u`, so HamClock's own restarts (after Setup Done, from its menu) don't loop into Setup.
- The panel's "Setup HamClock" (was "Restart HamClock") restarts with `EngineOptions.openSetup` (TS `openSetup?: boolean`), which adds `-u` to that run only; `takeOpenSetup()` clears it, so later settings restarts go straight to the clock.
- Android fakes this with a virtual tap on the countdown (`hamclock_jni.cpp` `setup_injector_worker`); it could switch to `-u` later.

## Findings

1. **HamClock's boot screen overlapped after an engine restart.** The "columns overlap" reported earlier was HamClock's own startup screen (the settings summary after the Setup countdown), not the RN panel; `onLayout` logs of the panel were the same and correct before and after a restart. `tftMsg()` keeps its row/column in function statics, and the Vega engine restarts `hamclock_main` in the same process, so a second run kept writing after the first run's last row (column 2, at varying heights).
   - Fix (Vega only): `EngineHost` exports `vega_engine_run` (run number); under `#if defined(_IS_VEGA)` `tftMsg` resets row/column/hold when the run number changes.
   - Other HamClock state kept in statics also survives an in-process restart. Nothing else visible was found; keep it in mind for Phase 6.
2. **WebView renderer restarts on every engine restart** (the WebView is remounted with a new key): `RendererService got disconnected` → `connected` about a second later. With memory normal (memAvailable ~300 MB, swapFree ~780 MB) the app survived every time (4 restarts this session). The earlier signal-5 crash came with `memory-extreme thrashing` (swapFree ~80 MB). Same failure as Phase 4 finding 1: a renderer death under memory pressure kills the app. The TV had been rebooted since then. The shell user can't see app processes, so per-process memory couldn't be checked. Phase 6/7: look for a renderer-gone callback in the Vega WebView, and avoid remounting the WebView on a restart if a reload is enough.
3. **The `vega` CLI fails when another `vda` device is connected** (`Cannot read properties of undefined (reading 'trim')`; here an Android Fire Stick at .68). Pass `-d 192.168.1.237:5555` to every `vega device` command (`run-p5.sh` does now).
4. Device log stream stalled again once (FRESH run); laptop checks were reliable.

## Open items

- Carried over to Phase 6: D-pad acceleration on hold; screensaver; Home/background; `getLogTail` noise; curl in the sandbox.
- Noted for later (user, after Phase 5):
  1. **Backend: no restart on Done.** Done in the Backend field should only save the value; the user may have more changes. Restart the engine once, when needed, on an explicit save (e.g. leaving the settings page or a Save/Apply button).
  2. **First-run presets** (TVs have no GPS, and HamClock can't set the TV's clock), written before HamClock's first Setup:
     - 2a. DE location from geoIP. Note: `-k` turns off the `-i` geoIP init (`Arduino.cpp`: `if (init_iploc && !skip_skip)`), so preset HamClock's stored setting rather than relying on `-i`.
     - 2b. NTP setting = "computer" (use the system time).
- Debug logging to trim for release (Phase 7): `[HCWEB]` keys, `[HCLAN]`, `[HCSET]`, `[HCFIELD]`.
