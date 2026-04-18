# ESP32 GPIO PRO — ESP32-S3

Control profesional de GPIOs vía web embebida con WebSocket en tiempo real.  
Firmware single-file para **ESP32-S3**, portado desde la versión estable WROOM.

---

## Características implementadas

| Módulo | Estado |
|--------|--------|
| Control GPIO — OUTPUT / INPUT / PWM / ADC | ✅ Funcionando |
| Interfaz web embebida (sin servidor externo) | ✅ Funcionando |
| WebSocket tiempo real (puerto 81) | ✅ Funcionando |
| Multi-usuario admin / viewer con sesiones y tokens | ✅ Funcionando |
| Scheduler NTP con días de semana | ✅ Funcionando |
| Historial de eventos (50 entradas) | ✅ Funcionando |
| Modo Provisioning (AP captive portal) | ✅ Funcionando |
| Persistencia NVS (configuración survives power-off) | ✅ Funcionando |
| IP estática configurable | ✅ Funcionando |
| MQTT (broker externo, pub/sub) | ✅ Funcionando |
| Relay WebSocket P2P (WebSocketsClient) | ✅ Implementado |
| QR P2P estilo Dahua | 🔜 Próxima versión |

---

## Hardware

| Parámetro | Valor |
|-----------|-------|
| MCU | ESP32-S3 rev2 |
| Flash | 4 MB |
| PSRAM | No requerida |
| GPIO controlados | 22 |
| LED onboard | GPIO 2 |
| Botón BOOT (reset total) | GPIO 0 |

---

## Mapa de pines (PIN_TABLE)

| Índice | GPIO | OUT | IN | PWM | ADC |
|--------|------|-----|----|-----|-----|
| 0 | 1 | ✓ | ✓ | ✓ | — |
| 1 | 2 (LED) | ✓ | ✓ | ✓ | — |
| 2 | 3 | ✓ | ✓ | ✓ | — |
| 3 | 4 | ✓ | ✓ | ✓ | ✓ |
| 4 | 5 | ✓ | ✓ | ✓ | ✓ |
| 5 | 7 | ✓ | ✓ | ✓ | — |
| 6 | 8 | ✓ | ✓ | ✓ | — |
| 7 | 9 | ✓ | ✓ | ✓ | — |
| 8 | 12 | ✓ | ✓ | ✓ | — |
| 9 | 13 | ✓ | ✓ | ✓ | — |
| 10 | 14 | ✓ | ✓ | ✓ | — |
| 11 | 15 | ✓ | ✓ | ✓ | — |
| 12 | 16 | ✓ | ✓ | ✓ | — |
| 13 | 17 | ✓ | ✓ | ✓ | — |
| 14 | 18 | ✓ | ✓ | ✓ | — |
| 15 | 21 | ✓ | ✓ | ✓ | — |
| 16 | 26 | ✓ | ✓ | ✓ | — |
| 17 | 33 | ✓ | ✓ | ✓ | ✓ |
| 18 | 34 | ✓ | ✓ | ✓ | ✓ |
| 19 | 35 | ✓ | ✓ | ✓ | ✓ |
| 20 | 36 | ✓ | ✓ | ✓ | ✓ |
| 21 | 37 | — | ✓ | — | ✓ |

> GPIOs 6-11 y 22-24 no se usan — están reservados internamente en el ESP32-S3.

---

## Configuración Arduino IDE

```
Board:              ESP32S3 Dev Module
Flash Size:         4MB
Partition Scheme:   Huge APP (3MB No OTA)
USB CDC On Boot:    Enabled
Upload Speed:       921600
```

---

## Librerías requeridas

Instalar desde **Tools → Manage Libraries**:

| Librería | Autor |
|----------|-------|
| WebSockets | Markus Sattler |
| ArduinoJson | Benoit Blanchon (v7) |
| PubSubClient | Nick O'Leary |

---

## Primer uso

