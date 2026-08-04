# luckfox-wifi-manager

WiFi manager app (Qt5 Quick / C++) for **Luckfox Pico Ultra W** (RV1106, ARM Cortex-A7) running Buildroot Linux with `linuxfb` display.

## Features

- Scan WiFi networks via `iw dev wlan0 scan`
- Connect to secured/open networks via `wpa_cli` (multi-step, no `wpa_passphrase` needed)
- Save networks as **hex PSK** (PBKDF2-SHA1 implemented in-app, no plaintext passwords)
- Auto-connect to saved networks on launch
- Forget saved networks (preserves other networks in `wpa_supplicant.conf`)
- On-screen virtual keyboard for password entry (touchscreen)
- Signal strength bars (0–4) + dBm values
- Async QProcess usage — no blocking calls in main thread

## Build

```bash
cd Wifi_Scan
./scripts/build.sh
```

Requires Buildroot SDK qmake:
`/home/tamnguyen/Desktop/LINUX/Build_Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake`

## Deploy

```bash
./scripts/deploy.sh        # deploy only (expect-based SSH to root@192.168.1.3)
./scripts/build.sh         # build + deploy
```

## Run on board

```bash
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/
export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib
/root/wifi-manager
```

## Target

- Board: Luckfox Pico Ultra W (RV1106), 800x480 RGB LCD
- Toolchain: Buildroot arm-buildroot-linux-gnueabihf
- Qt: Qt 5.15 (Core, Gui, Qml, Quick)
