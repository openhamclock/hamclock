# HamClock for Amazon Vega OS

This directory contains the standalone native application for HamClock targeted at Amazon's Linux-based **Vega OS** (Fire TV Stick 4K Select and newer Vega devices).

## Architecture

* **Core Engine:** The native C++ HamClock engine from `../ESPHamClock` is compiled as a Vega native TurboModule using `-D_WEB_ONLY`, `-D_CLOCK_1600x960`, `NO_UPGRADE`, and `_IS_LINUX`.
* **Execution Model:** On app start, the TurboModule spawns `hamclock_main()` on an internal POSIX thread listening on local loopback (`127.0.0.1:8080`).
* **Display & TV Remote Navigation:** The user interface is displayed in a fullscreen Vega WebView (`@amazon-devices/webview`). TV remote D-Pad events are bridged to HamClock's built-in virtual cursor (`handleVirtualCursorMove` and `handleVirtualCursorClick`).
* **Settings & Controls:** Pressing the Fire TV **Menu** or **Play/Pause** button opens the in-app settings modal to switch between local embedded engine and remote backend host.

## Prerequisites

* **OS:** Linux (Ubuntu, Fedora, Debian) or macOS
* **Vega Developer Tools (VDT):**
  The Vega CLI is installed in `~/vega/bin` with the active Vega SDK (0.24.x+).
  Make sure your shell loads the Vega environment:
  ```bash
  source ~/vega/env
  ```

## Building the Package

From the `vega/` directory:

```bash
# Debug build (generates aarch64, armv7, and x86_64 packages):
npm run build:debug

# Release build:
npm run build:release
```

The resulting packages (`.vpkg`) will be created under:
```
build/private/kepler/hamclock-vega/undefined/vega/aarch64/Debug/hamclock-vega_aarch64.vpkg
```

## Installing on Fire TV (Vega OS Hardware)

1. Connect your Fire TV device to the same Wi-Fi network as your workstation.
2. Under Fire TV **Settings > My Fire TV > Developer Options**, enable **Network Debugging**.
3. Connect and deploy using the Vega CLI:
   ```bash
   vega device connect <DEVICE_IP>
   vega device install-app build/private/kepler/hamclock-vega/undefined/vega/aarch64/Debug/hamclock-vega_aarch64.vpkg
   vega device run-app org.openhamclock.hamclock.main
   ```

## Project Structure

* `manifest.toml`: App ID, permissions, and landscape TV definition for Vega OS.
* `CMakeLists.txt`: Compiles `native/HamClockTurboModule.cpp` and links `../ESPHamClock` source tree into `libHamClockTurboModule.so`.
* `native/`: C++ TurboModule daemon thread bridge.
* `src/App.tsx`: Main React Native TV wrapper hosting the WebView and settings modal.
* `src/RemoteNav.ts`: Remote D-Pad key handler injection script.
* `node-compat.js`: Node 18 compatibility layer for Metro and Vega SDK tooling.
