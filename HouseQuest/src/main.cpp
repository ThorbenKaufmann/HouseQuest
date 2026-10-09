#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <qrcode.h>          // ESP-IDF QR generator (esp_qrcode_*)

// Optional defaults for the very first boot (see include/secrets.h.example).
// After that, WiFi / MQTT / PIN are configured on the device via the setup portal
// and stored in NVS.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST      ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT      1883
#endif
#ifndef MQTT_USER
#define MQTT_USER      ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD  ""
#endif
#ifndef ADMIN_PIN
#define ADMIN_PIN      "1234"
#endif

// ── Admin config ──────────────────────────────────────────────────────────────
#define XP_STEP    5
#define HP_STEP    10

// ── Hardware ──────────────────────────────────────────────────────────────────
// Display variant comes from platformio.ini (one env per kit).
#ifndef HQ_TFT_LED
#define HQ_TFT_LED       15
#endif
#ifndef HQ_TFT_ROTATION
#define HQ_TFT_ROTATION  1
#endif
#define MAX_PLAYERS   16
#define PLAYERS_PER_PAGE 4

// ── Touch calibration ─────────────────────────────────────────────────────────
// Stored in NVS. HQ_TOUCH_CAL (platformio.ini) is the factory default; without
// it, or when a finger is on the screen at boot, the panel calibrates itself.
uint16_t touchCalData[5];
bool     touchCalValid = false;

// ── MQTT / WiFi ───────────────────────────────────────────────────────────────
WiFiClient   espClient;
PubSubClient client(espClient);

// Written by the network task, read by the UI
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
bool dataReceived  = false;

// The network task (core 0) owns `client` while holding mqttMutex. The UI task
// (Arduino loop, core 1) only publishes, with a short timeout, so a slow
// connect / DNS lookup never freezes the touch UI.
SemaphoreHandle_t mqttMutex;
QueueHandle_t     mqttInbox;   // incoming messages → UI task

struct MqttMsg {
  char     topic[64];
  char     payload[400];
  uint16_t len;
};

// Rules web page on port 80. Started / stopped only by the network task;
// the setup portal clears webWanted to get port 80 for itself.
WebServer     web(80);
volatile bool webWanted  = true;
volatile bool webRunning = false;

#define MQTT_RETRY_MIN_MS   2000
#define MQTT_RETRY_MAX_MS  60000
#define WIFI_RETRY_MS      30000
unsigned long mqttRetryDelay = MQTT_RETRY_MIN_MS;

// ── Device config (NVS, editable via setup portal) ────────────────────────────
Preferences prefs;

char     cfgMqttHost[64];
uint16_t cfgMqttPort;
char     cfgMqttUser[32];
char     cfgMqttPass[64];
char     cfgAdminPin[5];

char     deviceId[20];            // "HouseQuest-A1B2C3" — AP name + MQTT client id

#define SETUP_PORTAL_TIMEOUT 300  // s — fallback / on-demand portal

// ── Screen state ──────────────────────────────────────────────────────────────
enum ScreenMode {
  SCREEN_PLAYERS,
  SCREEN_AWARDS,
  SCREEN_ADMIN_PIN,
  SCREEN_ADMIN_SELECT,
  SCREEN_ADMIN_ACTION,
  SCREEN_ADMIN_RULE,
  SCREEN_ADMIN_SETTINGS,
  SCREEN_QR
};
ScreenMode currentScreen = SCREEN_PLAYERS;

int           currentPage    = 0;
unsigned long lastPageSwitch = 0;

// ── Game data ─────────────────────────────────────────────────────────────────
struct Player {
  String name;
  int    xp;
  int    hp;           // >= 0 → resident;  < 0 → guest (penalty)
  String lastAction;   // e.g. "hp-10:2.1.2"  or  "xp+5"
};

struct Award {
  String title;
  String player;
  String duration;
};

Player players[MAX_PLAYERS];
int    playerCount   = 0;
Award  currentAward;
bool   awardAvailable = false;

// ── Global game settings ──────────────────────────────────────────────────────
int gameLevel = 1;   // applies to all players; persisted via MQTT retained

// ── Admin state ───────────────────────────────────────────────────────────────
static char pinInput[5]     = "";
static int  pinLen          = 0;
static bool pinError        = false;
static int  adminTarget     = -1;
static int  adminSelectPage = 0;

// ── Ruleset / By-Rule state ───────────────────────────────────────────────────
bool rulesetAvailable = false;

#define RULE_ID_MAX  12
static char ruleIdInput[RULE_ID_MAX + 1] = "";
static int  ruleIdLen                    = 0;
static bool ruleConfirm                  = false;  // false=entry, true=confirm
static bool ruleNotFound                 = false;

static int  ruleFoundHp                  = 0;
static int  ruleFoundXp                  = 0;
static int  ruleFoundLevel               = 0;
static char ruleFoundText[82]            = "";

// ── Display ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── Forward declarations ──────────────────────────────────────────────────────
void drawScreen();
void drawAdminPin();
void drawAdminSelect();
void drawAdminAction();
void drawAdminRule();
void drawAdminSettings();
void runSetupPortal();
void runTouchCalibration();


// =============================================================================
// WiFi / MQTT
// =============================================================================

void drawStatus(String text)
{
  tft.fillRect(0, 0, 320, 20, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 5);
  tft.print(text);
}

void loadConfig()
{
  prefs.begin("housequest", true);
  strlcpy(cfgMqttHost, prefs.getString("mqtt_host", MQTT_HOST).c_str(),     sizeof(cfgMqttHost));
  cfgMqttPort = prefs.getUShort("mqtt_port", MQTT_PORT);
  strlcpy(cfgMqttUser, prefs.getString("mqtt_user", MQTT_USER).c_str(),     sizeof(cfgMqttUser));
  strlcpy(cfgMqttPass, prefs.getString("mqtt_pass", MQTT_PASSWORD).c_str(), sizeof(cfgMqttPass));
  strlcpy(cfgAdminPin, prefs.getString("admin_pin", ADMIN_PIN).c_str(),     sizeof(cfgAdminPin));

  touchCalValid = prefs.getBytes("touch_cal", touchCalData, sizeof(touchCalData)) == sizeof(touchCalData);
#ifdef HQ_TOUCH_CAL
  if (!touchCalValid) {
    const uint16_t factoryCal[5] = HQ_TOUCH_CAL;
    memcpy(touchCalData, factoryCal, sizeof(touchCalData));
    touchCalValid = true;
  }
#endif
  prefs.end();

  uint32_t nic = (ESP.getEfuseMac() >> 24) & 0xFFFFFF;
  snprintf(deviceId, sizeof(deviceId), "HouseQuest-%06X", nic);
}

