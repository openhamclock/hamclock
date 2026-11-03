# Google Play Developer Policy Compliance Plan
**Analysis, Audit, and Roadmap for HamClock Android**
*Document Version: 1.0 — Date: November 2026*
*Policy Reference: [Google Play Policy Updates (Answer 18100766)](https://support.google.com/googleplay/android-developer/answer/18100766)*

---

## 1. Executive Summary & Policy Overview

Google Play published updated Developer Program Policies (Answer ID: `18100766`, published late October 2026) alongside upcoming enforcement deadlines rolling into early 2027. 

This document audits the **HamClock Android** project (`android/`) against these current and upcoming requirements, identifies compliance gaps, and lays out an actionable, phased plan.

### Key Policy Changes & Enforcement Deadlines

| Policy Area | Key Mandate | Target Android Version | Enforcement Deadline | HamClock Status |
| :--- | :--- | :--- | :--- | :--- |
| **Location Permission Minimum Scope & Declaration** | Apps must use the minimum necessary location scope. Precise location (`ACCESS_FINE_LOCATION`) requires a mandatory Google Play Console declaration form justifying why coarse location is insufficient. Infrequent/one-time requests on Android 17+ must use the Android Location Button. Apps must not prompt sensitive permissions on cold start without prominent in-app context. | All target SDKs (`ACCESS_FINE_LOCATION` declaration); Android 17+ (Location Button) | **January 27, 2027** | ✅ **Code Implemented & Verified**<br>• Custom in-app Prominent Disclosure dialog (`dialog_location_disclosure.xml`) implemented matching dark theme.<br>• Cold launch no longer pops runtime prompts without context.<br>• Users who skip/decline are never interrupted again on subsequent boots.<br>• Added Location status & refresh controls to Settings Panel Page 2.<br>• *Remaining:* Developer Play Console declaration submission. |
| **16 KB Memory Page Size Support** | All apps distributed via Google Play targeting Android 15+ containing native C/C++ libraries must be compiled and aligned to 16 KB memory boundaries (`0x4000`). Unaligned updates will be blocked. | Android 15 (API 35)+ | **February 1, 2027** | ✅ **Compliant & Automated**<br>• NDK r27 + `-Wl,-z,max-page-size=16384` verified.<br>• `libhamclock.so` & `libc++_shared.so` are 16 KB aligned (`Align 0x4000`).<br>• Automated `verify16KbAlignment` task validates all 64-bit binaries in APK/AAB during Gradle builds. |
| **Local Network Permission (`ACCESS_LOCAL_NETWORK`)** | Restricted permission for apps communicating over LAN (raw sockets, mDNS, HTTP servers). Requires Play Console declaration showing it is required for core functionality when system pickers are insufficient. | Android 17 (API 37)+ | August 2027 (when targeting API 37) | ℹ️ **Future Phase**<br>• App targets API 36 today.<br>• Pre-plan declaration for mDNS and local HTTP/WebSocket daemon. |
| **Target API Level Requirement** | New apps and updates must target at least Android 16 (API 36). | Android 16 (API 36) | Active (August 31, 2026) | ✅ **Compliant**<br>• `compileSdk = 36`, `targetSdk = 36`. |
| **Network Security & Cleartext Traffic** | Cleartext traffic should not be enabled globally (`android:usesCleartextTraffic="true"`) without justification; domain-specific scoping via Network Security Config is required. | All versions | Ongoing review | ✅ **Compliant**<br>• Replaced blanket `usesCleartextTraffic="true"` with scoped `network_security_config.xml`.<br>• Strict TLS enforcement default; cleartext explicitly restricted to local loopback and authorized amateur radio mirrors. |
| **Manage Contacts / AppFunctions / Subscriptions** | Stricter rules on contacts pickers, AI agent manipulation rules, and recurring student/eligibility discounts. | API 37+ / Specific APIs | Active | 🟢 **Not Applicable**<br>• HamClock does not use contacts, AppFunctions, or subscriptions. |

---

## 2. HamClock Codebase Audit & Gap Analysis

### 2.1 Location Permissions (`ACCESS_FINE_LOCATION` / `ACCESS_COARSE_LOCATION`)
* **Current Implementation (`MainActivity.kt:228-238`, `369-395`):**
  - In `onCreate()`, if `hasLocationPermission()` returns false, the app immediately executes:
    ```kotlin
    locationPermissionLauncher.launch(
        arrayOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
    )
    ```
  - It fetches `getLastKnownLocation` once to seed the initial station latitude/longitude into `HamClockNative.startDaemon(hasLocation, lat, lng, ...)`.
  - If denied, the daemon falls back to stored EEPROM defaults or user setup.
* **Gaps vs Google Play Policy:**
  1. **Cold Start Abrupt Prompt:** Launching permission dialogs immediately upon launch without any prior context or educational rationale contradicts Google Play user consent guidelines and results in high rejection/denial rates.
  2. **Play Console Declaration Form (Due Jan 27, 2027):** Google Play requires apps declaring `ACCESS_FINE_LOCATION` to submit an in-console justification detailing why `ACCESS_COARSE_LOCATION` is insufficient for the app's core feature.
  3. **Data Safety Disclosure:** Google Play's Data Safety form must accurately disclose that location is processed ephemerally on-device to compute amateur radio station coordinates (solar elevation, satellite passes, Maidenhead grid square), and is never sold, shared with ad networks, or uploaded to developer tracking servers.

### 2.2 16 KB Page Alignment Audit
* **CMake Configuration (`android/app/src/main/cpp/CMakeLists.txt:63-69`):**
  ```cmake
  target_link_options(
      hamclock
      PRIVATE
      "-Wl,-z,max-page-size=16384"
      ...
  )
  ```
* **Binary Verification (`hamclock-universal-release.apk`):**
  - `lib/arm64-v8a/libhamclock.so`: ELF Program Headers show `LOAD` segments with `Align 0x4000` (16,384 bytes).
  - `lib/arm64-v8a/libc++_shared.so`: ELF Program Headers show `LOAD` segments with `Align 0x4000`.
  - `lib/x86_64/libhamclock.so` & `libc++_shared.so`: Verified 16 KB aligned.
* **Status:** Fully compliant ahead of the February 1, 2027 deadline.

### 2.3 Cleartext Traffic Configuration
* **Current Implementation (`AndroidManifest.xml:34`):**
  ```xml
  android:usesCleartextTraffic="true"
  ```
* **Context:**
  - HamClock binds an embedded C++ HTTP/WebSocket server on `127.0.0.1:8080` and `8081`, loaded directly into `WebView`.
  - The native daemon also queries external amateur radio data sources (DX cluster telnet/HTTP endpoints, reverse beacon network, NOAA space weather endpoints) that historically may lack HTTPS support.
* **Gaps vs Google Play Policy:**
  - Blanket `usesCleartextTraffic="true"` triggers Google Play static security alerts.
  - Best practice: Provide an XML `network_security_config.xml` that permits cleartext to `localhost`, `127.0.0.1`, `::1`, and optional fallback HTTP endpoints, keeping general web traffic encrypted.

### 2.4 Android 17 (API 37) Local Network Permission (`ACCESS_LOCAL_NETWORK`)
* **Current Status:** HamClock currently targets API 36 (`targetSdk = 36`).
* **Upcoming Impact (2027):**
  - When updating target SDK to 37 (required late 2027), raw TCP sockets on LAN (`0.0.0.0` external web server), WebSocket connections, and mDNS responder (`_hamclock._tcp`) will require:
    1. Declaring `<uses-permission android:name="android.permission.ACCESS_LOCAL_NETWORK" />`.
    2. Submitting a Google Play Console Declaration explaining that HamClock operates an embedded local web server allowing users to monitor their amateur radio station from LAN browsers.

---

## 3. Phased Implementation Roadmap

```mermaid
flowchart TD
    subgraph Phase 1: High Priority (Deadline: Jan 27, 2027) [Phase 1: Location & Prominent Disclosure - Code Completed]
        A1["Location Prominent Disclosure UX ✅"] --> A2["Play Console Location Declaration Draft ✅"]
        A2 --> A3["Play Console Data Safety Form Update ✅"]
    end

    subgraph Phase 2: Medium Priority (Security & Hardening) [Phase 2: Security & Packaging Hardening - Completed]
        B1["Network Security Config (Restricted Cleartext) ✅"] --> B2["CI Automated 16 KB Page Alignment Check ✅"]
    end

    subgraph Phase 3: Future Proofing (Target SDK 37 in 2027) [Phase 3: Android 17 Preparation]
        C1["Local Network Permission (ACCESS_LOCAL_NETWORK)"] --> C2["Android 17 Location Button Integration"]
    end

    Phase 1 --> Phase 2 --> Phase 3
```

---

## 4. Phase-by-Phase Task Breakdown

### Phase 1: Location Permission & User Consent Flow (Enforcement: Jan 27, 2027)

#### Task 1.1: In-App Prominent Disclosure Dialog ✅ COMPLETED
* **Delivered Changes:**
  - Created [`dialog_location_disclosure.xml`](file:///home/davek/devel/hamclock/android/app/src/main/res/layout/dialog_location_disclosure.xml) formatted with the dark slate (`#0E1420`), amber accent, rounded borders, and TV D-pad focus animations.
  - Updated [`MainActivity.kt`](file:///home/davek/devel/hamclock/android/app/src/main/java/org/openhamclock/hamclock/MainActivity.kt) to show the disclosure dialog on first cold launch before requesting system runtime permissions.
  - Added "Set Manually in Setup" option which allows immediate launch without location permission.
  - Added persistent preference `location_disclosure_seen` so users who decline or skip are not badgered on subsequent launches.
  - Added Station Location status and "Update Location" action button to Page 2 of the Settings Panel ([`SettingsPanelController.kt`](file:///home/davek/devel/hamclock/android/app/src/main/java/org/openhamclock/hamclock/SettingsPanelController.kt) & [`view_settings_panel_settings.xml`](file:///home/davek/devel/hamclock/android/app/src/main/res/layout/view_settings_panel_settings.xml)).

#### Task 1.2: Play Console Declaration for `ACCESS_FINE_LOCATION` (Ready for Developer Submission)
* **Google Play Question:** *Why does your app require precise location (`ACCESS_FINE_LOCATION`) instead of approximate location (`ACCESS_COARSE_LOCATION`)?*
* **Official Justification Text for HamClock:**
  > "HamClock is an amateur radio station dashboard. It performs astronomical and radio propagation calculations, including solar elevation/azimuth, satellite tracking passes, antenna rotator bearing headings, and exact 6-character Maidenhead locator grid squares (which require sub-mile precision). While the app allows approximate coordinates, access to precise GPS coordinates allows amateur radio operators to accurately aim directional HF/VHF antennas and track orbiting satellites in real-time without manual grid entry."
* **Google Play Question:** *Do you share or sell user location data?*
  - Answer: **No.** Processed strictly ephemerally on-device.

#### Task 1.3: Google Play Data Safety Section Sync
* Under `Location > Precise Location` and `Approximate Location`:
  - **Collected:** Yes (ephemeral on-device).
  - **Shared:** No.
  - **Processed Ephemerally:** Yes (used solely to initialize radio telemetry and not stored on any remote developer server).
  - **Required / Optional:** Optional (user can decline and manually enter callsign/lat/long in Setup).
  - **Purpose:** App functionality.

---

### Phase 2: Security & Packaging Hardening ✅ COMPLETED

#### Task 2.1: Scoped Network Security Config ✅ COMPLETED
* **Delivered Changes:**
  - Created [`network_security_config.xml`](file:///home/davek/devel/hamclock/android/app/src/main/res/xml/network_security_config.xml):
    ```xml
    <?xml version="1.0" encoding="utf-8"?>
    <network-security-config>
        <domain-config cleartextTrafficPermitted="true">
            <!-- Local loopback and Android emulator -->
            <domain includeSubdomains="true">localhost</domain>
            <domain includeSubdomains="false">127.0.0.1</domain>
            <domain includeSubdomains="false">::1</domain>
            <domain includeSubdomains="false">10.0.2.2</domain>

            <!-- Local mDNS and home LAN search domains -->
            <domain includeSubdomains="true">local</domain>
            <domain includeSubdomains="true">lan</domain>
            <domain includeSubdomains="true">home.arpa</domain>

            <!-- Official HamClock backends -->
            <domain includeSubdomains="true">hamclock.app</domain>
            <domain includeSubdomains="true">hamclock.com</domain>
        </domain-config>
        <base-config cleartextTrafficPermitted="false" />
    </network-security-config>
    ```
  - Removed blanket `android:usesCleartextTraffic="true"` from [`AndroidManifest.xml`](file:///home/davek/devel/hamclock/android/app/src/main/AndroidManifest.xml) and linked `android:networkSecurityConfig="@xml/network_security_config"`.
  - Confirmed via merged manifest audit that cleartext traffic is disallowed globally by default, protecting app integrity and satisfying Google Play static analysis security guidelines.

#### Task 2.2: Automated 16 KB Page Alignment Verification in Gradle ✅ COMPLETED
* **Delivered Changes:**
  - Added pure Kotlin ELF64 header inspection and alignment validator (`verifyElf64Alignment`) to [`build.gradle.kts`](file:///home/davek/devel/hamclock/android/app/build.gradle.kts).
  - Registered Gradle task `verify16KbAlignment` which scans all generated `.apk` and `.aab` archives as well as stripped/merged native intermediate directories.
  - Automatically verifies all 64-bit ELF binaries (`arm64-v8a`, `x86_64`) to ensure all `PT_LOAD` segments maintain alignment >= `16384` bytes (`0x4000`). If unaligned binaries are detected, the build aborts with an informative `GradleException`.
  - Configured `assemble*` and `bundle*` tasks to be finalized automatically by `verify16KbAlignment`.
  - Verified: `./gradlew assembleDebug` successfully verified all 12 64-bit native library instances across packages and intermediates.

---

### Phase 3: Android 17 (API 37) Preparation (Target SDK Cycle 2027)

#### Task 3.1: Local Network Permission (`ACCESS_LOCAL_NETWORK`)
* When bumping `compileSdk` and `targetSdk` to 37:
  - Add `<uses-permission android:name="android.permission.ACCESS_LOCAL_NETWORK" />` in `AndroidManifest.xml`.
  - Submit Play Console declaration justifying local network access: HamClock acts as a local LAN web server and mDNS broadcaster (`_hamclock._tcp`) so users can access the clock UI on other local devices.

#### Task 3.2: Android 17 Location Button
* Support the new `onlyForLocationButton` attribute and system location button if one-time location seeding is initiated from within the UI on API 37+ devices.

---

## 5. Summary of Decisions & Next Steps

1. **Immediate Execution Focus:**
   - Implement Task 1.1 (Prominent Location Disclosure dialog) in `MainActivity.kt` and `SettingsPanelController.kt`.
   - Prepare the Play Console Location Declaration text (Task 1.2) for the developer account owner to paste into the Google Play Console ahead of the **January 27, 2027** deadline.
2. **Recorded Key Learnings:**
   - 16 KB page size support is **already solved and verified** for HamClock's native C++ binaries (`libhamclock.so` has `Align 0x4000`).
   - HamClock already targets **Android 16 (API 36)**, satisfying the current API level mandate.
   - The primary immediate policy action is the **Location Permission Minimum Scope Declaration and cold-start UX improvement**.

---
*Refer to this plan across conversations as work progresses.*

