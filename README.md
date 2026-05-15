# 🏠 Alarma Casa — ESP32 + ESP8266 + Home Assistant

Sistema de alarma domótica basado en microcontroladores, construido sobre los componentes reutilizados de una central X28.

## 📋 Descripción

Reemplaza completamente la placa central del X28 con una solución abierta, modular y con integración a Home Assistant y notificaciones por Telegram.

**Reutiliza del X28:**
- Sensores Z1–Z5 (NC normalmente cerrado)
- Sirenas (exterior e interior)
- Transformador 12V
- Batería de backup 12V

**Hardware nuevo:**
- ESP32 DevKit V1 (central)
- ESP8266 NodeMCU (teclado)
- MCP23017 (expansor I2C de E/S)
- OLED SSD1306 128×64
- Teclado matricial 4×4
- Módulo relay 5V 3 canales

---

## 🗂️ Estructura del repositorio

```
alarma-esp32/
├── central_esp32/
│   └── alarma_central_esp32.ino      ← Firmware ESP32 (panel central)
├── teclado_esp8266/
│   ├── teclado_principal.ino         ← Firmware ESP8266 (pantalla + teclado)
│   └── teclado_comunicacion.ino      ← Módulo WiFi/MQTT/UART
├── home_assistant/
│   ├── automations.yaml              ← Automatizaciones (Telegram, alertas)
│   └── configuration_and_lovelace.yaml ← Sensores MQTT + Dashboard
├── hardware/
│   └── diagrama_conexiones.md        ← Pinout, esquema, lista de materiales
└── docs/
    └── guia_instalacion.md           ← Paso a paso de instalación
```

---

## ⚙️ Zonas configuradas

| Zona | Sensor | Modo | Retardo entrada |
|------|--------|------|----------------|
| Z1 | Puerta principal | NC | ✅ 30 seg |
| Z2 | Movimiento + cristales | NC | ❌ Inmediato |
| Z3 | Cerramientos perimetrales | NC | ❌ Inmediato |
| Z4 | Ventana patio | NC | ❌ Inmediato |
| Z5 | Planta alta | NC | ❌ Inmediato |

---

## 🔗 Comunicación

```
[Teclado ESP8266] ←→ WiFi/MQTT ←→ [Central ESP32] ←→ WiFi/MQTT ←→ [Home Assistant]
        ↕                                  ↕                               ↕
   UART serie (backup cable)          MCP23017                      Telegram Bot
```

**Canal principal:** WiFi + MQTT (broker Mosquitto en HA)  
**Canal backup:** UART serie directo (funciona sin WiFi, sin internet)

---

## 📡 Topics MQTT

### Teclado → Central
| Topic | Payload | Descripción |
|-------|---------|-------------|
| `alarma/teclado/pin` | `"1234"` | PIN ingresado |
| `alarma/teclado/comando` | `"ARM_AWAY"` / `"DISARM"` / `"GET_STATE"` | Comandos |
| `alarma/teclado/heartbeat` | `millis()` | Keep-alive |

### Central → Teclado / HA
| Topic | Payload | Descripción |
|-------|---------|-------------|
| `alarma/central/estado` | `"DISARMED"` / `"ARMED_AWAY"` / `"TRIGGERED"` | Estado |
| `alarma/central/evento` | `"SIRENA_ON"` / `"FALLA_ALIMENTACION"` | Eventos |
| `alarma/central/alerta` | `"INTRUSION_DETECTADA"` | Alarma disparada |
| `alarma/teclado/beep` | `"OK"` / `"ERROR"` / `"ARMING"` / `"PENDING"` | Feedback sonoro |
| `alarma/zona/N` | `"ABIERTA"` / `"CERRADA"` | Estado por zona |
| `alarma/status` | `"online"` / `"offline"` | Presencia en red |

### Home Assistant → Central
| Topic | Payload | Descripción |
|-------|---------|-------------|
| `alarma/ha/comando` | `"ARM_AWAY"` / `"ARM_HOME"` / `"DISARM"` | Control desde HA |
| `alarma/ha/comando` | `"SET_PIN:<actual>:<nuevo>"` | Cambio de PIN |

---

## 🤖 Comandos Telegram disponibles

| Comando | Acción |
|---------|--------|
| `/alarma_arm` | Armar total |
| `/alarma_home` | Armar parcial (en casa) |
| `/alarma_off` | Desarmar |
| `/alarma_estado` | Consultar estado actual |

---

## 🛠️ Librerías Arduino necesarias

### ESP32 (central)
- `Adafruit MCP23017` (by Adafruit)
- `PubSubClient` (by Nick O'Leary)
- `ArduinoJson` v6 (by Benoit Blanchon)

### ESP8266 (teclado)
- `Adafruit SSD1306`
- `Adafruit GFX`
- `Keypad` (by Mark Stanley)
- `PubSubClient` (by Nick O'Leary)

---

## 🚀 Instalación rápida

1. Clonar este repositorio
2. Seguir [`docs/guia_instalacion.md`](docs/guia_instalacion.md)
3. Completar credenciales en los `.ino` (buscar `TU_WIFI`, `TU_PASSWORD`, `192.168.1.100`)
4. Flashear `central_esp32/` al ESP32
5. Flashear `teclado_esp8266/` al ESP8266
6. Importar `home_assistant/` en HA

---

## 🔒 Seguridad

- El PIN **nunca** se almacena ni valida en el teclado
- El PIN se guarda en la flash NVS del ESP32 (sobrevive reinicios)
- El cambio de PIN requiere conocer el PIN actual
- Comunicación MQTT con autenticación usuario/contraseña
- Last Will Testament MQTT para detectar desconexiones

---

## 📐 Diagrama de hardware

Ver [`hardware/diagrama_conexiones.md`](hardware/diagrama_conexiones.md) para:
- Pinout completo ESP32 + MCP23017
- Cableado de zonas NC con resistores EOL
- Circuito de supervisión 220V
- Conexión de relays para sirenas

---

## 📝 Licencia

MIT — libre para uso personal y modificación.
