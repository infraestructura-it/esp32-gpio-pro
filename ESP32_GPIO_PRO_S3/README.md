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
| Restauración de estado GPIO tras power-off | ✅ Funcionando |
| IP estática configurable | ✅ Funcionando |
| MQTT (broker externo, pub/sub) | ✅ Funcionando |
| Relay WebSocket P2P (WebSocketsClient) | ✅ Funcionando |
| OTA por navegador (Update.h) | ✅ Funcionando |
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
Partition Scheme:   Minimal SPIFFS (1.9MB APP with OTA / 128KB SPIFFS)
USB CDC On Boot:    Enabled
Upload Speed:       921600
```

> ⚠️ El esquema de partición **Minimal SPIFFS** es obligatorio para OTA. Con **Huge APP** no compila.

---

## Librerías requeridas

Instalar desde **Tools → Manage Libraries**:

| Librería | Autor |
|----------|-------|
| WebSockets | Markus Sattler |
| ArduinoJson | Benoit Blanchon (v7) |
| PubSubClient | Nick O'Leary |

`Update.h` viene incluida en el SDK del ESP32 — no requiere instalación adicional.

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

Esto borra toda la NVS: WiFi, usuarios, scheduler, MQTT, Relay y estado de GPIOs.

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
- **Estado físico de los GPIOs** — modo, valor (HIGH/LOW o duty cycle) y frecuencia PWM

Lo que **no** persiste:

- Sesiones de usuario — los tokens expiran al reiniciar, requiere login de nuevo

---

## Restauración de estado GPIO

Al encender, el firmware restaura automáticamente el estado físico de cada pin tal como estaba antes del apagado.

El estado se guarda en el namespace NVS `gpiostate` cada vez que ocurre un cambio desde cualquier origen:

| Origen del cambio | Se guarda |
|-------------------|-----------|
| UI web (toggle, slider PWM) | ✓ |
| API REST (digital / pwm / freq / mode) | ✓ |
| Scheduler NTP | ✓ |
| Relay WebSocket | ✓ |

Al arrancar, `restoreGPIOState()` aplica físicamente:

| Modo guardado | Acción al arrancar |
|---------------|--------------------|
| OUTPUT HIGH | `pinMode OUTPUT` + `digitalWrite HIGH` |
| OUTPUT LOW | `pinMode OUTPUT` + `digitalWrite LOW` |
| PWM | `ledcAttach` + `ledcWrite` con duty y freq guardados |
| INPUT | `pinMode INPUT` |
| ADC | `pinMode INPUT` + `analogSetPinAttenuation` |
| NONE | Sin acción — pin no tocado |

---

## OTA — Actualización por navegador

Permite actualizar el firmware sin cable USB enviando el `.bin` directamente desde el navegador.

### Cómo generar el .bin

```
Arduino IDE → Sketch → Export Compiled Binary
```

El archivo `.bin` se genera en la misma carpeta del proyecto.

### Cómo actualizar

1. Abrir `http://<IP>` → login como **admin**
2. Tab **Admin** → botón **ACTUALIZAR FIRMWARE**  
   (o directamente en el tab **OTA**)
3. Seleccionar el archivo `.bin`
4. Presionar **SUBIR FIRMWARE**
5. Barra de progreso en tiempo real
6. El dispositivo reinicia automáticamente al completar
7. Reconectar al mismo IP después del reinicio (~5 segundos)

### Cómo funciona internamente

```
Navegador  ──POST /update (.bin)──►  ESP32-S3
                                       │
                                       ├─ Update.begin()
                                       ├─ Update.write() x chunks
                                       ├─ Update.end()  ← verifica checksum
                                       ├─ server.send(200)
                                       └─ ESP.restart() → arranca con nuevo firmware
```

El ESP32-S3 tiene dos particiones de app en flash (esquema Minimal SPIFFS). La actualización escribe en la partición inactiva. Si falla en cualquier punto, la partición activa anterior sigue intacta.

El endpoint `POST /update` requiere autenticación con header `X-Token` y rol **admin**.

---

## Tabs de la interfaz web

