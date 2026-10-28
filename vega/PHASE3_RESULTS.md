# Phase 3 Results: LAN access

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`. Laptop: `192.168.1.1` (same LAN), which also owns a public AMPRNet address (`44.27.17.225`) used as the "outside" source. This file takes precedence over PHASE0–2_RESULTS.md where they conflict.

## Verification (on device; runner `tmp/vega-spike/run-p3.sh`; LAN toggled by remote keys)

| Check | Result |
|:--|:--|
| LAN off: laptop → `:8081/live.html` | ✅ connection refused (bound to loopback) |
| LAN on: laptop (private source) → `:8081/live.html` | ✅ 200, 39 KB page |
| LAN on: public source (`curl --interface 44.27.17.225`) | ✅ accepted, then reset by the filter; `rejectedConnections` 0 → 1 |
| App relaunch with no settings change keeps LAN on | ✅ (saved choice is used) |
| LAN on → off from the panel: laptop refused again | ✅ |
| TV address found inside the app sandbox | ✅ `getifaddrs` → `192.168.1.237` |
| Settings panel shows the URL | Not checked by eye (no screenshot tool); data seen in `[HCLAN]` log lines |

`:8080` (REST) was refused in every case because this data dir hasn't finished Setup (PHASE1 finding 5). This is expected, not a filter result.

## Design (as built)

- **Address filter** (`kepler/engine/LanFilter.cpp`, applied in the `accept` wrap in `SysWraps.cpp`), per ARCHITECTURE.md §3.3:
  - loopback is always allowed;
  - with LAN access on, also `10/8`, `172.16/12`, `192.168/16`, `169.254/16`, `fe80::/10`, `fc00::/7` (and IPv4-mapped forms);
  - anything else is closed right after `accept`. Helper threads (blocking accept loops) wait for the next connection. The engine thread (`select`+`accept` poll) gets `ECONNABORTED`, which `WiFiServer` treats as "no client".
  - The bind policy is unchanged (loopback unless LAN is on), so with LAN off the filter is a second layer only.
- **Saved LAN choice:** `<dataDir>/vega-lan-access` (`1`/`0`, written atomically).
  - `start()` without `lanAccess` uses the saved value (false if none). `DEFAULT_ENGINE_OPTIONS` no longer sets `lanAccess`.
  - Every `start`/`restart`/`setLanAccess` saves the value in effect. `restart(opts)` without `lanAccess` keeps the current one.
- **`getStatus()` additions:** `lanAddresses` (the TV's private IPv4 addresses on up interfaces; fallback is a UDP `connect`+`getsockname`, which sends nothing) and `rejectedConnections` (count this session). There is one event per new refused address, so a scanner can't flush the event ring.
- **Settings panel** (`App.tsx` `LanInfo`):
  - LAN on: "On a computer or phone on your network, open:" followed by `http://<ip>:<liveRwPort>/live.html` in amber, plus a refused count if it's above 0.
  - LAN off: a one-line explanation of what LAN access does.
  - The panel is 680 dp wide (was 560 dp) so the URL fits.
- **Diagnostics:** `[HCLAN] lan=… addrs=… refused=…` is logged when any of these change.
- **Removed "Reload display"** from the panel. The user reported it crashes, and the Android app has no such option. Panel now: Resume, LAN access, Restart HamClock, Exit app.
  - The crash wasn't diagnosed. The same WebView remount (`WEB_RELOAD`) still runs automatically after a page load error, so look at it if that path ever misbehaves.

## Open items

- **mDNS** (`_http._tcp`, `<name>.local`) is not done. It needs its own sandbox check: can the app join `224.0.0.251:5353`, and does a system responder already own the port?
- Look over the settings panel on the TV (layout and URL size at 10 ft).
- Carried over: D-pad acceleration on hold; screensaver backgrounds the app; `getLogTail` noise; curl in sandbox unverified; first-run callsign seed decision.
