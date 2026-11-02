# HamClock Technical Blueprint & Architecture Specification

> **Superseded.** This spec describes a native re-implementation of HamClock for Vega. The app as built takes a different approach: the HamClock engine runs inside the app (`libHcEngine.so`) and its own `live.html` page is shown in a WebView. Where this file differs, `PHASE0_RESULTS.md` … `PHASE7_RESULTS.md` (latest wins), [PLAN.md](PLAN.md) and [README.md](README.md) describe the actual design. The REST/command details and HamClock behavior below are still a useful reference.

This specification documents the complete application architecture, API surface, state machines, and input processing logic extracted from the HamClock native and mobile engine. It is formatted strictly as a platform-agnostic, universal technical specification for re-implementation on **Amazon Vega OS** (Linux/Web runtime).

---

## 1. System Topology & Architecture Overview

HamClock consists of a decoupled two-tier client-server architecture designed to run on resource-constrained embedded and smart display hardware:

```
+-----------------------------------------------------------------------------------+
|                              Vega OS / Host Platform                             |
+-----------------------------------------------------------------------------------+
|                                                                                   |
|  +-------------------------------------+   +-----------------------------------+  |
|  |     Frontend Presentation Layer     |   |      Native Daemon Core Engine    |  |
|  |   (HTML5 Canvas / WebSocket / JS)   |   |        (C++ HamClock Daemon)      |  |
|  +-------------------------------------+   +-----------------------------------+  |
|                     |                                        |                    |
|                     | [WebSocket: :8081 /live-ws]            |                    |
|                     |<-------------------------------------->|                    |
|                     | Updates, Touches, Cursors, Keys        |                    |
|                     |                                        |                    |
|  +-------------------------------------+                     |                    |
|  |      External REST Consumers        |   [REST HTTP: :8080]|                    |
|  | (Dashboard, Rotators, Web Browsers) |<------------------->|                    |
|  +-------------------------------------+                     |                    |
|                                                              |                    |
|                                        +---------------------------------------+  |
|                                        |        Host System Supervisor         |  |
|                                        | (Process Lifecycles, Network & Ports) |  |
|                                        +---------------------------------------+  |
|                                                              |                    |
|                                           HTTP Pipe Delegate | (External APIs)    |
|                                                              v                    |
|                                        +---------------------------------------+  |
|                                        |       Internet Services / Feeds       |  |
|                                        |    (NOAA, SWPC, VOACAP, DX Cluster)   |  |
|                                        +---------------------------------------+  |
+-----------------------------------------------------------------------------------+
```

### Core Architecture Components

1. **Native Core Daemon (Backend):**
   - Headless C++ application compiled with `-D_WEB_ONLY` and `-D_CLOCK_1600x960` (or 800x480 native coordinate space).
   - Manages non-volatile memory (EEPROM emulation), astro/solar/lunar calculations, map projections, VOACAP propagation matrices, DX spots, satellite tracking, and hardware interfacing.
   - Operates a local multi-threaded HTTP/WebSocket server.
2. **Frontend Presentation Client:**
   - Single-page web application (`live.html`) loaded via a local viewport or Chromium-based webview.
   - Renders a zero-alpha HTML5 2D canvas displaying initial PNG frames followed by dirty-region sprite patches over a WebSocket.
   - Contains a virtual cursor engine for remote control (D-Pad) navigation and an in-page modal iframe overlay (`#embed-overlay`) for external web resources.
3. **Host Supervisor Layer:**
   - Manages daemon process spawning, crash recovery, dynamic port negotiation, mDNS/DNS-SD network advertisement, WiFi power locks, and host location injection.

---

## 2. API Endpoints & Data Schemas

The application exposes two networking server interfaces:
- **Live Interactive WebSocket/HTTP Server** (Default RW Port: `8081`, RO Port: `8082`)
- **RESTful Management API Server** (Default Port: `8080`)

---

### 2.1 Live Interactive WebSocket Interface (`/live-ws`)

The interactive UI communicates bidirectionally over a persistent binary/text WebSocket connection established at `ws://<host>:<rw_port>/live-ws`.

#### A. Client-to-Server Messages

