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

unsigned long lastWifiCheck = 0;
unsigned long lastMQTTAttempt = 0;

TFT_eSPI tft = TFT_eSPI();

struct Player {
  String name;
  int xp;
  int hp;
  String role;
};

Player players[4];

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

void callback(char* topic, byte* payload, unsigned int length)
{
  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, payload, length);

  if(err)
  {
    Serial.println("JSON error");
    return;
  }

  String name = doc["name"];
  int xp = doc["xp"];
  int hp = doc["hp"];
  String role = doc["role"];

  for (int i=0;i<4;i++)
  {
    if(players[i].name == name || players[i].name == "")
    {
      players[i].name = name;
      players[i].xp = xp;
      players[i].hp = hp;
      players[i].role = role;
      break;
    }
  }

  dataReceived = true;

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

  if(p.hp < 25)
    hpColor = TFT_RED;
  else if(p.hp < 75)
    hpColor = TFT_YELLOW;
  else
    hpColor = TFT_GREEN;

  // Hintergrund
  tft.fillRect(10,y+20,200,10,TFT_DARKGREY);

  // HP Balken
  tft.fillRect(10,y+20,bar,10,hpColor);
}

void drawStatusLED(int x, int y, uint16_t color)
{
  tft.fillCircle(x,y,4,color);
}
void drawScreen()
{
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(3);
  tft.setCursor(40,5);
  tft.print("HOUSE QUEST");

  drawStatusLED(300,10, wifiConnected ? TFT_GREEN : TFT_RED);
  drawStatusLED(300,20, mqttConnected ? TFT_GREEN : TFT_ORANGE);
  drawStatusLED(300,30, dataReceived ? TFT_GREEN : TFT_YELLOW);

  for(int i=0;i<4;i++)
  {
    drawPlayer(60 + i*40, players[i]);
  }
}

#if 0



#define TFT_LED 15

void setup() {

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, LOW);   // Backlight AN

  tft.init();
  tft.setRotation(1);

  tft.fillScreen(TFT_RED);
  delay(1000);

  tft.fillScreen(TFT_GREEN);
  delay(1000);

  tft.fillScreen(TFT_BLUE);
  delay(1000);

  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(20,40);
  tft.println("ILI9341 OK");
}


#else
#define TFT_LED 15

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

#endif

void loop()
{
  handleWifi();

  handleMQTT();

  drawConnectionStatus();

  delay(50);
}