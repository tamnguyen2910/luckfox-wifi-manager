# WiFi Manager — Features, Checklist & Ideas

> App: `wifi-manager` — Qt5 Quick / C++ cho **Luckfox Pico Ultra W** (RV1106)
> Updated: 2026-08-06

---

## ✅ Features đã implement (committed)

| # | Tính năng | Commit | Trạng thái |
|---|-----------|--------|-----------|
| 1 | Scan WiFi networks qua `iw dev wlan0 scan` | — | ✅ |
| 2 | Connect secured/open networks qua `wpa_cli` (multi-step state machine, async) | `8bb31d6` | ✅ |
| 3 | Lưu mạng dưới dạng **hex PSK** (PBKDF2-SHA1 trong app, không lưu plaintext) | `65ef03a` | ✅ |
| 4 | Đọc được config legacy (hex PSK thiếu prefix `psk=`) | `87ee7ff` | ✅ |
| 5 | Forget saved networks (giữ nguyên các mạng khác) | — | ✅ |
| 6 | Virtual keyboard on-screen (touchscreen) | — | ✅ |
| 7 | Signal bars (0–4) + dBm | — | ✅ |
| 8 | **Atomic write** `/etc/wpa_supplicant.conf` (temp + rename) — chống config hỏng khi mất điện | `a56643d` | ✅ |
| 9 | **DHCP renewal** khi switch mạng — IP mới đúng network mới, không hiện IP cũ | `12ad245` | ✅ |
| 10 | **Rescan signal** kể cả khi đang connected (auto-scan 60s) | `6209f6b` | ✅ |
| 11 | **Watchdog wlan0** — phát hiện interface down ≥5s, dọn state, auto-reconnect khi up | `8e3599d` | ✅ |
| 12 | **Chống race** forget/connect — kill process + reset state + QMutex serialize config | `7770618` | ✅ |
| 13 | **Chống false "Connection timeout"** khi DHCP chậm (xóa timer 12s thừa) | `eeef16e` | ✅ |
| 14 | Dọn dead code `handleDhcpRenewalFinished` | `e8904e1` | ✅ |
| 15 | **Auto-reconnect mạng đã lưu khi khởi động** (ưu tiên "Tamnguyen", fallback mạng đầu) | `2973fe6` | ✅ |
| 16 | **Headless input simulation + debug logging** (sim commands via `/tmp/wifi_sim_cmd`, `[ACT]/[UI]/[STA]` tagged logs) | `HEAD` | ✅ |
| 17 | **Robust wrong-password detection** — per-AP handshake fail counting with `m_seenFourWay` guard (fixes multi-BSSID false positives) | `HEAD` | ✅ |
| 18 | **Set_network FAIL check** in state machine (fast error on bad network id) | `HEAD` | ✅ |
| 19 | **Unified abortConnect** — single teardown for config-fail, timeout, wrong-pwd, handshake-fail (stops timers, kills processes, resumes auto-scan) | `HEAD` | ✅ |
| 20 | **Dead code removed** — `updateDetailedStatus` (never called, no QML binding) | `HEAD` | ✅ |

---

## 📋 Checklist — Tính năng cần làm / đang xem xét

### 🔴 Ưu tiên cao (ảnh hưởng UX thực tế)

- [x] **Hiển thị lỗi sai mật khẩu**
  - Phân biệt "Sai mật khẩu" vs "Không tìm thấy mạng" vs "Quá xa"
  - Cách: theo dõi `wpa_state=4WAY_HANDSHAKE` lặp ≥3 lần mà không COMPLETED → "Sai mật khẩu"
  - Trạng thái `ASSOCIATING → DISCONNECTED` lặp → "Mạng quá xa / kênh nghẽn"
  - ✅ **DONE:** `m_seenFourWay` guard + per-AP handshake counting + `abortConnect("Wrong password")`