void saveConfig()
{
  prefs.begin("housequest", false);
  prefs.putString("mqtt_host", cfgMqttHost);
  prefs.putUShort("mqtt_port", cfgMqttPort);
  prefs.putString("mqtt_user", cfgMqttUser);
  prefs.putString("mqtt_pass", cfgMqttPass);
  prefs.putString("admin_pin", cfgAdminPin);
  prefs.end();
}

// Runs on core 0. Everything that may block (DNS, TCP connect) happens here.
void networkTask(void*)
{
  unsigned long lastMqttAttempt = 0;
  unsigned long wifiLostSince   = 0;

  for (;;) {
    if (webWanted != webRunning) {
      if (webWanted) web.begin(); else web.stop();
      webRunning = webWanted;
    }
    if (webRunning) web.handleClient();

    xSemaphoreTake(mqttMutex, portMAX_DELAY);

    bool wifiUp = WiFi.isConnected();
    if (!wifiUp) {
      // Auto-reconnect normally handles this; kick it only if it stays down.
      if (wifiLostSince == 0) wifiLostSince = millis();
      else if (millis() - wifiLostSince > WIFI_RETRY_MS) { WiFi.begin(); wifiLostSince = millis(); }
    } else {
      wifiLostSince = 0;
    }

    if (wifiUp && cfgMqttHost[0] != '\0') {
      if (client.connected()) {
        client.loop();
      } else if (millis() - lastMqttAttempt >= mqttRetryDelay) {
        lastMqttAttempt = millis();
        bool ok = (cfgMqttUser[0] != '\0')
                    ? client.connect(deviceId, cfgMqttUser, cfgMqttPass)
                    : client.connect(deviceId);
        if (ok) {
          client.subscribe("house/quest/players/+/state");
          client.subscribe("house/quest/settings");
          client.subscribe("house/quest/awards");
          mqttRetryDelay = MQTT_RETRY_MIN_MS;
        } else {
          mqttRetryDelay = min(mqttRetryDelay * 2, (unsigned long)MQTT_RETRY_MAX_MS);
          Serial.printf("MQTT connect failed (rc=%d), retry in %lus\n",
                        client.state(), mqttRetryDelay / 1000);
        }
      }
    }

    wifiConnected = wifiUp;
    mqttConnected = wifiUp && client.connected();
    xSemaphoreGive(mqttMutex);

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Looks the file up per request (serveStatic decides once at registration).
void serveFile(const char* path, const char* type)
{
  fs::File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("Web: %s missing on LittleFS\n", path);
    web.send(404, "text/plain; charset=utf-8",
             String(path) + " fehlt auf dem Panel.\n"
             "Dateisystem hochladen: pio run -t uploadfs");
    return;
  }
  web.sendHeader("Cache-Control", "no-cache");
  web.streamFile(f, type);
  f.close();
}

// Read-only rules page for everyone on the WiFi (files from LittleFS, see data/).
void setupWeb()
{
  web.on("/",           HTTP_GET, [] { serveFile("/index.html", "text/html; charset=utf-8"); });
  web.on("/rules.json", HTTP_GET, [] { serveFile("/rules.json", "application/json"); });
  web.on("/status.json", HTTP_GET, [] {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"level\":%d,\"device\":\"%s\"}", gameLevel, deviceId);
    web.send(200, "application/json", buf);
  });
  web.onNotFound([] {
    Serial.printf("Web: 404 %s\n", web.uri().c_str());
    web.send(404, "text/plain", "Not found");
  });
}

// Called from the UI task. Fails fast instead of waiting for a connect attempt.
bool mqttPublish(const char* topic, const char* payload)
{
  if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = client.connected() &&
            client.publish(topic, (const uint8_t*)payload, strlen(payload), true);
  xSemaphoreGive(mqttMutex);
  return ok;
}


// =============================================================================
// Ruleset — LittleFS
// =============================================================================

void initRuleset()
{
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS: mount failed");
    return;
  }
  rulesetAvailable = LittleFS.exists("/ruleset.json");
  Serial.printf("LittleFS: %u/%u bytes used, index.html %s, rules.json %s\n",
                LittleFS.usedBytes(), LittleFS.totalBytes(),
                LittleFS.exists("/index.html") ? "ok" : "MISSING",
                LittleFS.exists("/rules.json") ? "ok" : "MISSING");
  Serial.printf("Ruleset: %s\n", rulesetAvailable ? "loaded" : "not found");
}

// Scan JSONL file line-by-line — only one line in RAM at a time.
bool getRuleById(const char* id)
{
  if (!rulesetAvailable) return false;

  fs::File f = LittleFS.open("/ruleset.json", "r");
  if (!f) return false;

  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    // Fast pre-check before full JSON parse
    if (line.indexOf(id) < 0) continue;

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;

    if (strcmp(doc["id"] | "", id) == 0) {
      ruleFoundHp    = doc["hp"] | 10;
      ruleFoundXp    = doc["xp"] | 0;
      ruleFoundLevel = doc["l"]  | 1;
      strncpy(ruleFoundText, doc["t"] | "?", sizeof(ruleFoundText) - 1);
      ruleFoundText[sizeof(ruleFoundText) - 1] = '\0';
      found = true;
      break;
    }
  }
  f.close();
  return found;
}


// =============================================================================
// Player state
// =============================================================================

int findOrCreatePlayer(String name)
{
  for (int i = 0; i < playerCount; i++)
    if (players[i].name == name) return i;
  if (playerCount < MAX_PLAYERS) {
    players[playerCount].name = name;
    playerCount++;
    return playerCount - 1;
  }
  return -1;
}

bool publishPlayerState(int idx)
{
  if (idx < 0 || idx >= playerCount) return false;
  Player &p  = players[idx];
  String key = p.name;
  key.toLowerCase();
  String topic = "house/quest/players/" + key + "/state";

  char payload[256];
  JsonDocument doc;
  doc["name"]        = p.name;
  doc["xp"]          = p.xp;
  doc["hp"]          = p.hp;
  doc["role"]        = (p.hp >= 0) ? "resident" : "guest";
  doc["last_action"] = p.lastAction;
  serializeJson(doc, payload, sizeof(payload));

  return mqttPublish(topic.c_str(), payload);
}

void publishGameSettings()
{
  char payload[64];
  JsonDocument doc;
  doc["level"] = gameLevel;
  serializeJson(doc, payload, sizeof(payload));
  mqttPublish("house/quest/settings", payload);
}

// Network task context: just hand the message over to the UI task.
void callback(char* topic, byte* payload, unsigned int length)
{
  MqttMsg msg;
  if (length > sizeof(msg.payload)) return;
  strlcpy(msg.topic, topic, sizeof(msg.topic));
  memcpy(msg.payload, payload, length);
  msg.len = length;
  xQueueSend(mqttInbox, &msg, 0);   // drop if UI is far behind
}

