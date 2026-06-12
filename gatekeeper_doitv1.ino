#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoJson.h>

#define BUTTON_PIN 0
#define SS_PIN 5
#define RST_PIN 4
#define LED_R 14
#define LED_G 12
#define LED_B 13
#define BUZZER_PIN 27
#define ACTUATOR_PIN 26

MFRC522 rfidReader(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcdDisplay(0x27, 16, 2);
Servo gateServo;
Preferences preferences;

char deviceId[20] = "GATE_01";
char actuatorType[2] = "1"; // 1 = Servo, 0 = Relay
char mqttServer[40] = "192.168.0.100";
char mqttPort[6] = "1883";
String localAllowedTags;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttAttempt = 0;

bool waitingForResponse = false;
unsigned long responseTimeout = 0;
char currentScannedTag[32] = {0};

const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;
const int daylightOffset_sec = 0;

void setup()
{
  Serial.begin(115200);

  SPI.begin();
  rfidReader.PCD_Init();
  lcdDisplay.init();
  lcdDisplay.backlight();

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  preferences.begin("gatekeeper", false);
  String savedId = preferences.getString("deviceId", "GATE_01");
  String savedAct = preferences.getString("actuator", "1");
  String savedMqtt = preferences.getString("mqttSrv", "192.168.0.100");
  String savedPort = preferences.getString("mqttPrt", "1883");

  localAllowedTags = preferences.getString("tags", "");

  strncpy(deviceId, savedId.c_str(), sizeof(deviceId));
  strncpy(actuatorType, savedAct.c_str(), sizeof(actuatorType));
  strncpy(mqttServer, savedMqtt.c_str(), sizeof(mqttServer));
  strncpy(mqttPort, savedPort.c_str(), sizeof(mqttPort));

  if (actuatorType[0] == '1')
  {
    ESP32PWM::allocateTimer(0);
    gateServo.setPeriodHertz(50);
    gateServo.attach(ACTUATOR_PIN, 500, 2400);
    gateServo.write(0);
  }
  else
  {
    pinMode(ACTUATOR_PIN, OUTPUT);
    digitalWrite(ACTUATOR_PIN, LOW);
  }

  initializeDevice();
  setupWiFiManager();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  mqttClient.setBufferSize(1024);
  mqttClient.setServer(mqttServer, atoi(mqttPort));
  mqttClient.setCallback(mqttCallback);

  setIdleState();
}

void loop()
{
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    unsigned long startPress = millis();
    while (digitalRead(BUTTON_PIN) == LOW)
    {
      if (millis() - startPress > 10000)
      {
        setLedColor(255, 0, 0);
        lcdDisplay.clear();
        lcdDisplay.print(F("Resetando WiFi"));
        tone(BUZZER_PIN, 1000, 500);
        delay(500);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
      }
      delay(10);
    }
  }

  bool isOnline = (WiFi.status() == WL_CONNECTED && mqttClient.connected());

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!mqttClient.connected())
    {
      if (millis() - lastMqttAttempt > 5000)
      {
        lastMqttAttempt = millis();
        reconnectMQTT();
      }
    }
    else
    {
      mqttClient.loop();
    }
  }

  if (waitingForResponse && millis() > responseTimeout)
  {
    Serial.println(F("Timeout do Servidor. Usando Cache Offline."));
    waitingForResponse = false;
    processOfflineAccess(currentScannedTag);
  }

  if (!waitingForResponse)
  {
    if (!rfidReader.PICC_IsNewCardPresent() || !rfidReader.PICC_ReadCardSerial())
    {
      return;
    }

    memset(currentScannedTag, 0, sizeof(currentScannedTag));
    for (byte i = 0; i < rfidReader.uid.size; i++)
    {
      sprintf(&currentScannedTag[i * 2], "%02X", rfidReader.uid.uidByte[i]);
    }

    Serial.print(F("Tag Lida: "));
    Serial.println(currentScannedTag);

    if (isOnline)
    {
      JsonDocument doc;
      doc["tagRead"] = currentScannedTag;
      doc["mqttIdentifier"] = deviceId;

      char payload[256];
      serializeJson(doc, payload);

      mqttClient.publish("gatekeeper/access/request", payload);

      waitingForResponse = true;
      responseTimeout = millis() + 5000;

      setLedColor(0, 0, 255);
      lcdDisplay.clear();
      lcdDisplay.print(F("Validando..."));
    }
    else
    {
      processOfflineAccess(currentScannedTag);
    }

    rfidReader.PICC_HaltA();
  }
}

void processOfflineAccess(const char *tag)
{
  bool granted = isTagAuthorized(tag);

  if (granted)
  {
    grantAccess(tag, false);
  }
  else
  {
    denyAccess(tag, false);
  }

  saveOfflineLog(tag, granted);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  String topicStr = String(topic);

  if (topicStr.indexOf("/sync/") > 0)
  {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error)
    {
      JsonArray tagsArray = doc["allowedTags"];
      String newCache = "";
      for (JsonVariant v : tagsArray)
      {
        newCache += v.as<String>() + ",";
      }

      localAllowedTags = newCache;
      preferences.putString("tags", localAllowedTags);
      Serial.println(F("✅ Cache atualizado via MQTT."));
    }
  }

  if (topicStr.indexOf("/response/") > 0 && waitingForResponse)
  {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error)
    {
      bool isGranted = doc["isGranted"];
      waitingForResponse = false;

      if (isGranted)
      {
        grantAccess(currentScannedTag, true);
      }
      else
      {
        denyAccess(currentScannedTag, true);
      }
    }
  }
}

