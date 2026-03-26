#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>

const char* ssid = "***REMOVED***";
const char* password = "***REMOVED***";

const char* mqtt_server = "datacenter.itc-embedded.de";

WiFiClient espClient;
PubSubClient client(espClient);

bool wifiConnected = false;
bool mqttConnected = false;
bool dataReceived = false;

enum ScreenMode {
  SCREEN_PLAYERS,
  SCREEN_AWARDS
};

ScreenMode currentScreen = SCREEN_PLAYERS;

unsigned long lastScreenSwitch = 0;

#define SCREEN_INTERVAL 8000
#define TFT_LED 15
#define MAX_PLAYERS 16

struct Player {
  String name;
  int xp;
  int hp;
  String role;
};

struct Award {
  String title;
  String player;
  String duration;
};

Award currentAward;
bool awardAvailable = false;

Player players[MAX_PLAYERS];
int playerCount = 0;

#define PLAYERS_PER_PAGE 4

int currentPage = 0;
unsigned long lastPageSwitch = 0;

unsigned long lastWifiCheck = 0;
unsigned long lastMQTTAttempt = 0;

TFT_eSPI tft = TFT_eSPI();



void drawScreen(void);

void drawStatus(String text)
{
  tft.fillRect(0,0,320,20,TFT_DARKGREY);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5,5);
  tft.print(text);
}



void handleWifi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;

  if (millis() - lastWifiCheck < 5000)
    return;

  lastWifiCheck = millis();

  Serial.println("WiFi reconnect...");
  WiFi.begin(ssid, password);
}

void setup_wifi()
{
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    drawStatus("Connecting WiFi...");
  }

  wifiConnected = true;
}

void handleMQTT()
{
  if (!wifiConnected)
  {
    mqttConnected = false;
    return;
  }

  if (client.connected())
  {
    mqttConnected = true;
    client.loop();
    return;
  }

  mqttConnected = false;

  if (millis() - lastMQTTAttempt < 3000)
    return;

  lastMQTTAttempt = millis();

  Serial.println("MQTT reconnect...");

  if (client.connect("housequest_panel"))
  {
    Serial.println("MQTT connected");

    client.subscribe("house/quest/players/+/state");

    mqttConnected = true;
  }
}

int findOrCreatePlayer(String name)
{
  for(int i=0;i<playerCount;i++)
  {
    if(players[i].name == name)
        return i;
  }

  if(playerCount < MAX_PLAYERS)
  {
      players[playerCount].name = name;
      playerCount++;
      return playerCount-1;
  }

  return -1;
}

void callback(char* topic, byte* payload, unsigned int length)
{
  String topicStr = String(topic);

  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, payload, length);
  if (err)
  {
    Serial.println("JSON error");
    return;
  }

  // -------------------------
  // PLAYER STATE UPDATE
  // -------------------------
  if(topicStr.startsWith("house/quest/players/"))
  {
    String name = doc["name"] | "";
    int xp = doc["xp"] | 0;
    int hp = doc["hp"] | 100;
    String role = doc["role"] | "resident";

    if(name == "")
        return;

    int idx = findOrCreatePlayer(name);

    if (idx >= 0)
    {
      players[idx].xp = xp;
      players[idx].hp = hp;
      players[idx].role = role;

      Serial.printf("Player update: %s XP=%d HP=%d\n",
                    name.c_str(), xp, hp);
    }

    dataReceived = true;
  }

  // -------------------------
  // AWARD UPDATE
  // -------------------------
  else if(topicStr == "house/quest/awards")
  {
    currentAward.title = doc["title"] | "";
    currentAward.player = doc["player"] | "";
    currentAward.duration = doc["duration"] | "day";

    awardAvailable = true;

    Serial.printf("Award update: %s -> %s\n",
                  currentAward.title.c_str(),
                  currentAward.player.c_str());
  }

  // Bildschirm neu zeichnen
  drawScreen();
}

void handleScreenRotation()
{
  if(millis() - lastScreenSwitch < SCREEN_INTERVAL)
      return;

  lastScreenSwitch = millis();

  if(currentScreen == SCREEN_PLAYERS)
      currentScreen = SCREEN_AWARDS;
  else
      currentScreen = SCREEN_PLAYERS;

  drawScreen();
}