// UI task context.
void processMessage(const MqttMsg& msg)
{
  String topicStr = String(msg.topic);
  JsonDocument doc;
  if (deserializeJson(doc, msg.payload, msg.len)) return;

  if (topicStr.startsWith("house/quest/players/")) {
    String name = doc["name"] | "";
    if (name == "") return;
    int idx = findOrCreatePlayer(name);
    if (idx >= 0) {
      players[idx].xp         = doc["xp"]          | 0;
      players[idx].hp         = doc["hp"]          | 100;
      players[idx].lastAction = doc["last_action"] | "";
    }
    dataReceived = true;
  }
  else if (topicStr == "house/quest/settings") {
    gameLevel = doc["level"] | 1;
  }
  else if (topicStr == "house/quest/awards") {
    currentAward.title    = doc["title"]    | "";
    currentAward.player   = doc["player"]   | "";
    currentAward.duration = doc["duration"] | "day";
    awardAvailable = true;
  }
}

// Apply queued messages; redraw once per batch (retained burst on connect),
// or when the connection LEDs change.
void handleInbox()
{
  static bool lastWifi = false, lastMqtt = false;
  bool dirty = (wifiConnected != lastWifi) || (mqttConnected != lastMqtt);
  lastWifi = wifiConnected;
  lastMqtt = mqttConnected;

  MqttMsg msg;
  while (xQueueReceive(mqttInbox, &msg, 0) == pdTRUE) {
    processMessage(msg);
    dirty = true;
  }

  if (dirty && (currentScreen == SCREEN_PLAYERS || currentScreen == SCREEN_AWARDS))
    drawScreen();
}


// =============================================================================
// Normal display
// =============================================================================

void drawStatusLED(int x, int y, uint16_t color) { tft.fillCircle(x, y, 4, color); }

void drawHeader()
{
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(3);
  tft.setCursor(30, 5);
  tft.print("HOUSE QUEST");

  drawStatusLED(310, 10, wifiConnected ? TFT_GREEN  : TFT_RED);
  drawStatusLED(310, 20, mqttConnected ? TFT_GREEN  : TFT_ORANGE);
  drawStatusLED(310, 30, dataReceived  ? TFT_GREEN  : TFT_YELLOW);

  int pages = (playerCount + PLAYERS_PER_PAGE - 1) / PLAYERS_PER_PAGE;
  tft.setTextSize(1);
  tft.setCursor(280, 10);
  tft.printf("%d/%d", currentPage + 1, pages);
}

void drawPlayer(int y, Player p)
{
  bool isGuest = (p.hp < 0);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(10, y);  tft.print(p.name);
  tft.setCursor(120, y); tft.printf("XP %d", p.xp);

  tft.setTextColor(isGuest ? TFT_RED : TFT_WHITE);
  tft.setCursor(200, y); tft.printf("HP %d", p.hp);   // negative prints naturally

  int bar = map(abs(p.hp), 0, 100, 0, 200);
  tft.fillRect(10, y + 20, 200, 10, TFT_DARKGREY);

  if (isGuest) {
    // Red penalty bar grows from the right
    tft.fillRect(10 + (200 - bar), y + 20, bar, 10, TFT_RED);
  } else {
    uint16_t hpColor = (p.hp < 15) ? TFT_MAGENTA :
                       (p.hp < 25) ? TFT_RED      :
                       (p.hp < 75) ? TFT_YELLOW   : TFT_GREEN;
    tft.fillRect(10, y + 20, bar, 10, hpColor);
  }
}

void drawPlayersPage()
{
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  int start = currentPage * PLAYERS_PER_PAGE;
  for (int i = 0; i < PLAYERS_PER_PAGE; i++) {
    int idx = start + i;
    if (idx >= playerCount) break;
    drawPlayer(60 + i * 40, players[idx]);
  }
}

void drawAwardsPage()
{
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(60, 50);
  tft.print("HOUSE TITLES");
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(20, 120);
  tft.print(currentAward.title);
  tft.setTextSize(1);
  tft.setCursor(20, 150);
  tft.print("Holder: ");
  tft.print(currentAward.player);
}

void handlePaging()
{
  if (millis() - lastPageSwitch < 5000) return;
  lastPageSwitch = millis();
  int pages = (playerCount + PLAYERS_PER_PAGE - 1) / PLAYERS_PER_PAGE;
  if (++currentPage >= pages) currentPage = 0;
  drawScreen();
}


// =============================================================================
// Admin UI — shared helper
// =============================================================================

// Draws a labelled rounded button.
// Centres text using the built-in font (6 px/char × textSize).
void drawButton(int x, int y, int w, int h,
                uint16_t bg, uint16_t fg,
                const char* label, int textSize = 2)
{
  tft.fillRoundRect(x, y, w, h, 5, bg);
  tft.drawRoundRect(x, y, w, h, 5, TFT_WHITE);
  tft.setTextColor(fg);
  tft.setTextSize(textSize);
  int tw = strlen(label) * 6 * textSize;
  int th = 8 * textSize;
  tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  tft.print(label);
}


// =============================================================================
// Admin UI — PIN entry  (320×240)
//
//  0- 35  header "ADMIN PIN"
// 38- 67  4 PIN boxes
// 72- 79  error text (optional)
// 80-239  4×3 numpad
// =============================================================================

static const char* PIN_LABELS[12] = {
  "1","2","3","4","5","6","7","8","9","DEL","0","OK"
};
#define PIN_BW  106
#define PIN_BH   40
#define PIN_Y0   80

// Only the PIN boxes + error line — redrawn on every key press.
void drawPinBoxes()
{
  tft.fillRect(0, 36, 320, PIN_Y0 - 36, TFT_BLACK);

  for (int i = 0; i < 4; i++) {
    int bx = 64 + i * 52;
    tft.drawRect(bx, 38, 40, 28, TFT_WHITE);
    if (i < pinLen)
      tft.fillCircle(bx + 20, 52, 7, TFT_WHITE);
  }

  if (pinError) {
    tft.setTextColor(TFT_RED);
    tft.setTextSize(1);
    tft.setCursor(100, 71);
    tft.print("Wrong PIN!");
  }
}

void drawAdminPin()
{
  tft.fillScreen(TFT_BLACK);

  // Header
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(85, 8);
  tft.print("ADMIN PIN");

  drawPinBoxes();

  // Numpad
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int idx = r * 3 + c;
      int bx  = c * (PIN_BW + 1);
      int by  = PIN_Y0 + r * PIN_BH;
      uint16_t bg = TFT_DARKGREY;
      if (strcmp(PIN_LABELS[idx], "OK")  == 0) bg = TFT_DARKGREEN;
      if (strcmp(PIN_LABELS[idx], "DEL") == 0) bg = TFT_MAROON;
      drawButton(bx, by, PIN_BW, PIN_BH, bg, TFT_WHITE, PIN_LABELS[idx], 2);
    }
  }
}

