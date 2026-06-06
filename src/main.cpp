/*
 * AI Usage Tracker — Firmware MVP
 * Hardware: LILYGO T-Display-S3
 * Endpoint: GET http://192.168.0.203:8030/api/usage
 *
 * Build with PlatformIO: pio run -t upload
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include "config.h"

// ── LovyanGFX display config for T-Display-S3 ────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        { // SPI bus
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 0;
            cfg.freq_write = 40000000;
            cfg.pin_sclk   = PIN_LCD_SCLK;
            cfg.pin_mosi   = PIN_LCD_MOSI;
            cfg.pin_miso   = -1;
            cfg.pin_dc     = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        { // Panel
            auto cfg = _panel.config();
            cfg.pin_cs     = PIN_LCD_CS;
            cfg.pin_rst    = PIN_LCD_RST;
            cfg.pin_busy   = -1;
            cfg.panel_width  = LCD_WIDTH;
            cfg.panel_height = LCD_HEIGHT;
            cfg.offset_x     = 52;   // ST7789 window offset in 240×320 RAM
            cfg.offset_y     = 40;
            cfg.offset_rotation = 0;
            cfg.invert       = true; // required for T-Display ST7789
            _panel.config(cfg);
        }
        { // Backlight
            auto cfg = _light.config();
            cfg.pin_bl     = PIN_LCD_BL;
            cfg.invert     = false;
            cfg.freq       = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX display;

// ── Data model ────────────────────────────────────────────────────────────────
struct TierStats {
    float cost   = 0.0f;
    int   tokens = 0;
};

struct WeekDay {
    char  date[6] = {};  // "MM-DD"
    float cost    = 0.0f;
    int   tokens  = 0;
};

struct ClaudeUsage {
    int  session_pct          = -1;   // -1 = unavailable
    int  weekly_pct           = -1;
    char session_resets_in[12] = {};
    bool valid                = false;
};

struct UsageData {
    float     todayCost       = 0.0f;
    int       todayTokens     = 0;
    float     totalCost       = 0.0f;
    int       totalTokens     = 0;
    float     openrouterDaily = 0.0f;
    float     openrouterTotal = 0.0f;
    TierStats sonnet, haiku, opus, other;
    WeekDay   week[7];           // Phase 3: sparkline data, pre-computed
    bool      valid           = false;
};

// ── Persistent across sleep cycles (RTC memory) ──────────────────────────────
RTC_DATA_ATTR int bootCount = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
bool     connectWifi();
bool     fetchUsage(UsageData &out);
void     drawBoot();
void     drawSummary(const UsageData &d, float batPct);
void     drawTierScreen(const char *name, const TierStats &t, uint32_t color,
                        int idx, float totalCost,
                        const char *prevName, const char *nextName);
void     runDisplayLoop(const UsageData &d);
bool     fetchClaudeUsage(ClaudeUsage &out);
void     drawClaudeScreen(const ClaudeUsage &c);
void     drawError(const char *msg);
float    readBatteryPct();
uint32_t costColor(float cost);
bool     checkManualRefresh();  // long-press IO0 skips sleep
void     goSleep();

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    bootCount++;

    // Power on display (original T-Display has no separate power-enable pin)
    if (PIN_POWER_ON >= 0) {
        pinMode(PIN_POWER_ON, OUTPUT);
        digitalWrite(PIN_POWER_ON, HIGH);
    }

    display.init();
    display.setRotation(1);   // landscape, USB-C on right
    display.setBrightness(180);
    display.fillScreen(TFT_BLACK);

    drawBoot();  // 2.5s animated boot screen

    float batPct = readBatteryPct();

    if (!connectWifi()) {
        drawError("WiFi failed");
        goSleep();
        return;
    }

    UsageData data;
    if (!fetchUsage(data)) {
        drawError("Fetch failed");
        goSleep();
        return;
    }

    drawSummary(data, batPct);
    runDisplayLoop(data);
    goSleep();
}

void loop() {
    // Nothing — we use deep sleep; loop never runs
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
bool connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    display.setCursor(10, 110);
    display.setTextColor(TFT_CYAN);
    display.print("Connecting WiFi...");

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) return false;

    display.setTextColor(TFT_GREEN);
    display.setCursor(10, 125);
    display.print("Connected: ");
    display.print(WiFi.localIP());
    delay(400);
    return true;
}

// ── HTTP fetch + JSON parse ───────────────────────────────────────────────────
bool fetchUsage(UsageData &out) {
    HTTPClient http;
    http.begin(USAGE_URL);
    http.setTimeout(8000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[HTTP] error: %d\n", code);
        http.end();
        return false;
    }

    // /api/usage/summary is pre-filtered server-side
    // Response is compact (<512 bytes), no client filter needed
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[JSON] parse error: %s\n", err.c_str());
        return false;
    }

    out.todayCost       = doc["todayCost"]       | 0.0f;
    out.todayTokens     = doc["todayTokens"]     | 0;
    out.totalCost       = doc["totalCost"]       | 0.0f;
    out.totalTokens     = doc["totalTokens"]     | 0;
    out.openrouterDaily = doc["openrouterDaily"] | 0.0f;
    out.openrouterTotal = doc["openrouterTotal"] | 0.0f;
    // Phase 2: byTier breakdown (pre-bucketed server-side)
    out.sonnet.cost   = doc["byTier"]["sonnet"]["cost"]   | 0.0f;
    out.sonnet.tokens = doc["byTier"]["sonnet"]["tokens"] | 0;
    out.haiku.cost    = doc["byTier"]["haiku"]["cost"]    | 0.0f;
    out.haiku.tokens  = doc["byTier"]["haiku"]["tokens"]  | 0;
    out.opus.cost     = doc["byTier"]["opus"]["cost"]     | 0.0f;
    out.opus.tokens   = doc["byTier"]["opus"]["tokens"]   | 0;
    out.other.cost    = doc["byTier"]["other"]["cost"]    | 0.0f;
    out.other.tokens  = doc["byTier"]["other"]["tokens"]  | 0;
    // Phase 3: sparkline — 7-entry week[] array, pre-computed server-side
    JsonArray week = doc["week"].as<JsonArray>();
    int i = 0;
    for (JsonObject d : week) {
        if (i >= 7) break;
        strlcpy(out.week[i].date, d["date"] | "", sizeof(out.week[i].date));
        out.week[i].cost   = d["cost"]   | 0.0f;
        out.week[i].tokens = d["tokens"] | 0;
        i++;
    }
    out.valid         = true;

    Serial.printf("[usage] cost=%.4f tokens=%d total=%.2f\n",
                  out.todayCost, out.todayTokens, out.totalCost);
    return true;
}

// ── Display: summary screen ───────────────────────────────────────────────────
// ── Display: boot screen ────────────────────────────────────────────────────
void drawBoot() {
    display.fillScreen(C_BG);

    // "FLEET" hero text
    display.setTextColor(C_FG);
    display.setTextSize(4);
    int heroW = 5 * 24;  // "FLEET" = 5 chars x 24px
    display.setCursor((LCD_HEIGHT - heroW) / 2, 45);
    display.print("FLEET");

    // Subtitle
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    const char *sub = "AI USAGE MONITOR";
    int subW = strlen(sub) * 6;
    display.setCursor((LCD_HEIGHT - subW) / 2, 88);
    display.print(sub);

    // Animated loading bar (fills over 2.5s in 10 steps)
    display.fillRect(80, 130, 160, 4, C_DIVIDER);
    for (int step = 1; step <= 10; step++) {
        display.fillRect(80, 130, step * 16, 4, C_COST);
        delay(250);
    }
}

// Helper: center text in a panel
static void printCentered(int panelX, int panelW, int y, const char *str) {
    int strW = display.textWidth(str);
    display.setCursor(panelX + (panelW - strW) / 2, y);
    display.print(str);
}

void drawSummary(const UsageData &d, float batPct) {
    display.fillScreen(C_BG);

    // Vertical panel divider
    display.fillRect(PANEL_DIVIDER_X, 0, 2, LCD_WIDTH, C_DIVIDER);

    // ── LEFT PANEL: TODAY + TOTALS ──
    // "TODAY" label — teal brand color (hero number carries threshold signal)
    display.setTextColor(C_COST);
    display.setTextSize(1);
    display.setCursor(8, 2);
    display.print("TODAY");

    // Hero cost — size 3 fits within 114px left panel
    display.setTextColor(costColor(d.todayCost));
    char costStr[12];
    display.setTextSize(3);
    snprintf(costStr, sizeof(costStr), "$%.2f", d.todayCost);
    printCentered(0, LEFT_W, 12, costStr);

    // Tokens
    char tokStr[16];
    if (d.todayTokens >= 1000)
        snprintf(tokStr, sizeof(tokStr), "%.1fK tok", d.todayTokens / 1000.0f);
    else
        snprintf(tokStr, sizeof(tokStr), "%d tok", d.todayTokens);
    display.setTextColor(C_TOKENS);
    display.setTextSize(2);
    printCentered(0, LEFT_W, 48, tokStr);

    // Horizontal rule
    display.fillRect(4, 68, 144, 1, C_DIVIDER);

    // 30-day total
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 72);
    display.print("30-DAY");
    char totalStr[12];
    snprintf(totalStr, sizeof(totalStr), "$%.2f", d.totalCost);
    display.setTextColor(C_FG);
    display.setTextSize(3);
    printCentered(0, LEFT_W, 83, totalStr);

    // OpenRouter + battery (small, bottom of left panel)
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 112);
    display.printf("OR: $%.4f", d.openrouterTotal);
    display.setCursor(8, 123);
    display.printf("BAT: %2.0f%%", batPct);

    // ── RIGHT PANEL: TIER BREAKDOWN ──
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(RIGHT_X + 8, 4);
    display.print("TIERS");

    const char    *tierNames[]  = { "SONNET", "HAIKU", "OPUS", "OTHER" };
    const TierStats *tiers[]    = { &d.sonnet, &d.haiku, &d.opus, &d.other };
    const uint32_t  tierColors[]= { C_TIER_SONNET, C_TIER_HAIKU,
                                    C_TIER_OPUS,   C_TIER_OTHER };

    for (int i = 0; i < 4; i++) {
        int rowY = TIER_ROW_Y0 + i * TIER_ROW_H;
        uint32_t col = tierColors[i];

        // Left accent bar
        display.fillRect(RIGHT_X, rowY, 4, 25, col);

        // Tier name
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        display.setCursor(RIGHT_X + 9, rowY + 2);
        display.print(tierNames[i]);

        // Cost (right-aligned)
        char cStr[10];
        snprintf(cStr, sizeof(cStr), "$%.3f", tiers[i]->cost);
        display.setTextColor(col);
        display.setTextSize(2);
        int cW = (int)strlen(cStr) * 12;
        display.setCursor(LCD_HEIGHT - 4 - cW, rowY + 2);
        display.print(cStr);

        // Tokens (right-aligned, small)
        char tStr[10];
        if (tiers[i]->tokens >= 1000)
            snprintf(tStr, sizeof(tStr), "%.1fK", tiers[i]->tokens / 1000.0f);
        else
            snprintf(tStr, sizeof(tStr), "%d", tiers[i]->tokens);
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        int tW = (int)strlen(tStr) * 6;
        display.setCursor(LCD_HEIGHT - 4 - tW, rowY + 20);
        display.print(tStr);

        // Row separator (Erina spec)
        display.fillRect(RIGHT_X, rowY + 26, RIGHT_W, 1, C_DIVIDER);
    }
}

// ── Display: error screen ─────────────────────────────────────────────────────
// ── Display: tier detail screen ──────────────────────────────────────────────
void drawTierScreen(const char *name, const TierStats &t, uint32_t color,
                    int idx, float totalCost,
                    const char *prevName, const char *nextName) {
    display.fillScreen(C_BG);

    // ── Header bar (Y=0-26) ──
    display.fillRect(0, 0, LCD_HEIGHT, 26, 0x111111);

    // Tier name (left)
    display.setTextColor(color);
    display.setTextSize(3);
    display.setCursor(8, 2);
    display.print(name);

    // Progress indicator (right): "X/4"
    char progStr[4];
    snprintf(progStr, sizeof(progStr), "%d/4", idx + 1);
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(LCD_HEIGHT - 12 - (int)strlen(progStr) * 6, 2);
    display.print(progStr);

    // Rule below header
    display.fillRect(0, 26, LCD_HEIGHT, 2, C_DIVIDER);

    // ── Hero cost (Y=32) ──
    char costStr[12];
    if (t.cost >= 100.0f)     snprintf(costStr, sizeof(costStr), "$%.1f",  t.cost);
    else if (t.cost >= 10.0f) snprintf(costStr, sizeof(costStr), "$%.2f",  t.cost);
    else if (t.cost >= 1.0f)  snprintf(costStr, sizeof(costStr), "$%.3f",  t.cost);
    else                      snprintf(costStr, sizeof(costStr), "$%.4f",  t.cost);
    display.setTextColor(color);
    display.setTextSize(4);
    printCentered(0, LCD_HEIGHT, 32, costStr);

    // ── Tokens (Y=74) ──
    char tokStr[16];
    if (t.tokens >= 1000)
        snprintf(tokStr, sizeof(tokStr), "%.1fK tokens", t.tokens / 1000.0f);
    else
        snprintf(tokStr, sizeof(tokStr), "%d tokens", t.tokens);
    display.setTextColor(C_DIM);
    display.setTextSize(2);
    printCentered(0, LCD_HEIGHT, 68, tokStr);

    // ── Fleet share % (Y=100) ──
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    if (totalCost > 0.0f) {
        char shareStr[24];
        snprintf(shareStr, sizeof(shareStr), "%.0f%% of fleet spend",
                 t.cost / totalCost * 100.0f);
        printCentered(0, LCD_HEIGHT, 88, shareStr);
    } else {
        printCentered(0, LCD_HEIGHT, 88, "--");  // no data yet (Font0 is ASCII only)
    }

    // ── Share bar (Y=100) ──
    display.fillRect(8, 100, LCD_HEIGHT - 16, 6, C_DIVIDER);
    if (totalCost > 0.0f) {
        int fillW = (int)((LCD_HEIGHT - 16.0f) * (t.cost / totalCost));
        if (fillW > 0) display.fillRect(8, 100, fillW, 6, color);
    }

    // ── Prev/next hints (Y=112) ──
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    if (prevName) {
        display.setCursor(8, 112);
        display.print(prevName);
    }
    display.setTextColor(C_FG);
    printCentered(0, LCD_HEIGHT, 112, "> NOW <");
    if (nextName) {
        int nW = (int)strlen(nextName) * 6;
        display.setTextColor(C_DIM);
        display.setCursor(LCD_HEIGHT - 4 - nW, 112);
        display.print(nextName);
    }

    // ── Advance hint (Y=125) ──
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    printCentered(0, LCD_HEIGHT, 125, "hold IO14 to advance");
}

// ── Display loop: idle on main, then auto-cycle tiers ──────────────────────
void runDisplayLoop(const UsageData &d) {
    pinMode(PIN_BTN1, INPUT_PULLUP);  // IO0 — has internal pullup
    pinMode(PIN_BTN2, INPUT);          // GPIO35 — input-only, external pullup on PCB

    const char    *tierNames[]  = { "SONNET", "HAIKU", "OPUS", "OTHER" };
    const TierStats *tiers[]    = { &d.sonnet, &d.haiku, &d.opus, &d.other };
    const uint32_t  tierColors[]= { C_TIER_SONNET, C_TIER_HAIKU,
                                    C_TIER_OPUS,   C_TIER_OTHER };

    // Wait on main screen for MAIN_IDLE_MS (10s) or IO14 press
    uint32_t idleStart = millis();
    while (millis() - idleStart < MAIN_IDLE_MS) {
        // IO0 long-press: manual refresh
        if (digitalRead(PIN_BTN1) == LOW) {
            uint32_t held = millis();
            while (digitalRead(PIN_BTN1) == LOW) {
                if (millis() - held > 2000) { ESP.restart(); }
                delay(50);
            }
        }
        // IO14 press: jump to cycle immediately
        if (digitalRead(PIN_BTN2) == LOW) break;
        delay(50);
    }

    // Fleet total for share bar
    float fleetTotal = d.todayCost > 0.0f ? d.todayCost : 1.0f;

    // Cycle through all 4 tiers
    for (int i = 0; i < 4; i++) {
        const char *prev = (i > 0)     ? tierNames[i - 1] : nullptr;
        const char *next = (i < 3)     ? tierNames[i + 1] : nullptr;
        drawTierScreen(tierNames[i], *tiers[i], tierColors[i],
                       i, fleetTotal, prev, next);
        uint32_t dwellStart = millis();
        while (millis() - dwellStart < TIER_DWELL_MS) {
            if (digitalRead(PIN_BTN1) == LOW) {
                uint32_t held = millis();
                while (digitalRead(PIN_BTN1) == LOW) {
                    if (millis() - held > 2000) { ESP.restart(); }
                    delay(50);
                }
            }
            if (digitalRead(PIN_BTN2) == LOW) { delay(200); break; }  // advance
            delay(50);
        }
    }
    // ── Claude subscription screen ──
    ClaudeUsage claude;
    fetchClaudeUsage(claude);
    drawClaudeScreen(claude);
    uint32_t claudeStart = millis();
    while (millis() - claudeStart < CLAUDE_DWELL_MS) {
        if (digitalRead(PIN_BTN1) == LOW) {
            uint32_t held = millis();
            while (digitalRead(PIN_BTN1) == LOW) {
                if (millis() - held > 2000) { ESP.restart(); }
                delay(50);
            }
        }
        if (digitalRead(PIN_BTN2) == LOW) { delay(200); break; }
        delay(50);
    }

    // Brief return to main before sleep
    delay(1000);
}

// ── Claude subscription usage: fetch + display ─────────────────────────────────
static void printClaudeCentered(int y, const char *str) {
    int strW = display.textWidth(str);
    display.setCursor((LCD_HEIGHT - strW) / 2, y);
    display.print(str);
}

bool fetchClaudeUsage(ClaudeUsage &out) {
    HTTPClient http;
    http.begin(CLAUDE_USAGE_URL);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[claude] HTTP error: %d\n", code);
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[claude] JSON parse error: %s\n", err.c_str());
        return false;
    }
    // Endpoint: GET /api/claude/usage
    // Fields: claude5h_pct, claude7d_pct, resets_in (opt), reset_at (opt)
    out.session_pct = doc["claude5h_pct"] | -1;
    out.weekly_pct  = doc["claude7d_pct"] | -1;
    const char *ri  = doc["resets_in"] | "";   // human-readable e.g. "2h 14m"
    strlcpy(out.session_resets_in, ri, sizeof(out.session_resets_in));
    out.valid = (out.session_pct >= 0);
    Serial.printf("[claude] session=%d%% weekly=%d%% resets=%s\n",
                  out.session_pct, out.weekly_pct, out.session_resets_in);
    return out.valid;
}

void drawClaudeScreen(const ClaudeUsage &c) {
    display.fillScreen(C_BG);

    // ── Header bar ──
    display.fillRect(0, 0, LCD_HEIGHT, 26, 0x111111);
    display.setTextColor(C_CLAUDE_BRAND);
    display.setTextSize(3);
    display.setCursor(8, 2);
    display.print("CLAUDE");
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(LCD_HEIGHT - 46, 8);
    display.print("PLAN USAGE");
    display.fillRect(0, 26, LCD_HEIGHT, 2, C_DIVIDER);

    if (!c.valid) {
        display.setTextColor(TFT_RED);
        display.setTextSize(2);
        printClaudeCentered(54, "UNAVAILABLE");
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        printClaudeCentered(80, "Start claude-usage-server");
        printClaudeCentered(92, "on your MacBook");
        return;
    }

    // ── 5-hour session block ──
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    printClaudeCentered(32, "5-HOUR SESSION");

    char sessionStr[8];
    snprintf(sessionStr, sizeof(sessionStr), "%d%%", c.session_pct);
    uint32_t sessColor = (c.session_pct >= 80) ? C_COST_ALERT :
                         (c.session_pct >= 50) ? C_COST_WARN  : C_CLAUDE_BRAND;
    display.setTextColor(sessColor);
    display.setTextSize(4);
    printClaudeCentered(42, sessionStr);

    // Session gauge bar
    const int gX = 16, gW = LCD_HEIGHT - 32, gY = 84, gH = 8;
    display.fillRect(gX, gY, gW, gH, C_GAUGE_BG);
    int fill = (int)(gW * constrain(c.session_pct, 0, 100) / 100.0f);
    if (fill > 0) display.fillRect(gX, gY, fill, gH, sessColor);

    if (c.session_resets_in[0]) {
        char resetStr[24];
        snprintf(resetStr, sizeof(resetStr), "resets in %s", c.session_resets_in);
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        printClaudeCentered(96, resetStr);
    }

    // ── Weekly block ──
    display.fillRect(8, 107, LCD_HEIGHT - 16, 1, C_DIVIDER);
    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 111);
    display.print("7-DAY");
    char weeklyStr[8];
    snprintf(weeklyStr, sizeof(weeklyStr), "%d%%", c.weekly_pct);
    display.setTextColor(C_FG);
    display.setTextSize(2);
    display.setCursor(52, 108);
    display.print(weeklyStr);

    // Weekly mini-gauge
    const int wX = 104, wW = LCD_HEIGHT - 112, wY = 115, wH = 5;
    display.fillRect(wX, wY, wW, wH, C_GAUGE_BG);
    int wfill = (int)(wW * constrain(c.weekly_pct, 0, 100) / 100.0f);
    if (wfill > 0) display.fillRect(wX, wY, wfill, wH, C_FG);
}

void drawError(const char *msg) {
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_RED);
    display.setTextSize(2);
    display.setCursor(10, 60);
    display.print("ERROR");
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(10, 90);
    display.print(msg);
    delay(3000);
}

// ── Battery voltage → percentage ─────────────────────────────────────────────
float readBatteryPct() {
    // GPIO4 reads battery voltage via internal voltage divider (÷2)
    // ADC ref 3.3V, 12-bit (0–4095)
    // Full LiPo: 4.2V → ~2.1V at pin → ~2607 raw
    // Empty:     3.0V → ~1.5V at pin → ~1862 raw
    int raw = analogRead(PIN_BAT_ADC);
    Serial.printf("[bat] raw ADC=%d\n", raw);

    // No battery connected: ADC floats near 0
    // Stub to 100% so display doesn't show garbage during bench testing
    if (raw < 500) {
        Serial.println("[bat] no battery detected — stubbing to 100%");
        return 100.0f;
    }

    float vPin = (raw / 4095.0f) * 3.3f;
    float vBat = vPin * 2.0f;
    float pct  = (vBat - 3.0f) / (4.2f - 3.0f) * 100.0f;
    return constrain(pct, 0.0f, 100.0f);
}

// ── Color based on spend threshold ───────────────────────────────────────────
uint32_t costColor(float cost) {
    if (cost >= COST_ALERT)  return C_COST_ALERT;
    if (cost >= COST_WARN)   return C_COST_WARN;
    return C_COST_OK;
}

// ── Deep sleep ────────────────────────────────────────────────────────────────
bool checkManualRefresh() {
    pinMode(PIN_BTN1, INPUT_PULLUP);
    if (digitalRead(PIN_BTN1) != LOW) return false;

    display.setTextSize(1);
    display.setTextColor(TFT_CYAN);
    display.setCursor(4, LCD_WIDTH - 22);
    display.print("Hold 2s to refresh...");

    uint32_t pressStart = millis();
    while (digitalRead(PIN_BTN1) == LOW) {
        if (millis() - pressStart > 2000) {
            Serial.println("[btn] long-press - manual refresh");
            return true;
        }
        delay(50);
    }
    return false;
}

void goSleep() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    display.setBrightness(0);
    if (PIN_POWER_ON >= 0) digitalWrite(PIN_POWER_ON, LOW);

    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * uS_PER_MIN);
    Serial.printf("[sleep] going down for %d min\n", SLEEP_MINUTES);
    Serial.flush();
    esp_deep_sleep_start();
}