void drawConnectionStatus()
{
  #if 0
  tft.fillRect(0,0,320,20,TFT_DARKGREY);

  tft.setCursor(5,5);
  tft.setTextSize(1);

  if(!wifiConnected)
  {
    tft.setTextColor(TFT_RED);
    tft.print("WiFi: reconnecting...");
    return;
  }

  if(!mqttConnected)
  {
    tft.setTextColor(TFT_ORANGE);
    tft.print("MQTT: reconnecting...");
    return;
  }

  if(!dataReceived)
  {
    tft.setTextColor(TFT_YELLOW);
    tft.print("Waiting for game data...");
    return;
  }

  tft.setTextColor(TFT_GREEN);
  tft.print("ONLINE");
  #endif
}

void reconnect()
{
  while (!client.connected())
  {
    drawStatus("Connecting MQTT...");

    client.setKeepAlive(30);
    client.setSocketTimeout(2);

    if (client.connect("housequest_panel"))
    {
      mqttConnected = true;
      client.subscribe("house/quest/players/+/state");
    }
    else
    {
      mqttConnected = false;
      delay(2000);
    }
  }
}

void handlePaging()
{
  if(millis() - lastPageSwitch < 5000)
      return;

  lastPageSwitch = millis();

  int pages = (playerCount + PLAYERS_PER_PAGE - 1) / PLAYERS_PER_PAGE;

  currentPage++;

  if(currentPage >= pages)
      currentPage = 0;

  drawScreen();
}

void drawPlayer(int y, Player p)
{
  tft.setTextSize(2);

  // Name
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(10,y);
  tft.print(p.name);

  // XP
  tft.setCursor(120,y);
  tft.printf("XP %d",p.xp);

  // HP
  tft.setCursor(200,y);
  tft.printf("HP %d",p.hp);

  int bar = map(p.hp,0,100,0,200);

  uint16_t hpColor;

  if(p.hp < 15) {
      hpColor = TFT_MAGENTA;
  }
  else if(p.hp < 25) {
      hpColor = TFT_RED;
  }
  else if(p.hp < 75) {
    hpColor = TFT_YELLOW;
  }
  else {
    hpColor = TFT_GREEN;
  }

  // Hintergrund
  tft.fillRect(10,y+20,200,10,TFT_DARKGREY);

  // HP Balken
  tft.fillRect(10,y+20,bar,10,hpColor);
}

void drawStatusLED(int x, int y, uint16_t color)
{
  tft.fillCircle(x,y,4,color);
}

void drawHeader()
{
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(3);
  tft.setCursor(30,5);
  tft.print("HOUSE QUEST");

  drawStatusLED(310,10, wifiConnected ? TFT_GREEN : TFT_RED);
  drawStatusLED(310,20, mqttConnected ? TFT_GREEN : TFT_ORANGE);
  drawStatusLED(310,30, dataReceived ? TFT_GREEN : TFT_YELLOW);


  int pages = (playerCount + PLAYERS_PER_PAGE - 1) / PLAYERS_PER_PAGE;

  tft.setTextSize(1);
  tft.setCursor(280,10);
  tft.printf("%d/%d", currentPage+1, pages);
}



void drawPlayersPage()
{
  tft.fillScreen(TFT_BLACK);

  drawHeader();

  int start = currentPage * PLAYERS_PER_PAGE;

  for(int i=0;i<PLAYERS_PER_PAGE;i++)
  {
    int idx = start + i;

    if(idx >= playerCount)
        break;

    drawPlayer(60 + i*40, players[idx]);
  }
}

void drawAwardsPage()
{
  if(!awardAvailable)
      return;

  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(60,50);
  tft.print("HOUSE TITLES");

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);

  tft.setCursor(20,120);
  tft.print(currentAward.title);

  tft.setTextSize(1);
  tft.setCursor(20,150);
  tft.print("Holder:");

  tft.setCursor(80,150);
  tft.print(currentAward.player);
}

void drawScreen()
{
  tft.fillScreen(TFT_BLACK);

  drawHeader();

  if(currentScreen == SCREEN_PLAYERS)
      drawPlayersPage();
  else {
      if(!awardAvailable)
        drawPlayersPage();
      else
        drawAwardsPage();
  }
}


void setup()
{
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, LOW);   // Backlight AN

  tft.init();
  tft.setRotation(1);
  drawScreen();
  setup_wifi();

  client.setServer(mqtt_server,1883);
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

  handlePaging();
  //handleScreenRotation();

  drawConnectionStatus();

  delay(50);
}