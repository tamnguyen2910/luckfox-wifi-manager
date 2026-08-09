# 📶 Luckfox WiFi Manager

Qt5/QML WiFi manager for **Luckfox Pico Ultra W** (RV1106, ARM Cortex-A7, 800×480 RGB LCD) — clean UI, `wpa_cli` + `iw` + `udhcpc` via async QProcess, cross-compiled with Buildroot SDK.

![Platform](https://img.shields.io/badge/platform-Luckfox%20Pico%20Ultra%20W-blue)
![Qt](https://img.shields.io/badge/Qt-5.15.8-green)
![Build](https://img.shields.io/badge/build-qmake%20%2B%20Buildroot-orange)
![Backend](https://img.shields.io/badge/backend-wpa_cli%20%2B%20iw%20%2B%20udhcpc-1DB954)

---

## ✨ Features

- 🔍 **Scan & Connect** — `iw dev wlan0 scan` + `wpa_cli` multi-step (add_network → set_network → save_config → select_network)
- 🔐 **Hex PSK storage** — PBKDF2-SHA1 implemented in-app; passwords never stored as plaintext in `/etc/wpa_supplicant.conf`
- ⚡ **Auto-connect strongest** — on first launch, scans and connects to the saved network with the best dBm
- 📋 **Specific error messages** — distinguishes:
  - `"Wrong password — authentication failed with <ssid>"` (4-way handshake mismatch)
  - `"Cannot find <ssid> — network not in range"` (AP unreachable)
  - `"Cannot connect to <ssid> — association failed"` (association timeout)
  - `"Cannot connect to <ssid> — connection timed out"` (generic timeout)
- 🔄 **Auto-rollback** — on failed connect, immediately reconnects to the previous saved network (or strongest saved if no prior connect)
- 🛡 **Self-healing wlan0** — interface watchdog detects `NO-CARRIER` / wpa_supplicant death:
  - Removes stale `/var/run/wpa_supplicant/wlan0` socket
  - Restarts `wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf`
  - Brings `wlan0` back up automatically
- 🗂 **Network management** — `forget <ssid>` removes network from config (keeps others); `disconnect` clean disconnect
- 🧪 **Headless testing** — inject commands via `/tmp/wifi_sim_cmd` over SSH:
  ```bash
  ssh root@<board-ip> "echo 'scan' > /tmp/wifi_sim_cmd"
  ssh root@<board-ip> "echo 'connectsaved Tamnguyen' > /tmp/wifi_sim_cmd"
  ```
- 🎨 **Touch UI** — 800×480 `linuxfb`, on-screen virtual keyboard, signal strength bars (0–4), weak-signal warning (< -75 dBm)
- ⏱ **Adaptive auto-scan** — 30s–180s interval based on signal stability
- 🧵 **Async everywhere** — all `iw`/`wpa_cli`/`udhcpc` calls via `QProcess` (non-blocking Qt event loop)

## 🏗️ Architecture

```
┌────────────────────────────────────────────┐
│  QML UI (Qt Quick Controls 2.12)          │
│  - network list (SSID, dBm, secured flag) │
│  - password dialog + virtual keyboard     │
│  - error banner (transient, lastError)    │
└──────────────────┬─────────────────────────┘
                   │ Q_PROPERTY / Q_INVOKABLE
┌──────────────────▼─────────────────────────┐
│  C++ Backend (WifiManager)                 │
│  - scan: iw dev wlan0 scan (async)         │
│  - connect: wpa_cli state machine (5 steps)│
│  - status polling: wpa_cli status (2s)     │
│  - DHCP renew: udhcpc -i wlan0             │
│  - config I/O: atomic write (tmp+rename)   │
└──────────────────┬─────────────────────────┘
                   │ QProcess (async, non-blocking)
┌──────────────────▼─────────────────────────┐
│  CLI tools: iw / wpa_cli / udhcpc          │
│  → kernel WiFi driver → hardware (RV1106)  │
└────────────────────────────────────────────┘
```

**Why `wpa_cli` + `iw` + `udhcpc`?** Stock Luckfox rootfs provides these tools out of the box — no extra daemons or proprietary binaries. `wpa_supplicant` handles 802.11; `udhcpc` handles DHCP; the app orchestrates them.

---

## 📁 Project Structure

```
Wifi_Scan/
├── src/                        # C++ application source
│   ├── main.cpp                # embedded setup: linuxfb, fontdir, UTF-8, signal handler
│   ├── wifimanager.h           # WifiManager class (QObject, Q_PROPERTY bindings)
│   ├── wifimanager.cpp         # scan, connect, forget, watchdog, persistence
│   ├── wifi-manager.pro        # qmake project (TARGET = wifi-manager)
│   └── qml.qrc                 # embeds QML
├── qml/                        # QML UI (all in QRC)
│   └── main.qml                # root UI: scan bar, network list, error banner, password dialog
├── config/
│   └── board.env.example       # template (IP/user/pass) — copy to board.env
├── scripts/
│   ├── build.sh                # cross-compile (qmake + make) + deploy
│   ├── deploy.sh               # deploy only (expect-based SCP)
│   ├── deploy.exp              # expect driver for scp + chmod
│   └── run_app.exp             # run app on board + show startup log
├── build/                      # build output (gitignored; .gitkeep keeps dir)
├── wifi-manager.pro            # Qt project file
├── qml.qrc                     # QML resource manifest
└── LICENSE                     # MIT license
```

---

## 🚀 Quick Start

```bash
# 1. Board config (first time)
cp config/board.env.example config/board.env
# edit BOARD_IP, BOARD_USER, BOARD_PASS, BOARD_DEST

# 2. Cross-compile + deploy to board
./scripts/build.sh            # → build/wifi-manager (ARM ELF)

# 3. Run on board
./scripts/run_app.exp config/board.env   # starts app + prints startup log
```

> All board scripts use `expect` for password auth (no `sshpass` needed).  
> Default board password: `luckfox`.

### Manual run (via SSH)

```bash
ssh root@<board-ip>
cd /root
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/
export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib
./wifi-manager
```

---

## 🔧 Configuration

| Var | Default | Purpose |
|-----|---------|---------|
| `BOARD_USER` | `root` | SSH username |
| `BOARD_IP` | `192.168.1.2` | Board IP address |
| `BOARD_PASS` | `luckfox` | SSH password |
| `BOARD_DEST` | `/root` | Deploy directory on board |
| `QT_QPA_PLATFORM` | `linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480` | Qt embedded display |
| `QT_QPA_FONTDIR` | `/usr/share/fonts/dejavu/` | Font directory |
| `LD_LIBRARY_PATH` | `/usr/lib:/usr/lib/qt5/lib` | Qt library path |

**Real credentials live only in gitignored `config/board.env`** — never commit them.  
The template `config/board.env.example` is safe to push.

---

## 🛠️ Cross-Compilation

```bash
QMAKE=~/Desktop/LINUX/Build_Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake
cd build
$QMAKE ../wifi-manager.pro
make -j$(nproc)
# → build/wifi-manager (ELF 32-bit ARM, EABI5)
```

**Override board IP on the fly:**
```bash
BOARD_IP=192.168.1.11 ./scripts/build.sh
```

---

## 🧪 Verification on Board

```bash
# Process alive
pgrep -f wifi-manager

# QML loaded clean (0 ReferenceError / 0 TypeError)
./wifi-manager > /tmp/app.log 2>&1 &
sleep 3 && grep -c "ReferenceError\|TypeError" /tmp/app.log
# → should print 0

# Framebuffer rendered (non-zero bytes drawn)
cat /dev/fb0 | tr -d '\000' | wc -c   # ~1.5MB for 800×480×32

# WiFi scan works
iw dev wlan0 scan | grep -E "SSID|signal"

# wpa_supplicant running
pgrep wpa_supplicant

# DHCP lease acquired
ip -4 addr show wlan0
```

---

## 🎯 Roadmap

1. **WPA3 (SAE) support** — requires Buildroot rebuild with `CONFIG_SAE=y`
2. **5 GHz band** — ❌ hardware limitation (RV1106 WiFi module is 2.4 GHz only)
3. **Captive portal detection** — auto-open browser on portal networks
4. **Network priority UI** — drag-to-reorder saved networks
5. **Hotspot mode** — `wpa_supplicant` P2P / AP mode for tethering

---

## 📚 Docs

- This `README.md` — overview, build, run, config, verification
- `LICENSE` — MIT

---

## 📝 License

MIT — see `LICENSE` file.

**Real board credentials live only in gitignored `config/board.env`** — never commit them.