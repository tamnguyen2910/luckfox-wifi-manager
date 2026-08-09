# luckfox-wifi-manager

WiFi Manager for **Luckfox Pico Ultra W** (RV1106, ARM Cortex-A7) running Buildroot Linux.

Qt5/QML frontend + C++ backend; `linuxfb` display at 800×480. The app uses `iw dev wlan0 scan` for WiFi scan, `wpa_cli` for connection, and `udhcpc` for DHCP renewals — all non-blocking via async `QProcess` in the Qt event loop.

## Quick Start

```bash
# 1. Copy board config (credentials are git-ignored — never pushed)
cp config/board.env.example config/board.env
# Edit config/board.env with your board's actual IP, user, password

# 2. Build
./scripts/build.sh          # build + deploy to board in one step

# 3. Run on the board
./scripts/run_app.exp config/board.env   # starts app + prints startup log
```

## Project Layout

```
Wifi_Scan/
├── src/                 # C++ sources (wifimanager, main)
├── qml/                 # QML UI (main.qml, error bar, etc.)
├── config/
│   ├── board.env.example  # Template — copy to board.env and edit
│   └── board.env          # REAL config (git-ignored)
├── scripts/
│   ├── build.sh           # Build + deploy (qmake + make + expect SCP)
│   ├── deploy.sh          # Deploy only (expect-based)
│   ├── deploy.exp         # Expect driver for scp + chmod
│   └── run_app.exp        # Run app on board + show log
├── wifi-manager.pro     # Qt project file
└── qml.qrc               # QML resource manifest
```

## Key Features

### Auto-connect on startup
On first launch the app scans for available networks and auto-connects to the one with the strongest dBm signal.

### Specific error messages
On connect failure the error bar shows per-PROTOCOL:
- `"Wrong password — authentication failed with <ssid>"` — wrong credentials (4-way handshake mismatch)
- `"Cannot find <ssid> — network not in range"` — AP not reachable
- `"Cannot connect to <ssid> — association failed"` — association timeout
- `"Cannot connect to <ssid> — connection timed out"` — general timeout

### Auto-rollback
If a connect attempt fails with a wrong password, the app immediately rolls back to the previous saved network (or the strongest saved network if no previous connect).

### Self-healing wlan0
If wlan0 goes down (wpa_supplicant dead, interface `NO-CARRIER`):
- Watchdog detects `wlan0` admin-UP flag missing → starts `wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf`
- Recovery command removes stale `/var/run/wpa_supplicant/wlan0` socket, clears `wlan0`, restarts wpa_supplicant, brings interface up

### WPA3 / WPA2-PSK
All networks are WPA2-PSK (PBKDF2-SHA1, hex PSK stored in config). No WPA3 (SAE) support yet (requires Buildroot rebuild).

### Network list management
- `connectsaved <ssid>` — connect to a previously saved network
- `forget <ssid>` — remove a network from `wpa_supplicant.conf` (kept in wpa_supplicant runtime)
- `disconnect` — clean disconnect

### Simulation (headless testing)
Inject commands via `/tmp/wifi_sim_cmd` over SSH:
```bash
ssh root@<board_ip> "echo 'scan' > /tmp/wifi_sim_cmd"
ssh root@<board_ip> "echo 'connectsaved Tamnguyen' > /tmp/wifi_sim_cmd"
```
The app reads the file in `pollSimCommands()` and executes commands. No interactive session required.

## Build & Deploy

| Command | Description |
|---|---|
| `./scripts/build.sh` | Build + deploy to board (expect-based SSH) |
| `./scripts/deploy.sh` | Deploy only (binary in build/) |
| `./scripts/deploy.exp <board.env> <binary>` | Manual deploy with env file |

**Override**:
```bash
BOARD_IP=192.168.1.11 ./scripts/build.sh   # Use a different board IP
```

## Running on Board

```bash
# Manual SSH
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/
export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib
/root/wifi-manager
```

**Headless** (non-interactive):
```bash
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/
export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib
/root/wifi-manager > /tmp/wifiapp.log 2>&1 &
```

## Security Notes

- `config/board.env` is **git-ignored** — credentials (IP, username, password) are never committed.
- Expect scripts use `StrictHostKeyChecking=no` only for local dev; production: pin host key via `ssh-keyscan`, use SSH key auth (`ssh-copy-id`), and disable password authentication.
- Passwords are stored as **hex PSK** (PBKDF2-SHA1) in `/etc/wpa_supplicant.conf` with `0600` permissions — never plaintext.
- `config/board.env.example` is the template — no real credentials in it.

## Troubleshooting

### "Failed to connect to non-global ctrl_ifname: wlan0"
If you see this error, wpa_supplicant is not running. The app tries to restart it automatically via `ensureWpaSupplicant()`. If it still fails, try:
```bash
ssh root@<board_ip> "pkill -9 wpa_supplicant; wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf; ip link set wlan0 up"
```

### wlan0 "down" but admin-UP
On the Luckfox board `iw dev wlan0 scan` works but `wpa_supplicant` is dead — the admin-UP flag is present but no-carry.
Recovery: the watchdog detects `NO-CARRIER` and calls `startInterfaceRecovery()`.

### Connection timeout after 15s
Default max wait is **15s** (from 25s). If you still see the generic error, the issue is a weak signal, AP not in range, or wrong password — check with `iw dev wlan0 scan` manually to confirm the SSID is visible.

## Supported Hardware

| Board | WiFi Chip | Network |
|---|---|---|
| Luckfox Pico Ultra W | RV1106 / Ralink | 2.4 GHz |

5 GHz is not supported — the RV1106 WiFi module is a 2.4 GHz only chip.

## License

MIT — see `LICENSE` file.