| Command Format | Parameters | Purpose |
| :--- | :--- | :--- |
| `get_live.png?` | *None* | Requests an immediate full-frame 800x480 (or 1600x960) PNG screen capture. Sent on connection init and window resize. |
| `get_live.bin?` | *None* | Requests incremental dirty-rectangle update since the client's last acknowledged frame. Polled continuously at ~100ms intervals. |
| `set_touch?x=<X>&y=<Y>&button=<B>` | `x`: Integer `[0..799]`<br>`y`: Integer `[0..479]`<br>`button`: `0` (Primary Tap) or `1` (Secondary/Long-Press) | Injects touch/click events in native coordinate space. |
| `set_mouse?x=<X>&y=<Y>` | `x`: Integer `[-1..799]`<br>`y`: Integer `[-1..479]` | Injects hover/cursor coordinates for map crosshairs and tooltips. `(-1,-1)` indicates pointer exit. |
| `set_char?char=<C>&mod=<M>` | `char`: Literal, named (`Escape`, `Enter`, `Tab`, `Backspace`, `Delete`, `Space`, `ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`), or `0xHEX`<br>`mod`: `C` (Ctrl), `S` (Shift), or empty | Injects keyboard input into the core text queue without URL query syntax collisions. |

#### B. Server-to-Client Messages

1. **Full Frame Capture (Binary PNG):**
   - Transmitted as a binary WebSocket frame starting with PNG magic header (`0x89, 0x50, 0x4E, 0x47`). Renders directly to canvas context.
2. **Delta Block Patch (Binary Header + Sprite Sheet):**
   - Transmitted as an array buffer containing a 4-byte header followed by dirty region definitions, followed by a PNG sprite strip:
     ```
     Offset 0: blok_w     (uint8)  - Block width in pixels (typically 10px)
     Offset 1: blok_h     (uint8)  - Block height in pixels (typically 10px)
     Offset 2: n_regn_msb (uint8)  - High byte of region count
     Offset 3: n_regn_lsb (uint8)  - Low byte of region count
     Followed by n_regn triplets:
       Byte 4 + 3*i + 0: cvs_x_blocks (uint8) - Grid X position (multiply by blok_w)
       Byte 4 + 3*i + 1: cvs_y_blocks (uint8) - Grid Y position (multiply by blok_h)
       Byte 4 + 3*i + 2: n_long       (uint8) - Number of contiguous horizontal blocks
     ```
   - Accompanied by a concatenated horizontal PNG image strip containing only the updated pixel blocks.
3. **Control Directives (UTF-8 Text Frames):**
   - `full-screen`: Commands frontend to engage fullscreen display mode upon user interaction.
   - `open <url>`: Directs host to open target URL in external browser/tab.
   - `embed <url>`: Directs frontend to display target URL in the `#embed-overlay` modal dialog.
   - `paste`: Requests host/browser clipboard contents to be sent back via `set_char`.
   - `Session timed out`: Disconnects idle clients; signals client **not** to auto-reconnect.
   - `Too many connections`: Informs client the connection limit has been reached.

---

### 2.2 RESTful API Server (Port `8080`)

All REST endpoints support HTTP `GET` (and select `POST` payloads). Responses are plain text (`text/plain; charset=us-ascii`) using newline-separated, fixed-column key-value schemas unless otherwise noted.

#### A. Telemetry & Information Query Endpoints (GET)