void handleAdminPinTouch(uint16_t tx, uint16_t ty)
{
  if (ty < PIN_Y0) return;

  int c = tx / (PIN_BW + 1);
  int r = (ty - PIN_Y0) / PIN_BH;
  if (c > 2 || r > 3) return;

  int idx  = r * 3 + c;
  pinError = false;

  if (strcmp(PIN_LABELS[idx], "DEL") == 0) {
    if (pinLen > 0) pinInput[--pinLen] = '\0';
    drawPinBoxes();
    return;
  }

  if (strcmp(PIN_LABELS[idx], "OK") == 0) {
    if (strcmp(pinInput, cfgAdminPin) == 0) {
      pinLen = 0; pinInput[0] = '\0';
      adminSelectPage = 0;
      currentScreen = SCREEN_ADMIN_SELECT;
      drawAdminSelect();
    } else {
      pinError = true; pinLen = 0; pinInput[0] = '\0';
      drawPinBoxes();
    }
    return;
  }

  // Digit key
  if (pinLen < 4) { pinInput[pinLen++] = PIN_LABELS[idx][0]; pinInput[pinLen] = '\0'; }

  // Auto-confirm on 4th digit
  if (pinLen == 4) {
    if (strcmp(pinInput, cfgAdminPin) == 0) {
      pinLen = 0; pinInput[0] = '\0';
      adminSelectPage = 0;
      currentScreen = SCREEN_ADMIN_SELECT;
      drawAdminSelect();
    } else {
      pinError = true; pinLen = 0; pinInput[0] = '\0';
      drawPinBoxes();
    }
    return;
  }

  drawPinBoxes();
}


// =============================================================================
// Admin UI — Player select  (320×240)
//
//  0- 35  header + page indicator
// 40-203  4 player rows  (4 × 36 px = 144 px)
//208-239  bottom bar: [BACK] [◄ PREV] [NEXT ►]
// =============================================================================

#define SEL_ROW_H        36
#define SEL_Y0           40
#define ADMIN_SEL_PER_PAGE 4

void drawAdminSelect()
{
  tft.fillScreen(TFT_BLACK);

  int totalPages = (playerCount + ADMIN_SEL_PER_PAGE - 1) / ADMIN_SEL_PER_PAGE;
  int start      = adminSelectPage * ADMIN_SEL_PER_PAGE;
  int end        = min(start + ADMIN_SEL_PER_PAGE, playerCount);

  // Header
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 8);
  tft.print("SELECT PLAYER");

  // Page indicator (only when more than one page)
  if (totalPages > 1) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/%d", adminSelectPage + 1, totalPages);
    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(295, 14);
    tft.print(buf);
  }

  // Player rows for current page
  for (int i = start; i < end; i++) {
    int row = i - start;
    int y   = SEL_Y0 + row * SEL_ROW_H;
    drawButton(5, y, 310, SEL_ROW_H - 3,
               TFT_NAVY, TFT_WHITE,
               players[i].name.c_str(), 2);
  }

  // Bottom bar: [BACK] [SETTINGS] [< PREV / NEXT >]
  drawButton(  3, 208, 100, 28, TFT_MAROON,  TFT_WHITE, "BACK",     1);
  drawButton(107, 208, 106, 28, TFT_DARKGREY, TFT_WHITE, "SETTINGS", 1);
  if (adminSelectPage > 0)
    drawButton(217, 208, 100, 28, TFT_DARKGREY, TFT_WHITE, "< PREV", 1);
  else if (adminSelectPage < totalPages - 1)
    drawButton(217, 208, 100, 28, TFT_DARKGREY, TFT_WHITE, "NEXT >", 1);
}

void handleAdminSelectTouch(uint16_t tx, uint16_t ty)
{
  int totalPages = (playerCount + ADMIN_SEL_PER_PAGE - 1) / ADMIN_SEL_PER_PAGE;

  // Bottom bar
  if (ty >= 208) {
    if (tx <= 106) {
      currentScreen = SCREEN_PLAYERS;
      drawPlayersPage();
    } else if (tx >= 107 && tx <= 212) {
      currentScreen = SCREEN_ADMIN_SETTINGS;
      drawAdminSettings();
    } else if (tx >= 217 && adminSelectPage > 0) {
      adminSelectPage--;
      drawAdminSelect();
    } else if (tx >= 217 && adminSelectPage < totalPages - 1) {
      adminSelectPage++;
      drawAdminSelect();
    }
    return;
  }

  // Player rows
  int start = adminSelectPage * ADMIN_SEL_PER_PAGE;
  int end   = min(start + ADMIN_SEL_PER_PAGE, playerCount);
  for (int i = start; i < end; i++) {
    int row = i - start;
    int y   = SEL_Y0 + row * SEL_ROW_H;
    if (ty >= y && ty < y + SEL_ROW_H - 3) {
      adminTarget   = i;
      currentScreen = SCREEN_ADMIN_ACTION;
      drawAdminAction();
      return;
    }
  }
}


// =============================================================================
// Admin UI — Action panel  (320×240)
//
//  0- 64  header: name / role / XP + HP
// 67-109  [-5 XP]   [+5 XP]
//109-152  [-10 HP]  [+10 HP]
//152-182  [BY RULE]
//182-210  [Toggle role]
//210-239  [BACK]
// =============================================================================

void drawAdminAction()
{
  if (adminTarget < 0) return;
  Player &p = players[adminTarget];

  tft.fillScreen(TFT_BLACK);

  // Name + role
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.print(p.name);

  bool isResident = (p.hp >= 0);
  tft.setTextSize(1);
  tft.setTextColor(isResident ? TFT_GREEN : TFT_ORANGE);
  tft.setCursor(10, 28);
  tft.print(isResident ? "Mitbewohner" : "Gast");

  // Stats
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  char buf[24];
  snprintf(buf, sizeof(buf), "XP: %d", p.xp);
  tft.setCursor(10, 42);
  tft.print(buf);
  snprintf(buf, sizeof(buf), "HP: %d", p.hp);
  tft.setCursor(170, 42);
  tft.print(buf);

  tft.drawFastHLine(0, 64, 320, TFT_DARKGREY);

  // XP row
  drawButton(  3,  67, 152, 40, TFT_MAROON,    TFT_WHITE, "-5 XP",  2);
  drawButton(162,  67, 152, 40, TFT_DARKGREEN, TFT_WHITE, "+5 XP",  2);

  // HP row
  drawButton(  3, 109, 152, 40, TFT_MAROON,    TFT_WHITE, "-10 HP", 2);
  drawButton(162, 109, 152, 40, TFT_DARKGREEN, TFT_WHITE, "+10 HP", 2);

  // By Rule
  uint16_t ruleBg = rulesetAvailable ? TFT_NAVY : TFT_DARKGREY;
  drawButton(3, 152, 314, 28, ruleBg, TFT_WHITE, "BY RULE", 1);

  // Role override (forces HP across the 0 boundary)
  uint16_t roleBg = isResident ? TFT_MAROON : TFT_DARKGREEN;
  drawButton(3, 182, 314, 26, roleBg, TFT_WHITE,
             isResident ? "-> Set as GAST" : "-> Set as MITBEWOHNER", 1);

  // Back
  drawButton(3, 210, 314, 28, TFT_DARKGREY, TFT_WHITE, "BACK", 1);
}

