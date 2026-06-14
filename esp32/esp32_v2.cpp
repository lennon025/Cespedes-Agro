/*
 * Autor: Lennon Perdomo Céspedes
 * Contacto: lennon.perdomocespedes@gmail.com
 * Proyecto: Céspedes.Agro - Sistema IoT para agricultura urbana
 *
 * Siempre pensando en mi Cuba, desde Moscú, Rusia.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Preferences.h>
#include "SHTSensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BH1750.h>
#include "BluetoothSerial.h"

// ==================== CONFIGURACIÓN BLUETOOTH ====================
const char* dispositivoBT = "CespedesAgro";

// ==================== CONFIGURACIÓN MQTT ====================
const int mqtt_port = 1883;
const char* mqtt_topic_sensores = "cespedes/sensores";
const char* mqtt_topic_actuadores = "cespedes/actuadores";
const char* mqtt_alarm_topic = "cespedes/alarma";

// ==================== OBJETOS ====================
WiFiClient espClient;
PubSubClient client(espClient);
BluetoothSerial SerialBT;
Preferences preferences;

// ==================== SENSORES ====================
SHTSensor sht30;
BH1750 lightMeter(0x23);
const float factorCupula = 1.8;
#define ONE_WIRE_BUS 5
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
#define LLUVIA_PIN 33
#define FLOW_SENSOR_PIN 27

// ==================== RELÉS ====================
#define VALVULA_PIN 18
#define HUMIDIFICADOR_PIN 19
bool estadoValvula = false;
bool estadoHumidificador = false;

// ==================== CONFIGURACIÓN GUARDADA ====================
String wifiSSID = "";
String wifiPassword = "";
String mqttServer = "";

// ==================== ESTADO ====================
bool wifiConectado = false;
bool mqttConectado = false;
String wifiIP = "0.0.0.0";
unsigned long ultimaLecturaSensores = 0;
const unsigned long INTERVALO_LECTURA = 5000;
unsigned long ultimoIntentoWiFi = 0;
const unsigned long INTERVALO_RECONEXION_WIFI = 30000;
unsigned long ultimoIntentoMQTT = 0;

// ==================== DATOS SENSORES ====================
float lastTempAmb = 0, lastHumAmb = 0, lastTempAgua = 0;
bool lastLluvia = false;
float lastTempChip = 0;
uint16_t lastLux = 0;
float lastCaudal = 0;
String lastNivelLuz = "";

// ==================== SHT30 ERROR COUNTER ====================
int errorCounterSHT = 0;
const int MAX_ERRORS_SHT = 3;

// ==================== CAUDAL ====================
volatile unsigned long flowPulseCount = 0;
void IRAM_ATTR countPulse() {
  flowPulseCount++;
}

// ==================== ALARMA ====================
const float TEMP_ALARM_THRESHOLD = 70.0;
bool alarmaEnviada = false;
unsigned long ultimaAlarma = 0;
const unsigned long INTERVALO_REENVIO_ALARMA = 60000;

// ==================== IDIOMA ====================
String idioma = "ES";

// ==================== PROTOTIPOS ====================
void cargarConfiguracion();
void conectarWiFi();
void conectarMQTT();
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, String &nivelLuz);
float calcularCaudal();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void verificarAlarmaTemperatura(float tempChip);
void procesarComandoBT();
void mostrarAyuda();
void mostrarSensores();
void mostrarStatus();
void publicarEstadoRelays();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Wire.begin();

  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║   🌱 CESPEDESAGRO v3.0      ║");
  Serial.println("╚══════════════════════════════╝\n");

  // Cargar configuración guardada
  cargarConfiguracion();

  // Bluetooth
  if (!SerialBT.begin(dispositivoBT)) {
    Serial.println("Error al iniciar Bluetooth.");
  } else {
    Serial.println("📱 Bluetooth: " + String(dispositivoBT));
  }

  // SHT30
  if (sht30.init()) {
    Serial.println("✅ SHT30 iniciado correctamente.");
    sht30.setAccuracy(SHTSensor::SHT_ACCURACY_HIGH);
  } else {
    Serial.println("❌ ERROR: No se encontró el sensor SHT30.");
  }

  // DS18B20
  ds18b20.begin();
  Serial.println("✅ DS18B20 iniciado");

  // Lluvia
  pinMode(LLUVIA_PIN, INPUT);
  Serial.println("✅ Sensor lluvia");

  // BH1750
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("❌ Error al iniciar BH1750.");
  } else {
    Serial.println("✅ BH1750 iniciado");
    Wire.beginTransmission(0x23);
    Wire.write(0x40);
    Wire.write(31);
    Wire.endTransmission();
  }

  // Caudal
  analogReadResolution(12);
  pinMode(FLOW_SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), countPulse, RISING);
  Serial.println("✅ Sensor caudal");

  // Relés
  pinMode(VALVULA_PIN, OUTPUT);
  pinMode(HUMIDIFICADOR_PIN, OUTPUT);
  digitalWrite(VALVULA_PIN, LOW);
  digitalWrite(HUMIDIFICADOR_PIN, LOW);
  Serial.println("✅ Relés OK");

  // WiFi
  if (wifiSSID.length() > 0) {
    Serial.println("\n📡 WiFi guardado: " + wifiSSID);
    conectarWiFi();
  } else {
    Serial.println("\n⚠️  Sin WiFi configurado");
  }

  if (mqttServer.length() == 0) {
    Serial.println("⚠️  Sin MQTT configurado");
  }

  SerialBT.println("\nCespedesAgro listo");
  SerialBT.println("ES: COMANDOS | EN: HELP | RU: ПОМОЩЬ");
  Serial.println("\n✅ Sistema listo\n");
}

// ==================== LOOP ====================
void loop() {
  unsigned long ahora = millis();

  // Monitorear WiFi
  bool wifiAhora = (WiFi.status() == WL_CONNECTED);
  if (wifiAhora != wifiConectado) {
    wifiConectado = wifiAhora;
    if (wifiConectado) {
      wifiIP = WiFi.localIP().toString();
      Serial.println("\n✅ WiFi conectado. IP: " + wifiIP);
      client.setCallback(callbackMQTT);
      if (mqttServer.length() > 0) {
        conectarMQTT();
      }
    } else {
      wifiIP = "0.0.0.0";
      mqttConectado = false;
      Serial.println("❌ WiFi desconectado.");
    }
  }

  // MQTT
  if (wifiConectado && mqttServer.length() > 0) {
    if (!client.connected()) {
      if (ahora - ultimoIntentoMQTT > 10000) {
        conectarMQTT();
        ultimoIntentoMQTT = ahora;
      }
    } else {
      mqttConectado = true;
      client.loop();
    }
  } else if (!wifiConectado && wifiSSID.length() > 0) {
    if (ahora - ultimoIntentoWiFi >= INTERVALO_RECONEXION_WIFI) {
      Serial.println("🔄 Reintentando WiFi...");
      WiFi.reconnect();
      ultimoIntentoWiFi = ahora;
    }
  }

  // Leer sensores
  if (ahora - ultimaLecturaSensores >= INTERVALO_LECTURA) {
    float t, h, ta, tc;
    bool lluvia;
    uint16_t lux;
    String nivelLuz;

    lastCaudal = calcularCaudal();
    leerSensores(t, h, ta, lluvia, tc, lux, nivelLuz);

    lastTempAmb = t;
    lastHumAmb = h;
    lastTempAgua = ta;
    lastLluvia = lluvia;
    lastTempChip = tc;
    lastLux = lux;
    lastNivelLuz = nivelLuz;

    // Enviar por MQTT
    if (mqttConectado && client.connected()) {
      String json = "{";
      json += "\"temperatura\":" + String(t, 1) + ",";
      json += "\"humedad\":" + String(h, 1) + ",";
      json += "\"agua_temp\":" + String(ta, 1) + ",";
      json += "\"lluvia\":" + String(lluvia ? "true" : "false") + ",";
      json += "\"caudal\":" + String(lastCaudal, 1) + ",";
      json += "\"temp_chip\":" + String(tc, 1) + ",";
      json += "\"luz\":" + String(lux) + ",";
      json += "\"luz_txt\":\"" + nivelLuz + "\",";
      json += "\"valvula\":" + String(estadoValvula ? "true" : "false") + ",";
      json += "\"humificador\":" + String(estadoHumidificador ? "true" : "false");
      json += "}";
      client.publish(mqtt_topic_sensores, json.c_str());
      Serial.println("MQTT enviado: " + json);
    }

    verificarAlarmaTemperatura(tc);
    ultimaLecturaSensores = ahora;
  }

  // Comandos Bluetooth
  procesarComandoBT();
  
  delay(10);
}

// ==================== CONECTAR WIFI ====================
void conectarWiFi() {
  if (wifiSSID.length() == 0) return;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  Serial.print("Conectando a " + wifiSSID + " ");
  
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    wifiIP = WiFi.localIP().toString();
    Serial.println(" ✅");
  } else {
    wifiConectado = false;
    wifiIP = "0.0.0.0";
    Serial.println(" ❌");
  }
}

// ==================== CONECTAR MQTT ====================
void conectarMQTT() {
  if (!wifiConectado || mqttServer.length() == 0) return;
  
  client.setServer(mqttServer.c_str(), mqtt_port);
  String clientId = "ESP32_Cespedes_" + String(random(0xffff), HEX);
  
  Serial.print("MQTT a " + mqttServer + "... ");
  
  if (client.connect(clientId.c_str())) {
    mqttConectado = true;
    Serial.println("✅");
    client.subscribe(mqtt_topic_actuadores);
    publicarEstadoRelays();
  } else {
    mqttConectado = false;
    Serial.print("❌ Error: ");
    Serial.println(client.state());
  }
}

// ==================== LEER SENSORES (VERSIÓN ORIGINAL QUE FUNCIONA) ====================
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, String &nivelLuz) {
  
  // SHT30
  if (sht30.readSample()) {
    tempAmb = sht30.getTemperature();
    humAmb = sht30.getHumidity();
    errorCounterSHT = 0;
  } else {
    errorCounterSHT++;
    Serial.print("ERROR SHT30. Intento: ");
    Serial.println(errorCounterSHT);
    if (errorCounterSHT >= MAX_ERRORS_SHT) {
      Serial.println("Reinicializando SHT30...");
      sht30.init();
      sht30.setAccuracy(SHTSensor::SHT_ACCURACY_HIGH);
      errorCounterSHT = 0;
    }
  }

  // DS18B20
  ds18b20.requestTemperatures();
  tempAgua = ds18b20.getTempCByIndex(0);

  // Lluvia
  lluvia = (digitalRead(LLUVIA_PIN) == LOW);
  
  // Chip
  tempChip = temperatureRead();

  // BH1750
  uint16_t rawLux = lightMeter.readLightLevel();
  lux = rawLux * factorCupula;

  if (lux >= 100000) nivelLuz = "Pleno sol";
  else if (lux >= 20000) nivelLuz = "Parcialmente nublado";
  else if (lux >= 10000) nivelLuz = "Nublado";
  else if (lux >= 500) nivelLuz = "Sombra";
  else nivelLuz = "Muy oscuro";
}

// ==================== CAUDAL ====================
float calcularCaudal() {
  const float pulsosPorLitro = 450.0;
  float minutos = (float)INTERVALO_LECTURA / 60000.0;
  unsigned long pulsos = flowPulseCount;
  flowPulseCount = 0;
  if (minutos > 0 && pulsos > 0) {
    return (pulsos / pulsosPorLitro) / minutos;
  }
  return 0;
}

// ==================== CALLBACK MQTT (BIDIRECCIONAL) ====================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("📩 MQTT recibido [" + String(topic) + "]: " + msg);
  
  if (String(topic) == mqtt_topic_actuadores) {
    int start = msg.indexOf("{");
    int end = msg.lastIndexOf("}");
    if (start >= 0 && end > start) {
      String json = msg.substring(start, end + 1);
      
      // Buscar tipo
      int tipoIdx = json.indexOf("\"tipo\"");
      int estadoIdx = json.indexOf("\"estado\"");
      
      if (tipoIdx >= 0 && estadoIdx >= 0) {
        int tipoValStart = json.indexOf(":", tipoIdx) + 1;
        int tipoValEnd = json.indexOf(",", tipoValStart);
        if (tipoValEnd == -1) tipoValEnd = json.indexOf("}", tipoValStart);
        String tipo = json.substring(tipoValStart, tipoValEnd);
        tipo.replace("\"", "");
        tipo.replace(" ", "");
        
        int estadoValStart = json.indexOf(":", estadoIdx) + 1;
        int estadoValEnd = json.indexOf(",", estadoValStart);
        if (estadoValEnd == -1) estadoValEnd = json.indexOf("}", estadoValStart);
        String estadoStr = json.substring(estadoValStart, estadoValEnd);
        estadoStr.replace("\"", "");
        estadoStr.replace(" ", "");
        int estado = estadoStr.toInt();
        
        if (tipo == "valvula") {
          digitalWrite(VALVULA_PIN, estado ? HIGH : LOW);
          estadoValvula = estado;
          Serial.println("Válvula " + String(estado ? "ABIERTA" : "CERRADA"));
          if (SerialBT.hasClient()) SerialBT.println("✅ Válvula " + String(estado ? "ABIERTA" : "CERRADA") + " (MQTT)");
        } else if (tipo == "humificador") {
          digitalWrite(HUMIDIFICADOR_PIN, estado ? HIGH : LOW);
          estadoHumidificador = estado;
          Serial.println("Humidificador " + String(estado ? "ENCENDIDO" : "APAGADO"));
          if (SerialBT.hasClient()) SerialBT.println("✅ Humidificador " + String(estado ? "ON" : "OFF") + " (MQTT)");
        }
        
        publicarEstadoRelays();
      }
    }
  }
}

// ==================== PUBLICAR ESTADO RELAYS ====================
void publicarEstadoRelays() {
  if (!client.connected()) return;
  String estado = "{\"valvula\":" + String(estadoValvula ? "true" : "false") + 
                  ",\"humificador\":" + String(estadoHumidificador ? "true" : "false") + "}";
  client.publish("cespedes/estado", estado.c_str());
}

// ==================== ALARMA ====================
void verificarAlarmaTemperatura(float tempChip) {
  if (tempChip >= TEMP_ALARM_THRESHOLD) {
    if (mqttConectado && client.connected()) {
      String alarma = "{\"alarma\":\"sobrecalentamiento\",\"temp_chip\":" + String(tempChip, 1) + ",\"umbral\":" + String(TEMP_ALARM_THRESHOLD) + "}";
      client.publish(mqtt_alarm_topic, alarma.c_str());
    }
    if (SerialBT.hasClient()) {
      SerialBT.println("⚠️ ¡ALARMA! Temperatura del chip: " + String(tempChip, 1) + " °C");
    }
    unsigned long ahora = millis();
    if (!alarmaEnviada || (ahora - ultimaAlarma > INTERVALO_REENVIO_ALARMA)) {
      alarmaEnviada = true;
      ultimaAlarma = ahora;
    }
  } else {
    alarmaEnviada = false;
  }
}

// ==================== MOSTRAR AYUDA ====================
void mostrarAyuda() {
  if (idioma == "ES") {
    SerialBT.println("\n══════ COMANDOS ══════");
    SerialBT.println("COMANDOS    Ver esta ayuda");
    SerialBT.println("ESCANEAR    Buscar redes WiFi");
    SerialBT.println("WIFI RED|CLAVE  Guardar y conectar");
    SerialBT.println("MQTT IP     Configurar servidor MQTT");
    SerialBT.println("ESTADO      Estado del sistema");
    SerialBT.println("SENSORES    Leer todos los sensores");
    SerialBT.println("VALVULA 1   Abrir válvula");
    SerialBT.println("VALVULA 0   Cerrar válvula");
    SerialBT.println("HUMI 1      Encender humidificador");
    SerialBT.println("HUMI 0      Apagar humidificador");
    SerialBT.println("BORRAR      Borrar configuración");
    SerialBT.println("IP          Ver dirección IP");
    SerialBT.println("REINICIAR   Reiniciar dispositivo");
    SerialBT.println("IDIOMA ES/EN/RU  Cambiar idioma");
    SerialBT.println("═══════════════════════");
  }
  else if (idioma == "EN") {
    SerialBT.println("\n══════ COMMANDS ══════");
    SerialBT.println("HELP        Show this help");
    SerialBT.println("SCAN        Scan WiFi networks");
    SerialBT.println("WIFI NAME|PASS  Save and connect");
    SerialBT.println("MQTT IP     Configure MQTT server");
    SerialBT.println("STATUS      System status");
    SerialBT.println("SENSORS     Read all sensors");
    SerialBT.println("VALVE 1     Open valve");
    SerialBT.println("VALVE 0     Close valve");
    SerialBT.println("HUMI 1      Humidifier ON");
    SerialBT.println("HUMI 0      Humidifier OFF");
    SerialBT.println("CLEAR       Clear configuration");
    SerialBT.println("IP          Show IP address");
    SerialBT.println("RESTART     Restart device");
    SerialBT.println("LANG ES/EN/RU  Change language");
    SerialBT.println("═══════════════════════");
  }
  else if (idioma == "RU") {
    SerialBT.println("\n══════ КОМАНДЫ ══════");
    SerialBT.println("ПОМОЩЬ      Показать помощь");
    SerialBT.println("СКАН        Сканировать WiFi");
    SerialBT.println("WIFI ИМЯ|ПАРОЛЬ  Сохранить и подключить");
    SerialBT.println("MQTT IP     Настроить MQTT сервер");
    SerialBT.println("СТАТУС      Состояние системы");
    SerialBT.println("СЕНСОРЫ     Читать датчики");
    SerialBT.println("КЛАПАН 1    Открыть клапан");
    SerialBT.println("КЛАПАН 0    Закрыть клапан");
    SerialBT.println("ХУМИ 1      Включить увлажнитель");
    SerialBT.println("ХУМИ 0      Выключить увлажнитель");
    SerialBT.println("УДАЛИТЬ     Стереть настройки");
    SerialBT.println("IP          Показать IP адрес");
    SerialBT.println("РЕСТАРТ     Перезагрузить");
    SerialBT.println("ЯЗЫК ES/EN/RU  Сменить язык");
    SerialBT.println("═══════════════════════");
  }
}

// ==================== MOSTRAR SENSORES ====================
void mostrarSensores() {
  if (idioma == "ES") {
    SerialBT.println("\n═══ LECTURA SENSORES ═══");
    SerialBT.printf("🌡️  Temp amb: %.1f°C\n", lastTempAmb);
    SerialBT.printf("💧 Humedad: %.1f%%\n", lastHumAmb);
    SerialBT.printf("🌊 Temp agua: %.1f°C\n", lastTempAgua);
    SerialBT.printf("☔ Lluvia: %s\n", lastLluvia ? "SI" : "NO");
    SerialBT.printf("💨 Caudal: %.1f L/min\n", lastCaudal);
    SerialBT.printf("💡 Luz: %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
    SerialBT.printf("🔥 Chip: %.1f°C\n", lastTempChip);
    SerialBT.printf("🔧 Válvula: %s\n", estadoValvula ? "ABIERTA" : "CERRADA");
    SerialBT.printf("💨 Humidificador: %s\n", estadoHumidificador ? "ON" : "OFF");
    SerialBT.println("════════════════════════");
  }
  else if (idioma == "EN") {
    SerialBT.println("\n═══ SENSOR READINGS ═══");
    SerialBT.printf("🌡️  Temp: %.1f°C\n", lastTempAmb);
    SerialBT.printf("💧 Humidity: %.1f%%\n", lastHumAmb);
    SerialBT.printf("🌊 Water: %.1f°C\n", lastTempAgua);
    SerialBT.printf("☔ Rain: %s\n", lastLluvia ? "YES" : "NO");
    SerialBT.printf("💨 Flow: %.1f L/min\n", lastCaudal);
    SerialBT.printf("💡 Light: %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
    SerialBT.printf("🔥 Chip: %.1f°C\n", lastTempChip);
    SerialBT.printf("🔧 Valve: %s\n", estadoValvula ? "OPEN" : "CLOSED");
    SerialBT.printf("💨 Humidifier: %s\n", estadoHumidificador ? "ON" : "OFF");
    SerialBT.println("════════════════════════");
  }
  else if (idioma == "RU") {
    SerialBT.println("\n═══ ПОКАЗАНИЯ ДАТЧИКОВ ═══");
    SerialBT.printf("🌡️  Темп: %.1f°C\n", lastTempAmb);
    SerialBT.printf("💧 Влажность: %.1f%%\n", lastHumAmb);
    SerialBT.printf("🌊 Вода: %.1f°C\n", lastTempAgua);
    SerialBT.printf("☔ Дождь: %s\n", lastLluvia ? "ДА" : "НЕТ");
    SerialBT.printf("💨 Поток: %.1f L/min\n", lastCaudal);
    SerialBT.printf("💡 Свет: %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
    SerialBT.printf("🔥 Чип: %.1f°C\n", lastTempChip);
    SerialBT.printf("🔧 Клапан: %s\n", estadoValvula ? "ОТКРЫТ" : "ЗАКРЫТ");
    SerialBT.printf("💨 Увлажнитель: %s\n", estadoHumidificador ? "ВКЛ" : "ВЫКЛ");
    SerialBT.println("════════════════════════");
  }
}

// ==================== MOSTRAR STATUS ====================
void mostrarStatus() {
  if (idioma == "ES") {
    SerialBT.println("\n═══ ESTADO SISTEMA ═══");
    SerialBT.print("WiFi guardado: ");
    SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "Ninguno");
    SerialBT.print("WiFi: ");
    SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Desconectado");
    SerialBT.print("MQTT guardado: ");
    SerialBT.println(mqttServer.length() > 0 ? mqttServer : "Ninguno");
    SerialBT.print("MQTT: ");
    SerialBT.println(mqttConectado ? "✅ Conectado" : "❌ Desconectado");
    SerialBT.print("Válvula: ");
    SerialBT.println(estadoValvula ? "ABIERTA" : "CERRADA");
    SerialBT.print("Humidificador: ");
    SerialBT.println(estadoHumidificador ? "ON" : "OFF");
    SerialBT.println("══════════════════════");
  }
  else if (idioma == "EN") {
    SerialBT.println("\n═══ SYSTEM STATUS ═══");
    SerialBT.print("Saved WiFi: ");
    SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "None");
    SerialBT.print("WiFi: ");
    SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Disconnected");
    SerialBT.print("Saved MQTT: ");
    SerialBT.println(mqttServer.length() > 0 ? mqttServer : "None");
    SerialBT.print("MQTT: ");
    SerialBT.println(mqttConectado ? "✅ Connected" : "❌ Disconnected");
    SerialBT.print("Valve: ");
    SerialBT.println(estadoValvula ? "OPEN" : "CLOSED");
    SerialBT.print("Humidifier: ");
    SerialBT.println(estadoHumidificador ? "ON" : "OFF");
    SerialBT.println("══════════════════════");
  }
  else if (idioma == "RU") {
    SerialBT.println("\n═══ СОСТОЯНИЕ СИСТЕМЫ ═══");
    SerialBT.print("WiFi сохранен: ");
    SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "Нет");
    SerialBT.print("WiFi: ");
    SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Отключен");
    SerialBT.print("MQTT сохранен: ");
    SerialBT.println(mqttServer.length() > 0 ? mqttServer : "Нет");
    SerialBT.print("MQTT: ");
    SerialBT.println(mqttConectado ? "✅ Подключен" : "❌ Отключен");
    SerialBT.print("Клапан: ");
    SerialBT.println(estadoValvula ? "ОТКРЫТ" : "ЗАКРЫТ");
    SerialBT.print("Увлажнитель: ");
    SerialBT.println(estadoHumidificador ? "ВКЛ" : "ВЫКЛ");
    SerialBT.println("══════════════════════");
  }
}

// ==================== PROCESAR COMANDOS BLUETOOTH ====================
void procesarComandoBT() {
  if (!SerialBT.available()) return;
  
  String comando = SerialBT.readStringUntil('\n');
  comando.trim();
  String cmd = comando;
  cmd.toUpperCase();
  
  // AYUDA
  if (cmd == "COMANDOS" || cmd == "AYUDA" || cmd == "HELP" || cmd == "H" || cmd == "ПОМОЩЬ" || cmd == "П") {
    mostrarAyuda();
  }
  
  // ESCANEAR
  else if (cmd == "ESCANEAR" || cmd == "SCAN" || cmd == "СКАН") {
    SerialBT.println("🔍 Escaneando redes...");
    int n = WiFi.scanNetworks();
    if (n == 0) {
      SerialBT.println("No se encontraron redes");
    } else {
      for (int i = 0; i < n; i++) {
        SerialBT.printf("%d: %s (%d dBm) %s\n", 
                       i+1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                       WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "🔒" : "🔓");
      }
      SerialBT.println("\nConectar: WIFI nombre|clave");
    }
  }
  
  // WIFI
  else if (cmd.startsWith("WIFI ")) {
    String datos = comando.substring(5);
    int sep = datos.indexOf('|');
    
    if (sep > 0) {
      wifiSSID = datos.substring(0, sep);
      wifiPassword = datos.substring(sep + 1);
      
      preferences.begin("cespedes", false);
      preferences.putString("ssid", wifiSSID);
      preferences.putString("pass", wifiPassword);
      preferences.end();
      
      SerialBT.println("💾 WiFi guardado: " + wifiSSID);
      SerialBT.println("🔄 Conectando...");
      
      WiFi.disconnect();
      delay(500);
      conectarWiFi();
      
      if (wifiConectado) {
        SerialBT.println("✅ Conectado! IP: " + wifiIP);
      } else {
        SerialBT.println("❌ Error al conectar");
      }
    } else {
      SerialBT.println("❌ Formato: WIFI Red|Clave");
    }
  }
  
  // MQTT
  else if (cmd.startsWith("MQTT ")) {
    mqttServer = comando.substring(5);
    mqttServer.trim();
    
    preferences.begin("cespedes", false);
    preferences.putString("mqtt", mqttServer);
    preferences.end();
    
    SerialBT.println("💾 MQTT guardado: " + mqttServer);
    
    if (wifiConectado) {
      client.disconnect();
      conectarMQTT();
      SerialBT.println(mqttConectado ? "✅ MQTT conectado" : "❌ Error MQTT");
    } else {
      SerialBT.println("⚠️  Conecta WiFi primero");
    }
  }
  
  // ESTADO
  else if (cmd == "ESTADO" || cmd == "STATUS" || cmd == "СТАТУС") {
    mostrarStatus();
  }
  
  // SENSORES
  else if (cmd == "SENSORES" || cmd == "SENSORS" || cmd == "СЕНСОРЫ") {
    mostrarSensores();
  }
  
  // VALVULA 1
  else if ((cmd.startsWith("VALVULA ") || cmd.startsWith("VALVE ") || cmd.startsWith("КЛАПАН ")) && cmd.endsWith("1")) {
    digitalWrite(VALVULA_PIN, HIGH);
    estadoValvula = true;
    SerialBT.println("✅ Válvula ABIERTA");
    publicarEstadoRelays();
  }
  // VALVULA 0
  else if ((cmd.startsWith("VALVULA ") || cmd.startsWith("VALVE ") || cmd.startsWith("КЛАПАН ")) && cmd.endsWith("0")) {
    digitalWrite(VALVULA_PIN, LOW);
    estadoValvula = false;
    SerialBT.println("✅ Válvula CERRADA");
    publicarEstadoRelays();
  }
  
  // HUMI 1
  else if ((cmd.startsWith("HUMI ") || cmd.startsWith("ХУМИ ")) && cmd.endsWith("1")) {
    digitalWrite(HUMIDIFICADOR_PIN, HIGH);
    estadoHumidificador = true;
    SerialBT.println("✅ Humidificador ON");
    publicarEstadoRelays();
  }
  // HUMI 0
  else if ((cmd.startsWith("HUMI ") || cmd.startsWith("ХУМИ ")) && cmd.endsWith("0")) {
    digitalWrite(HUMIDIFICADOR_PIN, LOW);
    estadoHumidificador = false;
    SerialBT.println("✅ Humidificador OFF");
    publicarEstadoRelays();
  }
  
  // BORRAR
  else if (cmd == "BORRAR" || cmd == "CLEAR" || cmd == "УДАЛИТЬ") {
    preferences.begin("cespedes", false);
    preferences.clear();
    preferences.end();
    
    wifiSSID = "";
    wifiPassword = "";
    mqttServer = "";
    WiFi.disconnect();
    wifiConectado = false;
    mqttConectado = false;
    
    SerialBT.println("🗑️  Configuración borrada");
  }
  
  // IP
  else if (cmd == "IP") {
    if (wifiConectado) {
      SerialBT.println("📡 IP: " + wifiIP);
    } else {
      SerialBT.println("❌ WiFi no conectado");
    }
  }
  
  // REINICIAR
  else if (cmd == "REINICIAR" || cmd == "RESTART" || cmd == "РЕСТАРТ") {
    SerialBT.println("🔄 Reiniciando...");
    delay(1000);
    ESP.restart();
  }
  
  // IDIOMA
  else if (cmd.startsWith("IDIOMA ") || cmd.startsWith("LANG ") || cmd.startsWith("ЯЗЫК ")) {
    String nuevoIdioma = comando.substring(comando.lastIndexOf(' ') + 1);
    nuevoIdioma.toUpperCase();
    
    if (nuevoIdioma == "ES" || nuevoIdioma == "EN" || nuevoIdioma == "RU") {
      idioma = nuevoIdioma;
      preferences.begin("cespedes", false);
      preferences.putString("idioma", idioma);
      preferences.end();
      
      if (idioma == "ES") SerialBT.println("✅ Idioma: Español");
      else if (idioma == "EN") SerialBT.println("✅ Language: English");
      else SerialBT.println("✅ Язык: Русский");
    }
  }
  
  else if (comando.length() > 0) {
    SerialBT.println("❌ Comando no reconocido: " + comando);
    SerialBT.println("Escribe COMANDOS para ayuda");
  }
}

// ==================== CARGAR CONFIGURACIÓN ====================
void cargarConfiguracion() {
  preferences.begin("cespedes", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  mqttServer = preferences.getString("mqtt", "");
  idioma = preferences.getString("idioma", "ES");
  preferences.end();
  
  Serial.println("📂 Configuración:");
  Serial.println("   WiFi: " + (wifiSSID.length() > 0 ? wifiSSID : "(sin configurar)"));
  Serial.println("   MQTT: " + (mqttServer.length() > 0 ? mqttServer : "(sin configurar)"));
}