void saveOfflineLog(const char *tag, bool granted)
{
  String logs = preferences.getString("offLogs", "");
  logs += String(tag) + (granted ? ":1;" : ":0;");
  preferences.putString("offLogs", logs);
  Serial.println(F("💾 Log offline salvo no cache local."));
}

void flushOfflineLogs()
{
  String logs = preferences.getString("offLogs", "");
  if (logs.length() > 0)
  {
    mqttClient.publish("gatekeeper/access/offline_logs", logs.c_str());
    preferences.putString("offLogs", "");
    Serial.println(F("📤 Fila de logs offline esvaziada e enviada!"));
  }
}

void setupWiFiManager()
{
  lcdDisplay.clear();
  lcdDisplay.print(F("Conectando WiFi"));
  delay(1500);

  WiFiManager wm;
  WiFiManagerParameter custom_dev_id("devid", "ID", deviceId, 20);
  WiFiManagerParameter custom_actuator("actuator", "Atuador (1/0)", actuatorType, 2);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqttServer, 40);
  WiFiManagerParameter custom_mqtt_port("port", "Porta", mqttPort, 6);

  wm.addParameter(&custom_dev_id);
  wm.addParameter(&custom_actuator);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("Gatekeeper_AP"))
  {
    Serial.println(F("Modo Offline Ativo."));
  }
  else
  {
    strncpy(deviceId, custom_dev_id.getValue(), sizeof(deviceId));
    strncpy(actuatorType, custom_actuator.getValue(), sizeof(actuatorType));
    strncpy(mqttServer, custom_mqtt_server.getValue(), sizeof(mqttServer));
    strncpy(mqttPort, custom_mqtt_port.getValue(), sizeof(mqttPort));

    preferences.putString("deviceId", deviceId);
    preferences.putString("actuator", actuatorType);
    preferences.putString("mqttSrv", mqttServer);
    preferences.putString("mqttPrt", mqttPort);
  }
}

void reconnectMQTT()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  char lwtTopic[64];
  snprintf(lwtTopic, sizeof(lwtTopic), "gatekeeper/status/%s", deviceId);
  if (mqttClient.connect(deviceId, NULL, NULL, lwtTopic, 1, true, "offline"))
  {
    Serial.println(F("Conectado ao MQTT!"));
    mqttClient.publish(lwtTopic, "online", true);

    char responseTopic[64];
    snprintf(responseTopic, sizeof(responseTopic), "gatekeeper/access/response/%s", deviceId);
    mqttClient.subscribe(responseTopic);

    char syncTopic[64];
    snprintf(syncTopic, sizeof(syncTopic), "gatekeeper/access/sync/%s", deviceId);
    mqttClient.subscribe(syncTopic);

    mqttClient.publish("gatekeeper/access/sync/request", deviceId);
    flushOfflineLogs();
  }
}

bool isTagAuthorized(const char *tag)
{
  return (localAllowedTags.indexOf(String(tag)) >= 0);
}

void initializeDevice()
{
  setLedColor(255, 255, 255);
  tone(BUZZER_PIN, 2000, 300);
  delay(300);
  noTone(BUZZER_PIN);

  lcdDisplay.clear();
  lcdDisplay.print(F("   Gatekeeper   "));
  lcdDisplay.setCursor(0, 1);
  lcdDisplay.print(F(" Inicializando. "));
  delay(2000);
}

void setIdleState()
{
  setLedColor(0, 0, 255);
  lcdDisplay.clear();
  lcdDisplay.setCursor(0, 0);
  lcdDisplay.print(F(" Aproxime a tag "));
  lcdDisplay.setCursor(0, 1);
  if (WiFi.status() == WL_CONNECTED)
  {
    lcdDisplay.print(mqttClient.connected() ? F(" Online / MQTT ") : F(" Online / NoMQTT"));
  }
  else
  {
    lcdDisplay.print(F("  Modo Offline  "));
  }
}

void grantAccess(const char *tag, bool isOnline)
{
  setLedColor(0, 255, 0);
  lcdDisplay.clear();
  lcdDisplay.print(F("Acesso Liberado!"));

  tone(BUZZER_PIN, 3000, 150);
  delay(200);
  tone(BUZZER_PIN, 3000, 150);
  delay(200);
  noTone(BUZZER_PIN);

  if (actuatorType[0] == '1')
  {
    gateServo.write(90);
    delay(3000);
    gateServo.write(0);
  }
  else
  {
    digitalWrite(ACTUATOR_PIN, HIGH);
    delay(3000);
    digitalWrite(ACTUATOR_PIN, LOW);
  }
  setIdleState();
}

void denyAccess(const char *tag, bool isOnline)
{
  setLedColor(255, 0, 0);
  lcdDisplay.clear();
  lcdDisplay.print(F(" Acesso Negado! "));

  tone(BUZZER_PIN, 500, 1000);
  delay(1500);
  noTone(BUZZER_PIN);

  setIdleState();
}

void setLedColor(int r, int g, int b)
{
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}
