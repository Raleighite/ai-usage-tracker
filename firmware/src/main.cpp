/*
 * AI Usage Tracker — Firmware MVP
 * Hardware: LILYGO T-Display-S3
 * Endpoint: GET http://192.168.0.203:8030/api/usage
 *
 * Build with PlatformIO: python3 -m platformio run -e t_display
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiProv.h>
#include <Preferences.h>
#include <WebServer.h>
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
    int  sonnet_weekly_pct    = -1;
    int  total_weekly_pct     = -1;
    char session_resets_in[12]       = {};
    char sonnet_weekly_resets_in[12] = {};
    char total_weekly_resets_in[12]  = {};
    bool valid                = false;
};

struct CodexUsage {
    int  session_pct  = -1;  // -1 = unavailable
    char session_resets_in[12] = {};
    int  turns_5h     = 0;
    int  sessions_5h  = 0;
    int  tokens_5h    = 0;
    int  turns_24h    = 0;
    int  sessions_24h = 0;
    int  tokens_24h   = 0;
    int  turns_7d     = 0;
    int  sessions_7d  = 0;
    int  tokens_7d    = 0;
    bool valid         = false;
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

// ── Display view enum ────────────────────────────────────────────────────────
// BTN2 press cycles 1→4; AUTO restores after MANUAL_VIEW_TIMEOUT_MS inactivity
enum DisplayView {
    VIEW_AUTO           = 0,  // Smart: auto-picks most relevant screen
    VIEW_CLAUDE_SESSION = 1,  // Claude 5h session %
    VIEW_CODEX_SESSION  = 2,  // Codex 5h session %
    VIEW_SONNET_WEEKLY  = 3,  // Claude Sonnet 7d %
    VIEW_CLAUDE_WEEKLY  = 4,  // Claude total 7d %
    VIEW_COUNT          = 5
};

#define MANUAL_VIEW_TIMEOUT_MS  30000   // return to AUTO after 30s without input

// ── Persistent across restarts ────────────────────────────────────────────────
RTC_DATA_ATTR int bootCount = 0;
static volatile bool g_wifiReady = false;
static volatile bool g_provError = false;

// Dynamic server config (loaded from NVS on boot, set via config portal)
static char g_serverHost[64]  = SERVER_HOST;
static int  g_serverPort      = SERVER_PORT;
static char g_usageUrl[128]   = {};
static char g_claudeUrl[128]  = {};
static char g_codexUrl[128]   = {};

// ── Forward declarations ──────────────────────────────────────────────────────
bool     connectWifi();
bool     fetchUsage(UsageData &out);
void     drawBoot();
void     drawSummary(const UsageData &d, float batPct);
void     drawTierScreen(const char *name, const TierStats &t, uint32_t color,
                        int idx, float totalCost,
                        const char *prevName, const char *nextName);
void     runDisplayLoop(const ClaudeUsage &claude, const CodexUsage &codex);
bool     fetchClaudeUsage(ClaudeUsage &out);
void     drawClaudeScreen(const ClaudeUsage &c);
void     holdClaudeScreen();
bool     fetchCodexUsage(CodexUsage &out);
void     drawCodexScreen(const CodexUsage &c);
void     drawStatusScreen(const ClaudeUsage &claude, const CodexUsage &codex);
void     drawClaudeSessionView(const ClaudeUsage &c);
void     drawCodexSessionView(const CodexUsage &c);
void     drawSonnetWeeklyView(const ClaudeUsage &c);
void     drawClaudeWeeklyView(const ClaudeUsage &c);
void     drawError(const char *msg);
float    readBatteryPct();
uint32_t costColor(float cost);
bool     checkManualRefresh();  // long-press IO0 restarts the device
// Static helpers forward-declared so view-draw functions can call them
// regardless of definition order in this file.
static void     printClaudeCentered(int y, const char *str);
static uint32_t quotaColor(int pct);
static void     drawLargeQuotaBar(int pct, uint32_t color);
static void     drawCountdownLine(int y, const char *label, const char *resetText);
void     drawProvisioningScreen(const char *status = "Advertising over BLE...");
void     SysProvEvent(arduino_event_t *sys_event);
void     loadServerConfig();
void     saveServerConfig(const char *host, int port);
bool     isServerConfigured();
void     drawConfigPortalScreen(const char *ip, int secsLeft);
void     startConfigPortal();

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

    // Hold BOOT button during boot = force re-provisioning (clears saved WiFi creds)
    pinMode(PIN_BTN1, INPUT_PULLUP);
    bool forceReprovision = (digitalRead(PIN_BTN1) == LOW);
    if (forceReprovision) {
        Serial.println("[prov] BOOT held — re-provisioning");
        Preferences p; p.begin("srv_cfg", false); p.clear(); p.end();
    }

    // Start BLE provisioning (non-blocking).
    // Already provisioned: connects silently in <3s.
    // First boot or forced: advertises over BLE; open ESP BLE Provisioning app.
    WiFi.onEvent(SysProvEvent);
    WiFiProv.beginProvision(
        WIFI_PROV_SCHEME_BLE,
        WIFI_PROV_SCHEME_HANDLER_FREE_BTDM,
        WIFI_PROV_SECURITY_1,
        BLE_POP_PIN,
        BLE_DEVICE_NAME,
        nullptr, nullptr,
        forceReprovision
    );

    // Wait for connection; show provisioning screen if BLE advertising kicked in
    {
        uint32_t t0 = millis();
        bool provScreen = false;
        while (WiFi.status() != WL_CONNECTED) {
            if (g_provError) {
                drawError("WiFi auth failed");
                delay(ERROR_RETRY_MS);
                ESP.restart();
            }
            if (!provScreen && millis() - t0 > 2000) {
                drawProvisioningScreen();
                provScreen = true;
            }
            delay(100);
        }
        if (provScreen) {
            drawProvisioningScreen("Connected!");
            delay(800);
        }
    }
    delay(500);  // let BT memory cleanup settle
    loadServerConfig();
    if (!isServerConfigured()) startConfigPortal();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
        drawError("WiFi failed");
        delay(ERROR_RETRY_MS);
        return;
    }

    UsageData usage;
    if (!fetchUsage(usage)) {
        drawError("Summary failed");
        delay(ERROR_RETRY_MS);
        return;
    }

    ClaudeUsage claude;
    if (!fetchClaudeUsage(claude)) {
        drawError("Claude failed");
        delay(ERROR_RETRY_MS);
        return;
    }

    CodexUsage codex;
    if (!fetchCodexUsage(codex)) {
        Serial.println("[codex] unavailable; status screen will fall back if needed");
    }

    runDisplayLoop(claude, codex);
}

// ── BLE Provisioning ───────────────────────────────────────────────────────────
void SysProvEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            g_wifiReady = true;
            Serial.printf("[prov] WiFi connected: %s\n",
                          WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_PROV_START:
            Serial.println("[prov] BLE advertising");
            break;
        case ARDUINO_EVENT_PROV_CRED_RECV:
            Serial.println("[prov] Credentials received from app");
            break;
        case ARDUINO_EVENT_PROV_CRED_FAIL:
            Serial.println("[prov] Credential validation failed");
            g_provError = true;
            break;
        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            Serial.println("[prov] Credentials saved to NVS");
            break;
        case ARDUINO_EVENT_PROV_END:
            Serial.println("[prov] Provisioning complete");
            break;
        default:
            break;
    }
}

void drawProvisioningScreen(const char *status) {
    display.fillScreen(C_BG);

    display.setTextColor(C_CLAUDE_BRAND);
    display.setTextSize(2);
    display.setCursor(8, 4);
    display.print("WIFI SETUP");
    display.fillRect(0, 24, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 28);
    display.print("App: ESP BLE Provisioning");
    display.setTextColor(C_FG);
    display.setCursor(8, 38);
    display.print("Device: " BLE_DEVICE_NAME);

    display.fillRect(0, 50, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_DIM);
    display.setTextSize(1);
    int labelW = display.textWidth("ENTER PIN");
    display.setCursor((LCD_HEIGHT - labelW) / 2, 54);
    display.print("ENTER PIN");

    display.setTextSize(4);
    display.setTextColor(C_FG);
    int pinW = display.textWidth(BLE_POP_PIN);
    display.setCursor((LCD_HEIGHT - pinW) / 2, 64);
    display.print(BLE_POP_PIN);

    display.fillRect(0, 100, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_TOKENS);
    display.setTextSize(1);
    int sw = display.textWidth(status);
    display.setCursor((LCD_HEIGHT - sw) / 2, 104);
    display.print(status);

    display.setTextColor(C_DIM);
    display.setCursor(8, 120);
    display.print("Hold BOOT at startup = reprovision");
}

// ── Server config (NVS + HTTP config portal) ───────────────────────────────
void loadServerConfig() {
    Preferences prefs;
    prefs.begin("srv_cfg", true);
    String host = prefs.getString("host", SERVER_HOST);
    int    port = prefs.getInt("port", SERVER_PORT);
    prefs.end();
    strlcpy(g_serverHost, host.c_str(), sizeof(g_serverHost));
    g_serverPort = port;
    snprintf(g_usageUrl,  sizeof(g_usageUrl),  "http://%s:%d/api/usage/summary", g_serverHost, g_serverPort);
    snprintf(g_claudeUrl, sizeof(g_claudeUrl), "http://%s:%d/api/claude/usage",  g_serverHost, g_serverPort);
    snprintf(g_codexUrl,  sizeof(g_codexUrl),  "http://%s:%d/api/codex/usage",   g_serverHost, g_serverPort);
    Serial.printf("[cfg] server: %s:%d\n", g_serverHost, g_serverPort);
}

void saveServerConfig(const char *host, int port) {
    Preferences prefs;
    prefs.begin("srv_cfg", false);
    prefs.putString("host", host);
    prefs.putInt("port", port);
    prefs.putBool("ok", true);
    prefs.end();
    Serial.printf("[cfg] saved server: %s:%d\n", host, port);
}

bool isServerConfigured() {
    Preferences prefs;
    prefs.begin("srv_cfg", true);
    bool val = prefs.getBool("ok", false);
    prefs.end();
    return val;
}

void drawConfigPortalScreen(const char *ip, int secsLeft) {
    display.fillScreen(C_BG);
    display.setTextColor(C_CLAUDE_BRAND);
    display.setTextSize(2);
    display.setCursor(8, 4);
    display.print("SERVER SETUP");
    display.fillRect(0, 24, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 28);
    display.print("Open in browser:");

    char urlStr[32];
    snprintf(urlStr, sizeof(urlStr), "http://%s", ip);
    display.setTextColor(C_FG);
    display.setTextSize(1);
    int urlW = display.textWidth(urlStr);
    display.setCursor((LCD_HEIGHT - urlW) / 2, 40);
    display.print(urlStr);

    display.fillRect(0, 54, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_DIM);
    display.setTextSize(1);
    display.setCursor(8, 58);
    display.print("Set server host + port,");
    display.setCursor(8, 68);
    display.print("then click Save.");

    display.fillRect(0, 82, LCD_HEIGHT, 1, C_DIVIDER);

    display.setTextColor(C_DIM);
    display.setCursor(8, 86);
    display.print("Default:");
    char defStr[48];
    snprintf(defStr, sizeof(defStr), "%s:%d", g_serverHost, g_serverPort);
    display.setTextColor(C_TOKENS);
    display.setCursor(8, 96);
    display.print(defStr);

    char countdown[20];
    snprintf(countdown, sizeof(countdown), "Closes in %ds", secsLeft);
    display.setTextColor(C_DIM);
    display.setCursor(8, 118);
    display.print(countdown);
}

void startConfigPortal() {
    WebServer srv(80);
    bool configured = false;
    String ip = WiFi.localIP().toString();
    drawConfigPortalScreen(ip.c_str(), 60);

    srv.on("/", HTTP_GET, [&]() {
        String page = "<!DOCTYPE html><html><head><title>AI Tracker Setup</title>"
            "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px;}"
            "label{display:block;margin-top:12px;font-size:14px;color:#888;}"
            "input{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;font-size:16px;}"
            "button{background:#E5541B;color:#fff;border:none;padding:12px;width:100%;"
            "font-size:16px;margin-top:16px;cursor:pointer;}"
            "</style></head><body>"
            "<h2>&#9889; AI Usage Tracker</h2>"
            "<p>Enter your backend server address, then click Save.</p>"
            "<form method='POST' action='/save'>"
            "<label>Server Host (IP or hostname)</label>"
            "<input name='host' value='" + String(g_serverHost) + "' placeholder='192.168.0.203'>"
            "<label>Server Port</label>"
            "<input name='port' value='" + String(g_serverPort) + "' placeholder='8030'>"
            "<button type='submit'>Save &amp; Restart</button>"
            "</form></body></html>";
        srv.send(200, "text/html", page);
    });

    srv.on("/save", HTTP_POST, [&]() {
        String host = srv.arg("host");
        int    port = srv.arg("port").toInt();
        if (host.length() > 0 && port > 0 && port < 65536) {
            saveServerConfig(host.c_str(), port);
            srv.send(200, "text/html",
                "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:400px;"
                "margin:40px auto;padding:20px;'>"
                "<h2>&#9989; Saved!</h2><p>Device is restarting...</p></body></html>");
            configured = true;
        } else {
            srv.send(400, "text/plain", "Invalid host or port");
        }
    });

    srv.begin();
    uint32_t deadline = millis() + 60000;
    int lastSec = 60;
    while (!configured && millis() < deadline) {
        srv.handleClient();
        int secsLeft = (int)((deadline - millis()) / 1000);
        if (secsLeft != lastSec) {
            lastSec = secsLeft;
            drawConfigPortalScreen(ip.c_str(), secsLeft);
        }
        delay(10);
    }
    srv.stop();
    if (configured) { delay(1200); ESP.restart(); }
    // Timeout: proceed with defaults
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
bool connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();  // use provisioned credentials from NVS

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
    http.begin(g_usageUrl);
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

// ── View mode indicator: small label at top-right corner ────────────────────
static const char *VIEW_LABELS[] = {
    "AUTO", "CLAUDE5H", "CODEX5H", "SONNET7D", "TOTAL7D"
};

static void drawViewIndicator(DisplayView view) {
    const char *lbl = VIEW_LABELS[(int)view];
    display.setTextSize(1);
    display.setTextColor(view == VIEW_AUTO ? C_DIM : C_TOKENS);
    int lblW = display.textWidth(lbl);
    display.fillRect(LCD_HEIGHT - lblW - 5, 0, lblW + 4, 10, C_BG);
    display.setCursor(LCD_HEIGHT - lblW - 3, 1);
    display.print(lbl);
}

// ── Single focused metric: big % + progress bar + reset countdown ─────────────
static void drawFocusedMetric(const char *title, int pct, uint32_t color,
                               const char *resetLabel, const char *resetTime) {
    display.setTextSize(2);
    display.setTextColor(color);
    display.setCursor(8, 6);
    display.print(title);

    char pctStr[10];
    if (pct >= 0) snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
    else          strlcpy(pctStr, "--", sizeof(pctStr));
    display.setTextSize(4);
    display.setTextColor(C_FG);
    printClaudeCentered(30, pctStr);

    drawLargeQuotaBar(pct, color);
    drawCountdownLine(100, resetLabel, resetTime);
}

// ── Per-view draw functions ───────────────────────────────────────────────────
void drawClaudeSessionView(const ClaudeUsage &c) {
    display.fillScreen(C_BG);
    if (!c.valid) {
        display.setTextColor(TFT_RED); display.setTextSize(2);
        printClaudeCentered(54, "CLAUDE OFFLINE"); return;
    }
    drawFocusedMetric("CLAUDE SESSION",
                      c.session_pct, quotaColor(c.session_pct),
                      "RESET", c.session_resets_in);
}

void drawCodexSessionView(const CodexUsage &c) {
    display.fillScreen(C_BG);
    if (!c.valid) {
        display.setTextColor(TFT_RED); display.setTextSize(2);
        printClaudeCentered(54, "CODEX OFFLINE");
        display.setTextColor(C_DIM); display.setTextSize(1);
        printClaudeCentered(85, "codex-usage-server offline"); return;
    }
    drawFocusedMetric("CODEX SESSION",
                      c.session_pct, C_CODEX_BRAND,
                      "RESET", c.session_resets_in);
}

void drawSonnetWeeklyView(const ClaudeUsage &c) {
    display.fillScreen(C_BG);
    if (!c.valid) {
        display.setTextColor(TFT_RED); display.setTextSize(2);
        printClaudeCentered(54, "CLAUDE OFFLINE"); return;
    }
    drawFocusedMetric("SONNET 7D",
                      c.sonnet_weekly_pct, quotaColor(c.sonnet_weekly_pct),
                      "RESET", c.sonnet_weekly_resets_in);
}

void drawClaudeWeeklyView(const ClaudeUsage &c) {
    display.fillScreen(C_BG);
    if (!c.valid) {
        display.setTextColor(TFT_RED); display.setTextSize(2);
        printClaudeCentered(54, "CLAUDE OFFLINE"); return;
    }
    drawFocusedMetric("TOTAL 7D",
                      c.total_weekly_pct, quotaColor(c.total_weekly_pct),
                      "RESET", c.total_weekly_resets_in);
}

// ── Display loop: smart auto-screen + BTN2 manual view cycling ────────────────
//
// BTN2 (GPIO35) short press → cycles: Claude 5h → Codex 5h → Sonnet 7d →
//   Total 7d → wraps back to Claude 5h.
// AUTO mode restores after MANUAL_VIEW_TIMEOUT_MS of inactivity.
// BTN1 (IO0) long-press (2s) → hard restart.
void runDisplayLoop(const ClaudeUsage &claude, const CodexUsage &codex) {
    static DisplayView currentView = VIEW_AUTO;
    static uint32_t    lastInputMs = 0;

    pinMode(PIN_BTN1, INPUT_PULLUP);  // IO0  — internal pullup
    pinMode(PIN_BTN2, INPUT);          // GPIO35 — input-only

    // Return to AUTO after inactivity timeout
    if (currentView != VIEW_AUTO && millis() - lastInputMs > MANUAL_VIEW_TIMEOUT_MS) {
        currentView = VIEW_AUTO;
    }

    // Draw selected view
    switch (currentView) {
        case VIEW_CLAUDE_SESSION: drawClaudeSessionView(claude); break;
        case VIEW_CODEX_SESSION:  drawCodexSessionView(codex);  break;
        case VIEW_SONNET_WEEKLY:  drawSonnetWeeklyView(claude); break;
        case VIEW_CLAUDE_WEEKLY:  drawClaudeWeeklyView(claude); break;
        default:                  drawStatusScreen(claude, codex); break;
    }
    drawViewIndicator(currentView);

    // Button dwell loop
    uint32_t screenStart = millis();
    while (millis() - screenStart < STATUS_DWELL_MS) {
        // BTN1 (IO0): long-press 2s → restart
        if (digitalRead(PIN_BTN1) == LOW) {
            uint32_t held = millis();
            while (digitalRead(PIN_BTN1) == LOW) {
                if (millis() - held > 2000) { ESP.restart(); }
                delay(50);
            }
        }

        // BTN2 (GPIO35): short press → advance to next manual view
        if (digitalRead(PIN_BTN2) == LOW) {
            delay(50);                                   // debounce
            while (digitalRead(PIN_BTN2) == LOW) delay(20);  // wait for release
            lastInputMs = millis();
            if (currentView == VIEW_AUTO || (int)currentView >= VIEW_COUNT - 1) {
                currentView = VIEW_CLAUDE_SESSION;
            } else {
                currentView = (DisplayView)((int)currentView + 1);
            }
            break;  // redraw immediately with new view
        }
        delay(50);
    }
    // loop() will re-fetch data and call runDisplayLoop() again
}

// ── Claude subscription usage: fetch + display ─────────────────────────────────
static void printClaudeCentered(int y, const char *str) {
    int strW = display.textWidth(str);
    display.setCursor((LCD_HEIGHT - strW) / 2, y);
    display.print(str);
}

bool fetchClaudeUsage(ClaudeUsage &out) {
    HTTPClient http;
    http.begin(g_claudeUrl);
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
    // Fields used: session, Sonnet 7-day, and total 7-day utilization + reset countdowns.
    out.session_pct       = doc["claude5h_pct"] | -1;
    out.sonnet_weekly_pct = doc["sonnet7d_pct"] | -1;
    out.total_weekly_pct  = doc["total7d_pct"]  | (doc["claude7d_pct"] | -1);

    const char *sessionReset = doc["resets_in"] | "";
    const char *sonnetReset  = doc["sonnet7d_resets_in"] | "";
    const char *totalReset   = doc["total7d_resets_in"]  | "";
    strlcpy(out.session_resets_in, sessionReset, sizeof(out.session_resets_in));
    strlcpy(out.sonnet_weekly_resets_in, sonnetReset, sizeof(out.sonnet_weekly_resets_in));
    strlcpy(out.total_weekly_resets_in, totalReset, sizeof(out.total_weekly_resets_in));
    out.valid = (out.session_pct >= 0);
    Serial.printf("[claude] session=%d%% reset=%s sonnet7d=%d%% reset=%s total7d=%d%% reset=%s\n",
                  out.session_pct, out.session_resets_in,
                  out.sonnet_weekly_pct, out.sonnet_weekly_resets_in,
                  out.total_weekly_pct, out.total_weekly_resets_in);
    return out.valid;
}

static uint32_t quotaColor(int pct) {
    if (pct >= 90) return C_COST_ALERT;
    if (pct >= 70) return C_COST_WARN;
    return C_CLAUDE_BRAND;
}

static void drawQuotaRow(int y, const char *label, int pct, const char *resetText) {
    const int x = 8;
    const int w = LCD_HEIGHT - 16;
    const int barY = y + 24;
    const int barH = 10;

    display.setTextSize(1);
    display.setTextColor(C_DIM);
    display.setCursor(x, y);
    display.print(label);

    char pctStr[8];
    if (pct >= 0) snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
    else strlcpy(pctStr, "--", sizeof(pctStr));

    display.setTextSize(2);
    display.setTextColor(C_FG);
    int pctW = display.textWidth(pctStr);
    display.setCursor(x, y + 10);
    display.print(pctStr);

    display.setTextSize(1);
    display.setTextColor(C_DIM);
    const char *reset = resetText && resetText[0] ? resetText : "--";
    char resetStr[24];
    snprintf(resetStr, sizeof(resetStr), "reset %s", reset);
    int resetW = display.textWidth(resetStr);
    display.setCursor(LCD_HEIGHT - x - resetW, y + 15);
    display.print(resetStr);

    display.fillRect(x, barY, w, barH, C_GAUGE_BG);
    int fill = (pct >= 0) ? (int)(w * constrain(pct, 0, 100) / 100.0f) : 0;
    if (fill > 0) display.fillRect(x, barY, fill, barH, quotaColor(pct));
}

void drawClaudeScreen(const ClaudeUsage &c) {
    display.fillScreen(C_BG);

    if (!c.valid) {
        display.setTextColor(TFT_RED);
        display.setTextSize(2);
        printClaudeCentered(54, "UNAVAILABLE");
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        printClaudeCentered(85, "claude-usage-server offline");
        return;
    }

    drawQuotaRow(4,  "SESSION",    c.session_pct,       c.session_resets_in);
    drawQuotaRow(48, "SONNET 7D",  c.sonnet_weekly_pct, c.sonnet_weekly_resets_in);
    drawQuotaRow(92, "TOTAL 7D",   c.total_weekly_pct,  c.total_weekly_resets_in);
}

void holdClaudeScreen() {
    pinMode(PIN_BTN1, INPUT_PULLUP);
    pinMode(PIN_BTN2, INPUT);

    uint32_t start = millis();
    while (millis() - start < CLAUDE_DWELL_MS) {
        if (digitalRead(PIN_BTN1) == LOW) {
            uint32_t held = millis();
            while (digitalRead(PIN_BTN1) == LOW) {
                if (millis() - held > 2000) { ESP.restart(); }
                delay(50);
            }
        }
        if (digitalRead(PIN_BTN2) == LOW) { ESP.restart(); }
        delay(50);
    }
}

// ── Codex local session usage: fetch + display ───────────────────────────────
bool fetchCodexUsage(CodexUsage &out) {
    HTTPClient http;
    http.begin(g_codexUrl);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[codex] HTTP error: %d\n", code);
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[codex] JSON parse error: %s\n", err.c_str());
        return false;
    }

    out.session_pct = doc["codex5h_pct"] | (doc["codex_session_pct"] | (doc["session_pct"] | -1));
    const char *resetText = doc["codex5h_resets_in"] |
                            (doc["codex_session_resets_in"] |
                             (doc["session_resets_in"] | (doc["resets_in"] | "")));
    strlcpy(out.session_resets_in, resetText, sizeof(out.session_resets_in));

    out.turns_5h     = doc["codex5h_turns"]     | 0;
    out.sessions_5h  = doc["codex5h_sessions"]  | 0;
    out.tokens_5h    = doc["codex5h_tokens"]    | 0;
    out.turns_24h    = doc["codex24h_turns"]    | 0;
    out.sessions_24h = doc["codex24h_sessions"] | 0;
    out.tokens_24h   = doc["codex24h_tokens"]   | 0;
    out.turns_7d     = doc["codex7d_turns"]     | 0;
    out.sessions_7d  = doc["codex7d_sessions"]  | 0;
    out.tokens_7d    = doc["codex7d_tokens"]    | 0;
#if CODEX_SESSION_TOKEN_BUDGET > 0
    if (out.session_pct < 0) {
        out.session_pct = constrain((int)(out.tokens_5h * 100.0f / CODEX_SESSION_TOKEN_BUDGET), 0, 100);
    }
#endif
    out.valid        = true;

    Serial.printf("[codex] session=%d%% reset=%s 5h=%d turns/%d sessions/%d tokens 24h=%d/%d/%d 7d=%d/%d/%d\n",
                  out.session_pct, out.session_resets_in,
                  out.turns_5h, out.sessions_5h, out.tokens_5h,
                  out.turns_24h, out.sessions_24h, out.tokens_24h,
                  out.turns_7d, out.sessions_7d, out.tokens_7d);
    return true;
}

static void formatTokenCount(int tokens, char *buf, size_t len) {
    if (tokens >= 1000000) snprintf(buf, len, "%.1fM", tokens / 1000000.0f);
    else if (tokens >= 1000) snprintf(buf, len, "%.1fK", tokens / 1000.0f);
    else snprintf(buf, len, "%d", tokens);
}

static void drawCodexRow(int y, const char *label, int turns, int sessions, int tokens, int maxTokens) {
    const int x = 8;
    const int w = LCD_HEIGHT - 16;
    const int barY = y + 26;
    const int barH = 8;

    display.setTextSize(1);
    display.setTextColor(C_DIM);
    display.setCursor(x, y);
    display.print(label);

    char tokenStr[12];
    formatTokenCount(tokens, tokenStr, sizeof(tokenStr));
    display.setTextSize(2);
    display.setTextColor(C_FG);
    display.setCursor(x, y + 10);
    display.print(tokenStr);

    char meta[24];
    snprintf(meta, sizeof(meta), "%dt %ds", turns, sessions);
    display.setTextSize(1);
    display.setTextColor(C_DIM);
    int metaW = display.textWidth(meta);
    display.setCursor(LCD_HEIGHT - x - metaW, y + 15);
    display.print(meta);

    display.fillRect(x, barY, w, barH, C_GAUGE_BG);
    int fill = maxTokens > 0 ? (int)(w * constrain(tokens / (float)maxTokens, 0.0f, 1.0f)) : 0;
    if (fill > 0) display.fillRect(x, barY, fill, barH, C_CODEX_BRAND);
}

void drawCodexScreen(const CodexUsage &c) {
    display.fillScreen(C_BG);

    if (!c.valid) {
        display.setTextColor(TFT_RED);
        display.setTextSize(2);
        printClaudeCentered(54, "UNAVAILABLE");
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        printClaudeCentered(85, "codex-usage-server offline");
        return;
    }

    int maxTokens = max(c.tokens_5h, max(c.tokens_24h, c.tokens_7d));
    drawCodexRow(4,  "CODEX 5H",  c.turns_5h,  c.sessions_5h,  c.tokens_5h,  maxTokens);
    drawCodexRow(48, "CODEX 24H", c.turns_24h, c.sessions_24h, c.tokens_24h, maxTokens);
    drawCodexRow(92, "CODEX 7D",  c.turns_7d,  c.sessions_7d,  c.tokens_7d,  maxTokens);
}

static bool quotaMaxed(int pct) {
    return pct >= 100;
}

static void drawLargeQuotaBar(int pct, uint32_t color) {
    const int x = 10;
    const int y = 68;
    const int w = LCD_HEIGHT - 20;
    const int h = 22;

    display.drawRect(x - 1, y - 1, w + 2, h + 2, C_DIVIDER);
    display.fillRect(x, y, w, h, C_GAUGE_BG);
    int fill = pct >= 0 ? (int)(w * constrain(pct, 0, 100) / 100.0f) : 0;
    if (fill > 0) display.fillRect(x, y, fill, h, color);
}

static void drawCountdownLine(int y, const char *label, const char *resetText) {
    const char *reset = resetText && resetText[0] ? resetText : "--";
    char line[42];
    snprintf(line, sizeof(line), "%s %s", label, reset);
    display.setTextSize(2);
    display.setTextColor(C_FG);
    printClaudeCentered(y, line);
}

static void drawMiniCodexContext(const CodexUsage &codex) {
    char tokenStr[12];
    char meta[36];
    formatTokenCount(codex.tokens_5h, tokenStr, sizeof(tokenStr));
    snprintf(meta, sizeof(meta), "%s tok  %dt %ds", tokenStr, codex.turns_5h, codex.sessions_5h);
    display.setTextSize(1);
    display.setTextColor(C_DIM);
    printClaudeCentered(118, meta);
}

void drawStatusScreen(const ClaudeUsage &claude, const CodexUsage &codex) {
    display.fillScreen(C_BG);

    if (!claude.valid) {
        display.setTextColor(TFT_RED);
        display.setTextSize(2);
        printClaudeCentered(54, "CLAUDE OFFLINE");
        display.setTextColor(C_DIM);
        display.setTextSize(1);
        printClaudeCentered(85, "usage endpoint unavailable");
        return;
    }

    bool sonnetWeeklyMaxed = quotaMaxed(claude.sonnet_weekly_pct);
    bool claudeSessionMaxed = quotaMaxed(claude.session_pct);
    bool showCodex = sonnetWeeklyMaxed || claudeSessionMaxed;

    const char *title = showCodex ? "CODEX SESSION" : "CLAUDE SESSION";
    int pct = showCodex ? codex.session_pct : claude.session_pct;
    uint32_t color = showCodex ? C_CODEX_BRAND : quotaColor(pct);

    display.setTextSize(2);
    display.setTextColor(color);
    display.setCursor(8, 6);
    display.print(title);

    char pctStr[10];
    if (pct >= 0) snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
    else strlcpy(pctStr, "--", sizeof(pctStr));
    display.setTextSize(4);
    display.setTextColor(C_FG);
    printClaudeCentered(30, pctStr);

    drawLargeQuotaBar(pct, color);

    if (!showCodex) {
        drawCountdownLine(100, "CLAUDE RESET", claude.session_resets_in);
    } else {
        // Codex reset (upper row)
        if (!codex.valid) {
            display.setTextSize(1);
            display.setTextColor(C_COST_ALERT);
            printClaudeCentered(94, "codex endpoint unavailable");
        } else {
            drawCountdownLine(94, "CODEX RESET", codex.session_resets_in);
        }

        // Divider separating the two countdowns
        display.fillRect(8, 113, LCD_HEIGHT - 16, 1, C_DIVIDER);

        // Claude/Sonnet reset — label left (dim, size 1), value right (bright, size 2)
        const char *claudeLabel = sonnetWeeklyMaxed ? "SONNET RESET" : "CLAUDE RESET";
        const char *claudeReset = sonnetWeeklyMaxed
            ? (claude.sonnet_weekly_resets_in[0] ? claude.sonnet_weekly_resets_in : "--")
            : (claude.session_resets_in[0] ? claude.session_resets_in : "--");

        display.setTextSize(1);
        display.setTextColor(C_DIM);
        display.setCursor(8, 117);
        display.print(claudeLabel);

        display.setTextSize(2);
        display.setTextColor(C_CLAUDE_BRAND);
        int valW = display.textWidth(claudeReset);
        display.setCursor(LCD_HEIGHT - 8 - valW, 114);
        display.print(claudeReset);
    }
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

// ── Manual refresh / restart ─────────────────────────────────────────────────
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
