#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>

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

// Configuration Variables
char deviceId[20] = "GATE_01";
char actuatorType[2] = "1";  // 1 = Servo, 0 = Relay
char mqttServer[40] = "192.168.0.100";
char mqttPort[6] = "1883";
String localAllowedTags;

// Network & MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttAttempt = 0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // SP Timezone (-3h)
const int daylightOffset_sec = 0;

void setup() {
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

  localAllowedTags.reserve(512);

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

  if (actuatorType[0] == '1') {
    ESP32PWM::allocateTimer(0);
    gateServo.setPeriodHertz(50);
    gateServo.attach(ACTUATOR_PIN, 500, 2400);
    gateServo.write(0);
  } else {
    pinMode(ACTUATOR_PIN, OUTPUT);
    digitalWrite(ACTUATOR_PIN, LOW);
  }

  initializeDevice();
  setupWiFiManager();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  mqttClient.setServer(mqttServer, atoi(mqttPort));
  mqttClient.setCallback(mqttCallback);

  setIdleState();
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long startPress = millis();

    while (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - startPress > 10000) {
        setLedColor(255, 0, 0);
        lcdDisplay.clear();
        lcdDisplay.print(F("Resetando WiFi"));
        lcdDisplay.setCursor(0, 1);
        lcdDisplay.print(F("Aguarde..."));

        tone(BUZZER_PIN, 1000, 500);
        delay(500);
        noTone(BUZZER_PIN);

        WiFiManager wm;
        wm.resetSettings();

        delay(1000);
        ESP.restart();
      }
      delay(10);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      unsigned long currentMillis = millis();
      if (currentMillis - lastMqttAttempt > 5000) {
        lastMqttAttempt = currentMillis;
        reconnectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }

  if (!rfidReader.PICC_IsNewCardPresent() || !rfidReader.PICC_ReadCardSerial()) {
    return;
  }

  char scannedTag[32] = { 0 };
  for (byte i = 0; i < rfidReader.uid.size; i++) {
    sprintf(&scannedTag[i * 3], "%02X ", rfidReader.uid.uidByte[i]);
  }
  scannedTag[rfidReader.uid.size * 3 - 1] = '\0';

  Serial.print(F("Tag Lida: "));
  Serial.println(scannedTag);

  if (isTagAuthorized(scannedTag)) {
    grantAccess(scannedTag);
  } else {
    denyAccess(scannedTag);
  }

  rfidReader.PICC_HaltA();
}