- [ ] **Scan 5GHz** — ❌ **Hardware limitation**
  - ⚠️ Đã kiểm tra: `iw phy` chỉ hiển thị **Band 1 (2.4GHz channels 1-14)**, không có Band 2 (5GHz)
  - Chip RV1106/WiFi module không hỗ trợ 5GHz radio — không fix được bằng phần mềm
  - ✅ **CONFIRMED: Cannot implement**
- [ ] **WPA3 (SAE) support** — ⚠️ **Requires Buildroot rebuild**
  - Board's wpa_supplicant v2.6 compiled WITHOUT SAE support (verified: no SAE strings in binary)
  - Cần: buildroot rebuild với `CONFIG_SAE=y` / `BR2_PACKAGE_WPA_SUPPLICANT_SAE=y`
  - App-side: detect WPA3 networks từ scan output, dùng `key_mgmt=SAE` + `sae_password` khi connect
  - Không thể test mà không rebuild wpa_supplicant trên board

### 🟡 Ưu tiên trung bình

- [ ] **Validate SSID** — giới hạn ≤32 bytes (802.11), chặn ký tự điều khiển (newline, tab, ESC)
- [ ] **Hiển thị loại bảo mật** (WPA2/WPA3/WEP/Open) thay vì chỉ "Secured"
- [ ] **Refresh IP realtime** — theo dõi lease DHCP hết hạn, hiển thị IP đúng nhất
- [ ] **Ưu tiên mạng đã lưu lên đầu list** (sort saved first, rồi theo signal)
- [ ] **Tự reconnect khi app mất kết nối không phải do interface down** (roaming, AP off rồi on)

### 🟢 Nice-to-have (cải tiến nhỏ)

- [ ] Icon signal đầy đủ khi connected (hiện dBm trong top bar)
- [ ] LED/GPIO indicator trên board (LED xanh = connected, đỏ = lost)
- [ ] Đồng hồ realtime trong top bar
- [ ] Dark/light theme toggle
- [ ] Hiển thị channel + frequency của mạng trong list
- [ ] Export/import danh sách saved networks (backup)

---

## 💡 Ideas — Tính năng tương lai (chưa quyết định)

- **Auto-connect mạng mạnh nhất trong saved list** (dựa trên signal, không hardcode "Tamnguyen")
- **Thử lại tự động với backoff** khi connect fail (retry 3 lần, tăng dần 5s/10s/20s)
- **WiFi hotspot/AP mode** (dùng `hostapd`) — biến board thành access point khi không có mạng
- **Web config UI** (HTTP server nhẹ trên board) — cấu hình WiFi qua browser thay vì touchscreen
- **OTA update** — tự tải firmware mới khi có network
- **Network metrics logging** — lưu signal/RSSI theo thời gian, export CSV
- **QoS monitor** — ping latency tới gateway, hiển thị chất lượng kết nối
- **Multi-board sync** — đồng bộ saved networks giữa nhiều thiết bị qua mạng
- **Screenshot/debug mode** — chụp ảnh màn hình + gửi log khi gặp lỗi
- **i18n** — hỗ trợ tiếng Việt/English UI toggle

---

## 🔧 Known limitations

- **5GHz scan** — phụ thuộc hardware (cần xác nhận chip có radio 5GHz không)
- **WPA3** — chưa hỗ trợ SAE, chỉ WPA2-PSK
- **No captive portal** — không hiển thị trang đăng nhập khi vào mạng hotel/cafe
- **Single interface** — chỉ quản lý `wlan0`, không hỗ trợ multi-radio
- **No EAP/802.1X** — không connect được mạng enterprise (WPA-Enterprise)

---

## 🔨 Cách thêm tính năng mới (workflow)

1. Fix/tính năng mới → **build & deploy** → test trên board
2. **Confirm bởi user** → commit
3. Push lên `origin/main`
4. Update checklist này: chuyển `[ ]` → `[x]` + ghi commit hash

---

*Maintainer: tamnguyen2910*
