[diagrama_conexiones.md](https://github.com/user-attachments/files/27801522/diagrama_conexiones.md)

# ================================================================
#  DIAGRAMA DE CONEXIONES — ALARMA CENTRAL
#  ESP32 DevKit V1 + MCP23017
# ================================================================

## ALIMENTACIÓN
```
Transformador 12V DC
    ├── 12V → Relay sirenas / Batería backup (cargador)
    ├── 12V → Regulador 5V (LM7805 o módulo) → ESP32 VIN
    └── 12V → Optoacoplador PC817 → GPIO35 ESP32 (supervisión 220V)

Batería 12V (del X28 reutilizada)
    └── Conectada en paralelo al transformador via diodo (D1 1N4007)
        para backup automático sin corte
```

## I2C BUS  (MCP23017)
```
ESP32 GPIO21 (SDA) ──┬── MCP23017 SDA (pin 13)
                     └── [Pull-up 4k7Ω a 3.3V]

ESP32 GPIO22 (SCL) ──┬── MCP23017 SCL (pin 12)
                     └── [Pull-up 4k7Ω a 3.3V]

MCP23017:
  VDD (pin 18) → 3.3V
  VSS (pin 9)  → GND
  A0  (pin 15) → GND  ┐
  A1  (pin 16) → GND  ├─ Dirección I2C = 0x20
  A2  (pin 17) → GND  ┘
  RESET (pin 18) → 3.3V (o a GPIO para reset por software)

INTERRUPT:
  INTA (pin 20) → ESP32 GPIO34 (interrupt_pin)
  [INTA y INTB en MIRROR mode → un solo cable necesario]
```

## ZONAS  NC — Puerto A del MCP23017
```
Cableado estándar con resistor EOL (End of Line):

  ┌─── MCP23017 GPA0 ─── [Pull-up interno 100kΩ] ─── 3.3V (interno)
  │
  GPA0 ──── [ZONA Z1] ──── Resistor EOL 5k6Ω ──── GND

  Diagrama de resistencias:
       GPA0
        │
       [5k6Ω resistor del sensor EOL]
        │
       ─┤ Sensor NC ├──── GND

  Estados:
    Sensor CERRADO (OK):    GPA0 → LOW   (0V) → binary_sensor = false (normal)
    Sensor ABIERTO (alerta): GPA0 → HIGH (via pull-up) → binary_sensor = true
    Cable CORTADO:          GPA0 → HIGH (pull-up) → detectado como abierto
    Cable CORTOCIRCUITO:    GPA0 → LOW siempre → NO detectado (necesita resistor EOL supervisado)

  ⚠ Para supervisión completa (corte Y cortocircuito), usar doble resistor EOL:
       GPA0 ──[4k7Ω]── SENSOR_NC ──[4k7Ω]── GND
     Con circuito supervisor, pero para esta instalación el pull-up simple
     es suficiente para detectar apertura.

  Z1 Puerta Principal:  GPA0 (MCP pin 21)
  Z2 Movimiento/Vidrio: GPA1 (MCP pin 22)
  Z3 Cerramientos:      GPA2 (MCP pin 23)
  Z4 Ventana Patio:     GPA3 (MCP pin 24)
  Z5 Planta Alta:       GPA4 (MCP pin 25)
  GPA5: RESERVA
  GPA6: RESERVA
  GPA7: Sabotaje tapa   (NC a GND, se abre al destapar la caja)
```

## SALIDAS RELAYS — Puerto B del MCP23017
```
GPB0 (pin 1) ──→ IN relay módulo Sirena Exterior
GPB1 (pin 2) ──→ IN relay módulo Sirena Interior
GPB2 (pin 3) ──→ IN relay módulo Salida Auxiliar (LED exterior/luz)
GPB3-7: RESERVA

Módulo relay (5V, activo LOW):
  VCC → 5V
  GND → GND
  IN  → GPB0/1/2  (señal 3.3V del MCP, compatible con relay de 5V
                   que tiene optoacoplador en la entrada)

Sirenas del X28 (reutilizadas):
  Sirena + → Relay NO → 12V
  Sirena - → GND
```

## UART  (comunicación backup con teclado)
```
ESP32 GPIO16 (RX2) ──── Cable ──── ESP8266 GPIO15 (TX SoftSerial)
ESP32 GPIO17 (TX2) ──── Cable ──── ESP8266 GPIO13 (RX SoftSerial)
ESP32 GND          ──── Cable ──── ESP8266 GND    (IMPRESCINDIBLE)

Longitud máxima recomendada: 10 metros a 9600 baud
Para distancias mayores: usar RS485 con módulos MAX485
```

## SUPERVISIÓN ALIMENTACIÓN
```
12V transformador ──┬── [R1 10kΩ] ──── PC817 ánodo (pin 1)
                    └── (también carga batería vía diodo)

PC817 cátodo (pin 2) ──── GND
PC817 colector (pin 4) ──── 3.3V via [R2 10kΩ] ──── ESP32 GPIO35
PC817 emisor   (pin 3) ──── GND

Con 220V:  PC817 conduce → GPIO35 = HIGH  (alimentación OK)
Sin 220V:  PC817 apagado → GPIO35 = LOW   (corriendo en batería)
```

## PINOUT COMPLETO ESP32 DevKit V1
```
GPIO 21 → SDA I2C (MCP23017)
GPIO 22 → SCL I2C (MCP23017)
GPIO 34 → INTA MCP23017 (interrupt, solo entrada)
GPIO 35 → Supervisión 220V (solo entrada)
GPIO 16 → UART2 RX (del teclado)
GPIO 17 → UART2 TX (al teclado)
GPIO  2 → LED estado integrado
GPIO  4 → RESERVA
GPIO  5 → RESERVA
GPIO 18 → RESERVA
GPIO 19 → RESERVA
GPIO 23 → RESERVA
GPIO 25 → RESERVA
GPIO 26 → RESERVA
GPIO 27 → RESERVA
GPIO 32 → RESERVA
GPIO 33 → RESERVA
```

## TABLA DE MATERIALES NUEVOS NECESARIOS
```
Componente                  Cantidad    Uso
─────────────────────────────────────────────────────
ESP32 DevKit V1             1           Ya disponible
MCP23017                    1           Ya disponible (confirmado)
Resistor 4k7Ω 1/4W          2           Pull-up I2C SDA/SCL
Resistor 5k6Ω 1/4W          5           EOL zonas Z1-Z5
Resistor 10kΩ 1/4W          3           Divisor supervisión alimentación
Optoacoplador PC817          1           Supervisión 220V
Módulo relay 5V 3 canales   1           Sirena1 + Sirena2 + Aux
Diodo 1N4007                1           Separación batería/transformador
Cable par trenzado 4 hilos  1-2m        UART teclado↔central (si es local)
Caja plástica DIN o similar 1           Carcasa del panel central
```

## DIAGRAMA DE BLOQUES DEL SISTEMA
```
                    ┌─────────────────────────────┐
                    │      HOME ASSISTANT          │
                    │   + MQTT Broker              │
                    │   + Telegram Bot             │
                    └──────────┬──────────────────-┘
                               │ WiFi (192.168.1.x)
              ─────────────────┼──────────────────────
             │                 │                      │
    ┌────────┴──────┐  ┌───────┴──────┐    ┌─────────┴─────┐
    │  TECLADO      │  │  CENTRAL     │    │  Teléfono      │
    │  ESP8266      │  │  ESP32       │    │  Telegram App  │
    │  OLED+Keypad  │  │  MCP23017    │    └───────────────-┘
    └───────┬───────┘  └──────┬───────┘
            │   UART/WiFi     │
            └────────────────-┘
                              │
                   ┌──────────┼──────────┐
                   │          │          │
              ┌────┴───┐  ┌───┴───┐  ┌──┴────┐
              │Zonas   │  │Sirenas│  │Batería│
              │Z1-Z5   │  │12V    │  │12V    │
              │(NC)    │  │Relay  │  │Backup │
              └────────┘  └───────┘  └───────┘
```
