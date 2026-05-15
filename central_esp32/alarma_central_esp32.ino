/*
 * ================================================================
 *  ALARMA CENTRAL — ESP32 DevKit V1
 *  Framework: Arduino IDE (sin ESPHome)
 *
 *  Hardware:
 *    - ESP32 DevKit V1
 *    - MCP23017 (I2C 0x20)  → Zonas Z1-Z5 + sabotaje
 *    - Relays 5V x3          → Sirena ext, Sirena int, Aux
 *    - Optoacoplador PC817   → Supervisión 220V
 *    - UART2 (GPIO16/17)     → Teclado ESP8266 (backup cable)
 *
 *  Librerías necesarias (Arduino IDE → Gestor de bibliotecas):
 *    - Adafruit MCP23017     (by Adafruit)
 *    - PubSubClient          (by Nick O'Leary)
 *    - ArduinoJson           (by Benoit Blanchon) v6.x
 *
 *  Comunicación:
 *    WiFi/MQTT  → principal
 *    UART serie → backup si cae WiFi
 *
 *  Topics MQTT:
 *    RECIBE:  alarma/teclado/pin       "1234"
 *             alarma/teclado/comando   "ARM_AWAY|ARM_HOME|DISARM|GET_STATE"
 *    PUBLICA: alarma/central/estado    "DISARMED|ARMED_AWAY|ARMED_HOME|PENDING|TRIGGERED"
 *             alarma/central/evento    "SIRENA_ON|FALLA_ALIMENTACION|SABOTAJE_CAJA"
 *             alarma/central/alerta    "INTRUSION_DETECTADA"
 *             alarma/teclado/beep      "ARMING|PENDING|OK|ERROR"
 *             alarma/status            "online|offline"
 * ================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_MCP23X17.h>
#include <ArduinoJson.h>
#include <Preferences.h>     // Flash NVS del ESP32 (reemplaza EEPROM)

// ================================================================
//  CONFIG — EDITAR ANTES DE FLASHEAR
// ================================================================

const char* WIFI_SSID     = "TU_WIFI";
const char* WIFI_PASS     = "TU_PASSWORD";
const char* MQTT_SERVER   = "192.168.1.100";   // IP del broker (HA)
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "alarma";
const char* MQTT_PASS     = "mqtt_password";
const char* MQTT_CLIENT   = "alarma-central";
const char* PIN_DEFAULT   = "1234";

// ================================================================
//  PINES ESP32
// ================================================================

#define PIN_SDA           21    // I2C SDA → MCP23017
#define PIN_SCL           22    // I2C SCL → MCP23017
#define PIN_MCP_INT       34    // INTA del MCP23017 (solo input)
#define PIN_ALIM_220V     35    // Optoacoplador supervisión 220V
#define PIN_UART_RX       16    // UART2 RX ← teclado TX
#define PIN_UART_TX       17    // UART2 TX → teclado RX
#define PIN_LED           2     // LED integrado del DevKit

// ================================================================
//  PINES MCP23017
//  Puerto A (0-7) = entradas zonas
//  Puerto B (8-15) = salidas relays
// ================================================================

// Entradas (puerto A)
#define MCP_Z1            0     // GPA0 — Puerta principal
#define MCP_Z2            1     // GPA1 — Movimiento / cristales
#define MCP_Z3            2     // GPA2 — Cerramientos
#define MCP_Z4            3     // GPA3 — Ventana patio
#define MCP_Z5            4     // GPA4 — Planta alta
#define MCP_SABOTAJE      7     // GPA7 — Tapa de la caja

// Salidas (puerto B)
#define MCP_SIRENA_EXT    8     // GPB0 — Relay sirena exterior
#define MCP_SIRENA_INT    9     // GPB1 — Relay sirena interior
#define MCP_SALIDA_AUX    10    // GPB2 — Relay auxiliar / luz

// ================================================================
//  TOPICS MQTT
// ================================================================

#define TOPIC_PIN_RX        "alarma/teclado/pin"
#define TOPIC_CMD_RX        "alarma/teclado/comando"
#define TOPIC_HBT_RX        "alarma/teclado/heartbeat"
#define TOPIC_ESTADO_TX     "alarma/central/estado"
#define TOPIC_EVENTO_TX     "alarma/central/evento"
#define TOPIC_ALERTA_TX     "alarma/central/alerta"
#define TOPIC_BEEP_TX       "alarma/teclado/beep"
#define TOPIC_STATUS_TX     "alarma/status"
#define TOPIC_HA_ARM        "alarma/ha/comando"    // desde Home Assistant

// ================================================================
//  ESTADOS DE LA ALARMA
// ================================================================

enum EstadoAlarma {
  DESARMADA,
  ARMANDO,        // countdown salida
  ARMADA_TOTAL,
  ARMADA_PARCIAL,
  PENDIENTE,      // retardo entrada
  DISPARADA
};

// ================================================================
//  CONFIGURACIÓN DE ZONAS
// ================================================================

struct Zona {
  uint8_t     pin;          // pin MCP23017
  const char* nombre;
  bool        bypass_parcial;   // ignorar en modo ARM_HOME
  bool        retardo_entrada;  // true = genera PENDIENTE antes de DISPARADA
  bool        estado;           // lectura actual
  bool        estado_prev;
};

Zona zonas[] = {
  { MCP_Z1, "Z1-Puerta Principal", true,  true  },
  { MCP_Z2, "Z2-Movimiento",       false, false },
  { MCP_Z3, "Z3-Cerramientos",     false, false },
  { MCP_Z4, "Z4-Ventana Patio",    false, false },
  { MCP_Z5, "Z5-Planta Alta",      false, false },
};
const int NUM_ZONAS = 5;

// ================================================================
//  TIEMPOS (ms)
// ================================================================

#define TIEMPO_ARMADO_MS    30000UL   // 30s para salir antes de armar
#define TIEMPO_ENTRADA_MS   30000UL   // 30s retardo de entrada Z1
#define TIEMPO_SIRENA_MS   300000UL   // 5 min sirena máxima
#define TIEMPO_DEBOUNCE_MS     80UL   // debounce zonas
#define TIEMPO_RECONECT_MS   5000UL   // intento reconexión MQTT
#define TIEMPO_HBT_MS       10000UL   // heartbeat

// ================================================================
//  VARIABLES GLOBALES
// ================================================================

Adafruit_MCP23X17 mcp;
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);
Preferences       prefs;
HardwareSerial    serialTeclado(2);   // UART2

EstadoAlarma estadoActual  = DESARMADA;
EstadoAlarma estadoAnterior = DESARMADA;

String pinMaestro = PIN_DEFAULT;

// Timers
unsigned long timerArmado    = 0;
unsigned long timerEntrada   = 0;
unsigned long timerSirena    = 0;
unsigned long timerReconect  = 0;
unsigned long timerHBT       = 0;
unsigned long timerDebounce[5] = {0};

// Flags
bool mcpDisponible     = false;
bool wifiConectado     = false;
bool mqttConectado     = false;
bool alim220OK         = true;
bool sirenaActiva      = false;
bool mcpInterrupt      = false;

// Zona que disparó
String zonaDisparada = "Ninguna";

// ================================================================
//  SETUP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ALARMA CENTRAL — ESP32 ===");

  // LED status
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Supervisión 220V
  pinMode(PIN_ALIM_220V, INPUT);
  pinMode(PIN_MCP_INT, INPUT);

  // UART hacia teclado
  serialTeclado.begin(9600, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  // Flash NVS — cargar PIN guardado
  prefs.begin("alarma", false);
  pinMaestro = prefs.getString("pin", PIN_DEFAULT);
  Serial.println("PIN cargado desde flash");

  // I2C + MCP23017
  Wire.begin(PIN_SDA, PIN_SCL);
  inicializarMCP();

  // WiFi
  conectarWiFi();

  // MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setKeepAlive(15);
  mqtt.setBufferSize(512);

  // Interrupt del MCP (cambio de zona)
  attachInterrupt(digitalPinToInterrupt(PIN_MCP_INT),
                  []() { mcpInterrupt = true; },
                  FALLING);

  Serial.println("Setup completo — sistema DESARMADO");
  publicarEstado();
}

// ================================================================
//  LOOP
// ================================================================

void loop() {

  // Mantener WiFi y MQTT
  gestionarConexiones();

  // Leer zonas si hubo interrupción o cada 500ms
  static unsigned long ultimaLectura = 0;
  if (mcpInterrupt || millis() - ultimaLectura > 500) {
    mcpInterrupt = false;
    ultimaLectura = millis();
    leerZonas();
  }

  // Máquina de estados de la alarma
  procesarEstado();

  // Leer UART del teclado (backup)
  leerUART();

  // Supervisión alimentación 220V
  supervisarAlimentacion();

  // Heartbeat MQTT
  if (millis() - timerHBT > TIEMPO_HBT_MS) {
    timerHBT = millis();
    if (mqttConectado) mqtt.publish("alarma/central/heartbeat",
                                     String(millis()).c_str());
  }

  // LED status (parpadeo según estado)
  actualizarLED();
}

// ================================================================
//  INICIALIZAR MCP23017
// ================================================================

void inicializarMCP() {
  if (!mcp.begin_I2C(0x20)) {
    Serial.println("ERROR: MCP23017 no encontrado en 0x20");
    mcpDisponible = false;
    return;
  }
  mcpDisponible = true;
  Serial.println("MCP23017 OK");

  // Puerto A — entradas con pull-up (zonas NC)
  for (int i = 0; i < 8; i++) {
    mcp.pinMode(i, INPUT_PULLUP);
  }

  // Puerto B — salidas relays (activo LOW)
  mcp.pinMode(MCP_SIRENA_EXT, OUTPUT);
  mcp.pinMode(MCP_SIRENA_INT, OUTPUT);
  mcp.pinMode(MCP_SALIDA_AUX, OUTPUT);
  // Relays apagados al inicio
  mcp.digitalWrite(MCP_SIRENA_EXT, HIGH);
  mcp.digitalWrite(MCP_SIRENA_INT, HIGH);
  mcp.digitalWrite(MCP_SALIDA_AUX, HIGH);

  // Configurar INTA como interrupción al cambio, espejada (MIRROR=1)
  // El MCP genera INT cuando cualquier pin del puerto A cambia
  mcp.setupInterrupts(true, false, LOW);
  for (int i = 0; i < 8; i++) {
    mcp.setupInterruptPin(i, CHANGE);
  }
  mcp.clearInterrupts();
}

// ================================================================
//  LEER ZONAS
//  NC = normalmente LOW (pull-up activo, sensor cierra a GND)
//  Disparado = HIGH (sensor abierto, pull-up tira a 3.3V)
// ================================================================

void leerZonas() {
  if (!mcpDisponible) return;

  uint8_t portA = mcp.readGPIOA();   // lee los 8 bits del puerto A de una sola vez

  for (int i = 0; i < NUM_ZONAS; i++) {
    bool lectura = (portA >> zonas[i].pin) & 0x01;  // true = abierta/disparada

    // Debounce
    if (lectura != zonas[i].estado_prev) {
      timerDebounce[i] = millis();
      zonas[i].estado_prev = lectura;
    }

    if (millis() - timerDebounce[i] > TIEMPO_DEBOUNCE_MS) {
      if (lectura != zonas[i].estado) {
        zonas[i].estado = lectura;
        onZonaCambio(i);
      }
    }
  }

  // Sabotaje tapa
  bool sabotaje = (portA >> MCP_SABOTAJE) & 0x01;
  if (sabotaje) {
    Serial.println("SABOTAJE DETECTADO");
    mqtt.publish(TOPIC_EVENTO_TX, "SABOTAJE_CAJA");
  }
}

// ================================================================
//  EVENTO: ZONA CAMBIÓ
// ================================================================

void onZonaCambio(int idx) {
  bool abierta = zonas[idx].estado;

  Serial.printf("Zona %s: %s\n",
    zonas[idx].nombre,
    abierta ? "ABIERTA" : "CERRADA");

  // Publicar estado de la zona
  char topic[64];
  snprintf(topic, sizeof(topic), "alarma/zona/%d", idx + 1);
  mqtt.publish(topic, abierta ? "ABIERTA" : "CERRADA");

  // Enviar al teclado por UART
  char uartMsg[64];
  snprintf(uartMsg, sizeof(uartMsg), "ZONE:Z%d:%s\n",
           idx + 1, abierta ? "OPEN" : "CLOSED");
  serialTeclado.print(uartMsg);

  // Evaluar si corresponde disparar
  if (abierta) evaluarDisparo(idx);
}

// ================================================================
//  EVALUAR DISPARO
// ================================================================

void evaluarDisparo(int idxZona) {

  if (estadoActual == ARMADA_TOTAL) {
    if (zonas[idxZona].retardo_entrada) {
      // Zona con retardo → pasar a PENDIENTE
      cambiarEstado(PENDIENTE);
    } else {
      // Disparo inmediato
      zonaDisparada = String(zonas[idxZona].nombre);
      cambiarEstado(DISPARADA);
    }
  }

  else if (estadoActual == ARMADA_PARCIAL) {
    // En parcial, ignorar zonas con bypass
    if (!zonas[idxZona].bypass_parcial) {
      if (zonas[idxZona].retardo_entrada) {
        cambiarEstado(PENDIENTE);
      } else {
        zonaDisparada = String(zonas[idxZona].nombre);
        cambiarEstado(DISPARADA);
      }
    }
  }
}

// ================================================================
//  MÁQUINA DE ESTADOS
// ================================================================

void procesarEstado() {

  switch (estadoActual) {

    case ARMANDO:
      // Cuenta regresiva para salir
      if (millis() - timerArmado > TIEMPO_ARMADO_MS) {
        cambiarEstado(ARMADA_TOTAL);
      }
      break;

    case PENDIENTE:
      // Retardo de entrada — el usuario debe ingresar PIN
      if (millis() - timerEntrada > TIEMPO_ENTRADA_MS) {
        // Tiempo expirado sin desarmar → disparar
        cambiarEstado(DISPARADA);
      }
      break;

    case DISPARADA:
      // Apagar sirena automáticamente después de TIEMPO_SIRENA_MS
      if (sirenaActiva && millis() - timerSirena > TIEMPO_SIRENA_MS) {
        apagarSirenas();
        Serial.println("Sirena apagada por timeout");
      }
      break;

    default:
      break;
  }
}

// ================================================================
//  CAMBIAR ESTADO
// ================================================================

void cambiarEstado(EstadoAlarma nuevoEstado) {

  if (estadoActual == nuevoEstado) return;

  estadoAnterior = estadoActual;
  estadoActual   = nuevoEstado;

  Serial.printf("Estado: %s → %s\n",
    nombreEstado(estadoAnterior),
    nombreEstado(estadoActual));

  switch (nuevoEstado) {

    case ARMANDO:
      timerArmado = millis();
      mqtt.publish(TOPIC_BEEP_TX, "ARMING");
      break;

    case ARMADA_TOTAL:
    case ARMADA_PARCIAL:
      apagarSirenas();
      zonaDisparada = "Ninguna";
      mqtt.publish(TOPIC_BEEP_TX, "OK");
      break;

    case PENDIENTE:
      timerEntrada = millis();
      mqtt.publish(TOPIC_BEEP_TX, "PENDING");
      break;

    case DISPARADA:
      encenderSirenas();
      timerSirena = millis();
      mqtt.publish(TOPIC_ALERTA_TX, "INTRUSION_DETECTADA");
      mqtt.publish(TOPIC_EVENTO_TX, "SIRENA_ON");
      Serial.printf("Zona disparada: %s\n", zonaDisparada.c_str());
      break;

    case DESARMADA:
      apagarSirenas();
      zonaDisparada = "Ninguna";
      mqtt.publish(TOPIC_BEEP_TX, "OK");
      break;
  }

  publicarEstado();
  sincronizarTeclado();
}

// ================================================================
//  VALIDAR PIN  (núcleo del sistema)
// ================================================================

void validarPin(String pin) {

  if (pin != pinMaestro) {
    Serial.println("PIN INCORRECTO");
    mqtt.publish(TOPIC_BEEP_TX, "ERROR");
    mqtt.publish(TOPIC_EVENTO_TX, "PIN_INCORRECTO");

    // Respuesta al teclado por UART también
    serialTeclado.print("PIN_RESULT:WRONG\n");
    return;
  }

  Serial.println("PIN correcto");
  serialTeclado.print("PIN_RESULT:OK\n");

  // Toggle armar/desarmar
  switch (estadoActual) {
    case DESARMADA:
      cambiarEstado(ARMANDO);
      break;
    case ARMADA_TOTAL:
    case ARMADA_PARCIAL:
    case PENDIENTE:
    case DISPARADA:
    case ARMANDO:
      cambiarEstado(DESARMADA);
      break;
  }
}

// ================================================================
//  CALLBACK MQTT  (mensajes entrantes)
// ================================================================

void callbackMQTT(char* topic, byte* payload, unsigned int length) {

  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);

  // ---- PIN desde teclado ----
  if (topicStr == TOPIC_PIN_RX) {
    validarPin(msg);
  }

  // ---- Comando directo ----
  else if (topicStr == TOPIC_CMD_RX || topicStr == TOPIC_HA_ARM) {

    if (msg == "ARM_AWAY") {
      if (estadoActual == DESARMADA) cambiarEstado(ARMANDO);
    }
    else if (msg == "ARM_HOME") {
      if (estadoActual == DESARMADA) cambiarEstado(ARMADA_PARCIAL);
    }
    else if (msg == "DISARM") {
      cambiarEstado(DESARMADA);
    }
    else if (msg == "GET_STATE") {
      publicarEstado();
      sincronizarTeclado();
    }
    // Cambio de PIN: formato "SET_PIN:1234:5678" (viejo:nuevo)
    else if (msg.startsWith("SET_PIN:")) {
      procesarCambioPin(msg);
    }
  }
}

// ================================================================
//  CAMBIO DE PIN VÍA MQTT
//  Formato payload: "SET_PIN:<pin_actual>:<pin_nuevo>"
// ================================================================

void procesarCambioPin(String msg) {
  // msg = "SET_PIN:1234:9999"
  int sep1 = msg.indexOf(':', 8);   // después de "SET_PIN:"
  if (sep1 < 0) return;

  String pinActual = msg.substring(8, sep1);
  String pinNuevo  = msg.substring(sep1 + 1);

  if (pinActual != pinMaestro) {
    mqtt.publish(TOPIC_EVENTO_TX, "PIN_CAMBIO_FALLIDO");
    return;
  }
  if (pinNuevo.length() < 4 || pinNuevo.length() > 8) {
    mqtt.publish(TOPIC_EVENTO_TX, "PIN_INVALIDO");
    return;
  }

  pinMaestro = pinNuevo;
  prefs.putString("pin", pinMaestro);
  mqtt.publish(TOPIC_EVENTO_TX, "PIN_CAMBIADO");
  Serial.println("PIN actualizado y guardado en flash");
}

// ================================================================
//  SIRENAS
// ================================================================

void encenderSirenas() {
  if (!mcpDisponible) return;
  mcp.digitalWrite(MCP_SIRENA_EXT, LOW);   // relay activo LOW
  mcp.digitalWrite(MCP_SIRENA_INT, LOW);
  mcp.digitalWrite(MCP_SALIDA_AUX, LOW);
  sirenaActiva = true;
  Serial.println("SIRENAS ACTIVADAS");
}

void apagarSirenas() {
  if (!mcpDisponible) return;
  mcp.digitalWrite(MCP_SIRENA_EXT, HIGH);
  mcp.digitalWrite(MCP_SIRENA_INT, HIGH);
  mcp.digitalWrite(MCP_SALIDA_AUX, HIGH);
  sirenaActiva = false;
  Serial.println("Sirenas apagadas");
  if (mqttConectado) mqtt.publish(TOPIC_EVENTO_TX, "SIRENA_OFF");
}

// ================================================================
//  PUBLICAR ESTADO AL BROKER
// ================================================================

void publicarEstado() {
  if (!mqttConectado) return;

  const char* estadoStr = nombreEstado(estadoActual);
  mqtt.publish(TOPIC_ESTADO_TX, estadoStr, true);   // retain=true

  // JSON completo para HA
  StaticJsonDocument<256> doc;
  doc["estado"]         = estadoStr;
  doc["zona_disparada"] = zonaDisparada;
  doc["alim_220v"]      = alim220OK;
  doc["sirena"]         = sirenaActiva;
  doc["uptime_s"]       = millis() / 1000;

  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish("alarma/central/info", buf, true);
}

// ================================================================
//  SINCRONIZAR ESTADO CON TECLADO
// ================================================================

void sincronizarTeclado() {
  // Via UART (cable backup)
  char msg[32];
  snprintf(msg, sizeof(msg), "STATE:%s\n", nombreEstado(estadoActual));
  serialTeclado.print(msg);

  // Via MQTT
  char topicTeclado[] = "alarma/central/estado_teclado";
  snprintf(msg, sizeof(msg), "STATE:%s", nombreEstado(estadoActual));
  if (mqttConectado) mqtt.publish(topicTeclado, msg);
}

// ================================================================
//  LEER UART DEL TECLADO
// ================================================================

void leerUART() {
  if (!serialTeclado.available()) return;

  String linea = serialTeclado.readStringUntil('\n');
  linea.trim();
  if (linea.length() == 0) return;

  Serial.printf("[UART] %s\n", linea.c_str());

  if (linea.startsWith("PIN:")) {
    validarPin(linea.substring(4));
  }
  else if (linea.startsWith("CMD:")) {
    String cmd = linea.substring(4);
    if (cmd == "ARM_AWAY")  { if (estadoActual == DESARMADA) cambiarEstado(ARMANDO); }
    else if (cmd == "ARM_HOME")  { if (estadoActual == DESARMADA) cambiarEstado(ARMADA_PARCIAL); }
    else if (cmd == "DISARM")    { cambiarEstado(DESARMADA); }
    else if (cmd == "GET_STATE") { sincronizarTeclado(); }
  }
  else if (linea.startsWith("HBT:")) {
    // Heartbeat del teclado — responder con estado
    sincronizarTeclado();
  }
}

// ================================================================
//  SUPERVISIÓN ALIMENTACIÓN 220V
// ================================================================

void supervisarAlimentacion() {
  static unsigned long ultimaVerif = 0;
  if (millis() - ultimaVerif < 5000) return;
  ultimaVerif = millis();

  bool alimActual = digitalRead(PIN_ALIM_220V);   // HIGH = 220V OK

  if (alimActual != alim220OK) {
    alim220OK = alimActual;
    if (!alim220OK) {
      Serial.println("FALLA ALIMENTACIÓN 220V");
      mqtt.publish(TOPIC_EVENTO_TX, "FALLA_ALIMENTACION");
    } else {
      Serial.println("Alimentación 220V restaurada");
      mqtt.publish(TOPIC_EVENTO_TX, "ALIM_RESTAURADA");
    }
  }
}

// ================================================================
//  LED STATUS
//  DESARMADA     → LED apagado
//  ARMANDO       → parpadeo lento (1s)
//  ARMADA        → parpadeo rápido (200ms)
//  PENDIENTE     → parpadeo muy rápido (100ms)
//  DISPARADA     → LED fijo encendido
// ================================================================

void actualizarLED() {
  static unsigned long ultimoLED = 0;
  static bool ledOn = false;
  unsigned long intervalo = 0;

  switch (estadoActual) {
    case DESARMADA:     intervalo = 0;    break;
    case ARMANDO:       intervalo = 1000; break;
    case ARMADA_TOTAL:
    case ARMADA_PARCIAL: intervalo = 200; break;
    case PENDIENTE:     intervalo = 100;  break;
    case DISPARADA:
      digitalWrite(PIN_LED, HIGH);
      return;
  }

  if (intervalo == 0) {
    digitalWrite(PIN_LED, LOW);
    return;
  }

  if (millis() - ultimoLED > intervalo) {
    ultimoLED = millis();
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
  }
}

// ================================================================
//  GESTIÓN DE CONEXIONES WiFi / MQTT
// ================================================================

void gestionarConexiones() {

  // WiFi
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConectado) {
      wifiConectado = false;
      mqttConectado = false;
      Serial.println("WiFi desconectado — modo offline (UART activo)");
    }
    return;
  }

  if (!wifiConectado) {
    wifiConectado = true;
    Serial.printf("WiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
  }

  // MQTT
  if (!mqtt.connected()) {
    if (millis() - timerReconect > TIEMPO_RECONECT_MS) {
      timerReconect = millis();
      reconectarMQTT();
    }
  } else {
    mqtt.loop();
  }
}

// ================================================================
//  CONECTAR WiFi
// ================================================================

void conectarWiFi() {
  Serial.printf("Conectando a %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setAutoReconnect(true);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    Serial.printf("\nWiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi no disponible — continuando offline");
  }
}

// ================================================================
//  RECONECTAR MQTT
// ================================================================

void reconectarMQTT() {
  Serial.print("Conectando MQTT...");

  // Last Will: publicar "offline" si se desconecta inesperadamente
  if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS,
                   TOPIC_STATUS_TX, 1, true, "offline")) {

    // Suscripciones
    mqtt.subscribe(TOPIC_PIN_RX);
    mqtt.subscribe(TOPIC_CMD_RX);
    mqtt.subscribe(TOPIC_HBT_RX);
    mqtt.subscribe(TOPIC_HA_ARM);

    // Publicar online
    mqtt.publish(TOPIC_STATUS_TX, "online", true);

    mqttConectado = true;
    Serial.println(" OK");

    // Publicar estado actual inmediatamente
    publicarEstado();
    sincronizarTeclado();

  } else {
    mqttConectado = false;
    Serial.printf(" FALLO (rc=%d) — reintento en %lus\n",
                  mqtt.state(), TIEMPO_RECONECT_MS / 1000);
  }
}

// ================================================================
//  HELPER: nombre legible del estado
// ================================================================

const char* nombreEstado(EstadoAlarma e) {
  switch (e) {
    case DESARMADA:      return "DISARMED";
    case ARMANDO:        return "ARMING";
    case ARMADA_TOTAL:   return "ARMED_AWAY";
    case ARMADA_PARCIAL: return "ARMED_HOME";
    case PENDIENTE:      return "PENDING";
    case DISPARADA:      return "TRIGGERED";
    default:             return "UNKNOWN";
  }
}
