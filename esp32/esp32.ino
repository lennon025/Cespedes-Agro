#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "HDC1080JS.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BH1750.h>
#include "BluetoothSerial.h"

// ==================== CONFIGURACIÓN DE RED ====================
const char* ssid = "user";// usuario de la wifi
const char* password = "passwd"; contraseña de la wifi

// ==================== CONFIGURACIÓN MQTT ====================
const char* brokerPrimario = "192.168.1.2"; // IP primario de la orange pi que debe ser LANs 
const char* brokerSecundario = "192.168.1.3"; //IP Secundario de la orange pi que debe ser Wifi
const int puertoMQTT = 1883;
const char* topicoSensores = "cespedes/sensores";
const char* topicoActuadores = "cespedes/actuadores";
const char* topicoAlarma = "cespedes/alarma";

const char* brokerActual = brokerPrimario;

WiFiClient clienteWifi;
PubSubClient clienteMQTT(clienteWifi);

// ==================== CONFIGURACIÓN BLUETOOTH ====================
BluetoothSerial bluetooth;
const char* nombreBluetooth = "CespedesAgro";

// ==================== SENSORES ====================
HDC1080JS sensorAmbiente;                     // HDC1080 (temperatura y humedad)

// --- BH1750 (luz) ---
BH1750 sensorLuz(0x23);                       // Dirección I2C típica (0x23 ó 0x5C)
const float factorCupula = 1.8;               // Compensación de la cúpula difusora

#define pinOneWire 5
OneWire oneWire(pinOneWire);
DallasTemperature sensorAgua(&oneWire);       // DS18B20
#define pinLluvia 33
#define pinSuelo 36                            // HW-390 en GPIO36 (ADC1_CH0)

// Sensor de caudal YF-S201 en GPIO27 (pull‑up físico externo)
const int pinCaudal = 27;
volatile unsigned long pulsosCaudal = 0;
void IRAM_ATTR contarPulsos() {
  pulsosCaudal++;
}

// ==================== RELÉS ====================
#define pinValvula 18                         // ElectroVálvula (D18)
#define pinHumificador 19                     // Humificador (D19)

// ==================== VARIABLES DE ESTADO ====================
bool wifiConectado = false;
bool mqttConectado = false;
String ipWiFi = "0.0.0.0";

unsigned long ultimaLectura = 0;
const unsigned long intervaloLectura = 5000;  // cada 5 segundos

unsigned long ultimoIntentoWiFi = 0;
const unsigned long intervaloReconexionWiFi = 30000;

// Últimos valores de sensores (para Bluetooth y MQTT)
float ultimaTemperatura = 0, ultimaHumedad = 0, ultimaTempAgua = 0;
bool ultimaLluvia = false;
float ultimaTempChip = 0;
uint16_t ultimaLuz = 0;                       // lux después de aplicar factorCupula
int ultimoSueloPorcentaje = 0;                // humedad del suelo en % (0 = seco, 100 = mojado)
float ultimoCaudal = 0;                       // caudal instantáneo en L/min
String ultimoNivelLuz = "";                   // clasificación de luz

// ==================== CALIBRACIÓN DINÁMICA DEL SENSOR DE SUELO ====================
int sueloSecoMax = 0;        // valor más alto registrado (seco)
int sueloMojadoMin = 4095;   // valor más bajo registrado (mojado)

// Alarma por temperatura del chip
const float umbralAlarmaTemp = 70.0;          // °C
bool alarmaEnviada = false;
unsigned long ultimaAlarma = 0;
const unsigned long intervaloReenvioAlarma = 60000;

// ==================== PROTOTIPOS DE FUNCIONES ====================
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, int &porcentajeSuelo, String &nivelLuz);
float calcularCaudal();
void manejarComandosBluetooth();
void conectarMQTT();
void enviarEstadoRedBluetooth();
void verificarAlarmaTemp(float tempChip);
void callbackMQTT(char* topic, byte* payload, unsigned int length);