void applyHpDelta(Player &p, int delta)
{
  p.hp = max(-100, min(100, p.hp + delta));
}

void handleAdminActionTouch(uint16_t tx, uint16_t ty)
{
  if (adminTarget < 0) return;
  Player &p    = players[adminTarget];
  bool changed = false;

  if (ty >= 67 && ty < 109) {
    // XP row
    int delta = (tx < 160) ? -XP_STEP : XP_STEP;
    p.xp += delta;
    char act[16];
    snprintf(act, sizeof(act), "xp%+d", delta);
    p.lastAction = act;
    changed = true;
  }
  else if (ty >= 109 && ty < 152) {
    // HP row
    int delta = (tx < 160) ? -HP_STEP : HP_STEP;
    applyHpDelta(p, delta);
    char act[16];
    snprintf(act, sizeof(act), "hp%+d", delta);
    p.lastAction = act;
    changed = true;
  }
  else if (ty >= 152 && ty < 182) {
    // By Rule — open rule entry screen
    if (rulesetAvailable) {
      ruleIdLen = 0; ruleIdInput[0] = '\0';
      ruleConfirm = false; ruleNotFound = false;
      currentScreen = SCREEN_ADMIN_RULE;
      drawAdminRule();
    }
    return;
  }
  else if (ty >= 182 && ty < 210) {
    // Role override — force hp across the 0 boundary
    p.hp = (p.hp >= 0) ? -1 : 0;
    p.lastAction = (p.hp < 0) ? "demote" : "promote";
    changed = true;
  }
  else if (ty >= 210) {
    // Back to player select
    currentScreen = SCREEN_ADMIN_SELECT;
    drawAdminSelect();
    return;
  }

  if (changed) {
    bool ok = publishPlayerState(adminTarget);
    drawAdminAction();
    if (!ok) {
      // Overlay warning — change was applied locally but not sent
      tft.setTextColor(TFT_RED);
      tft.setTextSize(1);
      tft.fillRect(0, 56, 320, 9, TFT_BLACK);
      tft.setCursor(5, 57);
      tft.print("MQTT offline — stored locally only");
    }
  }
}


// =============================================================================
// Admin UI — By Rule  (320×240)
//
//  0- 14  header "BY RULE  →  PlayerName"
// 17- 47  input box  (entry mode)  /  rule title  (confirm mode)
// 50- 80  rule text lines          (confirm mode only)
// 55-213  numpad                   (entry mode only)
// 85-155  HP/XP impact + HP preview (confirm mode only)
//160-200  [CANCEL] [APPLY]         (confirm mode only)
//213-239  [BACK]  /  [BACK] [OK]   (both modes)
// =============================================================================

#define RULE_BW   105
#define RULE_BH    40
#define RULE_Y0    55

static const char* RULE_LABELS[12] = {
  "1","2","3","4","5","6","7","8","9",".","0","DEL"
};

// Only the rule-ID input box + "not found" — redrawn on every key press.
void drawRuleInput()
{
  tft.fillRect(0, 16, 320, RULE_Y0 - 17, TFT_BLACK);
  tft.drawRect(4, 18, 232, 28, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(9, 24);
  tft.print(ruleIdInput);
  tft.print("_");

  if (ruleNotFound) {
    tft.setTextColor(TFT_RED);
    tft.setTextSize(1);
    tft.setCursor(242, 25);
    tft.print("not found");
  }
}

void drawAdminRule()
{
  if (adminTarget < 0) return;
  Player &p = players[adminTarget];

  tft.fillScreen(TFT_BLACK);

  // Header
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(1);
  tft.setCursor(5, 4);
  tft.printf("BY RULE  ->  %s", p.name.c_str());

  if (!ruleConfirm) {
    // ── Entry mode ────────────────────────────────────────────────────────────
    drawRuleInput();

    // Numpad
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 3; c++) {
        int     idx = r * 3 + c;
        int     bx  = c * (RULE_BW + 1);
        int     by  = RULE_Y0 + r * RULE_BH;
        uint16_t bg = (strcmp(RULE_LABELS[idx], "DEL") == 0) ? TFT_MAROON : TFT_DARKGREY;
        drawButton(bx, by, RULE_BW, RULE_BH - 2, bg, TFT_WHITE, RULE_LABELS[idx], 2);
      }
    }

    // BACK | OK
    drawButton(  3, 213, 152, 25, TFT_DARKGREY,  TFT_WHITE, "BACK", 1);
    drawButton(162, 213, 152, 25, TFT_DARKGREEN, TFT_WHITE, "OK",   1);

  } else {
    // ── Confirm mode ──────────────────────────────────────────────────────────

    // Rule ID
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(5, 18);
    tft.printf("Rule  %s", ruleIdInput);

    // Rule text — wrap at ~53 chars (size 1 = 6px/char, 320px wide)
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    int   tlen   = strlen(ruleFoundText);
    const int wrap = 53;
    if (tlen <= wrap) {
      tft.setCursor(5, 40); tft.print(ruleFoundText);
    } else {
      int brk = wrap;
      while (brk > 0 && ruleFoundText[brk] != ' ') brk--;
      if (brk == 0) brk = wrap;
      char line1[wrap + 1];
      strncpy(line1, ruleFoundText, brk); line1[brk] = '\0';
      tft.setCursor(5, 40); tft.print(line1);
      tft.setCursor(5, 52); tft.print(ruleFoundText + brk + 1);
    }

    tft.drawFastHLine(0, 68, 320, TFT_DARKGREY);

    // HP / XP impact
    char impactHp[16], impactXp[16];
    snprintf(impactHp, sizeof(impactHp), "HP -%d", ruleFoundHp);
    snprintf(impactXp, sizeof(impactXp), "XP +%d", ruleFoundXp);
    tft.setTextSize(2);
    tft.setTextColor(ruleFoundHp > 0 ? TFT_RED : TFT_YELLOW);
    tft.setCursor(5, 74); tft.print(impactHp);
    if (ruleFoundXp > 0) {
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(170, 74); tft.print(impactXp);
    }

    // Before → after preview
    int simHp = max(-100, min(100, p.hp - ruleFoundHp));
    bool crossesBoundary = (p.hp >= 0) && (simHp < 0);
    tft.setTextSize(1);
    tft.setCursor(5, 98);
    tft.setTextColor(crossesBoundary ? TFT_ORANGE : TFT_DARKGREY);
    tft.printf("%s: %d HP -> %d HP%s",
               p.name.c_str(), p.hp, simHp,
               crossesBoundary ? "  -> GAST!" : "");

    // Level indicator — red if rule level exceeds the global game level
    bool levelOk = (ruleFoundLevel <= gameLevel);
    tft.setTextColor(levelOk ? TFT_DARKGREY : TFT_RED);
    tft.setCursor(5, 110);
    if (levelOk) {
      tft.printf("Lvl-%d rule", ruleFoundLevel);
    } else {
      tft.printf("Lvl-%d rule — game is Lvl %d  NOT APPLICABLE", ruleFoundLevel, gameLevel);
    }

    tft.drawFastHLine(0, 122, 320, TFT_DARKGREY);

    // Buttons — APPLY greyed out when rule level exceeds game level
    uint16_t applyBg = levelOk ? TFT_DARKGREEN : TFT_DARKGREY;
    drawButton(  3, 130, 152, 44, TFT_DARKGREY, TFT_WHITE, "CANCEL", 2);
    drawButton(162, 130, 152, 44, applyBg,      TFT_WHITE, "APPLY",  2);
    drawButton(  3, 208, 314, 28, TFT_MAROON,   TFT_WHITE, "BACK",   1);
  }
}