void setupWiFiManager() {
  lcdDisplay.clear();
  lcdDisplay.setCursor(0, 0);
  lcdDisplay.print(F("Conectando WiFi"));

  delay(1500);

  WiFiManager wm;

  WiFiManagerParameter custom_dev_id("devid", "ID do Equipamento", deviceId, 20);
  WiFiManagerParameter custom_actuator("actuator", "Atuador (1=Servo, 0=Rele)", actuatorType, 2);
  WiFiManagerParameter custom_mqtt_server("server", "IP do Servidor MQTT", mqttServer, 40);
  WiFiManagerParameter custom_mqtt_port("port", "Porta MQTT", mqttPort, 6);

  wm.addParameter(&custom_dev_id);
  wm.addParameter(&custom_actuator);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);

  // Define um tempo limite de 3 minutos (180 segundos) para o portal ficar aberto.
  wm.setConfigPortalTimeout(180);

  // Define uma função de callback (retorno) que será chamada ASSIM QUE o portal for iniciado,
  // ou seja, quando ele falhar em conectar ao Wi-Fi e abrir a rede "Gatekeeper_AP".
  wm.setAPCallback([](WiFiManager* myWiFiManager) {
    Serial.println(F("Modo AP (Portal) Iniciado."));

    // Feedback visual de que precisa ser configurado
    setLedColor(255, 165, 0);  // LED Laranja (Aviso)

    lcdDisplay.clear();
    lcdDisplay.setCursor(0, 0);
    lcdDisplay.print(F("Configure em:"));
    lcdDisplay.setCursor(0, 1);
    // Imprime o nome da rede AP criada
    lcdDisplay.print(myWiFiManager->getConfigPortalSSID());
  });

  // Tenta conectar. Se falhar, abre o portal. Se estourar o timeout, retorna "false".
  if (!wm.autoConnect("Gatekeeper_AP")) {
    Serial.println(F("Falha ao conectar e timeout atingido. Modo Offline Ativo."));
    // O sistema seguirá em frente no modo offline.
  } else {
    // Se conectou com sucesso, salva as configurações.
    Serial.println(F("Conectado ao Wi-Fi!"));

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

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  char lwtTopic[64];
  snprintf(lwtTopic, sizeof(lwtTopic), "gatekeeper/status/%s", deviceId);
  char lwtPayload[64];
  snprintf(lwtPayload, sizeof(lwtPayload), "{\"device\":\"%s\",\"status\":\"offline\"}", deviceId);

  if (mqttClient.connect(deviceId, NULL, NULL, lwtTopic, 1, true, lwtPayload)) {
    Serial.println(F("Conectado ao MQTT!"));

    char onlinePayload[64];
    snprintf(onlinePayload, sizeof(onlinePayload), "{\"device\":\"%s\",\"status\":\"online\"}", deviceId);
    mqttClient.publish(lwtTopic, onlinePayload, true);

    char topic[64];
    snprintf(topic, sizeof(topic), "gatekeeper/update/%s", deviceId);
    mqttClient.subscribe(topic);

    Serial.print(F("Inscrito no topico: "));
    Serial.println(topic);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  localAllowedTags = String((char*)payload, length);
  preferences.putString("tags", localAllowedTags);
  Serial.println(F("Base local atualizada via MQTT."));
}

void publishLog(const char* tag, bool authorized) {
  if (mqttClient.connected()) {
    struct tm timeinfo;
    char timestamp[30] = "Offline_Time";

    if (getLocalTime(&timeinfo)) {
      strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    }

    char payload[200];
    snprintf(payload, sizeof(payload),
             "{\"identificadorMqtt\":\"%s\",\"codigoTagLida\":\"%s\",\"acessoConcedido\":%s,\"dataHoraEvento\":\"%s\"}",
             deviceId, tag, authorized ? "true" : "false", timestamp);

    mqttClient.publish("gatekeeper/logs", payload);
  }
}

bool isTagAuthorized(const char* tag) {
  return (localAllowedTags.indexOf(tag) >= 0);
}

void initializeDevice() {
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

void setIdleState() {
  setLedColor(0, 0, 255);
  lcdDisplay.clear();
  lcdDisplay.setCursor(0, 0);
  lcdDisplay.print(F(" Aproxime a tag "));

  lcdDisplay.setCursor(0, 1);
  if (WiFi.status() == WL_CONNECTED) {
    lcdDisplay.print(mqttClient.connected() ? F(" Online / MQTT ") : F(" Online / NoMQTT"));
  } else {
    lcdDisplay.print(F("  Modo Offline  "));
  }
}

void grantAccess(const char* tag) {
  setLedColor(0, 255, 0);
  lcdDisplay.clear();
  lcdDisplay.print(F("Acesso Liberado!"));

  publishLog(tag, true);

  tone(BUZZER_PIN, 3000, 150);
  delay(200);
  tone(BUZZER_PIN, 3000, 150);
  delay(200);
  noTone(BUZZER_PIN);

  if (actuatorType[0] == '1') {
    gateServo.write(90);
    delay(3000);
    gateServo.write(0);
  } else {
    digitalWrite(ACTUATOR_PIN, HIGH);
    delay(3000);
    digitalWrite(ACTUATOR_PIN, LOW);
  }

  setIdleState();
}

void denyAccess(const char* tag) {
  setLedColor(255, 0, 0);
  lcdDisplay.clear();
  lcdDisplay.print(F(" Acesso Negado! "));

  publishLog(tag, false);

  tone(BUZZER_PIN, 500, 1000);
  delay(1500);
  noTone(BUZZER_PIN);

  setIdleState();
}

void setLedColor(int r, int g, int b) {
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
}