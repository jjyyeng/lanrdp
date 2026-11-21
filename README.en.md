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
- **File transfer in both directions** (the receiver picks the folder; 4 GiB per file):
  - Viewer → host: drag a file onto the view; or copy files elsewhere and click **Send file** on
    that cell's floating bar (**copying alone sends nothing** — up to 64 files per send). The small
    arrow on the right of the button is **Choose file…**
  - Host → viewer: copy files inside the remote view (Ctrl+C) and paste them on your own machine
    (Ctrl+V); or use **Host → Send file…** to pick files yourself and send them to whoever is
    currently driving this machine (unavailable while nobody is)
- **Two-way clipboard sync**: text and files; images and HTML are not synced
- **LAN auto-discovery**: *New Connection* lists machines currently waiting for viewers — click a
  row to fill in address and port (a machine with two network cables shows one row per address;
  pick whichever route you want). Typing the address by hand still works when discovery finds
  nothing
- **Audio**: the viewer can hear the host's system sound (downlink only; can be turned off under
  *Settings…*)
- **Privacy curtain** (`Ctrl+Alt+F11`, works even while another program has focus): every screen on
  this machine turns black at once, and frame delivery, audio and input rights all stop — the viewer
  sees a frozen picture, cannot click, and hears nothing. Press again to restore, and the picture
  comes back sharp immediately. Also available as **Host → Privacy curtain**
- **Panic stop** (`Ctrl+Alt+F12`, works even while another program has focus): stop listening and
  drop every connected viewer in one keystroke — nobody can connect again until you start listening
- **Automatic reconnect** with backoff (on by default), plus heartbeat-based dead-peer detection
- **Adaptive bitrate**: adjusts to network conditions; the cap is selectable in
  *Start Listening…* (4 / 8 / 12 / 16 Mbps, default 12)
- **Tray residency** (optional, off by default): minimizing or closing the window moves it to the
  notification area and sharing continues
- **Single instance** (optional, off by default): launching again focuses the existing window
  instead of opening a second one
- **Six switches in one place — *Settings…***: reconnect automatically / receive remote audio /
  minimize to tray / one window at a time / start waiting for connections at launch / mouse and
  keyboard. All are remembered except *mouse and keyboard*, which is re-approved on every launch
- **Quality readout**: hover the status bar to see round-trip time and decode time, plus a one-line
  verdict such as "network latency is high" or "this machine can't keep up with decoding"
- **UI in Chinese or English**: *Settings → Language* offers 中文 / English / Follow system, and the
  switch applies immediately (no restart)
- **Dark and light themes**: *Settings → Appearance*, applied immediately and remembered
- `windeployqt` turns the build into a portable folder that runs without a Qt installation

## Getting it

This repository is **source only** — no prebuilt packages are published. Build it yourself with
the steps below.

## Usage

### On the host (the machine being viewed)

1. Launch `lanrdp.exe` → menu **Host → Start Listening…** (`Ctrl+1`)
2. Enter a port (e.g. `9000`) and a password (at least 6 characters, ASCII, no spaces) → OK
3. Rather than reading the details out loud, click **Copy invitation**: address, port and password
   are copied as three lines — paste them to the other side (nothing is copied until the password
   is filled in and valid, since without it the text is useless)
4. To be reachable right after boot, tick **Start waiting for connections at launch** under
   *Settings…* (off by default). One switch does both jobs: register the auto-start entry and begin
   listening with the last port and password. Without a saved password it will not auto-start, and
   the status bar says so
5. Don't want the screen seen? Press `Ctrl+Alt+F11` for the privacy curtain — every screen goes
   black, the viewer's picture freezes, audio stops and input rights are taken back. Press it again
   to restore. To kick everyone out at once press `Ctrl+Alt+F12` (stops listening; nobody can
   reconnect until you start it again)
6. Windows Firewall prompts on the first listen — choose **Allow this program through**.
   **If you skip it, Windows will not ask again** and other machines will never be able to connect.
   To fix it afterwards, create a rule under *Windows Defender Firewall with Advanced Security →
   Inbound Rules*: either **Program** pointing at `lanrdp.exe`, or **Port** with the port you used
   (default `9000`), protocol TCP

### On the viewer

1. Launch `lanrdp.exe` → click **Connect to Remote Computer** in an empty cell
2. The dialog lists machines on the LAN that are waiting for viewers — click a row to fill in
   address and port (or type them by hand: run `ipconfig` on the host to see its IPv4 address);
   enter the same password → OK
3. Once connected: double-click a cell title (or click **Maximize** on the floating bar) to enlarge
   it, `Esc` to exit; drop files onto the view to transfer them; input-token state is shown on the
   floating bar
4. Copied files elsewhere with Ctrl+C? **Send file** on that cell's floating bar lights up — click
   it to actually send. For any other file, use the small arrow on the right of the button →
   **Choose file…**. The other direction works too: copy files inside the remote view (Ctrl+C) and
   paste them on your own machine (Ctrl+V)
5. If the host has two or more displays, a **Screen** button appears on the floating bar — use it
   to choose which one to view (only one is streamed at a time; single-monitor hosts never show
   this button)
6. To hear the host: menu **Settings → Settings…** → tick **Receive remote audio** (on by default,
   remembered)

### Things to know

- **Auto-discovery needs its own UDP rule**: the peer list relies on UDP broadcast (port `9001`),
  and firewalls allow protocols separately — with only TCP allowed the list simply stays empty.
  Typing the address by hand still works, so an empty list is not a failure
- **Only LAN peers are accepted**: the host accepts connections from addresses inside its own subnets
  (plus loopback). Anything from another subnet or over a VPN is dropped immediately and nothing is
  logged server-side. That is deliberate, not a fault — to go across subnets, both ends have to look
  like they share one
- **Both ends must run the same build.** A protocol mismatch shows up as a refused connection or a
  picture that never arrives
- **With "minimize to tray" enabled, closing the window does not quit**: the window
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
