# HamClock for Vega (Amazon Fire TV, Vega OS)

The HamClock engine (`../ESPHamClock`, built as `libHcEngine.so`) runs inside a React Native for Vega app.
The app shows HamClock's own `live.html` page in a WebView. Package ID: `org.openhamclock.hamclock`.

- `src/`: RN app (`App.tsx`: WebView, settings panel, first-run QR screen; `lifecycle.ts`).
- `hcengine/`: Turbo Module that hosts the engine (`kepler/engine/`: `EngineHost`, LAN filter, mDNS, settings).
- `scripts/`: `build.sh` (engine + app for each arch), `version.js` (version and build number).
- `test/`, `hcengine/test/host/`: Jest and host C++ tests.

## Prerequisites

- Vega SDK 2.0 (2.0.13986, as pinned in CI) and the `vega` CLI (`source ~/vega/env` in each shell). The manifest targets OS 2.0 with a minimum of 1.2.
- System Node (e.g. `/usr/bin/node` from dnf) for `npm`, `tsc`, `eslint` and Jest. The `vega` CLI uses its own bundled Node.
- `g++` and `make` for the host C++ tests.

```sh
cd vega
npm ci
(cd hcengine && npm ci)
```

## Build

```sh
source ~/vega/env
scripts/build.sh                  # Release, armv7
scripts/build.sh Debug armv7      # Debug
scripts/build.sh Release armv7 aarch64   # also aarch64 (optional; no arm64 Vega device known)
```

The engine has to be built for an architecture before the app is built for it; `build.sh` handles that order.
Packages are written to `build/<arch>-<release|debug>/hamclock_<arch>.vpkg`.

**Version and build number** come from `scripts/version.js`:
- Version: the `TAG` env or the current git tag, without the `v` (`v4.34b00.0` → `4.34b00.0`). Otherwise it is `edge`.
- Build number: the git commit count (the same as Android's `versionCode`).
- The TV refuses a build with a lower build number ("Package version decrease"). Uninstall first.
- `manifest.toml`'s `version` is only a placeholder.

**Debug vs Release:**
- In Debug, the JS debug log lines (`[HCWEB]`, `[HCAPP]`, `[HCSET]`, `[HCLAN]`, `[HCSETUP]`, `[HCFIELD]`, `[HCEV]`) are on (`__DEV__`).
- Debug also includes the self-test (`src/engine/selfTest`).
- Debug also accepts the engine's `watchdogSec` and `testSeedCallsign` options.
- Release leaves all of these out.

## Install and run on a TV

```sh
DEV=192.168.1.237:5555            # always pass -d; the CLI fails if another vda device is connected
vega device install-app   -d $DEV -p build/armv7-release/hamclock_armv7.vpkg
vega device launch-app    -d $DEV -a org.openhamclock.hamclock.main
vega device start-log-stream -d $DEV          # app log (journalctl on the TV doesn't show it)
vega device terminate-app -d $DEV -a org.openhamclock.hamclock.main
vega device uninstall-app -d $DEV -a org.openhamclock.hamclock   # clears app data (next start is a first run)
```

On first run, the app turns LAN access on and shows a QR code for `http://<tv-ip>:8081/live.html`. HamClock Setup can then be done with the remote or from a browser on the LAN. For testing, use callsign N0CALL.

Don't run the TV's `screenshooter`. It can't capture app windows, and while it runs it holds enough memory that the WebView renderer gets killed, which takes the app down with it (PHASE4_RESULTS.md).

## Tests

```sh
npm run typecheck      # tsc (needs hcengine/dist: `cd hcengine && npm run build:js`)
npm run lint
npm test               # Jest: lifecycle reducer, first-run logic, LAN/QR URLs
npm run test:engine    # host C++: LAN filter, mDNS wire format/answers/conflicts, settings validators
```

None of these need the TV.

## CI and releases

- `.github/workflows/vega.yml` runs on pull requests that touch `vega/` or `ESPHamClock/`, and by hand (Actions → Vega → Run workflow); not on push. It runs the tests, then a Release build with the Vega SDK installed in the runner (telemetry opted out). The `.vpkg` is uploaded as an artifact. It builds armv7; a manual run can tick `build_aarch64` to add aarch64.
  - GitHub shows "Run workflow" only for workflows on the default branch (`main`). Until `vega.yml` is there, start it with `gh workflow run vega.yml --ref <branch>` (add `-f build_aarch64=true` for aarch64).
- `.github/workflows/release.yml` (input `build_vega`) builds the Vega package with the release `TAG` and attaches `org.openhamclock.hamclock-<TAG>-vega-armv7.vpkg` to the release. Input `build_vega_aarch64` (default false) adds the aarch64 package. Both arches can share one build number, so aarch64 can be added to an existing release later.

## Known limitations

- **No subprocesses.** Vega apps can't run subprocesses, so HamClock's `curl`-based fetches fail. This affects the Sat_Alerts (hams.at) and Mesh_Mon panes, which stay in the menus but show no data. They need an in-process HTTP(S) client (see PLAN.md, "Needs work").
- **No start on boot or background running.** Vega has no supported way to do either.
- **aarch64 is optional and untested.** No arm64 Vega device is known, so builds default to armv7 only.

## Documentation

- [PLAN.md](PLAN.md): the maintained phase plan and decisions log.
- `PHASE0_RESULTS.md` … `PHASE8_RESULTS.md`: what was built and found in each phase. Where they conflict, the latest one wins.
- [ARCHITECTURE.md](ARCHITECTURE.md): the original design spec (a native re-implementation). It is superseded by the embedded-engine + WebView design.
- [AGENTS.md](AGENTS.md): notes for coding agents.