// ==================== CONFIGURACIÓN INICIAL ====================
void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Iniciar Bluetooth
  if (!bluetooth.begin(nombreBluetooth)) {
    Serial.println("Error al iniciar Bluetooth.");
  } else {
    Serial.println("Bluetooth iniciado como: " + String(nombreBluetooth));
  }

  // Configurar sensores
  sensorAmbiente.config();
  sensorAgua.begin();
  pinMode(pinLluvia, INPUT);

  // Inicializar BH1750 en modo de alta resolución continua
  if (!sensorLuz.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("Error al iniciar BH1750. Verifica conexión.");
  } else {
    Serial.println("BH1750 iniciado correctamente.");
    // Ajustar el registro MTreg a 31 para evitar saturación en exteriores
    Wire.beginTransmission(0x23);             // Dirección I2C (0x23 ó 0x5C)
    Wire.write(0x40);                         // Registro MTreg
    Wire.write(31);                           // Valor bajo para mayor rango (hasta ~100 000 lux)
    Wire.endTransmission();
  }

  pinMode(pinSuelo, INPUT);
  analogReadResolution(12);                   // Resolución ADC de 12 bits (0‑4095)

  // Configurar sensor de caudal (pull‑up externo → solo INPUT)
  pinMode(pinCaudal, INPUT);
  attachInterrupt(digitalPinToInterrupt(pinCaudal), contarPulsos, RISING);

  // Configurar relés
  pinMode(pinValvula, OUTPUT);
  pinMode(pinHumificador, OUTPUT);
  digitalWrite(pinValvula, LOW);
  digitalWrite(pinHumificador, LOW);

  Serial.println("Sensores y actuadores inicializados. Sistema listo.");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  clienteMQTT.setCallback(callbackMQTT);
}

// ==================== BUCLE PRINCIPAL ====================
void loop() {
  unsigned long ahora = millis();

  // 1. Gestión de WiFi no bloqueante
  bool estadoWiFi = (WiFi.status() == WL_CONNECTED);
  if (estadoWiFi != wifiConectado) {
    wifiConectado = estadoWiFi;
    if (wifiConectado) {
      ipWiFi = WiFi.localIP().toString();
      Serial.println("\nWiFi conectado. IP: " + ipWiFi);
      enviarEstadoRedBluetooth();
    } else {
      ipWiFi = "0.0.0.0";
      mqttConectado = false;
      Serial.println("WiFi desconectado.");
      enviarEstadoRedBluetooth();
    }
  }

  if (wifiConectado) {
    if (!clienteMQTT.connected()) {
      conectarMQTT();
    } else {
      if (!mqttConectado) {
        mqttConectado = true;
        enviarEstadoRedBluetooth();
      }
      clienteMQTT.loop();
    }
  } else {
    if (ahora - ultimoIntentoWiFi >= intervaloReconexionWiFi) {
      Serial.println("Reintentando conexión WiFi...");
      WiFi.reconnect();
      ultimoIntentoWiFi = ahora;
    }
  }

  // 2. Lectura periódica de sensores
  if (ahora - ultimaLectura >= intervaloLectura) {
    float tempAmb, humAmb, tempAgua, tempChip;
    bool lluvia;
    uint16_t lux;
    int porcentajeSuelo;
    String nivelLuz;

    ultimoCaudal = calcularCaudal();
    leerSensores(tempAmb, humAmb, tempAgua, lluvia, tempChip, lux, porcentajeSuelo, nivelLuz);

    ultimaTemperatura = tempAmb;
    ultimaHumedad = humAmb;
    ultimaTempAgua = tempAgua;
    ultimaLluvia = lluvia;
    ultimaTempChip = tempChip;
    ultimaLuz = lux;
    ultimoSueloPorcentaje = porcentajeSuelo;
    ultimoNivelLuz = nivelLuz;

    // Enviar por Bluetooth
    if (bluetooth.hasClient()) {
      String btMensaje = "--- Sensores ---\n";
      btMensaje += "Temp. ambiente: " + String(tempAmb, 1) + " C\n";
      btMensaje += "Humedad: " + String(humAmb, 1) + " %\n";
      btMensaje += "Temp. agua: " + String(tempAgua, 1) + " C\n";
      btMensaje += "Lluvia: " + String(lluvia ? "SI" : "NO") + "\n";
      btMensaje += "Caudal: " + String(ultimoCaudal, 1) + " L/min\n";
      btMensaje += "Temp. chip: " + String(tempChip, 1) + " C\n";
      btMensaje += "Luz: " + String(lux) + " lx (" + nivelLuz + ")\n";
      btMensaje += "Humedad suelo: " + String(porcentajeSuelo) + " %\n";
      btMensaje += "WiFi: " + String(wifiConectado ? ("Conectado " + ipWiFi) : "Desconectado") + "\n";
      btMensaje += "MQTT: " + String(mqttConectado ? ("Conectado " + String(brokerActual)) : "Desconectado") + "\n";
      bluetooth.print(btMensaje);
    }

    // Publicar por MQTT
    if (mqttConectado && clienteMQTT.connected()) {
      String json = "{";
      json += "\"temperatura\":" + String(tempAmb) + ",";
      json += "\"humedad\":" + String(humAmb) + ",";
      json += "\"agua_temp\":" + String(tempAgua) + ",";
      json += "\"lluvia\":" + String(lluvia ? "true" : "false") + ",";
      json += "\"caudal\":" + String(ultimoCaudal, 1) + ",";
      json += "\"temp_chip\":" + String(tempChip) + ",";
      json += "\"luz\":" + String(lux) + ",";
      json += "\"suelo\":" + String(porcentajeSuelo);
      json += "}";
      if (clienteMQTT.publish(topicoSensores, json.c_str())) {
        Serial.println("MQTT publicado: " + json);
      }
    }

    // Verificar alarma por temperatura del chip
    verificarAlarmaTemp(tempChip);

    ultimaLectura = ahora;
  }

  manejarComandosBluetooth();
  delay(10);
}