void handleAdminRuleTouch(uint16_t tx, uint16_t ty)
{
  if (adminTarget < 0) return;

  if (!ruleConfirm) {
    // ── Entry mode ────────────────────────────────────────────────────────────
    if (ty >= 213) {
      if (tx < 160) {
        // BACK → return to action screen
        currentScreen = SCREEN_ADMIN_ACTION;
        drawAdminAction();
      } else {
        // OK → look up the rule
        if (ruleIdLen > 0) {
          if (getRuleById(ruleIdInput)) {
            ruleConfirm  = true;
            ruleNotFound = false;
            drawAdminRule();
          } else {
            ruleNotFound = true;
            drawRuleInput();
          }
        }
      }
      return;
    }

    // Numpad
    if (ty < RULE_Y0) return;
    int c = tx / (RULE_BW + 1);
    int r = (ty - RULE_Y0) / RULE_BH;
    if (c > 2 || r > 3) return;
    int idx = r * 3 + c;

    if (strcmp(RULE_LABELS[idx], "DEL") == 0) {
      if (ruleIdLen > 0) ruleIdInput[--ruleIdLen] = '\0';
    } else if (ruleIdLen < RULE_ID_MAX) {
      ruleIdInput[ruleIdLen++] = RULE_LABELS[idx][0];
      ruleIdInput[ruleIdLen]   = '\0';
    }
    ruleNotFound = false;
    drawRuleInput();

  } else {
    // ── Confirm mode ──────────────────────────────────────────────────────────
    if (ty >= 130 && ty < 174) {
      if (tx < 160) {
        // CANCEL → back to entry
        ruleConfirm = false;
        drawAdminRule();
      } else {
        // APPLY — blocked if rule level exceeds global game level
        Player &p = players[adminTarget];
        if (ruleFoundLevel > gameLevel) return;

        // Build action string before mutating scores
        char act[32];
        if (ruleFoundHp > 0 && ruleFoundXp > 0)
          snprintf(act, sizeof(act), "hp-%d,xp+%d:%s", ruleFoundHp, ruleFoundXp, ruleIdInput);
        else if (ruleFoundHp > 0)
          snprintf(act, sizeof(act), "hp-%d:%s", ruleFoundHp, ruleIdInput);
        else if (ruleFoundXp > 0)
          snprintf(act, sizeof(act), "xp+%d:%s", ruleFoundXp, ruleIdInput);
        else
          snprintf(act, sizeof(act), "rule:%s", ruleIdInput);
        p.lastAction = act;

        if (ruleFoundHp > 0) applyHpDelta(p, -ruleFoundHp);
        if (ruleFoundXp > 0) p.xp = min(999, p.xp + ruleFoundXp);

        bool ok = publishPlayerState(adminTarget);

        // Reset and return to action screen
        ruleIdLen = 0; ruleIdInput[0] = '\0';
        ruleConfirm = false; ruleNotFound = false;
        currentScreen = SCREEN_ADMIN_ACTION;
        drawAdminAction();

        if (!ok) {
          tft.setTextColor(TFT_RED);
          tft.setTextSize(1);
          tft.fillRect(0, 56, 320, 9, TFT_BLACK);
          tft.setCursor(5, 57);
          tft.print("MQTT offline — stored locally only");
        }
      }
    }
    else if (ty >= 208) {
      // BACK → entry mode
      ruleConfirm = false;
      drawAdminRule();
    }
  }
}


// =============================================================================
// Admin UI — Settings  (320×240)
//
//  0- 35  header "SETTINGS"
// 40- 60  "Game Level" label + current level (large)
// 95-145  [Lvl -]  [Lvl +]
//154-172  WiFi / MQTT status lines
//176-204  [WIFI / MQTT SETUP] [TOUCH CAL]
//208-239  [BACK]
// =============================================================================

void drawAdminSettings()
{
  tft.fillScreen(TFT_BLACK);

  // Header
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(80, 8);
  tft.print("SETTINGS");

  tft.drawFastHLine(0, 34, 320, TFT_DARKGREY);

  // Game Level section
  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(5, 42);
  tft.print("GAME LEVEL");

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(4);
  tft.setCursor(140, 56);
  tft.printf("%d", gameLevel);

  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(5, 58);
  tft.print("Rules above this level");
  tft.setCursor(5, 68);
  tft.print("are not applicable.");

  drawButton(  3,  95, 152, 50, TFT_MAROON,    TFT_WHITE, "Lvl -", 2);
  drawButton(162,  95, 152, 50, TFT_DARKGREEN, TFT_WHITE, "Lvl +", 2);

  tft.drawFastHLine(0, 150, 320, TFT_DARKGREY);

  // Connection info
  tft.setTextSize(1);
  tft.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED);
  tft.setCursor(5, 155);
  if (wifiConnected) tft.printf("WiFi: %s  %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  else               tft.print("WiFi: not connected");
  tft.setTextColor(mqttConnected ? TFT_GREEN : TFT_ORANGE);
  tft.setCursor(5, 166);
  tft.printf("MQTT: %s:%u", cfgMqttHost[0] ? cfgMqttHost : "(not set)", cfgMqttPort);

  drawButton(  3, 176, 204, 28, TFT_NAVY,     TFT_WHITE, "WIFI / MQTT SETUP", 1);
  drawButton(211, 176, 106, 28, TFT_DARKGREY, TFT_WHITE, "TOUCH CAL",         1);

  // Back
  drawButton(3, 208, 314, 28, TFT_DARKGREY, TFT_WHITE, "BACK", 1);
}

