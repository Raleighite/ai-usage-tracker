#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"

// ── Agent HQ endpoint ────────────────────────────────────────────────────────
#define SERVER_HOST     "192.168.0.203"
#define SERVER_PORT     8030
#define USAGE_URL       "http://" SERVER_HOST ":" XSTR(SERVER_PORT) "/api/usage/summary"
#define XSTR(x)         STR(x)
#define STR(x)          #x

// ── Claude subscription usage endpoint (proxied via port 8030) ──────────────
// Served by Aha-San's usage server on the same host as /api/usage/summary
// Returns session, Sonnet 7-day, and total 7-day quota percentages + reset countdowns
#define CLAUDE_USAGE_URL    "http://" SERVER_HOST ":" XSTR(SERVER_PORT) "/api/claude/usage"

// ── ChatGPT Codex session usage endpoint ───────────────────────────────────
// Returns local OpenClaw Codex turn/session/token rollups from trajectory telemetry
#define CODEX_USAGE_URL     "http://" SERVER_HOST ":" XSTR(SERVER_PORT) "/api/codex/usage"

// ── perSession[] model filter ───────────────────────────────────────────────
// Model strings come back as full paths e.g. "meridian/claude-sonnet-4-6"
// Use strstr(model, FILTER_SONNET) != nullptr for resilient matching
#define FILTER_SONNET   "sonnet"
#define FILTER_HAIKU    "haiku"
#define FILTER_OPUS     "opus"

// ── Spend thresholds (USD/day) ───────────────────────────────────────────────
#define COST_WARN       1.00f   // yellow >= $1.00
#define COST_ALERT      5.00f   // red    >= $5.00

// ── Display timing ───────────────────────────────────────────────────
#define STATUS_DWELL_MS 30000   // time on selected status screen (30s)
#define CLAUDE_DWELL_MS STATUS_DWELL_MS
#define CODEX_DWELL_MS  STATUS_DWELL_MS
#define ERROR_RETRY_MS   5000   // retry delay after fetch/WiFi errors

// Optional fallback if /api/codex/usage does not expose codex5h_pct yet.
// Keep 0 to avoid pretending token counts are a real quota percentage.
#define CODEX_SESSION_TOKEN_BUDGET 0

// ── Color palette (RGB888, LovyanGFX converts internally) ───────────────────
#define C_BG            0x080808  // near-black background
#define C_FG            0xFFFFFF  // white — main values
#define C_DIM           0x888888  // gray — labels, units
#define C_COST          0x00E5CC  // mint teal — hero cost number
#define C_TOKENS        0x7EB8FF  // soft blue — token counts
#define C_TIER_SONNET   0xA855F7  // purple
#define C_TIER_HAIKU    0x22D3EE  // cyan
#define C_TIER_OPUS     0xFBBF24  // amber/gold
#define C_TIER_OTHER    0x6B7280  // slate gray
#define C_DIVIDER       0x1E1E1E  // subtle rule line
// Cost threshold colors (Erina)
#define C_COST_OK       0x00C853  // green  < $1.00
#define C_COST_WARN     0xFFD600  // yellow $1.00-$5.00
#define C_COST_ALERT    0xFF3D00  // red    >= $5.00

// Claude screen
#define C_CLAUDE_BRAND  0xE5541B  // Anthropic orange
#define C_GAUGE_BG      0x222222  // gauge track background
#define C_CODEX_BRAND   0x10A37F  // OpenAI green

// ── Layout geometry (landscape 240×135) ────────────────────────────────────
#define PANEL_DIVIDER_X  114     // vertical split x
#define LEFT_W           114     // left panel width
#define RIGHT_X          116     // right panel start x
#define RIGHT_W          124     // right panel width
// Tier row geometry (right panel)
#define TIER_ROW_H        27     // row height px
#define TIER_ROW_Y0       18     // first row y

// ── TTGO T-Display (original ESP32) pin map ────────────────────────────────
#define PIN_POWER_ON    -1      // Not needed on original T-Display
#define PIN_LCD_BL       4      // backlight
#define PIN_LCD_CS       5
#define PIN_LCD_DC      16
#define PIN_LCD_RST     23      // RST pin
#define PIN_LCD_MOSI    19
#define PIN_LCD_SCLK    18
#define PIN_BAT_ADC     34      // battery voltage sense (input-only GPIO)
#define PIN_BTN1         0      // BOOT button
#define PIN_BTN2        35      // KEY button (input-only, external pullup)

// ── Display (T-Display original: 135×240 physical, 240×135 landscape) ───────
#define LCD_WIDTH      135
#define LCD_HEIGHT     240
