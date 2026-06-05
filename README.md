# AI Usage Tracker — Firmware

**Hardware:** LILYGO T-Display-S3 (standard, 62×26mm PCB)
**Display:** ST7789 1.9" 170×320 color LCD (onboard)
**MCU:** ESP32-S3, dual-core 240MHz, WiFi built-in
**Power:** USB-C charging + 1.25mm JST LiPo (target: ~600mAh 502035 cell)

---

## Data Source

```
GET http://192.168.0.203:8030/api/usage
```

Response fields used:
- `todayCost` — today's total spend in USD (big display, color-coded)
- `todayTokens` — today's token count
- `totalCost` — 30-day cumulative cost
- `billing.openrouter.total` — OpenRouter spend

---

## Build & Flash

```bash
# Install PlatformIO CLI if needed
pip install platformio

# Set WiFi creds first
nano include/config.h   # WIFI_SSID / WIFI_PASSWORD

# Build + upload (T-Display-S3 connected via USB-C)
pio run -t upload -e t_display_s3

# Monitor serial output
pio device monitor
```

---

## Behavior

1. Boot → power on display → show splash
2. Connect WiFi → fetch `/api/usage` → parse JSON
3. Draw summary screen:
   - Today's cost (green/yellow/red by threshold)
   - Today's tokens
   - 30-day total + OpenRouter spend
   - Battery % (top-right corner)
4. Deep sleep for `SLEEP_MINUTES` (default: 5)
5. Wake → repeat from step 1

Boot count persists across sleep cycles via RTC memory.

---

## Config (`include/config.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `WIFI_SSID` | `"YOUR_SSID"` | WiFi network name |
| `WIFI_PASSWORD` | `"YOUR_PASSWORD"` | WiFi password |
| `SERVER_HOST` | `192.168.0.203` | Agent HQ host |
| `SERVER_PORT` | `8030` | Agent HQ port |
| `SLEEP_MINUTES` | `5` | Refresh interval |
| `COST_WARN` | `1.00` | Yellow threshold (USD/day) |
| `COST_ALERT` | `3.00` | Red threshold (USD/day) |

---

## Enclosure

Designed by Dr. Senku. PETG, matte black, 15° desk tilt.
- Board footprint: 62 × 26mm
- Total depth with LiPo: ~18mm (design clearance: 20mm)
- USB-C cutout: short bottom edge, horizontally centered
- Button access holes: left of USB-C on bottom edge

---

## Roadmap

- [ ] Phase 2: Switch to `/api/usage/summary` endpoint (Aha-San building)
- [ ] Per-agent cycle view (KEY button press)
- [ ] OpenAI spend via proxy endpoint
- [ ] WiFi provisioning (captive portal, no hardcoded creds)
- [ ] OTA firmware updates