void handleAdminSettingsTouch(uint16_t tx, uint16_t ty)
{
  if (ty >= 95 && ty < 145) {
    int prev = gameLevel;
    if (tx < 160) gameLevel = max(0, gameLevel - 1);
    else          gameLevel = min(9, gameLevel + 1);
    if (gameLevel != prev) {
      publishGameSettings();
      drawAdminSettings();
    }
    return;
  }
  if (ty >= 176 && ty < 205) {
    if (tx < 209) runSetupPortal();
    else          runTouchCalibration();
    drawAdminSettings();
    return;
  }
  if (ty >= 208) {
    currentScreen = SCREEN_ADMIN_SELECT;
    drawAdminSelect();
  }
}


// =============================================================================
// Setup portal — WiFiManager
//
// Opens the AP "HouseQuest-XXXXXX" (password shown on the display) with a
// web form for WiFi, MQTT broker and admin PIN at 192.168.4.1.
//  - at boot, when no WiFi is saved (waits until configured)
//  - at boot, when the saved WiFi is unreachable (gives up after timeout)
//  - on demand from the admin settings screen
// =============================================================================

WiFiManager wm;

WiFiManagerParameter paramMqttHost("mqtt_host", "MQTT Broker (Host/IP)",              "", 63);
WiFiManagerParameter paramMqttPort("mqtt_port", "MQTT Port",                          "", 5,
                                   "type='number' min='1' max='65535'");
WiFiManagerParameter paramMqttUser("mqtt_user", "MQTT User (leer = anonym)",          "", 31);
WiFiManagerParameter paramMqttPass("mqtt_pass", "MQTT Passwort (leer = unverändert)", "", 63,
                                   "type='password'");
WiFiManagerParameter paramAdminPin("admin_pin", "Admin-PIN, 4 Ziffern (leer = unverändert)", "", 4,
                                   "type='password' inputmode='numeric' pattern='[0-9]{4}'");

static char apPassword[9];

// Prefill the form with the current config; secrets stay blank.
void preparePortal()
{
  char port[6];
  snprintf(port, sizeof(port), "%u", cfgMqttPort);
  paramMqttHost.setValue(cfgMqttHost, 63);
  paramMqttPort.setValue(port,        5);
  paramMqttUser.setValue(cfgMqttUser, 31);
  paramMqttPass.setValue("",          63);
  paramAdminPin.setValue("",          4);

  // Fresh AP password each time; it is only shown on the panel itself
  snprintf(apPassword, sizeof(apPassword), "%08lu", (unsigned long)(esp_random() % 100000000UL));
}

void onPortalSave()
{
  strlcpy(cfgMqttHost, paramMqttHost.getValue(), sizeof(cfgMqttHost));

  long port = atol(paramMqttPort.getValue());
  if (port > 0 && port <= 65535) cfgMqttPort = (uint16_t)port;

  strlcpy(cfgMqttUser, paramMqttUser.getValue(), sizeof(cfgMqttUser));
  if (paramMqttPass.getValue()[0] != '\0')
    strlcpy(cfgMqttPass, paramMqttPass.getValue(), sizeof(cfgMqttPass));

  const char* pin = paramAdminPin.getValue();
  bool pinOk = strlen(pin) == 4;
  for (int i = 0; pinOk && i < 4; i++) pinOk = isdigit((unsigned char)pin[i]);
  if (pinOk) strlcpy(cfgAdminPin, pin, sizeof(cfgAdminPin));

  saveConfig();
  client.disconnect();   // caller holds mqttMutex (or network task not started yet)
  client.setServer(cfgMqttHost, cfgMqttPort);
  mqttRetryDelay = MQTT_RETRY_MIN_MS;
  Serial.printf("Config saved: mqtt=%s:%u user=%s\n", cfgMqttHost, cfgMqttPort, cfgMqttUser);
}

void drawSetupScreen(bool cancelable)
{
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 8);
  tft.print("WIFI / MQTT SETUP");
  tft.drawFastHLine(0, 30, 320, TFT_DARKGREY);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 42);  tft.print("1. Mit diesem WLAN verbinden:");
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(15, 56); tft.print(deviceId);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(15, 80); tft.print("Passwort:");
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(80, 76); tft.print(apPassword);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 110); tft.print("2. Im Browser oeffnen (falls nicht automatisch):");
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(15, 124); tft.print("http://192.168.4.1");

  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(5, 156); tft.print("3. WLAN waehlen, MQTT + PIN eintragen, Speichern.");

  if (cancelable)
    drawButton(3, 208, 314, 28, TFT_MAROON, TFT_WHITE, "CANCEL", 1);
}

// Boot: connect with saved WiFi, otherwise open the portal.
void setupWifi()
{
  static std::vector<const char*> menu = { "wifi", "info", "exit" };

  WiFi.mode(WIFI_STA);
  wm.setTitle("HouseQuest");
  wm.setMenu(menu);
  wm.addParameter(&paramMqttHost);
  wm.addParameter(&paramMqttPort);
  wm.addParameter(&paramMqttUser);
  wm.addParameter(&paramMqttPass);
  wm.addParameter(&paramAdminPin);
  wm.setSaveParamsCallback(onPortalSave);
  wm.setAPCallback([](WiFiManager*) { drawSetupScreen(false); });
  wm.setConnectTimeout(20);

  // Nothing saved → wait in the portal until configured.
  // Saved but unreachable (e.g. router still booting) → give up, loop() keeps retrying.
  wm.setConfigPortalTimeout(wm.getWiFiIsSaved() ? SETUP_PORTAL_TIMEOUT : 0);

  preparePortal();
  drawStatus("Connecting WiFi...");
  wifiConnected = wm.autoConnect(deviceId, apPassword);
}

// On demand from settings. Runs its own loop so the panel can cancel it.
void runSetupPortal()
{
  // Free port 80, then hold the mutex to pause the network task for the whole session
  drawStatus("Please wait...");
  webWanted = false;
  while (webRunning) delay(10);
  xSemaphoreTake(mqttMutex, portMAX_DELAY);
  client.disconnect();
  mqttConnected = false;
  preparePortal();

  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal(deviceId, apPassword);   // AP callback draws the screen
  drawSetupScreen(true);

  unsigned long start = millis();
  while (wm.getConfigPortalActive()) {
    wm.process();

    uint16_t tx, ty;
    bool cancel = tft.getTouch(&tx, &ty) && ty >= 208 && millis() - start > 1000;
    if (cancel || millis() - start > SETUP_PORTAL_TIMEOUT * 1000UL) {
      wm.stopConfigPortal();
      while (tft.getTouch(&tx, &ty)) delay(10);   // don't let CANCEL hit BACK below
      break;
    }
    delay(10);
  }
  wm.setConfigPortalBlocking(true);

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (!wifiConnected) WiFi.begin();
  xSemaphoreGive(mqttMutex);
  webWanted = true;
}


// =============================================================================
// QR screen — tap on the board shows the address of the rules page  (320×240)
//
//  QR code left (~170 px), hint text right, URL across the bottom.
//  Any tap or QR_SCREEN_TIMEOUT returns to the board.
// =============================================================================