// ==================== CONEXIÓN MQTT ====================
void conectarMQTT() {
  if (clienteMQTT.connected()) return;

  clienteMQTT.setServer(brokerActual, puertoMQTT);
  String idCliente = "ESP32_Cespedes_" + String(random(0xffff), HEX);

  if (clienteMQTT.connect(idCliente.c_str())) {
    mqttConectado = true;
    Serial.println("MQTT conectado a " + String(brokerActual));
    clienteMQTT.subscribe(topicoActuadores);
    enviarEstadoRedBluetooth();
  } else {
    mqttConectado = false;
    Serial.println("MQTT falló en " + String(brokerActual));
    brokerActual = (brokerActual == brokerPrimario) ? brokerSecundario : brokerPrimario;
    enviarEstadoRedBluetooth();
  }
}

// ==================== CALLBACK MQTT (control de relés desde la web) ====================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String mensaje;
  for (unsigned int i = 0; i < length; i++) mensaje += (char)payload[i];
  Serial.println("Mensaje recibido [" + String(topic) + "]: " + mensaje);

  if (String(topic) == topicoActuadores) {
    int inicio = mensaje.indexOf("{");
    int fin = mensaje.lastIndexOf("}");
    if (inicio >= 0 && fin > inicio) {
      String json = mensaje.substring(inicio, fin + 1);
      int posTipo = json.indexOf("\"tipo\"");
      int posEstado = json.indexOf("\"estado\"");
      if (posTipo >= 0 && posEstado >= 0) {
        // Extraer tipo de actuador
        int iniTipo = json.indexOf(":", posTipo) + 1;
        int finTipo = json.indexOf(",", iniTipo);
        if (finTipo == -1) finTipo = json.indexOf("}", iniTipo);
        String tipo = json.substring(iniTipo, finTipo);
        tipo.replace("\"", "");
        tipo.replace(" ", "");

        // Extraer estado (0 o 1)
        int iniEstado = json.indexOf(":", posEstado) + 1;
        int finEstado = json.indexOf(",", iniEstado);
        if (finEstado == -1) finEstado = json.indexOf("}", iniEstado);
        String estadoStr = json.substring(iniEstado, finEstado);
        estadoStr.replace("\"", "");
        estadoStr.replace(" ", "");
        int estado = estadoStr.toInt();

        if (tipo == "valvula") {
          digitalWrite(pinValvula, estado ? HIGH : LOW);
          Serial.println("Electroválvula " + String(estado ? "ENCENDIDA" : "APAGADA"));
        } else if (tipo == "humificador") {
          digitalWrite(pinHumificador, estado ? HIGH : LOW);
          Serial.println("Humificador " + String(estado ? "ENCENDIDO" : "APAGADO"));
        }
      }
    }
  }
}

// ==================== LECTURA DE TODOS LOS SENSORES ====================
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, int &porcentajeSuelo, String &nivelLuz) {
  // HDC1080 (temperatura y humedad ambiente)
  sensorAmbiente.readTempHumid();
  tempAmb = sensorAmbiente.getTemp();
  humAmb = sensorAmbiente.getRelativeHumidity();

  // DS18B20 (temperatura del agua)
  sensorAgua.requestTemperatures();
  tempAgua = sensorAgua.getTempCByIndex(0);

  // Lluvia (FC-28)
  lluvia = (digitalRead(pinLluvia) == LOW);

  // Temperatura interna del ESP32
  tempChip = temperatureRead();

  // BH1750: lectura bruta y compensación de cúpula
  uint16_t luxBruto = sensorLuz.readLightLevel();
  lux = luxBruto * factorCupula;

  // Clasificar nivel de luz
  if (lux >= 100000) nivelLuz = "Pleno sol";
  else if (lux >= 20000) nivelLuz = "Parcialmente nublado";
  else if (lux >= 10000) nivelLuz = "Nublado";
  else if (lux >= 500) nivelLuz = "Sombra";
  else nivelLuz = "Muy oscuro";

  // Humedad del suelo (HW-390): calibración dinámica
  int sueloBruto = analogRead(pinSuelo);
  if (sueloBruto > sueloSecoMax) {
    sueloSecoMax = sueloBruto;
  }
  if (sueloBruto < sueloMojadoMin) {
    sueloMojadoMin = sueloBruto;
  }
  if (sueloSecoMax - sueloMojadoMin < 10) {
    porcentajeSuelo = 0;   // aún sin calibrar
  } else {
    porcentajeSuelo = map(sueloBruto, sueloSecoMax, sueloMojadoMin, 0, 100);
    porcentajeSuelo = constrain(porcentajeSuelo, 0, 100);
  }
}

