/*
 * ============================================================
 * Céspedes.Agro v2.4 — Firmware para nodo ESP32
 * ============================================================
 * Autor: Lennon Perdomo Céspedes
 * Contacto: lennon.perdomocespedes@gmail.com
 * Desde Moscú, Rusia — Pensando siempre en Cuba.
 *
 * Este firmware lee sensores agrícolas (SHT30, BH1750, DS18B20,
 * YF-S201, sensor de lluvia) y los publica por MQTT al servidor
 * Orange Pi. También recibe comandos MQTT para activar/desactivar
 * electroválvulas y relés.
 *
 * Configuración inicial por Bluetooth (comandos AT).
 *
 * Versión: 2.4 — Soporte multi-área con area_id.
 * ============================================================
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

// ==================== CONFIGURACIÓN POR DEFECTO ====================
// El area_id puede cambiarse por Bluetooth sin recompilar.
// Valores típicos: "000" (estación central), "001", "002", etc.
String area_id = "001";

// Nombre que aparece al escanear dispositivos Bluetooth
const char* dispositivoBT = "CespedesAgro";

// ==================== HARDWARE — PINES ====================
#define ONE_WIRE_BUS       5    // DS18B20 (temperatura del agua)
#define LLUVIA_PIN         33   // Sensor de lluvia (digital)
#define FLOW_SENSOR_PIN    27   // YF-S201 (caudalímetro)

// Relés de electroválvulas (A-F)
#define VALVULA_A_PIN      18
#define VALVULA_B_PIN      19
#define VALVULA_C_PIN      21
#define VALVULA_D_PIN      22
#define VALVULA_E_PIN      23
#define VALVULA_F_PIN      25

// Relés generales (activadores auxiliares)
#define RELE_1_PIN         26
#define RELE_2_PIN         32

// ==================== OBJETOS ====================
WiFiClient espClient;
PubSubClient clientMQTT(espClient);
BluetoothSerial SerialBT;
Preferences preferences;

SHTSensor sht30;
BH1750 lightMeter(0x23);          // Dirección I2C del BH1750
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ==================== CONFIGURACIÓN DE RED (guardada en Preferences) ====================
String wifiSSID     = "";
String wifiPassword = "";
String mqttServer   = "";

// ==================== ESTADO DEL SISTEMA ====================
bool wifiConectado  = false;
bool mqttConectado  = false;
String wifiIP       = "0.0.0.0";

// Temporizadores para lecturas y reconexiones
unsigned long ultimaLecturaSensores = 0;
const unsigned long INTERVALO_LECTURA = 5000;        // Leer sensores cada 5 segundos
unsigned long ultimoIntentoWiFi = 0;
const unsigned long INTERVALO_RECONEXION_WIFI = 30000; // Reintentar WiFi cada 30 s
unsigned long ultimoIntentoMQTT = 0;

// ==================== DATOS DE SENSORES ====================
float    lastTempAmb   = 0;
float    lastHumAmb    = 0;
float    lastTempAgua  = 0;
bool     lastLluvia    = false;
float    lastTempChip  = 0;
uint16_t lastLux       = 0;
float    lastCaudal    = 0;
String   lastNivelLuz  = "";

// ==================== SHT30 — CONTADOR DE ERRORES ====================
int errorCounterSHT = 0;
const int MAX_ERRORS_SHT = 3;

// ==================== CAUDALÍMETRO (YF-S201) ====================
volatile unsigned long flowPulseCount = 0;
void IRAM_ATTR contarPulso() {
    flowPulseCount++;
}

// ==================== ALARMA POR SOBRECALENTAMIENTO DEL CHIP ====================
const float TEMP_ALARM_THRESHOLD = 70.0;       // °C
bool alarmaEnviada = false;
unsigned long ultimaAlarma = 0;
const unsigned long INTERVALO_REENVIO_ALARMA = 60000; // Reenviar cada 60 s

// ==================== IDIOMA DE LA INTERFAZ BLUETOOTH ====================
String idioma = "ES";   // "ES", "EN", "RU"

// ==================== ESTADO DE ACTUADORES ====================
bool estadoValvula[6] = {false, false, false, false, false, false}; // A,B,C,D,E,F
bool estadoRele1 = false;
bool estadoRele2 = false;

// Mapeo de letra de válvula a pin
int pinValvula(char letra) {
    switch (letra) {
        case 'A': return VALVULA_A_PIN;
        case 'B': return VALVULA_B_PIN;
        case 'C': return VALVULA_C_PIN;
        case 'D': return VALVULA_D_PIN;
        case 'E': return VALVULA_E_PIN;
        case 'F': return VALVULA_F_PIN;
        default:  return -1;
    }
}

// Mapeo de índice a letra
char letraValvula(int idx) {
    return 'A' + idx;
}

// ==================== PROTOTIPOS DE FUNCIONES ====================
void cargarConfiguracion();
void conectarWiFi();
void conectarMQTT();
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, String &nivelLuz);
float calcularCaudal();
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void verificarAlarmaTemperatura(float tempChip);
void procesarComandoBT();
void mostrarAyudaBT();
void mostrarSensoresBT();
void mostrarStatusBT();
void publicarEstadoRelays();
void publicarDatosSensores();

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    Wire.begin();

    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║   🌱 CESPEDES.AGRO v2.4      ║");
    Serial.println("╚══════════════════════════════╝\n");

    // --- Cargar configuración guardada en flash ---
    cargarConfiguracion();

    // --- Iniciar Bluetooth ---
    if (!SerialBT.begin(dispositivoBT)) {
        Serial.println("❌ Error al iniciar Bluetooth.");
    } else {
        Serial.println("📱 Bluetooth: " + String(dispositivoBT));
    }

    // --- Sensor SHT30 (temperatura y humedad ambiente) ---
    if (sht30.init()) {
        Serial.println("✅ SHT30 iniciado correctamente.");
        sht30.setAccuracy(SHTSensor::SHT_ACCURACY_HIGH);
    } else {
        Serial.println("❌ ERROR: No se encontró el sensor SHT30.");
    }

    // --- Sensor DS18B20 (temperatura del agua) ---
    ds18b20.begin();
    Serial.println("✅ DS18B20 iniciado");

    // --- Sensor de lluvia (entrada digital) ---
    pinMode(LLUVIA_PIN, INPUT);
    Serial.println("✅ Sensor de lluvia configurado");

    // --- Sensor BH1750 (luz) ---
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
        Serial.println("❌ Error al iniciar BH1750.");
    } else {
        Serial.println("✅ BH1750 iniciado");
        // Configurar sensibilidad alta para exteriores
        Wire.beginTransmission(0x23);
        Wire.write(0x40);
        Wire.write(31);
        Wire.endTransmission();
    }

    // --- Caudalímetro YF-S201 (entrada de pulsos con interrupción) ---
    analogReadResolution(12);
    pinMode(FLOW_SENSOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), contarPulso, RISING);
    Serial.println("✅ Sensor de caudal configurado");

    // --- Configurar pines de actuadores como salida ---
    int pinesValvulas[] = {VALVULA_A_PIN, VALVULA_B_PIN, VALVULA_C_PIN,
                           VALVULA_D_PIN, VALVULA_E_PIN, VALVULA_F_PIN};
    for (int i = 0; i < 6; i++) {
        pinMode(pinesValvulas[i], OUTPUT);
        digitalWrite(pinesValvulas[i], LOW);
    }
    pinMode(RELE_1_PIN, OUTPUT);
    digitalWrite(RELE_1_PIN, LOW);
    pinMode(RELE_2_PIN, OUTPUT);
    digitalWrite(RELE_2_PIN, LOW);
    Serial.println("✅ Relés configurados (6 válvulas + 2 generales)");

    // --- Conectar WiFi si hay credenciales guardadas ---
    if (wifiSSID.length() > 0) {
        Serial.println("\n📡 WiFi guardado: " + wifiSSID);
        conectarWiFi();
    } else {
        Serial.println("\n⚠️  Sin WiFi configurado. Usa Bluetooth para configurarlo.");
    }

    if (mqttServer.length() == 0) {
        Serial.println("⚠️  Sin servidor MQTT configurado.");
    }

    // --- Mensaje de bienvenida por Bluetooth ---
    SerialBT.println("\n🌱 CespedesAgro v2.4 listo");
    SerialBT.println("Area ID: " + area_id);
    SerialBT.println("ES: COMANDOS | EN: HELP | RU: ПОМОЩЬ");
    Serial.println("\n✅ Sistema listo. Area ID: " + area_id + "\n");
}

// ==================== LOOP PRINCIPAL ====================
void loop() {
    unsigned long ahora = millis();

    // --- Monitorear estado de WiFi ---
    bool wifiAhora = (WiFi.status() == WL_CONNECTED);
    if (wifiAhora != wifiConectado) {
        wifiConectado = wifiAhora;
        if (wifiConectado) {
            wifiIP = WiFi.localIP().toString();
            Serial.println("\n✅ WiFi conectado. IP: " + wifiIP);
            clientMQTT.setCallback(callbackMQTT);
            if (mqttServer.length() > 0) {
                conectarMQTT();
            }
        } else {
            wifiIP = "0.0.0.0";
            mqttConectado = false;
            Serial.println("❌ WiFi desconectado.");
        }
    }

    // --- Mantener conexión MQTT ---
    if (wifiConectado && mqttServer.length() > 0) {
        if (!clientMQTT.connected()) {
            if (ahora - ultimoIntentoMQTT > 10000) {
                conectarMQTT();
                ultimoIntentoMQTT = ahora;
            }
        } else {
            mqttConectado = true;
            clientMQTT.loop();
        }
    }

    // --- Reintentar WiFi si se perdió ---
    if (!wifiConectado && wifiSSID.length() > 0) {
        if (ahora - ultimoIntentoWiFi >= INTERVALO_RECONEXION_WIFI) {
            Serial.println("🔄 Reintentando conexión WiFi...");
            WiFi.reconnect();
            ultimoIntentoWiFi = ahora;
        }
    }

    // --- Leer y publicar sensores periódicamente ---
    if (ahora - ultimaLecturaSensores >= INTERVALO_LECTURA) {
        float t, h, ta, tc;
        bool lluvia;
        uint16_t lux;
        String nivelLuz;

        lastCaudal = calcularCaudal();
        leerSensores(t, h, ta, lluvia, tc, lux, nivelLuz);

        // Guardar últimas lecturas
        lastTempAmb  = t;
        lastHumAmb   = h;
        lastTempAgua = ta;
        lastLluvia   = lluvia;
        lastTempChip = tc;
        lastLux      = lux;
        lastNivelLuz = nivelLuz;

        // Publicar por MQTT
        if (mqttConectado && clientMQTT.connected()) {
            publicarDatosSensores();
        }

        // Verificar alarma de temperatura del chip
        verificarAlarmaTemperatura(tc);

        ultimaLecturaSensores = ahora;
    }

    // --- Procesar comandos Bluetooth ---
    procesarComandoBT();

    delay(10);
}

// ============================================================
// CONEXIÓN WIFI
// ============================================================
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

// ============================================================
// CONEXIÓN MQTT
// ============================================================
void conectarMQTT() {
    if (!wifiConectado || mqttServer.length() == 0) return;

    clientMQTT.setServer(mqttServer.c_str(), 1883);
    String clientId = "ESP32_" + area_id + "_" + String(random(0xffff), HEX);

    Serial.print("Conectando a MQTT (" + mqttServer + ")... ");

    if (clientMQTT.connect(clientId.c_str())) {
        mqttConectado = true;
        Serial.println("✅");

        // Suscribirse al tópico de actuadores de ESTA área
        String topicoActuadores = "cespedes/" + area_id + "/actuadores";
        clientMQTT.subscribe(topicoActuadores.c_str());
        Serial.println("   Suscrito a: " + topicoActuadores);

        // Publicar estado inicial de los relés
        publicarEstadoRelays();
    } else {
        mqttConectado = false;
        Serial.print("❌ Error. Código: ");
        Serial.println(clientMQTT.state());
    }
}

// ============================================================
// LECTURA DE TODOS LOS SENSORES
// ============================================================
void leerSensores(float &tempAmb, float &humAmb, float &tempAgua, bool &lluvia,
                  float &tempChip, uint16_t &lux, String &nivelLuz) {

    // --- SHT30: temperatura y humedad ambiente ---
    if (sht30.readSample()) {
        tempAmb = sht30.getTemperature();
        humAmb  = sht30.getHumidity();
        errorCounterSHT = 0;
    } else {
        errorCounterSHT++;
        Serial.print("⚠️  Error lectura SHT30. Intento: ");
        Serial.println(errorCounterSHT);
        if (errorCounterSHT >= MAX_ERRORS_SHT) {
            Serial.println("🔄 Reinicializando SHT30...");
            sht30.init();
            sht30.setAccuracy(SHTSensor::SHT_ACCURACY_HIGH);
            errorCounterSHT = 0;
        }
    }

    // --- DS18B20: temperatura del agua ---
    ds18b20.requestTemperatures();
    tempAgua = ds18b20.getTempCByIndex(0);

    // --- Sensor de lluvia (LOW = mojado) ---
    lluvia = (digitalRead(LLUVIA_PIN) == LOW);

    // --- Temperatura interna del chip ESP32 ---
    tempChip = temperatureRead();

    // --- BH1750: intensidad de luz ---
    uint16_t rawLux = lightMeter.readLightLevel();
    lux = rawLux * 1.8;   // Factor de corrección por cúpula / difusor

    // Clasificación cualitativa de la luz
    if (lux >= 100000)      nivelLuz = "Pleno sol";
    else if (lux >= 20000)  nivelLuz = "Parcialmente nublado";
    else if (lux >= 10000)  nivelLuz = "Nublado";
    else if (lux >= 500)    nivelLuz = "Sombra";
    else                    nivelLuz = "Muy oscuro";
}

// ============================================================
// CÁLCULO DE CAUDAL (YF-S201)
// ============================================================
float calcularCaudal() {
    const float pulsosPorLitro = 450.0;   // Valor típico del YF-S201
    float minutos = (float)INTERVALO_LECTURA / 60000.0;
    unsigned long pulsos = flowPulseCount;
    flowPulseCount = 0;   // Reiniciar contador para el próximo intervalo

    if (minutos > 0 && pulsos > 0) {
        return (pulsos / pulsosPorLitro) / minutos;   // L/min
    }
    return 0;
}

// ============================================================
// PUBLICAR DATOS DE SENSORES POR MQTT
// ============================================================
void publicarDatosSensores() {
    String topico = "cespedes/" + area_id + "/sensores";

    // Construir JSON manualmente para ahorrar RAM (sin ArduinoJson)
    String json = "{";
    json += "\"temperatura\":" + String(lastTempAmb, 1) + ",";
    json += "\"humedad\":"     + String(lastHumAmb, 1)  + ",";
    json += "\"agua_temp\":"   + String(lastTempAgua, 1) + ",";
    json += "\"lluvia\":"      + String(lastLluvia ? "true" : "false") + ",";
    json += "\"caudal\":"      + String(lastCaudal, 1)  + ",";
    json += "\"temp_chip\":"   + String(lastTempChip, 1) + ",";
    json += "\"luz\":"         + String(lastLux)        + ",";
    json += "\"luz_txt\":\""   + lastNivelLuz + "\"";
    json += "}";

    clientMQTT.publish(topico.c_str(), json.c_str());
    Serial.println("📤 MQTT → " + topico + " : " + json);
}

// ============================================================
// CALLBACK MQTT — RECIBIR COMANDOS DE ACTUADORES
// ============================================================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
    // Convertir payload a String
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.println("📩 MQTT recibido [" + String(topic) + "]: " + msg);

    // Buscar el objeto JSON dentro del mensaje
    int start = msg.indexOf("{");
    int end   = msg.lastIndexOf("}");
    if (start < 0 || end <= start) return;

    String json = msg.substring(start, end + 1);

    // Extraer campos "tipo" y "estado" sin usar ArduinoJson (para ahorrar RAM)
    int tipoIdx   = json.indexOf("\"tipo\"");
    int estadoIdx = json.indexOf("\"estado\"");

    if (tipoIdx < 0 || estadoIdx < 0) return;

    // Extraer valor de "tipo"
    int tipoValStart = json.indexOf(":", tipoIdx) + 1;
    int tipoValEnd   = json.indexOf(",", tipoValStart);
    if (tipoValEnd == -1) tipoValEnd = json.indexOf("}", tipoValStart);
    String tipo = json.substring(tipoValStart, tipoValEnd);
    tipo.replace("\"", "");
    tipo.replace(" ", "");

    // Extraer valor de "estado"
    int estadoValStart = json.indexOf(":", estadoIdx) + 1;
    int estadoValEnd   = json.indexOf(",", estadoValStart);
    if (estadoValEnd == -1) estadoValEnd = json.indexOf("}", estadoValStart);
    String estadoStr = json.substring(estadoValStart, estadoValEnd);
    estadoStr.replace("\"", "");
    estadoStr.replace(" ", "");
    int estado = estadoStr.toInt();

    // --- Actuar según el tipo de actuador ---
    if (tipo.startsWith("valvula")) {
        // Ej: "valvulaA", "valvulaB", etc.
        char letra = tipo.charAt(7);   // Después de "valvula" viene la letra
        int pin = pinValvula(letra);
        if (pin != -1) {
            digitalWrite(pin, estado ? HIGH : LOW);
            estadoValvula[letra - 'A'] = (estado == 1);
            Serial.printf("   ✅ Válvula %c %s\n", letra, estado ? "ABIERTA" : "CERRADA");
        }
    }
    else if (tipo == "activador1") {
        digitalWrite(RELE_1_PIN, estado ? HIGH : LOW);
        estadoRele1 = (estado == 1);
        Serial.printf("   ✅ Relé 1 %s\n", estado ? "ENCENDIDO" : "APAGADO");
    }
    else if (tipo == "activador2") {
        digitalWrite(RELE_2_PIN, estado ? HIGH : LOW);
        estadoRele2 = (estado == 1);
        Serial.printf("   ✅ Relé 2 %s\n", estado ? "ENCENDIDO" : "APAGADO");
    }

    // Notificar estado actual al servidor
    publicarEstadoRelays();
}

// ============================================================
// PUBLICAR ESTADO DE TODOS LOS RELÉS (feedback al servidor)
// ============================================================
void publicarEstadoRelays() {
    if (!clientMQTT.connected()) return;

    String topico = "cespedes/" + area_id + "/estado";
    String json = "{";

    // Estado de las 6 válvulas
    for (int i = 0; i < 6; i++) {
        json += "\"valvula" + String(letraValvula(i)) + "\":";
        json += estadoValvula[i] ? "true" : "false";
        if (i < 5) json += ",";
    }
    // Relés generales
    json += ",\"activador1\":" + String(estadoRele1 ? "true" : "false");
    json += ",\"activador2\":" + String(estadoRele2 ? "true" : "false");
    json += "}";

    clientMQTT.publish(topico.c_str(), json.c_str());
}

// ============================================================
// ALARMA POR SOBRECALENTAMIENTO DEL CHIP
// ============================================================
void verificarAlarmaTemperatura(float tempChip) {
    if (tempChip >= TEMP_ALARM_THRESHOLD) {
        if (mqttConectado && clientMQTT.connected()) {
            String topico = "cespedes/" + area_id + "/alarma";
            String alarma = "{\"alarma\":\"sobrecalentamiento\",";
            alarma += "\"temp_chip\":" + String(tempChip, 1) + ",";
            alarma += "\"umbral\":" + String(TEMP_ALARM_THRESHOLD) + "}";
            clientMQTT.publish(topico.c_str(), alarma.c_str());
        }

        unsigned long ahora = millis();
        if (!alarmaEnviada || (ahora - ultimaAlarma > INTERVALO_REENVIO_ALARMA)) {
            if (SerialBT.hasClient()) {
                SerialBT.println("⚠️ ¡ALARMA! Temp. chip: " + String(tempChip, 1) + " °C");
            }
            alarmaEnviada = true;
            ultimaAlarma = ahora;
        }
    } else {
        alarmaEnviada = false;
    }
}

// ============================================================
// INTERFAZ BLUETOOTH — MOSTRAR AYUDA
// ============================================================
void mostrarAyudaBT() {
    if (idioma == "ES") {
        SerialBT.println("\n══════ COMANDOS ══════");
        SerialBT.println("COMANDOS        Ver esta ayuda");
        SerialBT.println("ESCANEAR        Buscar redes WiFi");
        SerialBT.println("WIFI RED|CLAVE  Guardar y conectar WiFi");
        SerialBT.println("MQTT IP         Configurar servidor MQTT");
        SerialBT.println("ID 001          Cambiar ID del área (ej: 001)");
        SerialBT.println("ESTADO          Estado del sistema");
        SerialBT.println("SENSORES        Leer todos los sensores");
        SerialBT.println("VALVULA A 1     Abrir válvula A (A-F)");
        SerialBT.println("VALVULA A 0     Cerrar válvula A");
        SerialBT.println("RELE 1 1        Activar relé general 1");
        SerialBT.println("RELE 1 0        Desactivar relé general 1");
        SerialBT.println("BORRAR          Borrar configuración");
        SerialBT.println("IP              Ver dirección IP");
        SerialBT.println("REINICIAR       Reiniciar dispositivo");
        SerialBT.println("IDIOMA ES/EN/RU Cambiar idioma");
        SerialBT.println("═══════════════════════");
    }
    else if (idioma == "EN") {
        SerialBT.println("\n══════ COMMANDS ══════");
        SerialBT.println("HELP            Show this help");
        SerialBT.println("SCAN            Scan WiFi networks");
        SerialBT.println("WIFI NAME|PASS  Save and connect WiFi");
        SerialBT.println("MQTT IP         Configure MQTT server");
        SerialBT.println("ID 001          Change area ID");
        SerialBT.println("STATUS          System status");
        SerialBT.println("SENSORS         Read all sensors");
        SerialBT.println("VALVE A 1       Open valve A (A-F)");
        SerialBT.println("VALVE A 0       Close valve A");
        SerialBT.println("RELAY 1 1       Activate general relay 1");
        SerialBT.println("RELAY 1 0       Deactivate general relay 1");
        SerialBT.println("CLEAR           Clear configuration");
        SerialBT.println("IP              Show IP address");
        SerialBT.println("RESTART         Restart device");
        SerialBT.println("LANG ES/EN/RU   Change language");
        SerialBT.println("═══════════════════════");
    }
    else if (idioma == "RU") {
        SerialBT.println("\n══════ КОМАНДЫ ══════");
        SerialBT.println("ПОМОЩЬ          Показать помощь");
        SerialBT.println("СКАН            Сканировать WiFi");
        SerialBT.println("WIFI ИМЯ|ПАРОЛЬ Сохранить и подключить WiFi");
        SerialBT.println("MQTT IP         Настроить MQTT сервер");
        SerialBT.println("ID 001          Сменить ID зоны");
        SerialBT.println("СТАТУС          Состояние системы");
        SerialBT.println("СЕНСОРЫ         Читать датчики");
        SerialBT.println("КЛАПАН A 1      Открыть клапан A");
        SerialBT.println("КЛАПАН A 0      Закрыть клапан A");
        SerialBT.println("РЕЛЕ 1 1        Включить реле 1");
        SerialBT.println("РЕЛЕ 1 0        Выключить реле 1");
        SerialBT.println("УДАЛИТЬ         Стереть настройки");
        SerialBT.println("IP              Показать IP адрес");
        SerialBT.println("РЕСТАРТ         Перезагрузить");
        SerialBT.println("ЯЗЫК ES/EN/RU   Сменить язык");
        SerialBT.println("═══════════════════════");
    }
}

// ============================================================
// INTERFAZ BLUETOOTH — MOSTRAR LECTURAS DE SENSORES
// ============================================================
void mostrarSensoresBT() {
    if (idioma == "ES") {
        SerialBT.println("\n═══ LECTURA DE SENSORES ═══");
        SerialBT.printf("🌡️  Temp. ambiente: %.1f °C\n", lastTempAmb);
        SerialBT.printf("💧 Humedad:         %.1f %%\n", lastHumAmb);
        SerialBT.printf("🌊 Temp. agua:      %.1f °C\n", lastTempAgua);
        SerialBT.printf("☔  Lluvia:          %s\n",     lastLluvia ? "SI" : "NO");
        SerialBT.printf("💨 Caudal:          %.1f L/min\n", lastCaudal);
        SerialBT.printf("💡 Luz:             %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
        SerialBT.printf("🔥 Temp. chip:      %.1f °C\n", lastTempChip);
        SerialBT.println("═══════════════════════════");
    }
    else if (idioma == "EN") {
        SerialBT.println("\n═══ SENSOR READINGS ═══");
        SerialBT.printf("🌡️  Ambient temp:  %.1f °C\n", lastTempAmb);
        SerialBT.printf("💧 Humidity:       %.1f %%\n", lastHumAmb);
        SerialBT.printf("🌊 Water temp:     %.1f °C\n", lastTempAgua);
        SerialBT.printf("☔  Rain:           %s\n",     lastLluvia ? "YES" : "NO");
        SerialBT.printf("💨 Flow rate:      %.1f L/min\n", lastCaudal);
        SerialBT.printf("💡 Light:          %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
        SerialBT.printf("🔥 Chip temp:      %.1f °C\n", lastTempChip);
        SerialBT.println("═══════════════════════════");
    }
    else if (idioma == "RU") {
        SerialBT.println("\n═══ ПОКАЗАНИЯ ДАТЧИКОВ ═══");
        SerialBT.printf("🌡️  Темп. воздуха:  %.1f °C\n", lastTempAmb);
        SerialBT.printf("💧 Влажность:       %.1f %%\n", lastHumAmb);
        SerialBT.printf("🌊 Темп. воды:      %.1f °C\n", lastTempAgua);
        SerialBT.printf("☔  Дождь:           %s\n",     lastLluvia ? "ДА" : "НЕТ");
        SerialBT.printf("💨 Поток:           %.1f L/min\n", lastCaudal);
        SerialBT.printf("💡 Освещение:       %d lx (%s)\n", lastLux, lastNivelLuz.c_str());
        SerialBT.printf("🔥 Темп. чипа:      %.1f °C\n", lastTempChip);
        SerialBT.println("═══════════════════════════");
    }
}

// ============================================================
// INTERFAZ BLUETOOTH — MOSTRAR ESTADO DEL SISTEMA
// ============================================================
void mostrarStatusBT() {
    if (idioma == "ES") {
        SerialBT.println("\n═══ ESTADO DEL SISTEMA ═══");
        SerialBT.println("ID de área: " + area_id);
        SerialBT.print("WiFi guardado: ");
        SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "Ninguno");
        SerialBT.print("WiFi: ");
        SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Desconectado");
        SerialBT.print("MQTT guardado: ");
        SerialBT.println(mqttServer.length() > 0 ? mqttServer : "Ninguno");
        SerialBT.print("MQTT: ");
        SerialBT.println(mqttConectado ? "✅ Conectado" : "❌ Desconectado");
        SerialBT.println("═══════════════════════════");
    }
    else if (idioma == "EN") {
        SerialBT.println("\n═══ SYSTEM STATUS ═══");
        SerialBT.println("Area ID: " + area_id);
        SerialBT.print("Saved WiFi: ");
        SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "None");
        SerialBT.print("WiFi: ");
        SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Disconnected");
        SerialBT.print("Saved MQTT: ");
        SerialBT.println(mqttServer.length() > 0 ? mqttServer : "None");
        SerialBT.print("MQTT: ");
        SerialBT.println(mqttConectado ? "✅ Connected" : "❌ Disconnected");
        SerialBT.println("═══════════════════════════");
    }
    else if (idioma == "RU") {
        SerialBT.println("\n═══ СОСТОЯНИЕ СИСТЕМЫ ═══");
        SerialBT.println("ID зоны: " + area_id);
        SerialBT.print("WiFi сохранен: ");
        SerialBT.println(wifiSSID.length() > 0 ? wifiSSID : "Нет");
        SerialBT.print("WiFi: ");
        SerialBT.println(wifiConectado ? "✅ " + wifiIP : "❌ Отключен");
        SerialBT.print("MQTT сохранен: ");
        SerialBT.println(mqttServer.length() > 0 ? mqttServer : "Нет");
        SerialBT.print("MQTT: ");
        SerialBT.println(mqttConectado ? "✅ Подключен" : "❌ Отключен");
        SerialBT.println("═══════════════════════════");
    }
}

// ============================================================
// PROCESAR COMANDOS RECIBIDOS POR BLUETOOTH
// ============================================================
void procesarComandoBT() {
    if (!SerialBT.available()) return;

    String comando = SerialBT.readStringUntil('\n');
    comando.trim();
    String cmd = comando;
    cmd.toUpperCase();

    // --- AYUDA ---
    if (cmd == "COMANDOS" || cmd == "AYUDA" || cmd == "HELP" || cmd == "H" ||
        cmd == "ПОМОЩЬ" || cmd == "П") {
        mostrarAyudaBT();
    }

    // --- ESCANEAR REDES WIFI ---
    else if (cmd == "ESCANEAR" || cmd == "SCAN" || cmd == "СКАН") {
        SerialBT.println("🔍 Escaneando redes WiFi...");
        int n = WiFi.scanNetworks();
        if (n == 0) {
            SerialBT.println("No se encontraron redes.");
        } else {
            for (int i = 0; i < n; i++) {
                SerialBT.printf("%d: %s (%d dBm) %s\n",
                               i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                               WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "🔒" : "🔓");
            }
            SerialBT.println("\nPara conectar: WIFI nombre|clave");
        }
    }

    // --- CONFIGURAR WIFI ---
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
                SerialBT.println("❌ Error al conectar. Verifica nombre y clave.");
            }
        } else {
            SerialBT.println("❌ Formato: WIFI NombreDeRed|Contraseña");
        }
    }

    // --- CONFIGURAR SERVIDOR MQTT ---
    else if (cmd.startsWith("MQTT ")) {
        mqttServer = comando.substring(5);
        mqttServer.trim();

        preferences.begin("cespedes", false);
        preferences.putString("mqtt", mqttServer);
        preferences.end();

        SerialBT.println("💾 Servidor MQTT guardado: " + mqttServer);

        if (wifiConectado) {
            clientMQTT.disconnect();
            conectarMQTT();
            SerialBT.println(mqttConectado ? "✅ MQTT conectado" : "❌ Error al conectar MQTT");
        } else {
            SerialBT.println("⚠️  Conecta a WiFi primero.");
        }
    }

    // --- CAMBIAR ID DE ÁREA ---
    else if (cmd.startsWith("ID ")) {
        String nuevoID = comando.substring(3);
        nuevoID.trim();
        if (nuevoID.length() > 0) {
            area_id = nuevoID;
            preferences.begin("cespedes", false);
            preferences.putString("area_id", area_id);
            preferences.end();

            SerialBT.println("✅ ID de área cambiado a: " + area_id);
            Serial.println("✅ ID de área cambiado a: " + area_id);

            // Reconectar MQTT con el nuevo ID
            if (mqttConectado) {
                clientMQTT.disconnect();
                conectarMQTT();
            }
        }
    }

    // --- ESTADO DEL SISTEMA ---
    else if (cmd == "ESTADO" || cmd == "STATUS" || cmd == "СТАТУС") {
        mostrarStatusBT();
    }

    // --- LEER SENSORES ---
    else if (cmd == "SENSORES" || cmd == "SENSORS" || cmd == "СЕНСОРЫ") {
        mostrarSensoresBT();
    }

    // --- CONTROL DE VÁLVULAS (VALVULA A 1 / VALVULA A 0) ---
    else if (cmd.startsWith("VALVULA ") || cmd.startsWith("VALVE ") || cmd.startsWith("КЛАПАН ")) {
        String partes = comando;
        partes.replace("valvula ", "");
        partes.replace("VALVULA ", "");
        partes.replace("valve ", "");
        partes.replace("VALVE ", "");
        partes.replace("клапан ", "");
        partes.replace("КЛАПАН ", "");

        char letra = partes.charAt(0);
        int estado = partes.substring(2).toInt();

        int pin = pinValvula(letra);
        if (pin != -1) {
            digitalWrite(pin, estado ? HIGH : LOW);
            estadoValvula[letra - 'A'] = (estado == 1);
            SerialBT.printf("✅ Válvula %c %s\n", letra, estado ? "ABIERTA" : "CERRADA");
            publicarEstadoRelays();
        } else {
            SerialBT.println("❌ Válvula no válida. Usa A, B, C, D, E o F.");
        }
    }

    // --- CONTROL DE RELÉS GENERALES (RELE 1 1 / RELE 1 0) ---
    else if (cmd.startsWith("RELE ") || cmd.startsWith("RELAY ") || cmd.startsWith("РЕЛЕ ")) {
        int numRele = cmd.substring(5).toInt();
        int estado  = cmd.substring(7).toInt();

        if (numRele == 1) {
            digitalWrite(RELE_1_PIN, estado ? HIGH : LOW);
            estadoRele1 = (estado == 1);
        } else if (numRele == 2) {
            digitalWrite(RELE_2_PIN, estado ? HIGH : LOW);
            estadoRele2 = (estado == 1);
        }
        SerialBT.printf("✅ Relé %d %s\n", numRele, estado ? "ENCENDIDO" : "APAGADO");
        publicarEstadoRelays();
    }

    // --- BORRAR CONFIGURACIÓN ---
    else if (cmd == "BORRAR" || cmd == "CLEAR" || cmd == "УДАЛИТЬ") {
        preferences.begin("cespedes", false);
        preferences.clear();
        preferences.end();

        wifiSSID     = "";
        wifiPassword = "";
        mqttServer   = "";
        WiFi.disconnect();
        wifiConectado = false;
        mqttConectado = false;

        SerialBT.println("🗑️  Configuración borrada. Reinicia el dispositivo.");
    }

    // --- MOSTRAR IP ---
    else if (cmd == "IP") {
        if (wifiConectado) {
            SerialBT.println("📡 IP: " + wifiIP);
        } else {
            SerialBT.println("❌ WiFi no conectado.");
        }
    }

    // --- REINICIAR DISPOSITIVO ---
    else if (cmd == "REINICIAR" || cmd == "RESTART" || cmd == "РЕСТАРТ") {
        SerialBT.println("🔄 Reiniciando...");
        delay(1000);
        ESP.restart();
    }

    // --- CAMBIAR IDIOMA ---
    else if (cmd.startsWith("IDIOMA ") || cmd.startsWith("LANG ") || cmd.startsWith("ЯЗЫК ")) {
        String nuevoIdioma = comando.substring(comando.lastIndexOf(' ') + 1);
        nuevoIdioma.toUpperCase();

        if (nuevoIdioma == "ES" || nuevoIdioma == "EN" || nuevoIdioma == "RU") {
            idioma = nuevoIdioma;
            preferences.begin("cespedes", false);
            preferences.putString("idioma", idioma);
            preferences.end();

            if (idioma == "ES")      SerialBT.println("✅ Idioma: Español");
            else if (idioma == "EN") SerialBT.println("✅ Language: English");
            else                     SerialBT.println("✅ Язык: Русский");
        }
    }

    // --- COMANDO NO RECONOCIDO ---
    else if (comando.length() > 0) {
        SerialBT.println("❌ Comando no reconocido: " + comando);
        SerialBT.println("Escribe COMANDOS para ver la ayuda.");
    }
}

// ============================================================
// CARGAR CONFIGURACIÓN GUARDADA EN MEMORIA FLASH
// ============================================================
void cargarConfiguracion() {
    preferences.begin("cespedes", true);
    wifiSSID     = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("pass", "");
    mqttServer   = preferences.getString("mqtt", "");
    idioma       = preferences.getString("idioma", "ES");
    area_id      = preferences.getString("area_id", "001");
    preferences.end();

    Serial.println("📂 Configuración cargada:");
    Serial.println("   WiFi:    " + (wifiSSID.length() > 0 ? wifiSSID : "(sin configurar)"));
    Serial.println("   MQTT:    " + (mqttServer.length() > 0 ? mqttServer : "(sin configurar)"));
    Serial.println("   Area ID: " + area_id);
    Serial.println("   Idioma:  " + idioma);
}