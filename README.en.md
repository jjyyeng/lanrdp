**English** | [简体中文](README.md)

# lanrdp — Remote Desktop for Windows LANs

A Windows remote desktop written in Qt 6 / C++ / Win32: DXGI desktop capture, H.264 encoding
via Media Foundation, raw TCP transport. Built for **local networks** — low latency, no
external services, no installer.

## Features

- **One binary, two roles**: connect out to others as a viewer, or open a listener and be
  viewed — both at the same time
- **Multiple hosts at once**: one viewer can watch up to 4 hosts in a 2×2 grid (any cell can be
  maximized)
- **Multiple viewers**: one host accepts up to 4 viewers connected simultaneously
- **Monitor switching**: when the host has more than one display, the floating bar lets you pick
  which one to stream (one at a time)
- **Input token**: only one viewer can drive mouse and keyboard at a time — request it, yield it,
  or have the host revoke it
- **H.264 compression** (Media Foundation), or turn it off and send raw frames
- **File transfer**: drag a file onto the view; the receiver picks the destination folder; 4 GiB
  per file
- **Two-way clipboard sync**
- **Audio**: the viewer can hear the host's system sound (downlink only; can be disabled in
  settings)
- **Automatic reconnect** with backoff, plus heartbeat-based dead-peer detection
- **Adaptive bitrate**: adjusts to network conditions; the cap is selectable in
  *Start Listening…* (4 / 8 / 12 / 16 Mbps, default 8)
- **Tray residency** (optional, off by default): minimizing or closing the window moves it to the
  notification area and sharing continues
- **Single instance** (optional, off by default): launching again focuses the existing window
  instead of opening a second one
- **UI in Chinese or English**: *Settings → Language* offers 中文 / English / Follow system, and the
  switch applies immediately (no restart)
- Dark UI; `windeployqt` turns the build into a portable folder that runs without a Qt installation

## Getting it

This repository is **source only** — no prebuilt packages are published. Build it yourself with
the steps below.

## Usage

### On the host (the machine being viewed)

1. Launch `lanrdp.exe` → menu **Host → Start Listening…** (`Ctrl+1`)
2. Enter a port (e.g. `9000`) and a password (at least 6 characters, ASCII, no spaces) → OK
3. Windows Firewall prompts on the first listen — choose **Allow this program through**.
   **If you skip it, Windows will not ask again** and other machines will never be able to connect.
   To fix it afterwards, create a rule under *Windows Defender Firewall with Advanced Security →
   Inbound Rules*: either **Program** pointing at `lanrdp.exe`, or **Port** with the port you used
   (default `9000`), protocol TCP

### On the viewer

1. Launch `lanrdp.exe` → click **Connect to Remote Computer** in an empty cell
2. Enter the host's IP (run `ipconfig` on the host to see its IPv4 address), the port, and the
   same password → OK
3. Once connected: double-click a cell title (or click **Maximize** on the floating bar) to enlarge
   it, `Esc` to exit; drop files onto the view to transfer them; input-token state is shown on the
   floating bar
4. If the host has two or more displays, a **Screen** button appears on the floating bar — use it
   to choose which one to view (only one is streamed at a time; single-monitor hosts never show
   this button)
5. To hear the host: menu **Settings → Receive Remote Audio** (on by default)

### Things to know

- **Both ends must run the same build.** A protocol mismatch shows up as a refused connection or a
  picture that never arrives
- **With "minimize to notification area" enabled, closing the window does not quit**: the window
  goes to the notification area and the process keeps sharing. To really quit, right-click the tray
  icon → **Stop Sharing**, then close the window (on Windows 11 the tray icon hides under the `^`
  overflow by default)
- **Traffic is not encrypted** (no TLS) — trusted LANs only, do not expose it to the internet
- The executable is unsigned: builds you compile yourself are normally not blocked, but binaries
  obtained from someone else may trigger SmartScreen — click **More info → Run anyway**

## Build

Requirements: Qt 6.x (MinGW build), MinGW-w64 compiler, CMake ≥ 3.21, Ninja.

```bash
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=<your Qt install path>
cmake --build build
ctest --test-dir build
```

The output is `build/lanrdp.exe`.

- **Run it on this machine**: add Qt's `bin` directory to `PATH`, or
- **Copy it to another machine**: `windeployqt build/lanrdp.exe` collects the required DLLs next to
  the executable — copy the whole folder

## License

Source code is [MIT](LICENSE).

The build depends on Qt 6 (LGPL-3). This project links Qt dynamically only: it does not include,
distribute, or modify Qt sources, and therefore carries no Qt distribution obligations.