| Endpoint | Response Format & Key Schema Fields |
| :--- | :--- |
| `GET /get_config.txt` | Key-value lines:<br>`Screen`: `locked` \| `unlocked`<br>`MapStyles`: Core map projection styles and active rotation period<br>`MapProj`: Projection name (`Azimuthal`, `Mercator`, etc.)<br>`MapGrid`: Grid overlay style (`None`, `Maidenhead`, `Lat/Long`)<br>`MerCenter`: Center longitude (`deg`)<br>`Pane[0-3]`: Active data pane choices and rotation schedule<br>`LiveSpots`: Active spot source filter (`PSK`, `WSPR`, `RBN`)<br>`NCDXF`: Selected beacon monitoring mode and rotation timer<br>`Bright`: `min <= current <= max %`<br>`AutoDim`: Inactivity dim timeout and countdown<br>`OnOff_<Day>`: `HH:MM HH:MM` on/off timers for Sunday–Saturday |
| `GET /get_sys.txt` | `Version`: Core semver<br>`BuildSz`: `<Width> x <Height>`<br>`Platform`: Host platform identifier<br>`BEHost` / `BEPort`: Backend cloud host and port<br>`MACaddr`: MAC address in hex notation<br>`UpTime`: Duration formatted as `<days>d<HH>:<MM>:<SS>`<br>`EEPROM`: `<used> used of <total> bytes`<br>`NTP`: Server address, round-trip latency (`ms`) or error<br>`Debugs`: Subsystem log verbosity levels |
| `GET /get_de.txt`<br>`GET /get_dx.txt` | Information schema for DE (Transmitter/Host) and DX (Target) locations:<br>`time`: ISO 8601 string (`YYYY-MM-DDTHH:MM:SS`)<br>`tz`: Offset (`UTC+X`)<br>`lat` / `lng`: Decimal coordinates<br>`grid`: 4- or 6-character Maidenhead locator<br>`MoonAz` / `MoonEl`: Moon azimuth and elevation in degrees<br>`MoonRise` / `MoonSet`: Times (`HH:MM`) or `none`<br>`MoonVel`: Velocity (`m/s`)<br>`SunAz` / `SunEl`: Solar azimuth and elevation<br>`SunRise` / `SunSet`: Times (`HH:MM`) or `none`<br>`WxTemp`: Decimal temperature with `C` or `F`<br>`WxPressure`: Atmospheric pressure (`hPa` or `inHg`) with trend indicator (`rising`, `falling`, `steady`)<br>`WxHumidity`: Relative percentage<br>`WxWindSpd` / `WxWindDir`: Speed (`kph`/`mph`) and cardinal direction<br>`WxClouds` / `WxCondx`: Weather description strings<br>`WxFrom`: Attribution source |
| `GET /get_time.txt` | Current clock state:<br>`UTC`: ISO 8601 UTC timestamp<br>`Local`: ISO 8601 host local timestamp<br>`Offset`: Timezone offset in seconds<br>`JD`: Julian Date |
| `GET /get_spacewx.txt` | Live space weather indices:<br>`SSN`: Sunspot Number<br>`SFI`: Solar Flux Index<br>`Kp`: Planetary K-index<br>`Ap`: Planetary A-index<br>`XRay`: X-ray solar flare flux status (`A`, `B`, `C`, `M`, `X`)<br>`SolarWind`: Speed (`km/s`) and proton density |
| `GET /get_voacap.txt` | VOACAP HF band propagation matrix: Table containing band frequencies (`80m`–`10m`), reliability percentages (`0–100%`), and predicted SNR dB for current DE-to-DX path. |
| `GET /get_satellite.txt` | Current tracked satellite telemetry:<br>`Name`: Satellite identifier<br>`Az` / `El`: Real-time tracking azimuth and elevation (`deg`)<br>`Range` / `Rate`: Distance (`km`) and radial velocity (`km/s`)<br>`NextPass`: Rise time, set time, max elevation, and duration |
| `GET /get_satellites.txt` | Complete list of all loaded satellite names in local cache. |
| `GET /get_dxspots.txt` | Space-separated tabular feed of amateur radio DX cluster spots:<br>`# kHz Call UTC Mode Grid Lat Lng DEDist DEBearing` |
| `GET /get_livespots.txt` | Current spots from live sources (WSPR/PSK/RBN) matching station configuration. |
| `GET /get_livestats.txt` | Spot count summary grouped by HF band and continent. |
| `GET /get_ontheair.txt` | Live Parks on the Air (POTA) and Summits on the Air (SOTA) spot activations. |
| `GET /get_contests.txt` | Upcoming amateur radio contest calendar entries. |
| `GET /get_dxpeds.txt` | Scheduled DXpeditions with dates and operating bands. |
| `GET /get_aprs.txt` | APRS-IS packets received nearby:<br>`# connected <yes|no> dist_unit <km|mi> stations <count>`<br>`# Call Lat Lng Dist Bearing BearType Symbol Tbl Code AgeSec Course SpdMph AltFt TempF Hum WindMph GustMph BaroMb Comment` |
| `GET /get_sensors.txt` | Local telemetry from I2C/BME280/BMP280 environment sensors. |
| `GET /get_eclipse.txt` | Solar eclipse local circumstances and countdown timer. |
| `GET /get_antennas.txt` | Selected antenna profiles, azimuth offsets, and beam headings. |
| `GET /list_antennas.txt` | Complete list of configured antennas and index descriptors. |
| `GET /get_capture.bmp` | Full screen capture returned as binary BMP image payload (`image/bmp`). |
| `GET /get_gpio?pin=<MCP>&latched=<bool>` | Queries hardware GPIO pin states. |

