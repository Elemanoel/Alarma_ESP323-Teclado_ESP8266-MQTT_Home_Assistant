# GUÍA DE INSTALACIÓN — ALARMA ESP32 + HA + TELEGRAM

## PASO 1 — Preparar Home Assistant

### 1.1 Instalar Add-ons necesarios
En HA → Supervisor → Add-on Store:
- **ESPHome** (para compilar y flashear firmware)
- **Mosquitto broker** (servidor MQTT local)
- **File editor** (para editar archivos de configuración)

### 1.2 Configurar MQTT en HA
En `configuration.yaml`:
```yaml
mqtt:
  broker: localhost
  port: 1883
  username: alarma
  password: mqtt_password_alarma
```

O bien configurarlo via la integración MQTT en HA (Ajustes → Integraciones → MQTT).

### 1.3 Configurar Telegram Bot
1. Hablar con @BotFather en Telegram → /newbot
2. Anotar el TOKEN
3. Obtener el CHAT_ID hablando con @userinfobot

En `configuration.yaml`:
```yaml
telegram_bot:
  - platform: polling
    api_key: "TU_TOKEN_BOT"
    allowed_chat_ids:
      - TU_CHAT_ID

notify:
  - platform: telegram
    name: telegram_alarma
    chat_id: TU_CHAT_ID
```

Agregar a `secrets.yaml` de HA:
```yaml
telegram_chat_id: 123456789
```

---

## PASO 2 — Flashear el ESP32 (Alarma Central)

### 2.1 Desde ESPHome Add-on en HA
1. Abrir ESPHome Dashboard
2. Crear nuevo dispositivo → `alarma-central`
3. Copiar el contenido de `alarma_esp32_esphome.yaml`
4. Crear `secrets.yaml` con los valores del archivo `secrets.yaml` provisto
5. Conectar ESP32 por USB al servidor HA (o usar OTA después del primer flash)
6. Click "Install" → "Plug into this computer"

### 2.2 Primera vez (sin OTA)
```bash
# Desde línea de comando si se prefiere:
esphome run alarma_esp32_esphome.yaml
```

### 2.3 Actualizaciones posteriores (OTA via WiFi)
```bash
esphome run alarma_esp32_esphome.yaml
# ESPHome detecta el dispositivo en la red automáticamente
```

---

## PASO 3 — Modificar Firmware del Teclado (ESP8266)

### 3.1 Agregar librerías al sketch existente
En Arduino IDE → Gestor de bibliotecas:
- `PubSubClient` (Nick O'Leary) — para MQTT

### 3.2 Integrar protocolo_comunicacion.ino
1. Copiar el archivo `protocolo_comunicacion.ino` al mismo directorio que el sketch del teclado
2. En el sketch principal (el que ya tenés), agregar en `setup()`:
   ```cpp
   setupComunicacion();
   ```
3. Agregar en `loop()`:
   ```cpp
   loopComunicacion();
   ```
4. **REEMPLAZAR** la función `validarPin()` con la versión comentada al final del archivo protocolo

### 3.3 Conexión UART (si se usa cable backup)
Pines en ESP8266:
- TX SoftwareSerial: GPIO15 (D8)
- RX SoftwareSerial: GPIO13 (D7)

Conectar en cruz con ESP32:
- ESP8266 GPIO15 (TX) → ESP32 GPIO16 (RX)
- ESP8266 GPIO13 (RX) → ESP32 GPIO17 (TX)
- GND común obligatorio

---

## PASO 4 — Agregar Automatizaciones en Home Assistant

### 4.1 Crear archivo `alarma_automatizaciones.yaml`
Copiar el contenido del bloque de automatizaciones del archivo
`protocolo_comunicacion.ino` (al final, en comentarios).

### 4.2 Incluir en configuration.yaml
```yaml
automation: !include automatizaciones/alarma_automatizaciones.yaml
```

---

## PASO 5 — Lovelace (Dashboard HA)

Panel mínimo funcional:
```yaml
cards:
  - type: alarm-panel
    entity: alarm_control_panel.panel_alarma
    name: "Alarma Casa"
    states:
      - arm_away
      - arm_home

  - type: entities
    title: "Zonas"
    entities:
      - entity: binary_sensor.z1_puerta_principal
        name: "Puerta Principal"
      - entity: binary_sensor.z2_movimiento_interior
        name: "Movimiento"
      - entity: binary_sensor.z3_cerramientos
        name: "Cerramientos"
      - entity: binary_sensor.z4_ventana_patio
        name: "Ventana Patio"
      - entity: binary_sensor.z5_planta_alta
        name: "Planta Alta"

  - type: entities
    title: "Estado Sistema"
    entities:
      - entity: binary_sensor.alimentacion_220v
        name: "Alimentación 220V"
      - entity: sensor.alarma_central_zona_disparada
        name: "Zona Disparada"
      - entity: sensor.alarma_central_wifi_rssi
        name: "Señal WiFi"
      - entity: sensor.alarma_central_uptime
        name: "Uptime"
```

---

## PASO 6 — Prueba del Sistema

### Orden de prueba recomendado:
1. ✅ Verificar que el ESP32 aparece en HA (Ajustes → Integraciones)
2. ✅ Verificar que las 5 zonas muestran estado correcto (abrir/cerrar manualmente)
3. ✅ Armar desde HA y verificar que el teclado recibe el estado
4. ✅ Ingresar PIN correcto desde teclado y verificar desarmado
5. ✅ Ingresar PIN incorrecto y verificar que NO dispara localmente (ahora va a la central)
6. ✅ Abrir zona con alarma armada → verificar sirena + Telegram
7. ✅ Cortar alimentación 220V → verificar notificación Telegram
8. ✅ Probar modo "offline" (sin WiFi) via UART

---

## NOTAS DE SEGURIDAD IMPORTANTES

- El PIN **nunca se almacena** en el teclado (ESP8266). Solo la central lo conoce.
- Usar siempre `secrets.yaml` para contraseñas, nunca hardcodear en el YAML.
- Cambiar el PIN desde HA (sensor.text "PIN Maestro") o agregar un servicio dedicado.
- La API de HA está encriptada (clave en secrets.yaml).
- El broker MQTT debe tener autenticación habilitada (no dejar anónimo).
- Hacer backup de la configuración ESPHome antes de cada actualización.