#define QR_SCREEN_TIMEOUT 60000
unsigned long qrShownAt = 0;

static const int QR_X = 8, QR_Y = 8, QR_MAX_PX = 176, QR_QUIET = 2;

void drawQrModules(esp_qrcode_handle_t qr)
{
  int n     = esp_qrcode_get_size(qr);
  int scale = QR_MAX_PX / (n + 2 * QR_QUIET);
  int px    = (n + 2 * QR_QUIET) * scale;

  tft.fillRect(QR_X, QR_Y, px, px, TFT_WHITE);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(qr, x, y))
        tft.fillRect(QR_X + (x + QR_QUIET) * scale, QR_Y + (y + QR_QUIET) * scale,
                     scale, scale, TFT_BLACK);
}

void drawQrScreen()
{
  tft.fillScreen(TFT_BLACK);

  const int tx = QR_X + QR_MAX_PX + 10;
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(tx, 14);  tft.print("REGELN");

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(tx, 44);  tft.print("QR-Code mit dem");
  tft.setCursor(tx, 56);  tft.print("Handy scannen.");
  tft.setCursor(tx, 76);  tft.print("Gleiches WLAN");
  tft.setCursor(tx, 88);  tft.print("wie das Board.");
  tft.setTextColor(TFT_DARKGREY);
  tft.setCursor(tx, 168); tft.print("Tippen = zurueck");

  if (!wifiConnected) {
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(20, 90);
    tft.print("Kein WLAN");
    return;
  }

  char url[40];
  snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());

  esp_qrcode_config_t cfg = {};
  cfg.display_func       = drawQrModules;
  cfg.max_qrcode_version = 5;
  cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_MED;
  esp_qrcode_generate(&cfg, url);

  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(8, 205);
  tft.print(url);
}

void showQrScreen()
{
  currentScreen = SCREEN_QR;
  qrShownAt     = millis();
  drawQrScreen();
}


// =============================================================================
// Screen dispatch
// =============================================================================

void drawScreen()
{
  switch (currentScreen) {
    case SCREEN_PLAYERS:
      drawPlayersPage();
      break;
    case SCREEN_AWARDS:
      tft.fillScreen(TFT_BLACK);
      drawHeader();
      awardAvailable ? drawAwardsPage() : drawPlayersPage();
      break;
    case SCREEN_ADMIN_PIN:      drawAdminPin();      break;
    case SCREEN_ADMIN_SELECT:   drawAdminSelect();   break;
    case SCREEN_ADMIN_ACTION:   drawAdminAction();   break;
    case SCREEN_ADMIN_RULE:     drawAdminRule();     break;
    case SCREEN_ADMIN_SETTINGS: drawAdminSettings(); break;
    case SCREEN_QR:             drawQrScreen();      break;
  }
}


// =============================================================================
// Touch handling
// =============================================================================

// One action per tap: fire on touch-down, re-arm after the finger is lifted.
void handleTouch()
{
  static bool          touchDown     = false;
  static unsigned long lastTouchSeen = 0;

  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) {
    if (touchDown && millis() - lastTouchSeen > 60) touchDown = false;
    return;
  }
  lastTouchSeen = millis();
  if (touchDown) return;
  touchDown = true;

  Serial.printf("Touch x=%d y=%d screen=%d\n", tx, ty, (int)currentScreen);

  switch (currentScreen) {
    case SCREEN_PLAYERS:
    case SCREEN_AWARDS:
      // Hidden admin entry: bottom-left corner
      if (tx < 40 && ty > 200) {
        pinLen = 0; pinInput[0] = '\0'; pinError = false;
        currentScreen = SCREEN_ADMIN_PIN;
        drawAdminPin();
      } else {
        showQrScreen();
      }
      break;
    case SCREEN_QR:
      currentScreen = SCREEN_PLAYERS;
      drawPlayersPage();
      break;
    case SCREEN_ADMIN_PIN:      handleAdminPinTouch(tx, ty);      break;
    case SCREEN_ADMIN_SELECT:   handleAdminSelectTouch(tx, ty);   break;
    case SCREEN_ADMIN_ACTION:   handleAdminActionTouch(tx, ty);   break;
    case SCREEN_ADMIN_RULE:     handleAdminRuleTouch(tx, ty);     break;
    case SCREEN_ADMIN_SETTINGS: handleAdminSettingsTouch(tx, ty); break;
  }
}


// =============================================================================
// Touch calibration
// =============================================================================

void runTouchCalibration()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 100);
  tft.print("Touch calibration");
  tft.setCursor(20, 130);
  tft.print("Tap each corner");
  delay(2000);

  tft.calibrateTouch(touchCalData, TFT_WHITE, TFT_RED, 15);
  tft.setTouch(touchCalData);
  touchCalValid = true;

  prefs.begin("housequest", false);
  prefs.putBytes("touch_cal", touchCalData, sizeof(touchCalData));
  prefs.end();

  // Handy as HQ_TOUCH_CAL for a new variant in platformio.ini
  Serial.printf("Touch calibration saved: {%u,%u,%u,%u,%u}\n",
                touchCalData[0], touchCalData[1], touchCalData[2], touchCalData[3], touchCalData[4]);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(20, 110);
  tft.print("Calibration done!");
  delay(2000);
}


// =============================================================================
// Setup / Loop
// =============================================================================

void setup()
{
  Serial.begin(115200);

  pinMode(HQ_TFT_LED, OUTPUT);
  digitalWrite(HQ_TFT_LED, LOW);

  tft.init();
  tft.setRotation(HQ_TFT_ROTATION);

  loadConfig();

  // Finger on the screen at power-up → recalibrate (escape hatch for a bad calibration)
  if (!touchCalValid || tft.getTouchRawZ() > 600) {
    runTouchCalibration();
  } else {
    tft.setTouch(touchCalData);
  }

  initRuleset();
  drawScreen();
  setupWifi();

  client.setServer(cfgMqttHost, cfgMqttPort);
  client.setCallback(callback);
  client.setBufferSize(512);
  client.setKeepAlive(30);
  client.setSocketTimeout(2);

  mqttMutex = xSemaphoreCreateMutex();
  mqttInbox = xQueueCreate(24, sizeof(MqttMsg));
  setupWeb();
  xTaskCreatePinnedToCore(networkTask, "network", 8192, nullptr, 1, nullptr, 0);

  drawScreen();
}

void loop()
{
  handleInbox();
  handleTouch();

  // Auto-paging only on normal display screens
  if (currentScreen == SCREEN_PLAYERS || currentScreen == SCREEN_AWARDS)
    handlePaging();

  if (currentScreen == SCREEN_QR && millis() - qrShownAt > QR_SCREEN_TIMEOUT) {
    currentScreen = SCREEN_PLAYERS;
    drawPlayersPage();
  }

  delay(10);
}