1. Flashear el firmware con `DEBUG_MODE 0`
2. El ESP32-S3 levanta un AP: **`ESP32-GPIO-PRO`** (sin contraseña)
3. Conectarse desde cualquier dispositivo a ese AP
4. Abrir el navegador en **`http://192.168.4.1`**
5. Completar el formulario de provisioning:
   - SSID y contraseña de la red WiFi local
   - Usuario admin + contraseña
   - Timezone (`-5` para Colombia)
6. El dispositivo se reinicia y se conecta a la red
7. La IP asignada aparece en el **Serial Monitor** → `http://<IP>`

---

## Reset total

Desde el Serial Monitor (cualquier velocidad) escribir la letra **`r`**  
O mantener presionado el botón **BOOT (GPIO 0)** durante **3 segundos**.

Esto borra toda la NVS: WiFi, usuarios, scheduler, MQTT, Relay.

---

## Persistencia NVS

Todo lo siguiente sobrevive a cortes de energía:

- Credenciales WiFi
- Usuario admin y contraseña
- Nombres personalizados de pines
- Scheduler (hasta 20 eventos)
- Configuración MQTT
- Configuración Relay (host, puerto, token, TLS)
- IP estática y configuración de red
- Timezone

Lo que **no** persiste (reinicia al encender):

- Estado físico de los GPIOs (todos arrancan en el modo configurado pero con valor 0)
- Sesiones de usuario (requiere login de nuevo)

---

## Tabs de la interfaz web

### GPIOs
Control en tiempo real de los 22 pines. Cada tarjeta muestra el modo actual, valor, nombre personalizable y sparkline de historial. Modos disponibles según el pin: OUTPUT, INPUT, PWM, ADC.

### Scheduler
Hasta 20 eventos programados con sincronización NTP. Cada evento define: pin, acción (on/off/pwm), hora, minuto y días de semana activos. Funciona sin conexión a internet si el NTP ya sincronizó.

### Historial
Registro de los últimos 50 cambios de estado con timestamp, pin, modo, valor y usuario que realizó el cambio.

### Admin
Gestión de usuarios (hasta 5), cambio de contraseña, información del dispositivo y acceso a las configuraciones avanzadas (Red, MQTT, Relay).

### Red *(oculto hasta login admin)*
Configuración de IP estática o DHCP, gateway, máscara y DNS primario/secundario.

### MQTT *(oculto hasta login admin)*
Conexión a broker MQTT externo. Parámetros: host, puerto, clientId, usuario, contraseña, keepAlive, QoS, prefijo de topics, TLS. Publicación automática de estado y historial.

Topics publicados:
```
<prefix>/pin/+/state
<prefix>/history
<prefix>/ping
<prefix>/alert
```

Topics suscritos:
```
<prefix>/pin/+/set
<prefix>/pin/+/mode
```

---

## API REST

Todos los endpoints requieren autenticación via header `X-Token` excepto `/api/login`.

| Método | Endpoint | Descripción |
|--------|----------|-------------|
| POST | `/api/login` | Obtener token de sesión |
| POST | `/api/logout` | Cerrar sesión |
| GET | `/api/state` | Estado de todos los pines |
| GET | `/api/info` | Info del dispositivo |
| GET | `/api/history` | Historial de eventos |
| POST | `/api/history/clear` | Limpiar historial |
| POST | `/api/all/off` | Apagar todos los outputs |
| GET/POST | `/api/schedule` | Leer / crear eventos scheduler |
| DELETE | `/api/schedule/<id>` | Eliminar evento |
| GET/POST | `/api/users` | Leer / crear usuarios |
| DELETE | `/api/users/<name>` | Eliminar usuario |
| GET/POST | `/api/wifi/scan` | Escanear redes WiFi |
| POST | `/api/wifi/connect` | Conectar a red |
| GET | `/api/wifi/status` | Estado WiFi |
| GET/POST | `/api/net/config` | Configuración de red |
| GET/POST | `/api/mqtt/config` | Configuración MQTT |
| GET/POST | `/api/relay/config` | Configuración Relay |
| POST | `/api/relay/enable` | Activar/desactivar Relay |
| POST | `/api/relay/connect` | Conectar Relay manualmente |
| POST | `/api/relay/disconnect` | Desconectar Relay |
| GET/POST | `/api/p2p/config` | Configuración P2P |