#### B. Control & Configuration Endpoints (GET / POST)

| Command Endpoint | Query Parameters / Payload | Description & Execution Logic |
| :--- | :--- | :--- |
| `GET /set_newde?...` | `grid=<AB12>`, `lat=<X>`, `lng=<Y>`, `call=<CALL>` | Updates DE (home station) callsign, Maidenhead grid, and coordinates in EEPROM. |
| `GET /set_newdx?...` | `grid=<AB12>`, `lat=<X>`, `lng=<Y>` | Sets DX target coordinates and triggers recalculation of Great Circle path. |
| `GET /set_pane?...` | `Pane[0-3]=<Choice1,Choice2...>` | Reassigns the display modules assigned to data panes 0, 1, 2, or 3. |
| `GET /set_mapview?...` | `Style=<S>&Grid=<G>&Projection=<P>&RSS=<on\|off>&Night=<on\|off>` | Updates map visual presentation parameters. |
| `GET /set_mapcenter?...`| `lng=<X>` | Shifts Mercator center meridian to longitude `X`. |
| `GET /set_mapcolor?...`  | `setup=<name>&color=<R,G,B>` | Customizes feature rendering colors. |
| `GET /set_panzoom?...`   | `pan_x=<X>&pan_y=<Y>&zoom=<Z>` | Adjusts viewport pan offsets and zoom level. |
| `GET /set_time?...`      | `change=<delta>` \| `ISO=<timestamp>` \| `unix=<epoch>` \| `Now` | Adjusts or overrides internal reference time. |
| `GET /set_alarm?...`     | `state=<off\|armed>&time=<HH:MM>&utc=<yes\|no>` | Configures daily alarm clock. |
| `GET /set_once_alarm?...`| `state=<off\|armed>&time=<YYYY-MM-DDTHH:MM>&tz=<DE\|UTC>` | Schedules single-event alert. |
| `GET /set_stopwatch?...` | `reset` \| `run` \| `stop` \| `lap` \| `countdown=<mins>` | Controls stopwatch/timer subsystem. |
| `GET /set_title?...`     | `call\|title\|onair=[text]&fg=<R,G,B>&bg=<R,G,B\|rainbow>` | Modifies station banner text and palette. |
| `GET /set_displayOnOff?` | `on` \| `off` | Manually toggles screen sleep/standby mode. |
| `GET /set_displayTimes?` | `on=<HH:MM>&off=<HH:MM>&day=[Sun..Sat]&idle=<mins>` | Sets auto-sleep schedule and inactivity dimming. |
| `GET /set_screenlock?...`| `lock=<on\|off>` | Locks or unlocks touch screen input. |
| `GET /set_rotator?...`   | `state=[un]stop\|[un]auto&az=<X>&el=<Y>` | Commands antenna rotator azimuth/elevation positioning. |
| `GET /set_satname?...`   | `<satellite_name>` \| `none` | Selects active satellite for tracking and orbital overlay. |
| `GET /set_sattle?...`    | `name=<N>&t1=<LINE1>&t2=<LINE2>` | Injects custom 2-line NORAD orbital elements (TLEs). |
| `GET /set_cluster?...`   | `host=<host>&port=<port>` | Updates DX cluster TCP telnet server target. |
| `GET /set_voacap?...`    | `band=<B>&power=<W>&tz=<DE\|UTC>&mode=<M>` | Reconfigures VOACAP simulation parameters. |
| `GET /set_antennas?...`  | `antdeindex=<n>&antdxindex=<n>&antdeaz=<X>` | Updates antenna switch matrix and rotators. |
| `GET /set_touch?...`     | `x=<X>&y=<Y>` | Simulates touch tap at given coordinates via REST API. |
| `POST /set_rss?...`      | `reset` \| `add=<item>` \| `network` \| `interval=<s>` (POST body: text/rss) | Updates news ticker headlines. |
| `POST /set_adif?...`     | `pane=[0-3]` (POST body: ADIF text) | Uploads Amateur Data Interchange Format log records. |
| `POST /set_bmp?...`      | `pane=[1-3,map]&fit=[resize,crop,fill]` (POST body: BMP image) | Injects static graphic into pane or map background. |
| `GET /restart`           | *None* | Reboots the core daemon cleanly. |
| `GET /exit`              | *None* | Shuts down the core daemon process. |
| `GET /updateVersion`     | *None* | Triggers OTA software update check. |

