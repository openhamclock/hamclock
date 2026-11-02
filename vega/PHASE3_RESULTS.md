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

## mDNS

### Findings

1. **The TV already runs avahi-daemon** (uid 997) on `0.0.0.0:5353` and `[::]:5353`, and `wlan0` has joined `224.0.0.251`.
   - Its D-Bus API is unreachable from the app (no `/run/dbus`), there's no static services dir, and the SDK has no avahi/dbus client libraries. So the app runs its own responder.
   - Avahi already publishes the TV as `firestick-2.local`.
2. **The app sandbox allows the multicast socket.** It binds `224.0.0.251:5353` with `SO_REUSEADDR` alongside avahi, joins the group on the LAN interface, and sends and receives normally.
3. **`hamclock.local` was already taken** on this LAN (by .68, later .101.61: other HamClock devices). Probing is required, not optional.

### Design (`kepler/engine/MdnsResponder.cpp`)

- **When it runs:** a responder thread, started on the first engine start, advertises while LAN access is on and the engine supervisor is alive.
  - An engine restart doesn't interrupt it.
  - LAN off or engine stop sends goodbyes (TTL 0).
  - It re-announces if the TV's IP or the live port changes.
- **Names:** probes 3× at 250 ms for `hamclock.local` and the service name `HamClock`, using ANY plus A questions (simple responders such as the Android app's only answer A).
  - On a conflict it moves to `hamclock-N.local` and `HamClock (N)`.
  - If another host claims our name later, it re-probes.
- **Records:** A `<host>` → TV IP (120 s, cache-flush); `_http._tcp` PTR → `HamClock (N)`; SRV → `<host>:8081`; TXT `path=/live.html`; plus `_services._dns-sd._udp` PTR.
  - It answers ANY/A/PTR/SRV/TXT.
  - Legacy (non-5353 source port) queries get a unicast reply with the question echoed and TTL ≤ 10.
- **Avahi stays unaffected:** the socket is bound to the group address, so it receives only multicast. Unicast to `:5353` still reaches avahi (checked: `dig @192.168.1.237 -p 5353 firestick-2.local` answers).
- **Status and panel:**
  - `getStatus()` adds `mdnsState` (`off` | `probing` | `announced` | `error: …`) and `mdnsHost`.
  - Once the name is announced, the panel shows `http://<mdnsHost>:8081/live.html` first, then "or http://<ip>:8081/live.html".
- **Not implemented** (optional or unneeded): known-answer suppression, response delays, probe tie-break, AAAA/NSEC.

### Verification (from the laptop)

| Check | Result |
|:--|:--|
| Probe conflict with existing `hamclock.local` → claims `hamclock-2.local` | ✅ |
| `avahi-resolve -n hamclock-2.local` | ✅ 192.168.1.237 |
| `avahi-browse -r _http._tcp` shows "HamClock (2)" → `hamclock-2.local:8081` | ✅ |
| `curl http://hamclock-2.local:8081/live.html` | ✅ 200 |
| LAN off: service and name gone at once (goodbye) | ✅ |
| LAN on again: re-probes and re-announces; status off → probing → announced → off | ✅ |
| TV's own avahi still answers `firestick-2.local` | ✅ |
| Legacy unicast reply path | Not verified: `dig @224.0.0.251` times out from this multi-homed laptop for avahi too |

## Open items

- **To do: user-editable mDNS name** (the Android app has a text field). It needs text entry on the TV and a saved setting; the base name is fixed at `hamclock` for now.
- Look over the settings panel on the TV with the `.local` URL (layout and URL size at 10 ft).
- Carried over: D-pad acceleration on hold; screensaver backgrounds the app; `getLogTail` noise; curl in sandbox unverified; first-run callsign seed decision.

## Package ID

Changed to `org.openhamclock.hamclock` (main component `org.openhamclock.hamclock.main`), matching the Android app and the openhamclock.org domain. The old `com.openhamclock.hamclock` install was removed from the TV. A leftover `org.` build from `feat/vega-support` blocked the install ("Package version decrease") and had to be uninstalled first.
