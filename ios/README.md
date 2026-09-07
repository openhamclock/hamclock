# HamClock for iOS

This directory contains the standalone iOS wrapper for HamClock.

## Architecture

* **Core Engine:** The native C++ HamClock engine is compiled directly from `../../ESPHamClock` using `-D_WEB_ONLY`, `-D_CLOCK_1600x960`, and `-D_IS_IOS`.
* **Zero Source Duplication:** The project links the shared `ESPHamClock/`, `ArduinoLib/`, `wsServer/`, and `zlib-hc/` sources directly.
* **Objective-C++ Bridge:** `HamClockBridge` launches the `hamclock_main()` engine daemon on a background POSIX thread and exposes platform hooks for app restart, exit, URL opening, system clipboard, and HTTPS streaming.
* **Display & Touch:** Interactive HTML5/WebSocket frontend rendered inside an accelerated, fullscreen `WKWebView` with automatic viewport scaling.
* **Declarative Project Management:** Managed via [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`project.yml`) to keep the repository clean and avoid `.xcodeproj` merge conflicts.

---

## Building

### Option 1: Generate & Open with Xcode (Recommended on macOS)

1. Install **XcodeGen** (if not already installed):
   ```bash
   brew install xcodegen
   ```

2. Generate the Xcode project:
   ```bash
   cd ios
   xcodegen generate
   ```

3. Open `HamClock.xcodeproj` in Xcode:
   ```bash
   open HamClock.xcodeproj
   ```

4. Select an iOS Simulator or connected physical iPhone/iPad and click **Run** (⌘R).

---

### Option 2: Command Line Build (`xcodebuild`)

#### Generate and Build for iOS Simulator:
```bash
cd ios
xcodegen generate
xcodebuild -project HamClock.xcodeproj -scheme HamClock -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M4)' build
```

#### Archive for Physical Device:
```bash
xcodebuild -project HamClock.xcodeproj -scheme HamClock -destination 'generic/platform=iOS' -archivePath build/HamClock.xcarchive archive
```

---

## Features

* **Auto-Scaling Display:** Automatically adapts the 1600x960 HamClock interface to iPhone, iPad, and external displays in landscape orientation.
* **Screen Wake Lock:** Prevents display sleeping via `UIApplication.shared.isIdleTimerDisabled`.
* **Host Location Seeding:** Automatically queries iOS `CoreLocation` to seed the DE grid and coordinates on startup when permitted.
* **Bonjour / mDNS Discovery:** Publishes `_hamclock._tcp` and `_http._tcp` for discovery by other devices on the LAN.
* **Dark Settings HUD:** Configure backend servers, enable local network access, and view live QR codes for LAN web viewing.

---

## Requirements

* macOS with Xcode 15+ (iOS 15.0+ deployment target)
* XcodeGen (`brew install xcodegen`)