---

## 3. Global State Machine & Process Lifecycles

### 3.1 Daemon Execution & State Transition Matrix

The core backend daemon operates through the following discrete operational states:

```
+---------------+
|    STOPPED    |
+---------------+
        |  startDaemon()
        v
+---------------+      Missing Callsign / forceSetup
|  INITIALIZING |----------------------------------------+
+---------------+                                        |
        |  Valid Config (-k)                             |
        v                                                v
+---------------+    10s Timeout / Virtual Tap    +---------------+
| BOOT_COUNTDOWN|-------------------------------->| SETUP_WIZARD  |
+---------------+                                 +---------------+
        |                                                |
        | Countdown completes                            | User saves setup
        +-----------------------+------------------------+
                                |
                                v
                        +---------------+
                        | CLOCK_RUNNING |<-------+
                        +---------------+        |
                         |      |       |        | Config change / Host update
             User Exits  |      |       +--------+
                         |      |
                         |      | Restart requested (minusK)
                         |      v
                         |  +---------------+
                         |  |  RESTARTING   |
                         |  +---------------+
                         v
                +---------------+
                |  TERMINATED   |
                +---------------+
```

#### Detailed State Specifications

1. **`STOPPED`:**
   - Host supervisor ensures no lingering processes remain bound to application ports.
   - Filesystem verification: Ensures `hamclock_data/` directory exists and checks for `eeprom` file integrity.
2. **`INITIALIZING`:**
   - Non-volatile EEPROM cookie check: Reads byte at `0x0EF`. If value matches `0x5A`, preferences are valid. Callsign characters reside at `0x0F0..0x0FB`.
   - MAC address verification: Reads persistent MAC from `.mac_address`. If absent, generates locally administered unicast MAC:
     $$\text{Byte 0} = (\text{random} \ \& \ 0xFE) \ | \ 0x02$$
     Saves string formatted as `XX:XX:XX:XX:XX:XX`.
   - Host location injection: If host GPS/GNSS is enabled, writes normalized coordinates to `NV_DE_LAT`, `NV_DE_LNG`, Maidenhead grid to `NV_DE_GRID`, and calculates local timezone offset.
   - NTP default: If unset in NV memory, defaults NTP provider to OS system clock (`NTPSC_OS`).
3. **`BOOT_COUNTDOWN` vs. `SETUP_WIZARD`:**
   - **Normal Boot:** Launched with flag `-k`. Skips 10-second initial countdown and boots directly into `CLOCK_RUNNING`.
   - **First Run (Unconfigured):** Callsign absent. Flag `-k` omitted; flag `-g` (GeoIP) supplied. Daemon displays 10-second countdown.
   - **Direct Setup Mode (`forceSetup=true`):** Flag `-k` omitted. An internal injector worker thread polls for `displayReady()`, waits 150ms, then injects a synthetic touch event (`wifi_tt = TT_TAP` at `X=200, Y=200`) to force immediate entry into the configuration wizard.
4. **`CLOCK_RUNNING`:**
   - Main real-time event loop active.
   - Background threads service sensor polling, satellite propagation orbits, space weather fetch routines, and client WebSocket frames.
5. **`RESTARTING`:**
   - Initiated internally by the daemon via `android_request_restart(bool minus_K)` or REST `/restart`.
   - If `minus_K == false`, sets flag to enter countdown on relaunch.
   - Supervisor executes clean handoff: spawns detached supervisor process, halts existing runtime, releases network sockets, and re-executes daemon binary.
6. **`TERMINATED`:**
   - Initiated via UI exit or REST `/exit`. Closes listening sockets, unlinks temporary FIFOs, flushes EEPROM state to disk, and terminates host process.

---

### 3.2 Network Port Discovery & Allocation Ladder

To guarantee collision-free operation when running beside other services on the host:

