# Phase 7 Results: Release readiness

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`, Vega OS 2.0 (TV Ship/529). Package `org.openhamclock.hamclock`. Vega SDK 0.24.12112. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Verification

| Check | Result |
|:--|:--|
| FRESH Release install (no app data): first-run presets, Setup as N0CALL, live clock | ✅ (user) |
| Release shows no `[HC…]` debug lines; the self-test isn't in the bundle | ✅ device log |
| Pane menus complete (Phase 6 carry-over: Sat_Alerts, Mesh_Mon, Aurora, DRAP, …) | ✅ (user); `set_pane` accepts them |
| LAN toggle in Release (engine restart and WebView remount, the old "Reload display" crash path) | ✅ no crash, same app process |
| `vega.yml` on GitHub: SDK installs non-interactively, lint, tests, both `.vpkg` built | ✅ run 41074400476 (lint+tests 1m42s, build 7m03s) |
| CI artifact (armv7, build 1191) installed over 1187 on the TV | ✅ (user: running) |
| WebView memoized: `useComponentInstance is deprecated` no longer every 3 s | ✅ once at mount in 60 s (Release 1193) |
| Tests: tsc, lint (0 warnings), Jest 19/19, host C++ 108/0 | ✅ |

## What was built

### 7.1 Package ID
`org.openhamclock.hamclock` (`manifest.toml`, `app.json`, runner scripts). The old `com.` build was uninstalled.

### 7.2 Icon, title, version
- Icon `assets/image/icon.png` (from `feat/vega-support`) on the package and the component. Title "HamClock".
- `scripts/version.js` gives `--build-version` (release `TAG` without `v`, or `edge`) and `--build-number` (git commit count, like Android's `versionCode`). The CLI writes them to `meta-info/build-info.json`, and the TV reports exactly that. `manifest.toml`'s `version` is a placeholder.
- The TV refuses a lower build number ("Package version decrease"). Installing a build from an older commit needs an uninstall first.

### 7.3 Architectures
armv7 (tested) and aarch64 (built only; no arm64 Vega device known). `scripts/build.sh` builds the engine and then the app for each arch.

### 7.4 Release build
- JS debug lines are behind `__DEV__` (`debugLog`, the page's `dbg`). The self-test is `require`d only under `__DEV__`.
- Under `NDEBUG`, the engine ignores `watchdogSec` and `testSeedCallsign`.
- The Phase 6 `_IS_VEGA` change in `plotmgmnt.cpp` is reverted; the file is upstream again. That change sat in a fallthrough chain and made every pane choice above it unavailable on Vega.

### 7.5 Tests
- Jest (`npm test`): the lifecycle reducer, `firstRunVisible` and the LAN/QR URL helpers moved unchanged to `src/lifecycle.ts`; 19 tests in `test/lifecycle.test.ts`.
- Host C++ (`npm run test:engine`): `peerAllowed`, mDNS encode/decode, records/TTL, loopback answers, conflict detection and the validators. 108 checks; no engine source changes.

### 7.6 CI
- `.github/workflows/vega.yml`: a test job (tsc, lint, Jest, host C++), then a build job that installs the pinned SDK with `get_vvm.sh` (`NONINTERACTIVE=true`) and uploads both `.vpkg` files.
- Runs on `pull_request` (paths `vega/**`, `ESPHamClock/**`, the workflow itself) and `workflow_dispatch`. No push triggers (user decision).
- `release.yml`: input `build_vega` attaches `org.openhamclock.hamclock-<TAG>-vega-<arch>.vpkg` to the release (`continue-on-error`). Appstore submission is manual.
- Cleanup after the first run: `setup-node@v6` and `upload-artifact@v6` (Node 24; upload-artifact v5 still targets Node 20). SDK telemetry is opted out in both workflows (`optIn` set to false in `~/vega/telemetry/config.json`). `ubuntu-latest` is kept, as in the other workflows.
- Lint: `@amazon-devices/kepler/sdl-package-version-check-imports` is off (an informational notice on every import of a system-distributed library). The package.json pin check (`sdl-package-version-check`) stays an error. It caught `@amazon-devices/webview` `^4.0.2`, now `~4.0.2`.

### 7.7 Docs
`vega/README.md` (build, install, tests, versioning, Debug vs Release, CI, limitations). `ARCHITECTURE.md` now has a "superseded" banner. `AGENTS.md` points to the README.

### Extra: WebView memoized
`App.tsx` memoizes the WebView element (`useMemo` on showWeb, webGeneration, liveUri, webHasKeys, injectedJs, onBack). Status polling no longer re-renders it. Each render made Amazon's `WebView.kepler.js` log "useComponentInstance is deprecated".

## Left as is (known)

- `npm audit` in `vega/`: 36 findings (13 high), all in SDK/build tooling (kepler-cli-platform, eslint plugin, typescript-eslint, lodash, toml, minimatch). `npm audit fix` without `--force` would also bump `@amazon-devices/react-native-kepler` (a system-distributed runtime lib) and drop Hermes build packages, so it wasn't applied. `hcengine/`: 6 low (deepmerge, fix only with `--force`). Revisit with the SDK upgrade.
- npm deprecation notices (eslint 8, babel proposal plugins, glob 7, rimraf < 4, inflight, metro-react-native-babel-preset) and "TypeScript 5.8.3 is not officially supported by @typescript-eslint" come from the Vega RN template.
- After a LAN change the settings panel stays open, so the "Restarting" card is hidden behind it.
- Sat_Alerts (hams.at) and Mesh_Mon fail to fetch on Vega (no subprocesses, so no curl). See PLAN.md "Needs work".
- Merging `vega-app`: waiting for the user's review.
