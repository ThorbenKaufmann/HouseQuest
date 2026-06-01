#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* ssid        = "***REMOVED***";
const char* password    = "***REMOVED***";
const char* mqtt_server = "datacenter.itc-embedded.de";

// ── Admin config ──────────────────────────────────────────────────────────────
#define ADMIN_PIN  "1234"   // change to your PIN
#define XP_STEP    5
#define HP_STEP    10

// ── Hardware ──────────────────────────────────────────────────────────────────
#define TFT_LED       15
#define MAX_PLAYERS   16
#define PLAYERS_PER_PAGE 4

// ── Touch calibration ─────────────────────────────────────────────────────────
#define TOUCH_CALIBRATED 1
uint16_t touchCalData[5] = { 235, 3347, 377, 3385, 1 };

// ── MQTT / WiFi ───────────────────────────────────────────────────────────────
WiFiClient   espClient;
PubSubClient client(espClient);

bool wifiConnected = false;
bool mqttConnected = false;
bool dataReceived  = false;

unsigned long lastWifiCheck   = 0;
unsigned long lastMQTTAttempt = 0;

// ── Screen state ──────────────────────────────────────────────────────────────
enum ScreenMode {
  SCREEN_PLAYERS,
  SCREEN_AWARDS,
  SCREEN_ADMIN_PIN,
  SCREEN_ADMIN_SELECT,
  SCREEN_ADMIN_ACTION,
  SCREEN_ADMIN_RULE,
  SCREEN_ADMIN_SETTINGS
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

void setup_wifi()
{
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    drawStatus("Connecting WiFi...");
  }
  wifiConnected = true;
}

void handleWifi()
{
  if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; return; }
  wifiConnected = false;
  if (millis() - lastWifiCheck < 5000) return;
  lastWifiCheck = millis();
  WiFi.begin(ssid, password);
}

void handleMQTT()
{
  if (!wifiConnected) { mqttConnected = false; return; }
  if (client.connected()) { mqttConnected = true; client.loop(); return; }
  mqttConnected = false;
  if (millis() - lastMQTTAttempt < 3000) return;
  lastMQTTAttempt = millis();
  if (client.connect("housequest_panel")) {
    client.subscribe("house/quest/players/+/state");
    client.subscribe("house/quest/settings");
    mqttConnected = true;
  }
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
  if (!client.connected()) return false;

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

  return client.publish(topic.c_str(), (uint8_t*)payload, strlen(payload), true);
}

void publishGameSettings()
{
  if (!client.connected()) return;
  char payload[64];
  JsonDocument doc;
  doc["level"] = gameLevel;
  serializeJson(doc, payload, sizeof(payload));
  client.publish("house/quest/settings", (uint8_t*)payload, strlen(payload), true);
}

void callback(char* topic, byte* payload, unsigned int length)
{
  String topicStr = String(topic);
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;

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

  // Only refresh display if on a normal screen
  if (currentScreen == SCREEN_PLAYERS || currentScreen == SCREEN_AWARDS)
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

void drawAdminPin()
{
  tft.fillScreen(TFT_BLACK);

  // Header
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(85, 8);
  tft.print("ADMIN PIN");

  // PIN boxes
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
    drawAdminPin();
    return;
  }

  if (strcmp(PIN_LABELS[idx], "OK") == 0) {
    if (strcmp(pinInput, ADMIN_PIN) == 0) {
      pinLen = 0; pinInput[0] = '\0';
      adminSelectPage = 0;
      currentScreen = SCREEN_ADMIN_SELECT;
      drawAdminSelect();
    } else {
      pinError = true; pinLen = 0; pinInput[0] = '\0';
      drawAdminPin();
    }
    return;
  }

  // Digit key
  if (pinLen < 4) { pinInput[pinLen++] = PIN_LABELS[idx][0]; pinInput[pinLen] = '\0'; }

  // Auto-confirm on 4th digit
  if (pinLen == 4) {
    if (strcmp(pinInput, ADMIN_PIN) == 0) {
      pinLen = 0; pinInput[0] = '\0';
      adminSelectPage = 0;
      currentScreen = SCREEN_ADMIN_SELECT;
      drawAdminSelect();
    } else {
      pinError = true; pinLen = 0; pinInput[0] = '\0';
      drawAdminPin();
    }
    return;
  }

  drawAdminPin();
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
          } else {
            ruleNotFound = true;
          }
          drawAdminRule();
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
    drawAdminRule();

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
// 70-130  [Lvl -]  [Lvl +]
//         ... future settings rows ...
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

  tft.drawFastHLine(0, 152, 320, TFT_DARKGREY);

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
  if (ty >= 208) {
    currentScreen = SCREEN_ADMIN_SELECT;
    drawAdminSelect();
  }
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
  }
}


// =============================================================================
// Touch handling
// =============================================================================

unsigned long lastTouchDebounce = 0;

void handleTouch()
{
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return;
  if (millis() - lastTouchDebounce < 200) return;
  lastTouchDebounce = millis();

  Serial.printf("Touch x=%d y=%d screen=%d\n", tx, ty, (int)currentScreen);

  switch (currentScreen) {
    case SCREEN_PLAYERS:
    case SCREEN_AWARDS:
      // Hidden admin entry: bottom-left corner
      if (tx < 40 && ty > 200) {
        pinLen = 0; pinInput[0] = '\0'; pinError = false;
        currentScreen = SCREEN_ADMIN_PIN;
        drawAdminPin();
      }
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

  Serial.println("\n=== Calibration done ===");
  Serial.print("uint16_t touchCalData[5] = { ");
  for (int i = 0; i < 5; i++) {
    Serial.print(touchCalData[i]);
    if (i < 4) Serial.print(", ");
  }
  Serial.println(" };");
  Serial.println("Set TOUCH_CALIBRATED 1 and paste above.");

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

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, LOW);

  tft.init();
  tft.setRotation(1);

#if TOUCH_CALIBRATED
  tft.setTouch(touchCalData);
#else
  runTouchCalibration();
#endif

  initRuleset();
  drawScreen();
  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  client.setBufferSize(512);
  client.setKeepAlive(30);
  client.setSocketTimeout(2);

  drawScreen();
}

void loop()
{
  handleWifi();
  handleMQTT();
  handleTouch();

  // Auto-paging only on normal display screens
  if (currentScreen == SCREEN_PLAYERS || currentScreen == SCREEN_AWARDS)
    handlePaging();

  delay(50);
}
