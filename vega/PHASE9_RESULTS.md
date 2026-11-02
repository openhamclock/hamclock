# Phase 9 Results: LAN toggle restart UX

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`, Vega OS 2.0. Vega SDK 2.0.13986. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Problem

Turning LAN access on or off restarts the engine at once (the bind address changes). The settings panel stays open on purpose, so the user sees the QR code after turning LAN on. But the "Restarting HamClock…" status card is behind the panel, so the panel looked unchanged while the engine restarted. The engine reports the new `lanAccess` at once, so the QR code showed up before the new server was listening, and a scan in that window failed. A second press during the restart started another restart.

## What changed (app only; no engine changes)

- `lifecycle.ts`: new `engineBusy(state)`, true while the lifecycle is `RESTARTING` or `STARTING`, or while `restartHold` is set.
- Minimum display time: a LAN restart takes about a second on the TV (LAN monitor from the laptop: off → on again in ≤ 1 s; the app polls every 500 ms). Without a minimum, the first device test showed no visible notice and no lock on the button. So a LAN change now dispatches `RESTART_REQUESTED` with `hold`, which sets `restartHold`. The App clears it with `RESTART_HOLD_DONE` after `LAN_RESTART_HOLD_MS` (2 s). The notice stays up for at least that long, and until the new run listens. Other restarts (backend, Setup HamClock) close the panel and don't hold.
- `App.tsx`:
  - New `EngineNotice`. While the engine is busy, it takes the place of the address and QR code in the panel: a pulsing dot, "Restarting HamClock…", and either "The address and QR code appear here when HamClock is ready." (LAN on) or "LAN access is off: only this TV can open HamClock." (LAN off).
  - It also shows the error (`ERROR`) or "HamClock has stopped." (`STOPPED`), since the status card is hidden behind the panel then too.
  - The LAN access button ignores presses while the engine is busy. It stays focusable, so focus doesn't jump.
  - The panel still stays open after a LAN change. When the new run is listening, the QR code (LAN on) or the "LAN access is off" box appears in place of the notice.
- `test/lifecycle.test.ts`: `engineBusy` through a LAN restart (stopping → starting → listening), not for `ERROR`/`STOPPED`, and held until `RESTART_HOLD_DONE`. Jest 21/21.

## Verification

| Check | Result |
|:--|:--|
| tsc, lint, Jest 21/21 | ✅ |
| First device test (without the hold): restart ≤ 1 s, notice not visible; QR scan right after it appeared worked | ✅ (user) → hold added |
| Release build armv7 (SDK 2.0.13986), no compiler warnings | ✅ |
| TV with the 2 s hold: LAN on → off: "Restarting HamClock…" with the pulsing dot, then the "LAN access is off" box | ✅ (user) |
| TV: LAN off → on: notice, then QR code; QR scan works right after it appears | ✅ (user) |
| TV: pressing LAN again during the restart does nothing | ✅ (user) |
| TV: Resume during the restart: status card, then the clock | ✅ (user) |

## Notes

- The TV had dropped off the SDK's device list (`vega device list`: "No devices found") although it answered ping and port 5555 was open. `vda connect 192.168.1.237:5555` (the SDK's `bin/tools/vda`) brought it back. README updated.
- The TV refused to install the local build over the installed one ("Uninstall existing package to update"), probably the LAT build from the Appstore. It was uninstalled (app data cleared, set up again as N0CALL). Reinstall the LAT build from the Appstore to continue the LAT on this TV.