```
               Check Preferred Port
               (RW: 8081, RO: 8082, REST: 8080)
                       |
            +----------+----------+
            | Available?          |
           YES                    NO
            |                     |
      Bind Sockets                Kill orphaned daemon PIDs
                                  Wait 250ms & re-test
                                          |
                               +----------+----------+
                               | Freed?              |
                              YES                    NO
                               |                     |
                         Bind Sockets       Traverse Fallback Ladder
                                            RW:   [8083, 8084, 8086, 8091]
                                            RO:   [8085, 8087, 8092, 8093]
                                            REST: [8088, 8085, 8089, 8090]
                                                     |
                                            Bind first open port
                                            (or disable REST if exhausted)
```

---

### 3.3 Network Access Filtering & mDNS Service Discovery

1. **Loopback vs. LAN Security Filter:**
   - Incoming socket connections (`accept` / `accept4`) are validated against an IP filter:
     - **Default Mode:** Strictly permits loopback traffic (`127.0.0.0/8` and `::1`). External connection attempts are terminated immediately with `ECONNABORTED`.
     - **External LAN Mode:** Permits RFC 1918 private IPv4 networks (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`) and IPv6 Link-Local/ULA (`fe80::/10`, `fc00::/7`).
2. **mDNS & DNS-SD Advertisement:**
   - **DNS-SD Registration:** Registers service `_http._tcp` with configured hostname on port `actualRwPort`.
   - **A-Record Multicast Responder:** Dedicated UDP socket bound to `224.0.0.251:5353` directly resolving queries for `<hostname>.local` to the host's active IPv4 interface address.

---

### 3.4 Native HTTP Network Pipe Delegate

To eliminate native TLS library deprecation and trust-store synchronization issues on embedded Linux distributions, external HTTPS network requests from the C++ core engine are piped through a host delegate:

```
[Native C++ Engine]                            [Host Runtime Delegate]
         |                                                 |
         | 1. Dispatches fetch request                     |
         |    (URL, User-Agent, Headers, write_fd)         |
         |------------------------------------------------>|
         |                                                 |
         |                                                 | 2. Initiates native HTTPS
         |                                                 |    connection via host TLS
         |                                                 |
         |                                                 | 3. Pipes raw byte stream
         | 4. Reads streamed response from read_fd         |    directly into write_fd
         |<================================================|
         |                                                 |
         | 5. Processed in memory                          | 6. Closes write_fd
```

---

## 4. User Interaction & Input Dispatch Logic

### 4.1 Input Mode Matrix & Coordinate Transformation

The display layout operates on a fixed logical canvas resolution:
- **Base Logical Space:** $800 \times 480$ pixels ($W \times H$).
- **High-DPI 2x Logical Space:** $1600 \times 960$ pixels.

#### Coordinate Translation Formula
For any physical pointer or touch event at window viewport coordinates $(X_{client}, Y_{client})$:

$$\text{scale} = \frac{\text{canvas.clientWidth}}{800}$$

$$X_{app} = \text{clamp}\left(\left\lfloor \frac{X_{client} - \text{rect.left}}{\text{scale}} \right\rfloor, 0, 799\right)$$

$$Y_{app} = \text{clamp}\left(\left\lfloor \frac{Y_{client} - \text{rect.top}}{\text{scale}} \right\rfloor, 0, 479\right)$$

---

### 4.2 Direct Touch & Pointer State Machine

```
              +--------------+
              |     IDLE     |
              +--------------+
                     |
                     | pointerdown (X, Y)
                     v
              +--------------+
              | POINTER_DOWN |
              +--------------+
                |          |
   Moves > 5px  |          | Timer reaches 500ms
 (cancel timer) |          | (no movement)
                v          v
+------------------+     +------------------------+
| DRAGGING / HOVER |     |  LONG_PRESS_TRIGGERED  |
+------------------+     +------------------------+
        |                            |
        | pointerup                  | pointerup
        v                            v
+------------------+     +------------------------+
| Discard Touch    |     | Discard (already fired)|
+------------------+     +------------------------+
        |
        | pointerup without movement (< 5px)
        v
+------------------------------------+
| Dispatch Primary / Secondary Touch |
| Button 0: Primary Tap              |
| Button 1: Right/Ctrl-Tap (Tooltip) |
+------------------------------------+
```

1. **`pointerdown`:**
   - Records starting coordinate $(X_0, Y_0)$ and timestamp $T_0$.
   - Arm 500ms long-press timer (`LONGPRESS_MS = 500`).
   - Hides virtual cursor if pointer type is direct touch or stylus.
2. **`pointermove`:**
   - Calculates Euclidean jitter:
     $$\Delta = \max(|X - X_0|, |Y - Y_0|)$$
   - If $\Delta > 5\text{px}$, cancels long-press timer.
   - Throttles mouse hover updates to 100ms intervals; dispatches `set_mouse?x=X&y=Y`.
3. **`pointerup`:**
   - Cancels long-press timer.
   - If long-press has already fired: resets state and exits.
   - If $\Delta \le 5\text{px}$:
     - Evaluates button classification:
       $$\text{button} = \begin{cases} 1 & \text{if } (\text{button} = 0 \text{ with Ctrl/Meta}) \lor \text{button} \in \{1, 2\} \\ 0 & \text{otherwise} \end{cases}$$
     - Dispatches `set_touch?x=X&y=Y&button=<button>`.
4. **Long-Press Expiry (500ms):**
   - Sets `longpress_fired = true`.
   - Dispatches `set_touch?x=X_0&y=Y_0&button=1` to open context tooltips and info panes.

---

### 4.3 Virtual Cursor & TV Remote (D-Pad) Navigation

For television and smart display interfaces lacking direct touch input, HamClock implements an on-screen accelerated virtual cursor:

```
               [DPAD Direction Pressed]
                          |
             +------------+------------+
             | Is Cursor Visible?      |
            NO                        YES
             |                         |
      Wake Cursor in place             |
      (No positional shift)            |
                                       v
                     +----------------------------------+
                     | Accelerate Step Size:            |
                     | Repeat interval < 200ms:         |
                     |   n > 30: Step = 24px            |
                     |   n > 18: Step = 16px            |
                     |   n > 8:  Step = 10px            |
                     |   Else:   Step = 6px             |
                     +----------------------------------+
                                       |
                                       v
                     +----------------------------------+
                     | Update (X, Y) clamped to bounds  |
                     | Render DOM hardware transform    |
                     | Send set_mouse?x=X&y=Y           |
                     | Reset 12-second hide timer       |
                     +----------------------------------+
                                       |
           +---------------------------+---------------------------+
           | Target: Bottom-Right Corner                           | Target: Standard Coordinates
           v                                                       v
+-------------------------------+                       +-------------------------------+
| Hand focus to Native Settings |                       | Retain virtual cursor focus   |
+-------------------------------+                       +-------------------------------+
```

#### D-Pad Center / Enter Key Logic:
1. **Key Down:**
   - Starts 500ms timer.
2. **Long-Press Threshold (> 500ms):**
   - Aborts touch dispatch.
   - Triggers native Settings/Configuration Modal.
3. **Key Up (< 500ms):**
   - If cursor was previously hidden: wakes cursor to visible state (prevents blind clicks).
   - If cursor is hovering over settings button: launches settings dialog.
   - Otherwise: triggers cursor shrink animation (`scale(0.85)` for 120ms) and dispatches `set_touch?x=X&y=Y&button=0`.
4. **Inactivity Timeout:**
   - Auto-hides virtual cursor element after 12 seconds of D-Pad inactivity.

---

### 4.4 Modal In-Page Overlay (`#embed-overlay`)

When an interactive badge or live web element (e.g., ADSB exchange, satellite map, QRZ bio) is invoked:

1. Daemon transmits WebSocket directive: `embed <url>`.
2. Frontend updates iframe DOM:
   - Sets `#embed-frame.src = url`.
   - Displays `#embed-overlay` with a semi-transparent backdrop over the canvas.
   - Directs input focus to the modal's Close button.
3. Fallback Detection:
   - Starts a 4-second timer. If site framing is blocked by target `X-Frame-Options` or `Content-Security-Policy: frame-ancestors`, displays a visible banner:
     *"Taking a while to load? This site may not allow embedding -- try 'Open in new tab' above."*
4. Dismissal Flow:
   - Triggered by `Escape` key, Remote Back button, or Close button click.
   - Clears iframe src to `about:blank` (terminates background JS/audio).
   - Hides modal container and returns focus to main clock canvas.

---

### 4.5 Keyboard & Clipboard Ingestion Flow

1. **Hardware Keyboard:**
   - Characters are mapped and filtered to eliminate collision with HTTP query parsing:
     - Single ASCII characters are encoded as hex: `'A'` $\rightarrow$ `0x41`, `'='` $\rightarrow$ `0x3D`.
     - Standard navigational keys are passed as named identifiers: `Escape`, `Enter`, `Tab`, `Backspace`, `Delete`, `Space`, `ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`.
     - Keypad separators (`Decimal`, `Separator`) are normalized to `'.'`.
     - Dispatched as `set_char?char=<val>&mod=<C><S>`.
2. **Clipboard Paste:**
   - When user triggers paste or daemon emits `paste` text frame:
     - Accesses system clipboard via `navigator.clipboard.readText()`.
     - Serializes characters sequentially across the input queue via `sendKey()`.

---

## 5. Amazon Vega OS Migration Blueprint

This section provides the architectural mapping for implementing HamClock on **Amazon Vega OS** (replacing Android/AOSP dependencies with clean Linux standards).

```
+-----------------------------------+---------------------------------------+
| Android / AOSP Implementation     | Amazon Vega OS Migration Pattern      |
+-----------------------------------+---------------------------------------+
| Java/Kotlin Activity & NDK JNI    | Native C++ systemd daemon + Web View  |
| Android WebView                   | Chromium / Blink Web Application View |
| android.net.nsd.NsdManager        | Avahi daemon (avahi-client / D-Bus)   |
| WifiManager.WifiLock              | Linux systemd-logind inhibitor locks  |
| SharedPreferences                 | JSON / INI file in user data directory|
| BootReceiver (BOOT_COMPLETED)     | systemd user service (WantedBy=graphical)
| RestartActivity (:restart process)| systemctl --user restart hamclock.service
| HttpURLConnection pipe fallback   | libcurl with system ca-certificates   |
+-----------------------------------+---------------------------------------+
```

### Component Implementation Tasks for Vega OS

1. **Core Process Supervision:**
   - Compile native HamClock C++ engine as a native Linux binary linked against `libcurl`, `libpng`, `zlib`, and `pthread`.
   - Package as a standard system service managed via systemd:
     ```ini
     [Unit]
     Description=HamClock Core Daemon
     After=network-online.target
     Wants=network-online.target

     [Service]
     Type=simple
     ExecStart=/usr/bin/hamclock -d /var/lib/hamclock -w 8081 -r 8082 -e 8080 -t 80 -k
     Restart=on-failure
     RestartSec=2s

     [Install]
     WantedBy=default.target
     ```
2. **Frontend Presentation Shell:**
   - Instantiate a fullscreen, accelerated Web App container loading `http://127.0.0.1:8081/live.html`.
   - Ensure the Web engine supports HTML5 2D Canvas Hardware Acceleration and WebSockets.
3. **Network Discovery & mDNS:**
   - Replace Android's `MdnsResponder` and `NsdManager` with native Linux **Avahi**:
     - Publish `_http._tcp` service announcing port `8081`.
     - Ensure system `nss-mdns` is active to resolve `<hostname>.local`.
4. **Display Standby & Sleep Prevention:**
   - Replace `WAKE_LOCK` with a standard D-Bus `org.freedesktop.ScreenSaver` inhibit lock or Wayland idle inhibitor protocol.
5. **Direct HTTPS Execution:**
   - Replace the Android JNI `android_connect_http` pipe mechanism with native `libcurl` calls using standard system OpenSSL root certificates.
6. **Input Binding:**
   - Bind TV remote keycodes (Vega OS D-Pad and Media keys) directly to the corresponding DOM keyboard events (`ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`, `Enter`, `Escape`) to leverage the existing virtual cursor logic.

---

### Verification Checklist for Implementation

- [ ] Core daemon starts on loopback `127.0.0.1` and binds ports `8080`, `8081`, and `8082`.
- [ ] Initial connection receives full binary PNG frame followed by delta sprite patches.
- [ ] Touch / pointer coordinates map accurately to $800 \times 480$ logical boundaries.
- [ ] 500ms long press activates secondary button (tooltip inspector).
- [ ] D-Pad navigation wakes and moves virtual cursor with multi-stage acceleration.
- [ ] In-page iframe modal opens on `embed <url>` and closes cleanly on `Escape` or Back.
- [ ] REST API responds to queries on port `8080` with newline-delimited key-value data.
- [ ] Host GNSS coordinates successfully write to non-volatile memory on first boot.