### GPIOs
Control en tiempo real de los 22 pines. Cada tarjeta muestra el modo actual, valor, nombre personalizable y sparkline de historial. Modos disponibles según el pin: OUTPUT, INPUT, PWM, ADC.

### Scheduler
Hasta 20 eventos programados con sincronización NTP. Cada evento define: pin, acción (on/off/pwm), hora, minuto y días de semana activos.

### Historial
Registro de los últimos 50 cambios de estado con timestamp, pin, modo, valor y usuario.

### Admin
Gestión de usuarios (hasta 5), cambio de contraseña, información del dispositivo y acceso a Red, MQTT, Relay y OTA.

### Red *(oculto hasta login admin)*
Configuración de IP estática o DHCP, gateway, máscara y DNS.

### MQTT *(oculto hasta login admin)*
Conexión a broker MQTT externo con pub/sub de estado e historial.

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

### OTA *(oculto hasta login admin)*
Actualización de firmware por navegador sin cable USB.

---

## API REST

Todos los endpoints requieren header `X-Token` excepto `/api/login`.

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
| DELETE | `/api/users/<n>` | Eliminar usuario |
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
| POST | `/update` | Subir firmware OTA (.bin) |

---

## Módulo Relay (WebSocket P2P)

Cada dispositivo genera un **Device ID único** basado en su MAC. El servidor relay puede ser el `relay.js` del proyecto, desplegado en VPS o Railway.

Flujo:
```
ESP32-S3  ──WebSocket──►  Relay Server  ◄──WebSocket──  App / Browser
              path: /ws?id=<deviceId>&role=device
```

Comandos aceptados:
```json
{ "cmd": "on",  "pin": 0 }
{ "cmd": "off", "pin": 0 }
{ "cmd": "pwm", "pin": 0, "val": 128 }
{ "cmd": "state" }
```

---

## Modo DEBUG

```cpp
#define DEBUG_MODE 1
```

AP directo sin provisioning, login deshabilitado, sin NVS. Acceso en `http://192.168.4.1`.

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

## Namespaces NVS

| Namespace | Contenido |
|-----------|-----------|
| `cfg` | WiFi, usuarios, timezone |
| `names` | Nombres personalizados de pines |
| `sched` | Eventos del scheduler |
| `gpiostate` | Modo, valor y frecuencia de cada GPIO |
| `mqttcfg` | Configuración MQTT completa |
| `netcfg` | IP estática, gateway, DNS |
| `relaycfg` | Host, puerto, token, TLS del relay |

Un reset total borra todos los namespaces.

---

## Arquitectura del firmware

```
setup()
  ├─ memset pinState / users / sessions / sched / hist
  ├─ loadConfig()          → WiFi, usuarios, timezone
  ├─ loadNames()           → nombres de pines
  ├─ loadSched()           → eventos scheduler
  ├─ restoreGPIOState()    → aplica físicamente el estado de cada pin
  └─ connectWiFi / startNormalMode / mqttInit / relayInit / registerOTARoutes

loop()
  ├─ server.handleClient()
  ├─ webSocket.loop()
  ├─ checkScheduler()
  ├─ mqttLoop()
  ├─ relayLoop()
  ├─ checkResetButton()
  └─ checkSerialReset()
```

Sin FreeRTOS, sin `xTaskCreatePinnedToCore`, sin punteros dinámicos. Objetos globales directos — crítico para estabilidad en ESP32-S3.

---

## Historial de versiones

| Versión | Cambios |
|---------|---------|
| v1.0 | Port WROOM → ESP32-S3, tabla de pines S3, módulo Relay WebSocket |
| v1.1 | Restauración de estado GPIO tras power-off, clearAllConfig completo |
| v2.0 | OTA por navegador (Update.h), partición Minimal SPIFFS |

---

## Próximas versiones

- **QR P2P estilo Dahua** — generación de QR con Device ID para emparejamiento desde app Android

---

## Autor

**Jairo Sepúlveda** — Director General IIT (Infraestructura-IT)  
Bogotá, Colombia · 2026
