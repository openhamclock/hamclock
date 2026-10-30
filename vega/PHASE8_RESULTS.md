# Phase 8 Results: Vega SDK 2.0 upgrade

Device: Fire TV Stick "calypso", armv7l, `192.168.1.237`, Vega OS 2.0 (TV Ship/529). Package `org.openhamclock.hamclock`. **Vega SDK 2.0.13986** (was 0.24.12112), CLI 1.4.7 (already the latest). Branch `vega-sdk2` off `vega-app`. This file takes precedence over earlier PHASEn_RESULTS.md where they conflict.

## Verification

| Check | Result |
|:--|:--|
| Baseline on 0.24 (before any change): `npm ci`, tsc, lint, Jest 19/19, host C++ 108/0 | ✅ |
| `vega project doctor` after the update | ✅ all checks, no warnings |
| Same tests on 2.0: tsc, lint, Jest 19/19, host C++ 108/0 | ✅ |
| Clean Release build, armv7 + aarch64, with the 2.0 toolchain | ✅ no compiler warnings or errors (168 compile steps per arch) |
| Package: `assets/kepler-sdk-version` 2.0.13986, build 1195, `os_versions` 1.2 and 2.0, armv7 `libHcEngine.so` | ✅ |
| FRESH Release install (uninstall first): first-run QR screen, LAN on (`:8081/live.html` 200) | ✅ (user + curl) |
| Setup as N0CALL: geoIP DE and NTP = Computer preset, TV switches to the clock by itself | ✅ (user) |
| D-pad, hold speed-up, Back/Menu, settings panel, QR, LAN toggle (no crash) | ✅ (user) |
| `get_sys.txt`: `Platform HamClock-vega`, `F2:05:1B:A5:8F:63` (locally administered), S/N 463834979 | ✅ |
| mDNS: "HamClock (2)" / `hamclock-2.local`, because the Android device (192.168.1.68) holds `hamclock` | ✅ conflict handled |
| No `[HC…]` lines, no crash signal; `useComponentInstance is deprecated` 0 times (Phase 7: once at mount) | ✅ device log |
| Screensaver held off past the idle timeout | ✅ (user) |
| Home and return: live clock again | ✅ (user) |
| `vega.yml` on GitHub with SDK 2.0.13986 (`workflow_dispatch` on `vega-sdk2`) | ✅ run 41112215945 (lint+tests 1m24s, build 7m33s) |

## What changed

### 8.1 SDK
- `vega sdk install 2.0.13986 --non-interactive`, then `vega sdk use 2.0.13986`. 0.24.12112 stays installed (`vega sdk use 0.24.12112` to go back).
- The install printed a Python traceback from the SDK's own perf tool (`KeplerPerfCLI`, bundled numpy: `TypeError: readonly attribute`) and then finished normally. Our build doesn't use that tool.
- `vega sdk list-remote` lists 2.0.13986 first; the plain `tail` of its output only showed 0.23.x (it is paginated, newest first).

### 8.2 Project (`vega project update --os-version 2.0 --os-min 1.2`)
- `manifest.toml`: `[os.version]` target 2.0, min **1.2** (decision: keep older Vega devices). The tool removed the `/com.amazon.vega.os@IVega_1_2` needs entry: in 2.0, `vega build` inserts the OS modules itself. The packaged manifest has `needs` `IVega_1_2` (the min) and `wants` `IVega_2_0` (the target), plus the RN runtime module.
- `package.json`: `@amazon-devices/webview` `~4.0.2` → `~4.0.0` (the SDK's profile; still allows 4.0.2–4.0.5). `react-native-kepler` stays `~4.0.0`. `vega project install --fix` changed nothing further.
- `package.json` gained `overrides` for `react` 19.2.0 / `react-native` 0.83.0, as in the 2.0 templates (`npm ls` already showed one copy of each).
- No `vega-sdk-requirements.json` or other new project file is expected by 2.0 (not in the templates, and `doctor` doesn't ask for one).

### 8.3 Packages
- The app was already on React 19.2.0 / RN 0.83.0 / Hermes (the 0.24 template), so no framework upgrade was needed. The 2.0 `vegaWebview` and `helloWorld` templates have the same versions and devDependencies.
- `@amazon-devices/eslint-plugin-kepler` 0.1.15 installed (0.1.16 latest, within `^0.1.4`); `kepler-cli-platform` 0.22.14 (latest).
- `npm audit`: unchanged (app 36: 10 low, 13 moderate, 13 high, all build tooling; `hcengine/` 6 low). The 2.0 template has the same tooling, so it would show the same. The deprecation notices and the "TypeScript 5.8.3 is not officially supported by @typescript-eslint" warning remain (also in the template).

### 8.4 Engine
- `hcengine/` matches the 2.0 `basic-turbo-module` template (package.json, configs, `react-native.config.js`, `AutoLinkInit.cpp`). No source or CMake changes. The engine rebuilt with the 2.0 toolchain without warnings.

### 8.5 App code
- No changes needed: tsc, lint and Jest pass as before.

### 8.6 CI
- `VEGA_SDK_VERSION` is `2.0.13986` in `vega.yml` and `release.yml`. Run 41112215945 passed; its build log uses only `sdk/vega-sdk/main/2.0.13986`.
- The Actions tab has no "Run workflow" button for Vega: GitHub shows it only for workflows on the default branch (`main`), and `vega.yml` isn't there yet. `gh workflow run vega.yml --ref <branch>` works, because the workflow is already registered.

### 8.7 aarch64 optional (user decision)
- No arm64 Vega device is known, and the Appstore takes both arches with the same build number, so aarch64 can be added later. Builds now default to **armv7 only**.
- `scripts/build.sh` defaults to armv7 (`scripts/build.sh Release armv7 aarch64` for both).
- `vega.yml`: `workflow_dispatch` input `build_aarch64` (boolean, default false); PRs build armv7 only. The job is now "Build .vpkg".
- `release.yml`: input `build_vega_aarch64` (default false); without it only `…-vega-armv7.vpkg` is attached. That makes 10 inputs.

## Compared with the 2.0 templates (kept as is)
- The template `tsconfig.json` no longer extends `@react-native/typescript-config` (it lists its own options). Ours still extends it and works.
- The template manifest sets `build-number = 1` ("floor so local builds pass vpt validate"). Our builds always pass `--build-number`.
- Our `.eslintrc` keeps the Kepler eslint plugin (the template has none).

## Left as is (known)
- Same as PHASE7_RESULTS.md "Left as is": `npm audit` findings in tooling, template deprecation notices, the LAN-change panel hiding the "Restarting" card, Sat_Alerts/Mesh_Mon without curl.
- The device log shows `Releasing 3 lock failed` from the system power manager (`resmgr-monitor-power`) at launch; not from the app.