// ==================== CÁLCULO DE CAUDAL (L/min) ====================
float calcularCaudal() {
  const float pulsosPorLitro = 450.0;          // YF-S201 típico
  float minutos = (float)intervaloLectura / 60000.0;
  unsigned long pulsos = pulsosCaudal;
  pulsosCaudal = 0;
  return (pulsos / pulsosPorLitro) / minutos;
}

// ==================== MANEJAR COMANDOS BLUETOOTH ====================
void manejarComandosBluetooth() {
  if (!bluetooth.available()) return;

  String comando = bluetooth.readString();
  comando.trim();
  comando.toUpperCase();

  if (comando == "STATUS") {
    String btMensaje = "--- LECTURA INSTANTANEA ---\n";
    btMensaje += "Temp. ambiente: " + String(ultimaTemperatura, 1) + " C\n";
    btMensaje += "Humedad: " + String(ultimaHumedad, 1) + " %\n";
    btMensaje += "Temp. agua: " + String(ultimaTempAgua, 1) + " C\n";
    btMensaje += "Lluvia: " + String(ultimaLluvia ? "SI" : "NO") + "\n";
    btMensaje += "Caudal: " + String(ultimoCaudal, 1) + " L/min\n";
    btMensaje += "Temp. chip: " + String(ultimaTempChip, 1) + " C\n";
    btMensaje += "Luz: " + String(ultimaLuz) + " lx (" + ultimoNivelLuz + ")\n";
    btMensaje += "Humedad suelo: " + String(ultimoSueloPorcentaje) + " %\n";
    btMensaje += "WiFi: " + String(wifiConectado ? ("Conectado " + ipWiFi) : "Desconectado") + "\n";
    btMensaje += "MQTT: " + String(mqttConectado ? ("Conectado " + String(brokerActual)) : "Desconectado") + "\n";
    bluetooth.print(btMensaje);
  }
  else if (comando == "INFO") {
    enviarEstadoRedBluetooth();
  }
  else {
    bluetooth.println("Comandos: STATUS, INFO");
  }
}

// ==================== ENVIAR ESTADO DE RED POR BLUETOOTH ====================
void enviarEstadoRedBluetooth() {
  if (!bluetooth.hasClient()) return;

  String info = "--- ESTADO DE RED ---\n";
  info += "WiFi: ";
  info += wifiConectado ? ("Conectado " + ipWiFi) : "Desconectado";
  info += "\nMQTT: ";
  info += mqttConectado ? ("Conectado a " + String(brokerActual)) : "Desconectado";
  info += "\nBroker primario: " + String(brokerPrimario);
  info += "\nBroker secundario: " + String(brokerSecundario) + "\n";
  bluetooth.print(info);
}

// ==================== ALARMA DE TEMPERATURA DEL CHIP ====================
void verificarAlarmaTemp(float tempChip) {
  if (tempChip >= umbralAlarmaTemp) {
    if (mqttConectado && clienteMQTT.connected()) {
      String alarma = "{\"alarma\":\"sobrecalentamiento\",\"temp_chip\":" + String(tempChip, 1) + ",\"umbral\":" + String(umbralAlarmaTemp) + "}";
      clienteMQTT.publish(topicoAlarma, alarma.c_str());
    }
    if (bluetooth.hasClient()) {
      bluetooth.println("¡ALARMA! Temperatura del chip: " + String(tempChip, 1) + " °C (umbral: " + String(umbralAlarmaTemp) + ")");
    }
    unsigned long ahora = millis();
    if (!alarmaEnviada || (ahora - ultimaAlarma > intervaloReenvioAlarma)) {
      alarmaEnviada = true;
      ultimaAlarma = ahora;
    }
  } else {
    alarmaEnviada = false;
  }
}