---

## Módulo Relay (WebSocket P2P)

El firmware incluye un cliente WebSocket que se conecta a un servidor relay externo para permitir control remoto fuera de la red local.

Cada dispositivo genera un **Device ID único** basado en su MAC:
```
<prefix_hex><suffix_hex>   →  ej. A1B2C3D4E5F6...
```

El servidor relay puede ser el `relay.js` desarrollado para este proyecto, desplegado en VPS, Railway o GitHub Codespaces.

Flujo de conexión:
```
ESP32-S3  ──WebSocket──►  Relay Server  ◄──WebSocket──  App / Browser
              path: /ws?id=<deviceId>&role=device
```

Comandos que acepta el relay desde la app:
```json
{ "cmd": "on",  "pin": 0 }
{ "cmd": "off", "pin": 0 }
{ "cmd": "pwm", "pin": 0, "val": 128 }
{ "cmd": "state" }
```

---

## Modo DEBUG

Para desarrollo sin necesidad de provisioning:

```cpp
#define DEBUG_MODE 1
```

En modo DEBUG:
- El ESP32 levanta el AP directamente sin pedir configuración
- El login está deshabilitado (token fijo `DEBUG_ADMIN_TOKEN`)
- No se lee ni escribe NVS
- Acceso directo en `http://192.168.4.1`

---

## Constantes configurables

```cpp
#define MAX_USERS       5       // Usuarios simultáneos máx
#define MAX_SCHED      20       // Eventos scheduler máx
#define MAX_HIST       50       // Entradas de historial
#define SESSION_MS   1800000    // Duración sesión (30 min)
#define RESET_HOLD_MS  3000     // ms para reset por botón
#define PWM_FREQ_DEF   5000     // Frecuencia PWM default Hz
#define PWM_RES           8     // Resolución PWM bits (0-255)
#define RELAY_PING_MS  25000    // Intervalo ping relay
#define RELAY_RETRY_MS  5000    // Reintento conexión relay
```

---

## Arquitectura del firmware

El firmware corre en el loop principal de Arduino **sin FreeRTOS**, siguiendo el mismo patrón del WROOM original que funciona de forma estable:

```
setup() → inicialización → provisioning o conexión WiFi
loop()  → server.handleClient()
        → webSocket.loop()
        → checkScheduler()
        → mqttLoop()
        → relayLoop()
        → checkResetButton()
        → checkSerialReset()
```

Todos los objetos son **globales directos** (sin `new`, sin punteros nulos, sin `xTaskCreatePinnedToCore`). Esto es crítico para la estabilidad en el ESP32-S3.

---

## Nota de portabilidad WROOM → S3

La única diferencia funcional entre la versión WROOM y esta versión S3 es la tabla de pines. Los GPIOs 6-11 (bus flash) y 22-24 (internos) del S3 fueron reemplazados por GPIO 1, 3, 7, 8, 9. Toda la lógica de control, UI, API y persistencia es idéntica.

---

## Próximas versiones

- **QR P2P estilo Dahua** — generación de QR con Device ID en pantalla web para emparejamiento desde app Android sin configuración manual
- **Restauración de estado GPIO al encender** — los pines con OUTPUT activo se restauran físicamente tras un power-off
- **OTA por navegador** — actualización de firmware sin cable desde la interfaz web

---

## Autor

**Jairo Sepúlveda** — Director General IIT (Infraestructura-IT)  
Bogotá, Colombia · 2026
