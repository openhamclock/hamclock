# Running HamClock on Raspberry Pi & Debian

Depending on your platform and setup, choose the installation method that best fits your environment:

### For Dedicated Raspberry Pi Hardware:
* **[Approach 1: Pre-built Flashable Images (Raspberry Pi Only — Recommended)](#approach-1-pre-built-flashable-images-raspberry-pi-only)** — Flash a ready-to-run SD card image and boot; no terminal commands or manual setup required.

### For Debian PCs, Servers, or Existing Raspberry Pi OS Installs:
* **[Approach 2: Automated Installation Script (Existing Raspberry Pi OS & Debian)](#approach-2-automated-installation-script-existing-raspberry-pi-os--debian)** — Run the helper script on an existing system to automatically install dependencies, detect display resolution, compile, and configure autostart.
* **[Approach 3: Building from Source (Raspberry Pi OS & Debian)](#approach-3-building-from-source-raspberry-pi-os--debian)** — Compile manually using `make` for full control over build targets and options.

---

## Approach 1: Pre-built Flashable Images (Raspberry Pi Only)

> [!NOTE]
> Pre-built disk images are designed specifically for Raspberry Pi hardware (Pi 3, Pi 4, Pi 5, Pi Zero 2 W). If you are running standard Debian on a PC or laptop, or want to install HamClock alongside other software on an existing operating system, skip to **[Approach 2](#approach-2-automated-installation-script-existing-raspberry-pi-os--debian)** or **[Approach 3](#approach-3-building-from-source)** below.

The easiest and fastest way to get HamClock running on a dedicated Raspberry Pi is using the official pre-built disk images from the **[hamclock-pi-image](https://github.com/openhamclock/hamclock-pi-image)** project.

* **GitHub Repository:** [openhamclock/hamclock-pi-image](https://github.com/openhamclock/hamclock-pi-image)
* **Latest Release:** [Download Flashable Images](https://github.com/openhamclock/hamclock-pi-image/releases) (e.g. [Release `hamclock-images-2026-10-08-27`](https://github.com/openhamclock/hamclock-pi-image/releases/tag/hamclock-images-2026-10-08-27))

These images come with Raspberry Pi OS and HamClock completely pre-installed, pre-configured, and managed as an auto-restarting systemd service.

### Choosing an Image Variant

Images are published for both **Trixie** (Debian 13 testing / modern Raspberry Pi OS) and **Bookworm** (Debian 12), across three display variants:

| Variant | Display Mode | Screen Required? | Best For |
|---|---|---|---|
| **`desktop`** | Full X11 desktop; HamClock autostarts fullscreen | Yes (HDMI) | Pi 4 / Pi 5 with monitor & mouse. Clickable web links open in a full browser. |
| **`web`** | Headless daemon; access HamClock from any browser on your LAN | No | Pi Zero 2 W, home servers, or running without a physical monitor. |
| **`fb0`** | Direct Linux framebuffer (`/dev/fb0`); no desktop overhead | Yes (HDMI) | Dedicated always-on kiosk displays on lower-RAM boards (Zero 2 W / Pi 3). |

Images are provided at multiple resolutions: **800x480**, **1600x960**, **2400x1440**, and **3200x1920**.

### How to Flash and Boot

1. Download the `.img.xz` file for your desired variant and resolution from the [Releases page](https://github.com/openhamclock/hamclock-pi-image/releases).
2. Open **[Raspberry Pi Imager](https://www.raspberrypi.com/software/)**:
   - **Choose Device:** Select your Raspberry Pi model (Pi 4, Pi 5, Pi Zero 2 W, etc.).
   - **Choose OS:** Scroll down and choose **Use custom**, then select the downloaded `.img.xz` file (no need to decompress).
   - **Choose Storage:** Select your microSD card.
3. When prompted to apply OS customization settings:
   - Configure your WiFi network SSID and password (recommended for headless `web` variant).
   - For `desktop` builds, enable **auto-login** so HamClock starts automatically on boot without pausing at a login prompt.
4. Flash the card, insert it into your Raspberry Pi, and power on.

> **Default Credentials:** Username `pi`, Password `pi` (change via `passwd` after first login).  
> **Initial WiFi Onboarding:** If WiFi was not pre-configured in Imager, the Pi broadcasts a temporary captive portal hotspot named **`HamClock-Setup`**. Connect from your phone or laptop to select your home network and enter the WiFi password.

---

## Approach 2: Automated Installation Script (Existing Raspberry Pi OS & Debian)

If you already have a working Raspberry Pi OS or Debian system (Bullseye, Bookworm, or Trixie) and want to install HamClock without re-flashing:

1. Download and run the automated installer script:
   ```bash
   curl -O https://raw.githubusercontent.com/openhamclock/hamclock/main/debian/install-hc-rpi
   chmod +x install-hc-rpi
   ./install-hc-rpi
   ```
2. The interactive script will:
   - Install required build dependencies (`build-essential`, `libx11-dev`, etc.).
   - Automatically detect your display resolution and select the best fit.
   - Compile HamClock.
   - Configure desktop autostart shortcuts.

For a detailed step-by-step walkthrough of setting up Raspberry Pi OS from scratch and running the script, see [G6NHU's setup guide](https://qso365.co.uk/2024/05/how-to-set-up-a-hamclock-for-your-shack/).

---

## Approach 3: Building from Source (Raspberry Pi OS & Debian)

You can also compile HamClock directly from source code on Raspberry Pi OS or standard Debian:

### 1. Install Build Dependencies

```bash
sudo apt update
sudo apt install -y build-essential libx11-dev libgpiod-dev
```

### 2. Build for Your Target Display

Choose the build target matching your intended display type and resolution:

* **Desktop / X11 (Default):**
  ```bash
  make -j 4 hamclock-1600x960
  # Or: hamclock-800x480, hamclock-2400x1440, hamclock-3200x1920
  ```
* **Headless Web Server:**
  ```bash
  make -j 4 hamclock-web-1600x960
  ```
* **Direct Framebuffer (fb0):**
  ```bash
  make -j 4 hamclock-fb0-1600x960
  ```

### 3. Run HamClock

```bash
./hamclock-1600x960 &
```

For web builds, open `http://<your-pi-ip>:8081/live.html` in any web browser.

---

## Backend Server Configuration

HamClock connects to a backend server for space weather, VOACAP propagation predictions, satellite tracking, and cluster data.

* The official Open HamClock Backend is available at **`ohb.hamclock.app:80`** (or **`hamclock.com:80`**).
* To configure your backend host, open the on-screen Setup menu (Page 1) or specify it via command line:
  ```bash
  ./hamclock-1600x960 -b ohb.hamclock.app:80
  ```
