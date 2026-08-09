# luckfox-wifi-manager

WiFi manager app (Qt5 Quick / C++) for **Luckfox Pico Ultra W** (RV1106, ARM Cortex-A7) running Buildroot Linux with `linuxfb` display.

## Features

- Scan WiFi networks via `iw dev wlan0 scan`
- Connect to secured/open networks via `wpa_cli` (multi-step, no `wpa_passphrase` needed)
- Save networks as **hex PSK** (PBKDF2-SHA1 implemented in-app, no plaintext passwords)
- Auto-connect to the **strongest saved network** on launch (by dBm)
- Specific error messages: **"Wrong password"** vs **"Network not in range"** vs **"Association failed"**
- Auto-rollback to the previous saved network on connect failure
- Self-healing: `ensureWpaSupplicant()` auto-restarts wpa_supplicant if its control socket is missing; interface watchdog recovers a down wlan0
- Forget saved networks (preserves other networks in `wpa_supplicant.conf`)
- On-screen virtual keyboard for password entry (touchscreen)
- Signal strength bars (0–4) + dBm values, weak-signal warning (< -75 dBm)
- Adaptive auto-scan (30s–180s based on signal stability)
- Async QProcess usage — no blocking calls in main thread
- Headless input simulation (`/tmp/wifi_sim_cmd`) for automated testing

## Project Layout

```
Wifi_Scan/
├── src/                 # C++ sources (wifimanager, main)
├── qml/                 # QML UI (main.qml)
├── config/
│   ├── board.env.example  # Template — copy to board.env and edit
│   └── board.env          # REAL config (git-ignored, contains IP/user/pass)
├── scripts/
│   ├── build.sh           # Build (qmake + make) and deploy
│   ├── deploy.sh          # Deploy only (expect)
│   ├── deploy.exp         # Expect driver for scp+chmod
│   └── run_app.exp        # Run app on board + show startup log
├── FEATURES.md           # Feature list & improvement checklist
├── wifi-manager.pro      # Qt project file
└── qml.qrc               # QML resource manifest
```

## Setup (one time)

```bash
cp config/board.env.example config/board.env
# Edit config/board.env with your board's IP, user, password
```

`config/board.env` is git-ignored — credentials never get pushed.

## Build & Deploy

```bash
./scripts/build.sh          # build + deploy in one step
./scripts/deploy.sh         # deploy only (binary must exist in build/)
```

Overrides (optional): `BOARD_IP=... ./scripts/build.sh`

Requires Buildroot SDK qmake:
`/home/tamnguyen/Desktop/LINUX/Build_Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake`

## Run on board

```bash
./scripts/run_app.exp config/board.env   # starts app + shows startup log
```

Or manually via SSH:

```bash
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/
export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib
/root/wifi-manager
```

## Debug / Headless Testing

The app polls `/tmp/wifi_sim_cmd` on the board for injected commands:

| Command | Effect |
|---|---|
| `scan` | trigger a scan |
| `connectsaved <ssid>` | connect to a saved network |
| `connect <ssid> <pass>` | connect with password |
| `forget <ssid>` | forget network |
| `disconnect` | disconnect |
| `status` | log current status |

Write commands over SSH:

```bash
ssh root@<board_ip> "echo 'connectsaved Tamnguyen' > /tmp/wifi_sim_cmd"
```

App logs (`[ACT]` from C++, `[UI]` from QML) go to stdout — watch via `tail` in the SSH session that runs the app, or redirect to a file.

## Security Notes

- `config/board.env` (IP/user/password) is **git-ignored** — never commit it
- Expect scripts use `StrictHostKeyChecking=no` for dev convenience only
- For production: pin the host key (`ssh-keyscan` → `known_hosts`), use SSH key auth (`ssh-copy-id`), disable password auth
- Passwords are stored as hex PSK (PBKDF2-SHA1) in `/etc/wpa_supplicant.conf` with `0600` permissions, never plaintext

## Target

- Board: Luckfox Pico Ultra W (RV1106), 800x480 RGB LCD
- Toolchain: Buildroot arm-buildroot-linux-gnueabihf
- Qt: Qt 5.15 (Core, Gui, Qml, Quick)
