/*
 * ================================================================
 *  PARCHE TECLADO ESP8266 — integración con alarma central
 *  Agregar este código al sketch del teclado que ya existe
 *
 *  Librería adicional necesaria:
 *    - PubSubClient (by Nick O'Leary)
 *
 *  Conexión UART backup:
 *    GPIO15 (D8) → TX SoftwareSerial → ESP32 GPIO16 (RX2)
 *    GPIO13 (D7) → RX SoftwareSerial → ESP32 GPIO17 (TX2)
 *    GND ← común
 *
 *  INSTRUCCIONES DE INTEGRACIÓN:
 *  1. Copiar este archivo en la misma carpeta que el .ino principal
 *  2. En setup():  agregar  setupComunicacion();
 *  3. En loop():   agregar  loopComunicacion();
 *  4. Reemplazar validarPin() con la versión de abajo
 *  5. Agregar pantallaConectando() si se desea mostrar WiFi status
 * ================================================================
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

// ------- CONFIG -------
const char* WIFI_SSID     = "TU_WIFI";
const char* WIFI_PASS     = "TU_PASSWORD";
const char* MQTT_SERVER   = "192.168.1.100";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "alarma";
const char* MQTT_PASS     = "mqtt_password";
const char* MQTT_CLIENT   = "alarma-teclado";

// Topics (deben coincidir exactamente con la central)
#define TOPIC_PIN_TX        "alarma/teclado/pin"
#define TOPIC_CMD_TX        "alarma/teclado/comando"
#define TOPIC_HBT_TX        "alarma/teclado/heartbeat"
#define TOPIC_ESTADO_RX     "alarma/central/estado_teclado"
#define TOPIC_BEEP_RX       "alarma/teclado/beep"
#define TOPIC_EVENTO_RX     "alarma/central/evento"
#define TOPIC_STATUS_TX     "alarma/teclado/status"

// UART backup (no usar GPIO1/3 que son TX/RX del USB)
SoftwareSerial serialCentral(13, 15);  // RX=GPIO13(D7), TX=GPIO15(D8)

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

bool   mqttConectado      = false;
bool   pinEnviado         = false;   // esperando respuesta de la central
unsigned long timerReconect = 0;
unsigned long timerHBT      = 0;

// ================================================================
//  SETUP DE COMUNICACIÓN
//  Llamar al final de setup()
// ================================================================
void setupComunicacion() {
  serialCentral.begin(9600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setAutoReconnect(true);

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setKeepAlive(15);

  // Esperar WiFi máximo 8 segundos sin bloquear el teclado
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {
    delay(250);
  }
}

// ================================================================
//  LOOP DE COMUNICACIÓN
//  Llamar al inicio de loop()
// ================================================================
void loopComunicacion() {

  // MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (millis() - timerReconect > 5000) {
        timerReconect = millis();
        reconectarMQTT();
      }
    } else {
      mqtt.loop();
      mqttConectado = true;
    }
  } else {
    mqttConectado = false;
  }

  // Heartbeat cada 10s
  if (millis() - timerHBT > 10000) {
    timerHBT = millis();
    if (mqttConectado) {
      mqtt.publish(TOPIC_HBT_TX, String(millis()).c_str());
    } else {
      // Heartbeat por UART
      serialCentral.printf("HBT:%lu\n", millis());
    }
  }

  // Leer respuestas de la central por UART
  leerUARTCentral();
}

// ================================================================
//  REEMPLAZA validarPin() ORIGINAL
//  El PIN ya no se valida localmente — se envía a la central
// ================================================================
void validarPin() {

  // Eliminar el '#' final si quedó en el buffer
  String pinLimpio = bufferEntrada;
  if (pinLimpio.endsWith("#"))
    pinLimpio.remove(pinLimpio.length() - 1);

  if (pinLimpio.length() < 4) {
    bufferEntrada = "";
    return;
  }

  // Enviar PIN a la central
  enviarPin(pinLimpio);

  pinEnviado = true;
  bufferEntrada = "";

  // Mostrar "esperar..." mientras la central responde
  estadoActual = UI_HOME;
  dibujarUI();
}

// ================================================================
//  ENVIAR PIN
// ================================================================
void enviarPin(String pin) {
  if (mqttConectado) {
    mqtt.publish(TOPIC_PIN_TX, pin.c_str());
  } else {
    // Fallback UART
    serialCentral.printf("PIN:%s\n", pin.c_str());
  }
}

// ================================================================
//  ENVIAR COMANDO DIRECTO (armar/desarmar sin PIN)
//  Usar solo si se implementa autenticación por otro medio
// ================================================================
void enviarComando(const char* cmd) {
  if (mqttConectado) {
    mqtt.publish(TOPIC_CMD_TX, cmd);
  } else {
    serialCentral.printf("CMD:%s\n", cmd);
  }
}

// ================================================================
//  CALLBACK MQTT — mensajes de la central
// ================================================================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {

  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String t = String(topic);

  // Estado del panel → actualizar pantalla
  if (t == TOPIC_ESTADO_RX) {
    // Formato: "STATE:ARMED_AWAY"
    if (msg.startsWith("STATE:")) {
      String estado = msg.substring(6);
      actualizarDesdeEstado(estado);
    }
  }

  // Comando de beep
  else if (t == TOPIC_BEEP_RX) {
    ejecutarBeep(msg);
    pinEnviado = false;
  }

  // Eventos informativos
  else if (t == TOPIC_EVENTO_RX) {
    if (msg == "FALLA_ALIMENTACION") {
      agregarEvento("SIN 220V");
    } else if (msg == "SABOTAJE_CAJA") {
      agregarEvento("SABOTAJE!");
    } else if (msg == "PIN_INCORRECTO") {
      agregarEvento("PIN INCORRECTO");
      ejecutarBeep("ERROR");
      pinEnviado = false;
    } else if (msg == "PIN_CAMBIADO") {
      agregarEvento("PIN CAMBIADO");
    }
    dibujarUI();
  }
}

// ================================================================
//  ACTUALIZAR PANTALLA SEGÚN ESTADO RECIBIDO
// ================================================================
void actualizarDesdeEstado(String estado) {

  if (estado == "DISARMED") {
    sistemaArmado    = false;
    alarmaDisparada  = false;
    estadoActual     = UI_HOME;

  } else if (estado == "ARMED_AWAY") {
    sistemaArmado    = true;
    alarmaDisparada  = false;
    estadoActual     = UI_HOME;

  } else if (estado == "ARMED_HOME") {
    sistemaArmado    = true;
    alarmaDisparada  = false;
    estadoActual     = UI_HOME;

  } else if (estado == "ARMING") {
    // Mostramos HOME con estado armado parcialmente
    sistemaArmado    = false;
    estadoActual     = UI_HOME;

  } else if (estado == "PENDING") {
    // Retardo de entrada — pedir PIN urgente
    estadoActual     = UI_INGRESO_PIN;
    ejecutarBeep("PENDING");

  } else if (estado == "TRIGGERED") {
    alarmaDisparada  = true;
    estadoActual     = UI_ALARMA;
    ejecutarBeep("ERROR");
  }

  dibujarUI();
}

// ================================================================
//  LEER UART DE LA CENTRAL (backup cable)
// ================================================================
void leerUARTCentral() {
  while (serialCentral.available()) {
    String linea = serialCentral.readStringUntil('\n');
    linea.trim();
    if (linea.length() == 0) continue;

    if (linea.startsWith("STATE:")) {
      actualizarDesdeEstado(linea.substring(6));

    } else if (linea.startsWith("BEEP:")) {
      ejecutarBeep(linea.substring(5));

    } else if (linea == "PIN_RESULT:OK") {
      pinEnviado = false;
      ejecutarBeep("OK");

    } else if (linea == "PIN_RESULT:WRONG") {
      pinEnviado = false;
      ejecutarBeep("ERROR");
      agregarEvento("PIN INCORRECTO");
      dibujarUI();

    } else if (linea.startsWith("ZONE:")) {
      // ZONE:Z1:OPEN → mostrar en eventos
      agregarEvento(linea.substring(5));
      dibujarUI();
    }
  }
}

// ================================================================
//  EJECUTAR BEEP SEGÚN TIPO
// ================================================================
void ejecutarBeep(String tipo) {
  if (tipo == "ARMING") {
    // 3 beeps cortos
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delay(80);
      digitalWrite(BUZZER_PIN, LOW);  delay(80);
    }
  } else if (tipo == "PENDING") {
    // 6 beeps rápidos
    for (int i = 0; i < 6; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delay(60);
      digitalWrite(BUZZER_PIN, LOW);  delay(60);
    }
  } else if (tipo == "OK") {
    // 1 beep largo
    digitalWrite(BUZZER_PIN, HIGH); delay(300);
    digitalWrite(BUZZER_PIN, LOW);
  } else if (tipo == "ERROR") {
    // 2 beeps largos
    for (int i = 0; i < 2; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delay(400);
      digitalWrite(BUZZER_PIN, LOW);  delay(200);
    }
  }
}

// ================================================================
//  RECONECTAR MQTT
// ================================================================
void reconectarMQTT() {
  if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS,
                   TOPIC_STATUS_TX, 1, true, "offline")) {

    mqtt.subscribe(TOPIC_ESTADO_RX);
    mqtt.subscribe(TOPIC_BEEP_RX);
    mqtt.subscribe(TOPIC_EVENTO_RX);
    mqtt.publish(TOPIC_STATUS_TX, "online", true);

    mqttConectado = true;

    // Pedir estado actual a la central
    mqtt.publish(TOPIC_CMD_TX, "GET_STATE");
  }
}

// ================================================================
//  PANTALLA STATUS BAR — REEMPLAZA dibujarStatus() ORIGINAL
//  Agrega ícono MQTT además del WiFi
// ================================================================
void dibujarStatus() {
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 0);
  display.print(horaActual());

  display.setCursor(55, 0);
  if (WiFi.status() == WL_CONNECTED) {
    display.print(mqttConectado ? "MQTT" : "WiFi");
  } else {
    display.print("UART");  // modo cable
  }

  display.drawLine(0, 10, 128, 10, WHITE);
}
