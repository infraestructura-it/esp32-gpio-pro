/*
 * ESP32 GPIO PRO - OUTPUT / INPUT / PWM / ADC (ESP32-S3)
 * Scheduler NTP | Multi-usuario admin/viewer | Login con token
 *
 * LIBRERIAS (Tools > Manage Libraries):
 *   - WebSockets  por Markus Sattler
 *   - ArduinoJson por Benoit Blanchon (v7)
 *
 * ─────────────────────────────────────────────────
 *  DEBUG_MODE = 1  →  Sin login, sin NVS, AP directo
 *  DEBUG_MODE = 0  →  Comportamiento normal con NVS
 * ─────────────────────────────────────────────────
 *
 * PRIMER USO (modo normal):
 *   1. Conectate a WiFi "ESP32-GPIO-PRO"
 *   2. Abre http://192.168.4.1
 *   3. Configura WiFi + usuario admin + TZ (-5 Colombia)
 *   4. Accede con la IP del Serial Monitor
 * RESET TOTAL: escribe r en Serial Monitor
 */

#define DEBUG_MODE 0   // NORMAL: login, NVS, aprovisionamiento

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <time.h>

#define HTTP_PORT 80
#define WS_PORT 81
#define DNS_PORT 53
#define AP_SSID "ESP32-GPIO-PRO"
#define AP_PASS ""
#define PWM_FREQ_DEF 5000
#define PWM_RES 8
#define NUM_PINS 22
#define MAX_USERS 5
#define MAX_SCHED 20
#define MAX_HIST 50
#define TOKEN_LEN 16
#define SESSION_MS 1800000UL
#define RESET_HOLD_MS 3000
#define PIN_BOOT 0

// Token fijo para modo debug (no necesita login)
#define DEBUG_TOKEN "DEBUG_ADMIN_TOKEN"

struct PinInfo { uint8_t pin; bool canOut,canIn,canPWM,canADC; };
const PinInfo PIN_TABLE[NUM_PINS] = {
  {1,true,true,true,false},{2,true,true,true,false},{3,true,true,true,false},
  {4,true,true,true,true},{5,true,true,true,true},{7,true,true,true,false},
  {8,true,true,true,false},{9,true,true,true,false},{12,true,true,true,false},
  {13,true,true,true,false},{14,true,true,true,false},{15,true,true,true,false},
  {16,true,true,true,false},{17,true,true,true,false},{18,true,true,true,false},
  {21,true,true,true,false},{26,true,true,true,false},{33,true,true,true,true},
  {34,true,true,true,true},{35,true,true,true,true},{36,true,true,true,true},
  {37,false,true,false,true},
};
enum PinMode_t { PM_NONE=0,PM_OUTPUT,PM_INPUT,PM_PWM,PM_ADC };
struct PinState {
  PinMode_t mode; int value; uint32_t freq; uint8_t ledCh;
  char name[24]; char lastChange[20]; char lastUser[16];
};
struct User    { char name[24]; char pass[64]; char role[8]; bool active; };
struct Session { char token[TOKEN_LEN+1]; char user[24]; char role[8];
                 unsigned long expires; bool active; };
struct SchedEvent {
  uint8_t id,pin,hour,min,days; char action[8];
  uint16_t pwm_val; bool enabled,active;
};
struct HistEntry {
  char time[20]; uint8_t pin; char mode[8]; int value; char name[24]; char user[16];
};
PinState   pinState[NUM_PINS];
User       users[MAX_USERS];
Session    sessions[3];
SchedEvent sched[MAX_SCHED];
HistEntry  hist[MAX_HIST];
uint8_t    histHead=0,histCount=0,nextLedCh=0,userCount=0;
enum AppMode { MODE_PROVISION,MODE_NORMAL };
AppMode       appMode=MODE_PROVISION;
String        savedSSID="",savedPASS="",deviceIP="";
int           tzOffset=-5;
uint8_t       wsClients=0;
unsigned long bootTime=0;
bool          ntpSynced=false;
Preferences      prefs;
WebServer        server(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);
DNSServer        dns;

// ── MQTT P2P ──────────────────────────────────────
#include <PubSubClient.h>

// Config MQTT — editable en runtime desde la UI
struct MqttConfig {
  char   host[64];
  int    port;
  char   clientIdCfg[32];
  int    keepAlive;
  char   user[32];
  char   pass[32];
  bool   tls;
  int    qos;
  char   topicState[32];
  char   topicAlert[32];
  char   topicHistory[32];
  uint32_t intervalState;
  uint32_t intervalPing;
  MqttConfig(){
    strlcpy(host,"broker.emqx.io",64); port=1883;
    clientIdCfg[0]='\0'; keepAlive=60;
    user[0]='\0'; pass[0]='\0';
    tls=false; qos=1;
    strlcpy(topicState,"state",32);
    strlcpy(topicAlert,"alert",32);
    strlcpy(topicHistory,"history",32);
    intervalState=10000; intervalPing=25000;
  }
};
MqttConfig mqttCfg;

struct MqttTabs { bool state; bool control; bool history; bool alerts; };

WiFiClient       mqttWifiClient;
PubSubClient     mqttPubSub(mqttWifiClient);

// ── RELAY (WebSocket P2P) ──────────────────────────────────────────────────
struct RelayConfig {
  char   host[64];
  int    port;
  char   token[64];
  bool   tls;
  bool   enabled;
};
RelayConfig relayCfg;
bool        relayEnabled   = false;
bool        relayConnected = false;
String      relayDeviceId  = "";
unsigned long relayLastPing  = 0;
#define RELAY_PING_MS  25000
#define RELAY_RETRY_MS  5000
WebSocketsClient wsRelay;
MqttTabs         mqttTabs        = {true,true,true,true};
String           deviceSerial    = "";
String           mqttClientId    = "";
bool             mqttEnabled     = false;
bool             mqttConnected   = false;
unsigned long    mqttLastPing    = 0;
unsigned long    mqttLastState   = 0;

// ── RED / NETWORK ──────────────────────────────────
struct NetConfig {
  bool   dhcp;
  char   ip[16];
  char   mask[16];
  char   gw[16];
  char   dns1[16];
  char   dns2[16];
  NetConfig(){
    dhcp=true; ip[0]='\0';
    strlcpy(mask,"255.255.255.0",16);
    gw[0]='\0';
    strlcpy(dns1,"8.8.8.8",16);
    strlcpy(dns2,"8.8.4.4",16);
  }
};
NetConfig netCfgSt;

// Prototipos MQTT + NET
void mqttInit();
void mqttLoop();
void mqttSetEnabled(bool en);
void mqttPublishState();
void mqttPublishHistory();
void mqttPublishAlert(uint8_t pin, const char* msg);
void registerP2PRoutes();
void relayInit();
void relayLoop();
bool relayConnect();
void relayDisconnect();
void relayOnMessage(uint8_t* payload, size_t len);
void relayPublishState();
void saveRelayCfgNVS();
void loadRelayCfgNVS();
void registerRelayRoutes();
String relayGetConfigJSON();
void registerMqttConfigRoutes();
void registerNetRoutes();
void registerWifiRoutes();
void saveMqttCfgNVS();
void loadMqttCfgNVS();
void saveNetCfgNVS();
void loadNetCfgNVS();
void applyStaticIP();
String mqttGetConfigJSON();
String netGetConfigJSON();

// Prototipos
const char* modeName(PinMode_t m);
PinMode_t   modeFromStr(const char* s);
bool        applyMode(int idx, PinMode_t nm);
String      buildStateJSON();
void        broadcastPin(int idx, const char* usr);
void        addHist(uint8_t pin, const char* mode, int value, const char* usr, const char* name);
Session*    findSession(const String& token);
Session*    getAuth();
bool        requireAuth(bool adminOnly=false);
String      nowStr();
String      hashPass(const String& pass);
String      genToken();
int         pinIndex(uint8_t pin);
int         findUser(const char* name);
void        addCORS();
void        sendJSON(int code, const String& b);
void        sendError(int code, const char* m);
void        execSched(SchedEvent& ev, const char* usr);
void        saveUsers(); void saveSched(); void saveNames();
void        loadConfig(); void loadSched(); void loadNames();
void        saveGPIOState(); void loadGPIOState(); void restoreGPIOState();
void        clearAllConfig(); bool connectWiFi();
void        sendAppPage(); void sendProvPage();
void        startNormalMode(); void startProvisionMode();
void        onWSEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t len);
void        checkScheduler(); void checkResetButton(); void checkSerialReset();

// ══════════════════════════════════════════════════
//  HTML — APP PRINCIPAL
//  CAMBIO DEBUG: loginPage oculto por defecto,
//  appShell visible, TOKEN y ROLE inyectados
// ══════════════════════════════════════════════════
// HTML PROGMEM
// HTML PROGMEM
// HTML PROGMEM
// HTML PROGMEM
const char APP_1[] PROGMEM = R"XHTMLX(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPIO PRO</title>
<style>

@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@700;900&family=Rajdhani:wght@400;600;700&display=swap');
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#03050a;--panel:#07101f;--card:#0b1628;--card2:#0e1c35;
  --a:#00ffe0;--a2:#0080ff;--on:#39ff14;--warn:#ffd166;
  --err:#ff4466;--ora:#ff6b35;--pur:#c77dff;--pink:#ff6b9d;
  --mo:'Share Tech Mono',monospace;--or:'Orbitron',sans-serif;--ra:'Rajdhani',sans-serif;
  --tx:#c8d8e8;--tx2:rgba(200,216,232,.55);--tx3:rgba(200,216,232,.28);
  --bdr:rgba(255,255,255,.07);--bdr2:rgba(255,255,255,.12);
  --inp-bg:rgba(0,0,0,.45);--card-bg:linear-gradient(145deg,var(--panel),var(--card));
}
body.light{
  --bg:#edf1f7;--panel:#dde4ef;--card:#cdd8e8;--card2:#c0cede;
  --a:#0055cc;--a2:#003fa0;--on:#1a7a00;--warn:#a06000;
  --err:#bb0022;--ora:#aa3300;--pur:#6020a0;--pink:#a02060;
  --tx:#1a2232;--tx2:rgba(26,34,50,.62);--tx3:rgba(26,34,50,.38);
  --bdr:rgba(0,0,0,.12);--bdr2:rgba(0,0,0,.18);
  --inp-bg:rgba(255,255,255,.7);--card-bg:linear-gradient(145deg,var(--panel),var(--card));
}
body{background:var(--bg);color:var(--tx);font-family:var(--ra);min-height:100vh;overflow-x:hidden;transition:background .3s,color .3s;}
body::before{opacity:.4} body.light::before{display:none} body.light::before{display:none}
body::before{content:'';position:fixed;inset:0;pointer-events:none;z-index:0;
  background:
    radial-gradient(ellipse 80% 50% at 10% 5%,rgba(0,255,224,.05),transparent),
    radial-gradient(ellipse 60% 50% at 90% 90%,rgba(0,128,255,.06),transparent),
    repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(0,255,224,.015) 40px),
    repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(0,255,224,.015) 40px);}

/* ── TOP BAR ── */
.bar{position:sticky;top:0;z-index:200;background:rgba(3,5,10,.95);
  backdrop-filter:blur(24px);border-bottom:1px solid rgba(0,255,224,.08);
  padding:0 18px;height:52px;display:flex;align-items:center;justify-content:space-between;
  box-shadow:0 2px 30px rgba(0,0,0,.7);}
.logo{font-family:var(--or);font-weight:900;font-size:1.062rem;letter-spacing:.12em;
  background:linear-gradient(90deg,var(--a),var(--a2));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.logo-s{font-family:var(--mo);font-size:0.496rem;letter-spacing:.18em;
  -webkit-text-fill-color:rgba(200,216,232,.2);display:block;margin-top:1px;}
.bar-r{display:flex;align-items:center;gap:10px;}
.pill{display:flex;align-items:center;gap:5px;font-family:var(--mo);font-size:0.566rem;
  letter-spacing:.1em;padding:3px 9px;border-radius:20px;border:1px solid;
  text-transform:uppercase;transition:all .4s;}
.pill .d{width:5px;height:5px;border-radius:50%;}
@keyframes bl{0%,100%{opacity:1}50%{opacity:.1}}
.poff{color:rgba(200,216,232,.3);border-color:rgba(255,255,255,.07);}
.poff .d{background:rgba(200,216,232,.3);}
.pon{color:var(--on);border-color:rgba(57,255,20,.3);}
.pon .d{background:var(--on);animation:bl 2s infinite;}
.perr{color:var(--err);border-color:rgba(255,68,102,.3);}
.perr .d{background:var(--err);}
.pwarn{color:var(--warn);border-color:rgba(255,209,102,.3);}
.pwarn .d{background:var(--warn);animation:bl .8s infinite;}
.user-pill{display:flex;align-items:center;gap:6px;font-family:var(--mo);font-size:0.614rem;
  color:rgba(200,216,232,.5);cursor:pointer;padding:4px 8px;border-radius:4px;
  border:1px solid rgba(255,255,255,.06);transition:all .2s;}
.user-pill:hover{border-color:rgba(255,255,255,.14);color:var(--tx);}
.user-dot{width:20px;height:20px;border-radius:50%;display:flex;align-items:center;
  justify-content:center;font-size:0.649rem;font-weight:700;font-family:var(--or);}
.ud-admin{background:rgba(0,255,224,.15);color:var(--a);}
.ud-viewer{background:rgba(199,125,255,.15);color:var(--pur);}

/* ── TABS ── */
)XHTMLX";
const char APP_2[] PROGMEM = R"XHTMLX(
.tabs{display:flex;align-items:center;gap:2px;padding:0 18px;
  background:rgba(3,5,10,.8);border-bottom:1px solid rgba(255,255,255,.04);
  position:sticky;top:52px;z-index:190;}
.tab{font-family:var(--mo);font-size:0.614rem;letter-spacing:.12em;padding:10px 14px;
  color:rgba(200,216,232,.3);cursor:pointer;border-bottom:2px solid transparent;
  transition:all .2s;text-transform:uppercase;}
.tab:hover{color:rgba(200,216,232,.7);}
.tab.act{color:var(--a);border-bottom-color:var(--a);}
.tpage{display:none;}.tpage.act{display:block;}

/* ── LAYOUT ── */
.wrap{max-width:1100px;margin:0 auto;padding:18px 14px 60px;position:relative;z-index:1;}

/* ── CARDS ── */
.card{border-radius:10px;padding:16px 18px;margin-bottom:14px;position:relative;overflow:hidden;
  background:var(--card-bg,linear-gradient(145deg,var(--panel),var(--card)));
  border:1px solid rgba(255,255,255,.055);
  box-shadow:0 6px 24px rgba(0,0,0,.4),inset 0 1px 0 rgba(255,255,255,.04);}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;}
.ca::before{background:linear-gradient(90deg,var(--a),var(--a2));}
.cg::before{background:linear-gradient(90deg,var(--on),#00cc44);}
.cb::before{background:linear-gradient(90deg,var(--a2),var(--pur));}
.cp::before{background:linear-gradient(90deg,var(--pur),var(--pink));}
.ct{font-family:var(--or);font-size:0.684rem;letter-spacing:.18em;color:var(--a);
  text-transform:uppercase;margin-bottom:12px;
  display:flex;align-items:center;justify-content:space-between;}
.ig{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:12px;}
@media(max-width:600px){.ig{grid-template-columns:1fr 1fr;}}
.ic{background:rgba(0,0,0,.35);border:1px solid rgba(255,255,255,.05);
  border-radius:7px;padding:9px 11px;}
.ik{font-family:var(--mo);font-size:0.519rem;letter-spacing:.12em;
  color:rgba(200,216,232,.28);text-transform:uppercase;margin-bottom:3px;}
.iv{font-family:var(--or);font-size:0.968rem;color:var(--a);}
.btn-row{display:flex;gap:7px;flex-wrap:wrap;}
.btn{font-family:var(--ra);font-weight:700;font-size:0.767rem;letter-spacing:.14em;
  text-transform:uppercase;padding:8px 14px;border:none;border-radius:5px;
  cursor:pointer;transition:all .2s;position:relative;overflow:hidden;}
.btn::after{content:'';position:absolute;inset:0;background:rgba(255,255,255,.08);opacity:0;transition:.15s;}
.btn:hover::after{opacity:1;}.btn:active{transform:scale(.96);}
.btn:disabled{opacity:.2;cursor:not-allowed;}
.bc{background:rgba(0,255,224,.08);color:var(--a);border:1px solid rgba(0,255,224,.25);}
.bb{background:rgba(0,128,255,.08);color:var(--a2);border:1px solid rgba(0,128,255,.25);}
.bg{background:rgba(57,255,20,.08);color:var(--on);border:1px solid rgba(57,255,20,.25);}
.br{background:rgba(255,68,102,.08);color:var(--err);border:1px solid rgba(255,68,102,.25);}
.bp{background:rgba(199,125,255,.08);color:var(--pur);border:1px solid rgba(199,125,255,.25);}

/* ── GPIO GRID ── */
.gpio-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px;}
.gc{border-radius:9px;padding:13px;position:relative;overflow:hidden;
  background:var(--card-bg,linear-gradient(145deg,var(--panel),var(--card2)));
  border:1px solid rgba(255,255,255,.06);
  box-shadow:0 4px 14px rgba(0,0,0,.35);transition:all .25s;}
.gc::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:rgba(255,255,255,.07);transition:all .3s;}
.gc.m-out{border-color:rgba(57,255,20,.22);}
.gc.m-out::before{background:linear-gradient(90deg,var(--on),#00cc44);box-shadow:0 0 8px rgba(57,255,20,.4);}
.gc.m-in{border-color:rgba(76,201,240,.2);}
.gc.m-in::before{background:linear-gradient(90deg,#4cc9f0,var(--a2));}
.gc.m-pwm{border-color:rgba(255,209,102,.2);}
.gc.m-pwm::before{background:linear-gradient(90deg,var(--warn),#ff9f1c);}
.gc.m-adc{border-color:rgba(199,125,255,.2);}
.gc.m-adc::before{background:linear-gradient(90deg,var(--pur),var(--pink));}

/* GPIO header */
.gh{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:8px;}
)XHTMLX";
const char APP_3[] PROGMEM = R"XHTMLX(
.gn{display:flex;flex-direction:column;gap:2px;}
.gnum{font-family:var(--or);font-weight:900;font-size:1.18rem;
  color:rgba(200,216,232,.35);transition:color .3s;letter-spacing:.04em;}
.gc.m-out .gnum{color:var(--on);}.gc.m-in .gnum{color:#4cc9f0;}
.gc.m-pwm .gnum{color:var(--warn);}.gc.m-adc .gnum{color:var(--pur);}
.gname-wrap{display:flex;align-items:center;gap:4px;}
.gname{font-family:var(--ra);font-size:0.885rem;font-weight:600;color:rgba(200,216,232,.55);
  background:none;border:none;outline:none;width:100%;max-width:120px;
  cursor:pointer;padding:1px 3px;border-radius:2px;transition:all .2s;}
.gname:focus{background:rgba(0,255,224,.06);border:1px solid rgba(0,255,224,.3);
  color:var(--a);cursor:text;}
.edit-ico{font-size:0.708rem;color:rgba(200,216,232,.2);cursor:pointer;flex-shrink:0;}
.gcaps{display:flex;gap:3px;flex-wrap:wrap;}
.cap{font-family:var(--mo);font-size:0.472rem;padding:1px 4px;border-radius:2px;border:1px solid;}
.co{color:var(--on);border-color:rgba(57,255,20,.25);background:rgba(57,255,20,.07);}
.ci{color:#4cc9f0;border-color:rgba(76,201,240,.25);background:rgba(76,201,240,.07);}
.cpw{color:var(--warn);border-color:rgba(255,209,102,.25);background:rgba(255,209,102,.07);}
.cad{color:var(--pur);border-color:rgba(199,125,255,.25);background:rgba(199,125,255,.07);}

/* Mode buttons */
.ms{display:flex;gap:3px;margin-bottom:8px;flex-wrap:wrap;}
.mb{font-family:var(--mo);font-size:0.566rem;padding:2px 6px;border-radius:2px;
  border:1px solid var(--bdr2);background:rgba(128,128,128,.08);
  color:var(--tx3);cursor:pointer;transition:all .18s;}
.mb:hover{border-color:rgba(255,255,255,.22);color:var(--tx);}
.mb.ao{background:rgba(57,255,20,.1);color:var(--on);border-color:rgba(57,255,20,.3);}
.mb.ai{background:rgba(76,201,240,.1);color:#4cc9f0;border-color:rgba(76,201,240,.3);}
.mb.ap{background:rgba(255,209,102,.1);color:var(--warn);border-color:rgba(255,209,102,.3);}
.mb.aa{background:rgba(199,125,255,.1);color:var(--pur);border-color:rgba(199,125,255,.3);}

/* Control */
.gctrl{min-height:44px;margin-bottom:6px;}
.out-row{display:flex;align-items:center;justify-content:space-between;}
.vd{font-family:var(--or);font-size:1.298rem;font-weight:900;
  color:rgba(200,216,232,.15);transition:all .2s;}
.vd.hi{color:var(--on);text-shadow:0 0 12px rgba(57,255,20,.5);}
.tg{width:44px;height:22px;background:rgba(0,0,0,.5);
  border:1px solid rgba(255,255,255,.1);border-radius:11px;
  position:relative;cursor:pointer;transition:all .25s;flex-shrink:0;}
.tg::before{content:'';position:absolute;top:3px;left:3px;width:14px;height:14px;
  border-radius:50%;background:rgba(200,216,232,.22);transition:.25s;}
.tg.on{background:rgba(57,255,20,.14);border-color:rgba(57,255,20,.4);
  box-shadow:0 0 10px rgba(57,255,20,.2);}
.tg.on::before{left:25px;background:var(--on);box-shadow:0 0 7px var(--on);}
.in-row{display:flex;align-items:center;justify-content:space-between;gap:6px;}
.rv{font-family:var(--or);font-size:1.121rem;color:rgba(200,216,232,.25);}
.rv.hi{color:#4cc9f0;text-shadow:0 0 10px rgba(76,201,240,.4);}
.rbtn{font-family:var(--mo);font-size:0.566rem;padding:4px 8px;border-radius:2px;
  background:rgba(76,201,240,.08);color:#4cc9f0;border:1px solid rgba(76,201,240,.25);
  cursor:pointer;transition:all .18s;}
.rbtn:hover{background:rgba(76,201,240,.16);}
.pwm-w{display:flex;flex-direction:column;gap:4px;}
.pwm-t{display:flex;align-items:baseline;justify-content:space-between;}
.pwmv{font-family:var(--or);font-size:1.062rem;color:var(--warn);}
.pwmp{font-family:var(--mo);font-size:0.59rem;color:rgba(255,209,102,.45);}
input[type=range]{width:100%;height:3px;border-radius:2px;outline:none;
  -webkit-appearance:none;background:rgba(255,255,255,.08);cursor:pointer;margin:4px 0;}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;
  border-radius:50%;background:var(--warn);box-shadow:0 0 6px rgba(255,209,102,.5);cursor:pointer;}
.freq-r{display:flex;gap:4px;}
.finp{flex:1;background:rgba(0,0,0,.4);border:1px solid rgba(255,255,255,.07);
)XHTMLX";
const char APP_4[] PROGMEM = R"XHTMLX(
  color:var(--tx);font-family:var(--mo);font-size:0.708rem;padding:3px 6px;
  border-radius:2px;outline:none;}
.finp:focus{border-color:rgba(255,209,102,.4);}
.fbtn{font-family:var(--mo);font-size:0.543rem;padding:3px 6px;border-radius:2px;
  background:rgba(255,209,102,.08);color:var(--warn);
  border:1px solid rgba(255,209,102,.25);cursor:pointer;white-space:nowrap;}
.fbtn:hover{background:rgba(255,209,102,.16);}
.adc-w{display:flex;flex-direction:column;gap:4px;}
.adc-t{display:flex;align-items:baseline;justify-content:space-between;}
.adcv{font-family:var(--or);font-size:1.062rem;color:var(--pur);}
.adcvt{font-family:var(--mo);font-size:0.661rem;color:rgba(199,125,255,.45);}
.abar{height:3px;background:rgba(255,255,255,.06);border-radius:2px;overflow:hidden;}
.abf{height:100%;background:linear-gradient(90deg,var(--pur),var(--pink));
  border-radius:2px;transition:width .35s;width:0%;}
.abtns{display:flex;gap:4px;margin-top:3px;}
.abtn{font-family:var(--mo);font-size:0.543rem;padding:3px 6px;border-radius:2px;
  background:rgba(199,125,255,.08);color:var(--pur);
  border:1px solid rgba(199,125,255,.22);cursor:pointer;transition:all .18s;}
.abtn:hover{background:rgba(199,125,255,.16);}
.none-txt{font-family:var(--mo);font-size:0.614rem;color:rgba(200,216,232,.16);padding:8px 0;}

/* Sparkline */
.spark{width:100%;height:28px;margin-top:4px;}

/* GPIO footer: ultimo cambio */
.gfoot{font-family:var(--mo);font-size:0.519rem;color:rgba(200,216,232,.18);
  margin-top:6px;padding-top:5px;border-top:1px solid rgba(255,255,255,.04);
  display:flex;align-items:center;justify-content:space-between;}
.gfoot-t{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80%;}
.wb{display:inline-block;font-family:var(--mo);font-size:0.472rem;padding:1px 4px;
  border-radius:2px;background:rgba(255,159,28,.1);color:var(--warn);
  border:1px solid rgba(255,159,28,.2);margin-right:2px;}

/* ── SCHEDULER ── */
.sched-table{width:100%;border-collapse:collapse;font-family:var(--mo);font-size:0.708rem;}
.sched-table th{text-align:left;padding:7px 10px;color:rgba(200,216,232,.28);
  letter-spacing:.1em;font-size:0.566rem;text-transform:uppercase;
  border-bottom:1px solid rgba(255,255,255,.05);}
.sched-table td{padding:8px 10px;border-bottom:1px solid rgba(255,255,255,.04);
  color:var(--tx);vertical-align:middle;}
.sched-table tr:hover td{background:rgba(128,128,128,.05);}
.day-badge{display:inline-block;width:18px;height:18px;line-height:18px;text-align:center;
  border-radius:3px;font-size:0.496rem;margin:0 1px;font-family:var(--mo);}
.day-on{background:rgba(0,255,224,.12);color:var(--a);}
.day-off{background:rgba(255,255,255,.04);color:rgba(200,216,232,.2);}
.sbadge{font-family:var(--mo);font-size:0.519rem;padding:2px 6px;border-radius:10px;border:1px solid;}
.s-on{color:var(--on);border-color:rgba(57,255,20,.3);background:rgba(57,255,20,.08);}
.s-off{color:rgba(200,216,232,.3);border-color:rgba(255,255,255,.1);}
.icon-btn{background:none;border:none;cursor:pointer;font-size:1.062rem;
  opacity:.4;transition:opacity .18s;padding:2px 4px;}
.icon-btn:hover{opacity:1;}

/* ── HISTORY ── */
.hist-list{display:flex;flex-direction:column;gap:0;}
.hi{display:flex;align-items:flex-start;gap:10px;padding:9px 0;
  border-bottom:1px solid rgba(255,255,255,.04);}
.hi:last-child{border-bottom:none;}
.hi-dot{width:8px;height:8px;border-radius:50%;flex-shrink:0;margin-top:4px;}
.hi-out{background:var(--on);box-shadow:0 0 6px var(--on);}
.hi-in{background:#4cc9f0;}
.hi-pwm{background:var(--warn);}
.hi-adc{background:var(--pur);}
.hi-sys{background:rgba(200,216,232,.3);}
.hi-body{flex:1;}
.hi-top{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:2px;}
.hi-msg{font-family:var(--ra);font-size:0.885rem;color:var(--tx);}
.hi-time{font-family:var(--mo);font-size:0.566rem;color:rgba(200,216,232,.28);white-space:nowrap;}
.hi-meta{font-family:var(--mo);font-size:0.59rem;color:rgba(200,216,232,.3);}
.hi-user{color:var(--a2);}

/* ── MODAL ── */
)XHTMLX";
const char APP_5[] PROGMEM = R"XHTMLX(
.ov{position:fixed;inset:0;background:rgba(0,0,0,.75);backdrop-filter:blur(6px);
  z-index:500;display:none;align-items:center;justify-content:center;padding:20px;}
.ov.show{display:flex;}
.modal{background:var(--card-bg,linear-gradient(145deg,var(--panel),var(--card)));
  border:1px solid rgba(0,255,224,.18);border-radius:12px;
  padding:24px;width:100%;max-width:420px;position:relative;
  box-shadow:0 20px 60px rgba(0,0,0,.6);}
.modal::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;
  border-radius:12px 12px 0 0;background:linear-gradient(90deg,var(--a),var(--a2));}
.modal-t{font-family:var(--or);font-size:0.826rem;letter-spacing:.16em;
  color:var(--a);text-transform:uppercase;margin-bottom:18px;}
.close-btn{position:absolute;top:14px;right:14px;background:none;border:none;
  color:rgba(200,216,232,.3);cursor:pointer;font-size:1.298rem;
  transition:color .2s;line-height:1;}
.close-btn:hover{color:var(--tx);}
.fld{margin-bottom:12px;}
.flbl{font-family:var(--mo);font-size:0.566rem;letter-spacing:.12em;
  color:rgba(200,216,232,.28);text-transform:uppercase;margin-bottom:4px;}
.inp{background:var(--inp-bg,rgba(0,0,0,.5));border:1px solid var(--bdr2);color:var(--tx);
  font-family:var(--mo);font-size:0.885rem;padding:9px 12px;border-radius:5px;
  outline:none;width:100%;transition:border-color .2s;}
.inp:focus{border-color:rgba(0,255,224,.4);}
select.inp{cursor:pointer;}
.days-row{display:flex;gap:5px;}
.day-chk{display:none;}
.day-lbl{width:30px;height:30px;display:flex;align-items:center;justify-content:center;
  border-radius:4px;border:1px solid rgba(255,255,255,.1);
  background:rgba(0,0,0,.4);color:rgba(200,216,232,.35);
  cursor:pointer;font-family:var(--mo);font-size:0.649rem;transition:all .18s;}
.day-chk:checked+.day-lbl{background:rgba(0,255,224,.12);color:var(--a);
  border-color:rgba(0,255,224,.35);}

/* ── USERS PANEL ── */
.user-table{width:100%;border-collapse:collapse;font-family:var(--mo);font-size:0.708rem;}
.user-table th{text-align:left;padding:7px 10px;color:rgba(200,216,232,.28);
  letter-spacing:.1em;font-size:0.543rem;text-transform:uppercase;
  border-bottom:1px solid rgba(255,255,255,.05);}
.user-table td{padding:8px 10px;border-bottom:1px solid rgba(255,255,255,.04);
  color:var(--tx);vertical-align:middle;}
.role-badge{font-family:var(--mo);font-size:0.519rem;padding:2px 7px;
  border-radius:10px;border:1px solid;}
.r-admin{color:var(--a);border-color:rgba(0,255,224,.3);background:rgba(0,255,224,.08);}
.r-viewer{color:var(--pur);border-color:rgba(199,125,255,.3);background:rgba(199,125,255,.08);}

/* ── LOGIN ── */
#loginPage{position:fixed;inset:0;background:var(--bg);z-index:1000;
  display:flex;align-items:center;justify-content:center;padding:20px;}
.login-card{width:100%;max-width:360px;background:var(--card-bg,linear-gradient(145deg,var(--panel),var(--card)));
  border:1px solid rgba(0,255,224,.2);border-radius:12px;padding:28px 24px;
  position:relative;box-shadow:0 20px 50px rgba(0,0,0,.7);}
.login-card::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;
  border-radius:12px 12px 0 0;background:linear-gradient(90deg,var(--a),var(--a2));
  box-shadow:0 0 20px rgba(0,255,224,.3);}
.login-logo{text-align:center;margin-bottom:24px;}
.login-logo h1{font-family:var(--or);font-weight:900;font-size:1.416rem;letter-spacing:.1em;
  background:linear-gradient(90deg,var(--a),var(--a2));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.login-logo p{font-family:var(--mo);font-size:0.59rem;color:rgba(200,216,232,.22);
  letter-spacing:.18em;margin-top:3px;}
.lerr{background:rgba(255,68,102,.08);border:1px solid rgba(255,68,102,.2);
  border-radius:4px;padding:8px 12px;font-family:var(--mo);font-size:0.708rem;
  color:var(--err);margin-bottom:12px;display:none;}
.pw-wrap{position:relative;}
.pw-wrap .inp{padding-right:38px;}
.eye{position:absolute;right:10px;top:50%;transform:translateY(-50%);
  background:none;border:none;color:rgba(200,216,232,.3);cursor:pointer;font-size:1.062rem;}
)XHTMLX";
const char APP_6[] PROGMEM = R"XHTMLX(
.submit-btn{width:100%;padding:11px;background:rgba(0,255,224,.1);color:var(--a);
  border:1px solid rgba(0,255,224,.3);border-radius:5px;font-family:var(--or);
  font-weight:700;font-size:0.767rem;letter-spacing:.18em;text-transform:uppercase;
  cursor:pointer;transition:all .2s;margin-top:4px;}
.submit-btn:hover{background:rgba(0,255,224,.18);box-shadow:0 0 20px rgba(0,255,224,.1);}
.submit-btn:active{transform:scale(.97);}

/* ── MISC ── */
.log-box{background:rgba(0,0,0,.25);border:1px solid var(--bdr);
  border-radius:6px;padding:9px 11px;font-family:var(--mo);font-size:0.637rem;
  line-height:1.9;max-height:120px;overflow-y:auto;color:rgba(200,216,232,.3);}
.log-box::-webkit-scrollbar{width:3px;}
.log-box::-webkit-scrollbar-thumb{background:rgba(0,255,224,.15);border-radius:2px;}
.le{display:block;}.lc{color:var(--a);}.lg{color:var(--on);}
.lr{color:var(--err);}.lw{color:var(--warn);}.lo{color:var(--ora);}.lp{color:var(--pur);}
.sep{border:none;border-top:1px solid rgba(255,255,255,.04);margin:12px 0;}
.empty{font-family:var(--mo);font-size:0.684rem;color:rgba(200,216,232,.2);
  text-align:center;padding:20px;}
.clk{font-family:var(--mo);font-size:0.661rem;color:rgba(200,216,232,.4);letter-spacing:.08em;}
@media(max-width:500px){.gpio-grid{grid-template-columns:1fr 1fr;}}


/* Toggle Switch */
.sw{width:42px;height:22px;background:rgba(0,0,0,.5);border:1px solid rgba(255,255,255,.1);
  border-radius:11px;position:relative;cursor:pointer;flex-shrink:0;transition:all .22s;}
.sw::before{content:'';position:absolute;top:3px;left:3px;width:14px;height:14px;
  border-radius:50%;background:rgba(200,216,232,.22);transition:.22s;}
.sw.on{background:rgba(0,255,224,.12);border-color:rgba(0,255,224,.4);}
.sw.on::before{left:23px;background:var(--a);box-shadow:0 0 6px var(--a);}
/* ── CONFIG FORMS (MQTT + RED) ── */
.cfg-section{margin-bottom:14px;}
.cfg-section-title{font-family:var(--mo);font-size:0.543rem;letter-spacing:.14em;text-transform:uppercase;
  color:rgba(0,255,224,.35);margin-bottom:8px;padding-bottom:5px;
  border-bottom:1px solid rgba(255,255,255,.05);}
.cfg-row2{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px;}
.cfg-row4{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:8px;margin-bottom:10px;}
.cfg-field{display:flex;flex-direction:column;gap:5px;}
.cfg-lbl{font-family:var(--mo);font-size:0.566rem;color:rgba(200,216,232,.4);letter-spacing:.06em;}
.cfg-inp{background:var(--inp-bg,rgba(0,0,0,.45));border:1px solid var(--bdr2);border-radius:7px;
  padding:9px 11px;color:var(--tx);font-family:var(--mo);font-size:0.732rem;width:100%;box-sizing:border-box;
  outline:none;transition:border-color .18s;}
.cfg-inp:focus{border-color:rgba(0,255,224,.4);background:rgba(0,255,224,.04);}
.cfg-inp::placeholder{color:rgba(200,216,232,.18);}
select.cfg-inp{cursor:pointer;}
select.cfg-inp option{background:#0d1117;color:var(--tx);}
.cfg-val-ro{background:rgba(0,0,0,.3);border:1px solid rgba(255,255,255,.06);border-radius:7px;
  padding:9px 11px;font-family:var(--mo);font-size:0.708rem;color:var(--a);letter-spacing:.06em;min-height:35px;}
.ip-inp{letter-spacing:.06em;}
@media(max-width:600px){.cfg-row2{grid-template-columns:1fr;}.cfg-row4{grid-template-columns:1fr 1fr;}}
/* ── WIFI TAB ── */
.wifi-net{display:flex;align-items:center;gap:10px;padding:11px 13px;
  background:rgba(0,0,0,.3);border:1px solid rgba(255,255,255,.06);border-radius:8px;
  cursor:pointer;transition:all .18s;user-select:none;}
.wifi-net:hover{background:rgba(0,255,224,.05);border-color:rgba(0,255,224,.2);}
.wifi-net.selected{background:rgba(0,255,224,.08);border-color:rgba(0,255,224,.4);}
.wifi-net-ssid{font-family:var(--ra);font-weight:700;font-size:0.85rem;color:var(--tx);flex:1;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.wifi-net-ssid.current{color:var(--a);}
.wifi-net-info{display:flex;align-items:center;gap:8px;flex-shrink:0;}
.wifi-badge{font-family:var(--mo);font-size:0.472rem;padding:2px 6px;border-radius:4px;
)XHTMLX";
const char APP_7[] PROGMEM = R"XHTMLX(
  letter-spacing:.08em;text-transform:uppercase;}
.wifi-badge.open{background:rgba(57,255,20,.1);color:var(--on);border:1px solid rgba(57,255,20,.2);}
.wifi-badge.secured{background:rgba(255,68,102,.08);color:#ff8899;border:1px solid rgba(255,68,102,.18);}
.wifi-badge.current{background:rgba(0,255,224,.1);color:var(--a);border:1px solid rgba(0,255,224,.25);}
/* Barras RSSI */
.rssi-bars{display:flex;align-items:flex-end;gap:2px;height:16px;}
.rssi-bar{width:4px;border-radius:1px;background:rgba(255,255,255,.1);}
.rssi-bar.lit{background:var(--a);}
.rssi-bar:nth-child(1){height:4px;}.rssi-bar:nth-child(2){height:7px;}
.rssi-bar:nth-child(3){height:11px;}.rssi-bar:nth-child(4){height:16px;}
/* Spinner escaneo */
.wifi-spin{width:28px;height:28px;border:3px solid rgba(0,255,224,.1);
  border-top-color:var(--a);border-radius:50%;animation:spin .7s linear infinite;margin:0 auto;}
@keyframes spin{to{transform:rotate(360deg);}}
/* Pill colores */
.pon{color:var(--on);border-color:rgba(57,255,20,.3);}
.pon .d{background:var(--on);box-shadow:0 0 5px var(--on);}
.pwarn{color:var(--warn);border-color:rgba(255,209,102,.3);}
.pwarn .d{background:var(--warn);}


/* MQTT Config Forms */
/* ── CONFIG FORMS (MQTT + RED) ── */
.cfg-section{margin-bottom:14px;}
.cfg-section-title{font-family:var(--mo);font-size:0.543rem;letter-spacing:.14em;text-transform:uppercase;
  color:rgba(0,255,224,.35);margin-bottom:8px;padding-bottom:5px;
  border-bottom:1px solid rgba(255,255,255,.05);}
.cfg-row2{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px;}
.cfg-row4{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:8px;margin-bottom:10px;}
.cfg-field{display:flex;flex-direction:column;gap:5px;}
.cfg-lbl{font-family:var(--mo);font-size:0.566rem;color:rgba(200,216,232,.4);letter-spacing:.06em;}
.cfg-inp{background:var(--inp-bg,rgba(0,0,0,.45));border:1px solid var(--bdr2);border-radius:7px;
  padding:9px 11px;color:var(--tx);font-family:var(--mo);font-size:0.732rem;width:100%;box-sizing:border-box;
  outline:none;transition:border-color .18s;}
.cfg-inp:focus{border-color:rgba(0,255,224,.4);background:rgba(0,255,224,.04);}
.cfg-inp::placeholder{color:rgba(200,216,232,.18);}
select.cfg-inp{cursor:pointer;}
select.cfg-inp option{background:#0d1117;color:var(--tx);}
.cfg-val-ro{background:rgba(0,0,0,.3);border:1px solid rgba(255,255,255,.06);border-radius:7px;
  padding:9px 11px;font-family:var(--mo);font-size:0.708rem;color:var(--a);letter-spacing:.06em;min-height:35px;}
.ip-inp{letter-spacing:.06em;}
@media(max-width:600px){.cfg-row2{grid-template-columns:1fr;}.cfg-row4{grid-template-columns:1fr 1fr;}}


/* ── THEME TOGGLE ── */
.theme-toggle{display:flex;align-items:center;gap:6px;cursor:pointer;
  padding:5px 10px;border-radius:20px;border:1px solid var(--bdr2);
  background:rgba(128,128,128,.1);transition:all .22s;user-select:none;
  flex-shrink:0;}
.theme-toggle:hover{background:rgba(128,128,128,.2);}
.theme-icon{font-size:0.9rem;line-height:1;transition:transform .3s;}
.ttheme-sw{width:32px;height:18px;background:rgba(0,0,0,.4);
  border:1px solid var(--bdr2);border-radius:9px;position:relative;
  transition:all .22s;flex-shrink:0;}
.ttheme-sw::before{content:'';position:absolute;top:2px;left:2px;
  width:12px;height:12px;border-radius:50%;
  background:rgba(200,216,232,.5);transition:.22s;}
body.light .ttheme-sw{background:rgba(0,100,220,.15);border-color:rgba(0,80,200,.3);}
body.light .ttheme-sw::before{left:16px;background:var(--a);}
</style>
</head>
<body>

<div id="loginPage">
  <div class="login-card">
    <div class="login-logo">
      <h1>GPIO PRO</h1>
      <p>ESP32 CONTROL SYSTEM</p>
    </div>
    <div class="lerr" id="lerr">Usuario o contrasena incorrectos</div>
    <div class="fld">
      <div class="flbl">Usuario</div>
      <input class="inp" id="lusr" type="text" placeholder="usuario" autocomplete="username">
    </div>
    <div class="fld">
      <div class="flbl">Contrasena</div>
      <div class="pw-wrap">
        <input class="inp" id="lpwd" type="password" placeholder="contrasena" autocomplete="current-password">
)XHTMLX";
const char APP_8[] PROGMEM = R"XHTMLX(
        <button class="eye" onclick="toggleEye('lpwd',this)">&#128065;</button>
      </div>
    </div>
    <button class="submit-btn" onclick="doLogin()">INGRESAR</button>
  </div>
</div>

<div id="appShell" style="display:none">
  <div class="bar">
    <div class="logo">GPIO PRO
      <span class="logo-s">ESP32 CONTROL SYSTEM</span>
    </div>
    <div class="bar-r">
      <span class="clk" id="clk">--:--:--</span>
      <div class="pill poff" id="pill"><span class="d"></span><span id="pillTxt">WS</span></div>
      <div class="theme-toggle" id="themeTgl" onclick="toggleTheme()" title="Cambiar tema"><span class="theme-icon" id="themeIcon">&#9790;</span><div class="ttheme-sw" id="themeSw"></div><span class="theme-icon">&#9728;</span></div>
      <div class="user-pill" id="userPill" onclick="showTab('admin')">
        <div class="user-dot ud-admin" id="uDot">A</div>
        <span id="uName">---</span>
      </div>
    </div>
  </div>

  <div class="tabs">
    <div class="tab act" onclick="showTab('gpio')" id="tab-gpio">GPIOs</div>
    <div class="tab" onclick="showTab('sched')" id="tab-sched">Scheduler</div>
    <div class="tab" onclick="showTab('hist')" id="tab-hist">Historial</div>
    <div class="tab" onclick="showTab('admin')" id="tab-admin">Admin</div>
    <div class="tab" onclick="showTab('net')" id="tab-net" style="display:none">Red</div>
    <div class="tab" onclick="showTab('mqtt')" id="tab-mqtt" style="display:none">MQTT</div>
  </div>

  <!-- GPIO TAB -->
  <div class="tpage act" id="page-gpio">
    <div class="wrap">
      <div class="card ca" style="margin-bottom:14px">
        <div class="ct">Dispositivo</div>
        <div class="ig">
          <div class="ic"><div class="ik">IP</div><div class="iv" id="dIP">---</div></div>
          <div class="ic"><div class="ik">UPTIME</div><div class="iv" id="dUp">---</div></div>
          <div class="ic"><div class="ik">HORA NTP</div><div class="iv" id="dNTP">---</div></div>
          <div class="ic"><div class="ik">WS</div><div class="iv" id="dWS">---</div></div>
        </div>
        <div class="btn-row">
          <button class="btn bb" onclick="fetchInfo()">INFO</button>
          <button class="btn bb" onclick="fetchState()">ESTADO</button>
          <button class="btn br" onclick="allOff()">TODO OFF</button>
        </div>
      </div>
      <div class="gpio-grid" id="gpioGrid"></div>
    </div>
  </div>

  <!-- SCHEDULER TAB -->
  <div class="tpage" id="page-sched">
    <div class="wrap">
      <div class="card cb">
        <div class="ct">Eventos programados
          <button class="btn bc" onclick="openSchedModal(null)">+ NUEVO</button>
        </div>
        <div id="schedBody">
          <div class="empty">Cargando...</div>
        </div>
      </div>
    </div>
  </div>

  <!-- HISTORY TAB -->
  <div class="tpage" id="page-hist">
    <div class="wrap">
      <div class="card cp">
        <div class="ct">Historial de cambios
          <button class="btn br" onclick="clearHistory()">LIMPIAR</button>
        </div>
        <div id="histBody">
          <div class="empty">Cargando...</div>
        </div>
      </div>
    </div>
  </div>

  <!-- ADMIN TAB -->
  <div class="tpage" id="page-admin">
    <div class="wrap">
      <div class="card ca">
        <div class="ct">Informacion del sistema</div>
        <div class="ig">
          <div class="ic"><div class="ik">IP</div><div class="iv" id="adIP">---</div></div>
          <div class="ic"><div class="ik">UPTIME</div><div class="iv" id="adUp">---</div></div>
          <div class="ic"><div class="ik">HORA</div><div class="iv" id="adNTP">---</div></div>
          <div class="ic"><div class="ik">WS</div><div class="iv" id="adWS">---</div></div>
        </div>
        <div class="btn-row">
          <button class="btn br" onclick="resetWifi()">RESET WIFI</button>
          <button class="btn br" onclick="doLogout()">CERRAR SESION</button>
        </div>
      </div>
      <div class="card cb" id="usersCard">
        <div class="ct">Usuarios
)XHTMLX";
const char APP_9[] PROGMEM = R"XHTMLX(
          <button class="btn bc" onclick="openUserModal()">+ USUARIO</button>
        </div>
        <div id="usersBody"><div class="empty">Cargando...</div></div>
      </div>
      <div class="card">
        <div class="ct">Log de sistema</div>
        <div class="log-box" id="logBox"><span class="le lc">Iniciado...</span></div>
      </div>
    </div>
  </div>
</div>

<!-- MODAL SCHEDULER -->
<div class="ov" id="schedModal">
  <div class="modal">
    <button class="close-btn" onclick="closeModal('schedModal')">&#x2715;</button>
    <div class="modal-t" id="schedModalT">Nuevo evento</div>
    <input type="hidden" id="sId" value="">
    <div class="fld">
      <div class="flbl">GPIO</div>
      <select class="inp" id="sPin"></select>
    </div>
    <div class="fld">
      <div class="flbl">Accion</div>
      <select class="inp" id="sAct" onchange="updateActVal()">
        <option value="on">ON (digital HIGH)</option>
        <option value="off">OFF (digital LOW)</option>
        <option value="pwm">PWM (valor)</option>
        <option value="toggle">Toggle</option>
      </select>
    </div>
    <div class="fld" id="sPwmFld" style="display:none">
      <div class="flbl">Valor PWM (0-255)</div>
      <input class="inp" id="sPwmV" type="number" min="0" max="255" value="128">
    </div>
    <div class="fld">
      <div class="flbl">Hora</div>
      <input class="inp" id="sTime" type="time" value="08:00">
    </div>
    <div class="fld">
      <div class="flbl">Dias de la semana</div>
      <div class="days-row">
        <input type="checkbox" class="day-chk" id="dL" value="1"><label class="day-lbl" for="dL">L</label>
        <input type="checkbox" class="day-chk" id="dM" value="2"><label class="day-lbl" for="dM">M</label>
        <input type="checkbox" class="day-chk" id="dX" value="3"><label class="day-lbl" for="dX">X</label>
        <input type="checkbox" class="day-chk" id="dJ" value="4"><label class="day-lbl" for="dJ">J</label>
        <input type="checkbox" class="day-chk" id="dV" value="5"><label class="day-lbl" for="dV">V</label>
        <input type="checkbox" class="day-chk" id="dS" value="6"><label class="day-lbl" for="dS">S</label>
        <input type="checkbox" class="day-chk" id="dD" value="0"><label class="day-lbl" for="dD">D</label>
      </div>
    </div>
    <div class="fld">
      <div class="flbl">Estado</div>
      <select class="inp" id="sEn">
        <option value="1">Activo</option>
        <option value="0">Inactivo</option>
      </select>
    </div>
    <div class="btn-row" style="margin-top:14px">
      <button class="btn bg" onclick="saveSched()">GUARDAR</button>
      <button class="btn br" id="sDelBtn" onclick="delSched()" style="display:none">ELIMINAR</button>
    </div>
  </div>
</div>

<!-- MODAL USUARIO -->
<div class="ov" id="userModal">
  <div class="modal">
    <button class="close-btn" onclick="closeModal('userModal')">&#x2715;</button>
    <div class="modal-t">Nuevo usuario</div>
    <div class="fld">
      <div class="flbl">Nombre de usuario</div>
      <input class="inp" id="nusr" type="text" placeholder="usuario">
    </div>
    <div class="fld">
      <div class="flbl">Contrasena</div>
      <div class="pw-wrap">
        <input class="inp" id="npwd" type="password" placeholder="contrasena">
        <button class="eye" onclick="toggleEye('npwd',this)">&#128065;</button>
      </div>
    </div>
    <div class="fld">
      <div class="flbl">Rol</div>
      <select class="inp" id="nrol">
        <option value="admin">Admin (control total)</option>
        <option value="viewer">Viewer (solo lectura)</option>
      </select>
    </div>
    <div class="btn-row" style="margin-top:14px">
      <button class="btn bg" onclick="saveUser()">CREAR</button>
    </div>
  </div>
</div>


  <!-- RED / NETWORK TAB — WiFi + DHCP/IP fija -->
  <div class="tpage" id="page-net">
    <div class="wrap">

      <!-- ① Estado WiFi actual -->
      <div class="card ca">
        <div class="ct">Estado WiFi
          <div class="pill" id="wifiStatPill"><span class="d"></span><span id="wifiStatTxt">---</span></div>
)XHTMLX";
const char APP_10[] PROGMEM = R"XHTMLX(
        </div>
        <div class="cfg-row4">
          <div class="cfg-field">
            <div class="cfg-lbl">SSID</div>
            <div class="cfg-val-ro" id="wifiCurSSID">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">IP actual</div>
            <div class="cfg-val-ro" id="netCurIP">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">Mascara</div>
            <div class="cfg-val-ro" id="netCurMask">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">Gateway</div>
            <div class="cfg-val-ro" id="netCurGW">---</div>
          </div>
        </div>
        <div class="cfg-row4">
          <div class="cfg-field">
            <div class="cfg-lbl">DNS</div>
            <div class="cfg-val-ro" id="netCurDNS">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">RSSI</div>
            <div class="cfg-val-ro" id="wifiCurRSSI">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">Canal</div>
            <div class="cfg-val-ro" id="wifiCurCH">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">MAC</div>
            <div class="cfg-val-ro" id="wifiCurMAC" style="font-size:0.59rem">---</div>
          </div>
        </div>
      </div>

      <!-- ② Escaneo de redes -->
      <div class="card cb">
        <div class="ct">Redes WiFi disponibles
          <button class="btn bb" id="wifiScanBtn" onclick="wifiScan()"
            style="padding:5px 14px;font-size:0.614rem">&#x21BB; ESCANEAR</button>
        </div>
        <div id="wifiScanning" style="display:none;text-align:center;padding:20px 0">
          <div class="wifi-spin"></div>
          <div style="font-family:var(--mo);font-size:0.614rem;color:rgba(200,216,232,.3);margin-top:10px">
            Escaneando redes...</div>
        </div>
        <div id="wifiNetList" style="display:flex;flex-direction:column;gap:6px;min-height:40px">
          <div style="font-family:var(--mo);font-size:0.614rem;color:rgba(200,216,232,.2);
            text-align:center;padding:16px 0">Presiona ESCANEAR para buscar redes</div>
        </div>
      </div>

      <!-- ③ Panel conectar -->
      <div class="card" id="wifiConnectCard" style="display:none">
        <div class="ct">Conectar a red</div>
        <div class="cfg-row2">
          <div class="cfg-field">
            <div class="cfg-lbl">Red seleccionada</div>
            <div class="cfg-val-ro" id="wifiSelSSID" style="color:var(--a)">---</div>
          </div>
          <div class="cfg-field">
            <div class="cfg-lbl">Seguridad</div>
            <div class="cfg-val-ro" id="wifiSelSec">---</div>
          </div>
        </div>
        <div class="cfg-field" id="wifiPassRow">
          <div class="cfg-lbl">Contrasena WiFi</div>
          <div style="position:relative">
            <input class="cfg-inp" id="wifiPass" type="password"
              placeholder="Ingresa la contrasena" style="padding-right:38px">
            <button onclick="toggleEye('wifiPass',this)"
              style="position:absolute;right:10px;top:50%;transform:translateY(-50%);
              background:none;border:none;color:rgba(200,216,232,.3);cursor:pointer;font-size:1.062rem">
              &#128065;</button>
          </div>
        </div>
        <div id="wifiConnMsg" style="display:none;font-family:var(--mo);font-size:0.637rem;
          padding:10px 14px;border-radius:7px;margin:10px 0;line-height:1.7"></div>
        <div id="wifiProgBar" style="display:none;margin:8px 0">
          <div style="font-family:var(--mo);font-size:0.543rem;color:rgba(200,216,232,.3);
            margin-bottom:5px" id="wifiProgTxt">Guardando...</div>
          <div style="background:rgba(0,0,0,.4);border-radius:4px;height:4px;overflow:hidden">
            <div id="wifiProgFill" style="height:100%;width:0%;
              background:linear-gradient(90deg,var(--a),var(--a2));
)XHTMLX";
const char APP_11[] PROGMEM = R"XHTMLX(
              border-radius:4px;transition:width .4s ease"></div>
          </div>
        </div>
        <div class="btn-row">
          <button class="btn bg" id="wifiConnBtn" onclick="wifiConnect()">GUARDAR Y CONECTAR</button>
          <button class="btn" onclick="wifiCancelSel()"
            style="background:rgba(255,255,255,.03);color:rgba(200,216,232,.4);
            border:1px solid rgba(255,255,255,.07)">CANCELAR</button>
        </div>
        <div style="font-family:var(--mo);font-size:0.543rem;color:rgba(200,216,232,.18);
          margin-top:8px;line-height:1.8;padding:8px;background:rgba(0,0,0,.2);border-radius:6px">
          <span style="color:rgba(255,209,102,.5)">&#9432;</span>
          Las credenciales se guardan en flash y el ESP32 reinicia.
          Conectate a tu WiFi y usa la nueva IP.
        </div>
      </div>

      <!-- ④ Nueva IP + redirect -->
      <div id="wifiNewIpCard" style="display:none" class="card">
        <div class="ct" style="color:var(--on)">&#10003; Credenciales guardadas</div>
        <div style="font-family:var(--mo);font-size:0.661rem;color:rgba(200,216,232,.45);
          margin-bottom:14px;line-height:1.9">
          El ESP32 reinicia y se conecta a la red.<br>
          Conectate a tu WiFi y accede por:
        </div>
        <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
          <div class="cfg-val-ro" id="wifiNewIP"
            style="font-size:1.038rem;letter-spacing:.08em;flex:1;min-width:160px;
            color:var(--a);border-color:rgba(0,255,224,.3);text-align:center">---</div>
          <button class="btn bg" id="wifiRedirectBtn" onclick="wifiRedirect()"
            style="white-space:nowrap">IR A NUEVA IP</button>
        </div>
        <div id="wifiRedirectCount" style="font-family:var(--mo);font-size:0.59rem;
          color:rgba(200,216,232,.25);margin-top:8px;text-align:center"></div>
      </div>

      <!-- ⑤ DHCP / IP fija -->
      <div class="card">
        <div class="ct">Configuracion IP</div>
        <div style="display:flex;align-items:center;justify-content:space-between;
          padding:13px 14px;background:rgba(0,128,255,.05);border:1px solid rgba(0,128,255,.2);
          border-radius:9px;margin-bottom:14px;">
          <div>
            <div style="font-family:var(--or);font-size:0.732rem;color:var(--a2);letter-spacing:.1em">DHCP</div>
            <div id="dhcpDesc" style="font-family:var(--mo);font-size:0.543rem;
              color:rgba(200,216,232,.3);margin-top:2px">IP asignada automaticamente por el router</div>
          </div>
          <div class="sw on" id="dhcpSw" onclick="dhcpToggle()"></div>
        </div>
        <div id="staticIpBlock" style="display:none">
          <div class="cfg-section">
            <div class="cfg-section-title">IPv4 estatica</div>
            <div class="cfg-row2">
              <div class="cfg-field">
                <div class="cfg-lbl">Direccion IP</div>
                <input class="cfg-inp ip-inp" id="netIP" type="text" placeholder="192.168.1.100" maxlength="15">
              </div>
              <div class="cfg-field">
                <div class="cfg-lbl">Mascara de subred</div>
                <input class="cfg-inp ip-inp" id="netMask" type="text" placeholder="255.255.255.0" value="255.255.255.0" maxlength="15">
              </div>
            </div>
            <div class="cfg-row2">
              <div class="cfg-field">
                <div class="cfg-lbl">Gateway</div>
                <input class="cfg-inp ip-inp" id="netGW" type="text" placeholder="192.168.1.1" maxlength="15">
              </div>
              <div class="cfg-field">
                <div class="cfg-lbl">DNS primario</div>
                <input class="cfg-inp ip-inp" id="netDNS1" type="text" placeholder="8.8.8.8" value="8.8.8.8" maxlength="15">
              </div>
            </div>
            <div class="cfg-row2">
              <div class="cfg-field">
                <div class="cfg-lbl">DNS secundario</div>
)XHTMLX";
const char APP_12[] PROGMEM = R"XHTMLX(
                <input class="cfg-inp ip-inp" id="netDNS2" type="text" placeholder="8.8.4.4" value="8.8.4.4" maxlength="15">
              </div>
            </div>
          </div>
          <div id="netValidMsg" style="display:none;font-family:var(--mo);font-size:0.637rem;
            padding:8px 12px;border-radius:6px;margin-bottom:10px;"></div>
        </div>
        <div class="btn-row">
          <button class="btn bg" onclick="saveNetConfig()">GUARDAR IP</button>
          <button class="btn bb" onclick="loadNetConfig()">RECARGAR</button>
        </div>
      </div>

    </div>
  </div>

  <!-- MQTT CONFIG TAB -->
  <div class="tpage" id="page-mqtt">
    <div class="wrap">
      <div class="card ca">
        <div class="ct">Parametros MQTT
          <div class="pill poff" id="mqttConPill"><span class="d"></span><span id="mqttConTxt">DESCONECTADO</span></div>
        </div>

        <!-- Master toggle -->
        <div style="display:flex;align-items:center;justify-content:space-between;
          padding:12px 14px;background:rgba(0,255,224,.04);border:1px solid rgba(0,255,224,.14);
          border-radius:8px;margin-bottom:14px;">
          <div>
            <div style="font-family:var(--or);font-size:0.732rem;color:var(--a);letter-spacing:.1em">MQTT P2P</div>
            <div style="font-family:var(--mo);font-size:0.543rem;color:rgba(200,216,232,.3);margin-top:2px">Puente con app movil via broker</div>
          </div>
          <div class="sw" id="mqttMasterSw" onclick="mqttMasterToggle()"></div>
        </div>

        <!-- Broker -->
        <div class="cfg-section">
          <div class="cfg-section-title">Broker</div>
          <div class="cfg-row2">
            <div class="cfg-field" style="flex:2">
              <div class="cfg-lbl">Host / IP</div>
              <input class="cfg-inp" id="mqttHost" type="text" placeholder="broker.emqx.io" value="broker.emqx.io">
            </div>
            <div class="cfg-field" style="flex:1">
              <div class="cfg-lbl">Puerto</div>
              <input class="cfg-inp" id="mqttPort" type="number" placeholder="1883" value="1883" min="1" max="65535">
            </div>
          </div>
          <div class="cfg-row2">
            <div class="cfg-field">
              <div class="cfg-lbl">Cliente ID</div>
              <input class="cfg-inp" id="mqttClientId" type="text" placeholder="esp32-gpio-XXXX">
            </div>
            <div class="cfg-field">
              <div class="cfg-lbl">Keep Alive (s)</div>
              <input class="cfg-inp" id="mqttKA" type="number" value="60" min="10" max="300">
            </div>
          </div>
        </div>

        <!-- Auth -->
        <div class="cfg-section">
          <div class="cfg-section-title">Autenticacion (opcional)</div>
          <div class="cfg-row2">
            <div class="cfg-field">
              <div class="cfg-lbl">Usuario</div>
              <input class="cfg-inp" id="mqttUser" type="text" placeholder="usuario">
            </div>
            <div class="cfg-field">
              <div class="cfg-lbl">Contrasena</div>
              <div style="position:relative">
                <input class="cfg-inp" id="mqttPass" type="password" placeholder="contrasena" style="padding-right:34px">
                <button onclick="toggleEye('mqttPass',this)" style="position:absolute;right:8px;top:50%;transform:translateY(-50%);background:none;border:none;color:rgba(200,216,232,.3);cursor:pointer;font-size:1.003rem">&#128065;</button>
              </div>
            </div>
          </div>
        </div>

        <!-- TLS -->
        <div class="cfg-section">
          <div class="cfg-section-title">Seguridad</div>
          <div style="display:flex;align-items:center;justify-content:space-between;padding:8px 0">
            <div>
              <div style="font-family:var(--ra);font-weight:600;font-size:0.885rem">TLS / SSL</div>
              <div style="font-family:var(--mo);font-size:0.519rem;color:rgba(200,216,232,.28);margin-top:1px">Cifrado — usar puerto 8883</div>
)XHTMLX";
const char APP_13[] PROGMEM = R"XHTMLX(
            </div>
            <div class="sw" id="mqttTlsSw" onclick="mqttToggleTls()"></div>
          </div>
        </div>

        <!-- Topics -->
        <div class="cfg-section">
          <div class="cfg-section-title">Topics (prefijo automatico: gpio/{serial}/)</div>
          <div class="cfg-row2">
            <div class="cfg-field">
              <div class="cfg-lbl">State topic</div>
              <input class="cfg-inp" id="mqttTState" type="text" value="state" placeholder="state">
            </div>
            <div class="cfg-field">
              <div class="cfg-lbl">Alert topic</div>
              <input class="cfg-inp" id="mqttTAlert" type="text" value="alert" placeholder="alert">
            </div>
          </div>
          <div class="cfg-row2">
            <div class="cfg-field">
              <div class="cfg-lbl">History topic</div>
              <input class="cfg-inp" id="mqttTHistory" type="text" value="history" placeholder="history">
            </div>
            <div class="cfg-field">
              <div class="cfg-lbl">QoS (suscripcion)</div>
              <select class="cfg-inp" id="mqttQos">
                <option value="0">0 — At most once</option>
                <option value="1" selected>1 — At least once</option>
              </select>
            </div>
          </div>
        </div>

        <!-- Intervalos -->
        <div class="cfg-section">
          <div class="cfg-section-title">Intervalos de publicacion</div>
          <div class="cfg-row2">
            <div class="cfg-field">
              <div class="cfg-lbl">Estado (ms)</div>
              <input class="cfg-inp" id="mqttIntState" type="number" value="10000" min="1000" step="500">
            </div>
            <div class="cfg-field">
              <div class="cfg-lbl">Ping/LWT (ms)</div>
              <input class="cfg-inp" id="mqttIntPing" type="number" value="25000" min="5000" step="1000">
            </div>
          </div>
        </div>

        <div class="btn-row" style="margin-top:6px">
          <button class="btn bg" onclick="saveMqttConfig()">GUARDAR Y RECONECTAR</button>
          <button class="btn bb" onclick="loadMqttConfig()">RECARGAR</button>
          <button class="btn br" onclick="mqttDisconnectNow()">DESCONECTAR</button>
        </div>

        <!-- Status log -->
        <div style="margin-top:12px">
          <div style="font-family:var(--mo);font-size:0.543rem;color:rgba(200,216,232,.25);letter-spacing:.12em;text-transform:uppercase;margin-bottom:5px">Log MQTT</div>
          <div class="log-box" id="mqttLog"><span class="le lc">Listo...</span></div>
        </div>
      </div>
    </div>
  </div>

<script>

// ── TEMA CLARO / OSCURO ──────────────────────────────────────────────────
var darkMode = true;
function toggleTheme(){
  darkMode = !darkMode;
  if(darkMode){
    document.body.classList.remove("light");
    G("themeIcon").textContent = String.fromCharCode(9790);
    try{localStorage.setItem("theme","dark");}catch(e){}
  } else {
    document.body.classList.add("light");
    G("themeIcon").textContent = String.fromCharCode(9728);
    try{localStorage.setItem("theme","light");}catch(e){}
  }
}
function initTheme(){
  var t="dark";
  try{t=localStorage.getItem("theme")||"dark";}catch(e){}
  if(t==="light"){
    darkMode=false;
    document.body.classList.add("light");
    if(G("themeIcon")) G("themeIcon").textContent=String.fromCharCode(9728);
  }
}


var TOKEN="";
var ROLE="";
var MY_USER="";
var ws=null;
var isAdmin=false;

var PINS=[
  {p:2, o:true, i:true, pw:true, ad:false,r:true, n:"LED onboard"},
  {p:4, o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 4"},
  {p:5, o:true, i:true, pw:true, ad:false,r:true, n:"GPIO 5"},
  {p:12,o:true, i:true, pw:true, ad:false,r:true, n:"GPIO 12"},
  {p:13,o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 13"},
  {p:14,o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 14"},
  {p:15,o:true, i:true, pw:true, ad:false,r:true, n:"GPIO 15"},
  {p:16,o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 16"},
)XHTMLX";
const char APP_14[] PROGMEM = R"XHTMLX(
  {p:17,o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 17"},
  {p:18,o:true, i:true, pw:true, ad:false,r:false,n:"SPI SCK"},
  {p:19,o:true, i:true, pw:true, ad:false,r:false,n:"SPI MISO"},
  {p:21,o:true, i:true, pw:true, ad:false,r:false,n:"I2C SDA"},
  {p:22,o:true, i:true, pw:true, ad:false,r:false,n:"I2C SCL"},
  {p:23,o:true, i:true, pw:true, ad:false,r:false,n:"SPI MOSI"},
  {p:25,o:true, i:true, pw:true, ad:false,r:false,n:"DAC1"},
  {p:26,o:true, i:true, pw:true, ad:false,r:false,n:"DAC2"},
  {p:27,o:true, i:true, pw:true, ad:false,r:false,n:"GPIO 27"},
  {p:32,o:true, i:true, pw:true, ad:true, r:false,n:"ADC CH4"},
  {p:33,o:true, i:true, pw:true, ad:true, r:false,n:"ADC CH5"},
  {p:34,o:false,i:true, pw:false,ad:true, r:false,n:"ADC VP"},
  {p:35,o:false,i:true, pw:false,ad:true, r:false,n:"ADC VN"},
  {p:36,o:false,i:true, pw:false,ad:true, r:false,n:"ADC 36"}
];
var ST={};
var SPARK={};
PINS.forEach(function(p){
  ST[p.p]={mode:"none",value:0,freq:5000,name:p.n,lastChange:"",lastUser:""};
  SPARK[p.p]=[];
});
var SCHED_DATA=[];

function hesc(s){return s.split("&").join("&amp;").split("<").join("&lt;").split(">").join("&gt;").split(String.fromCharCode(34)).join("&quot;");}
function G(i){return document.getElementById(i);}
function ts(){var d=new Date();return("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2)+":"+("0"+d.getSeconds()).slice(-2);}
function fmtUp(s){if(s==null)return"---";var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sc=s%60;return("0"+h).slice(-2)+":"+("0"+m).slice(-2)+":"+("0"+sc).slice(-2);}
function log(t,c){var b=G("logBox");if(!b)return;var s=document.createElement("span");s.className="le "+(c||"lc");s.textContent="["+ts()+"] "+t;b.appendChild(s);b.insertAdjacentHTML("beforeend","<br>");b.scrollTop=b.scrollHeight;}
function pill(c,t){G("pill").className="pill "+c;G("pillTxt").textContent=t;}
function toggleEye(id){var i=G(id);i.type=i.type==="password"?"text":"password";}
function authH(){return {"Content-Type":"application/json","X-Token":TOKEN};}
function apiGet(url){return fetch(url,{headers:authH()}).then(function(r){return r.json();});}
function apiPost(url,body){return fetch(url,{method:"POST",headers:authH(),body:JSON.stringify(body)}).then(function(r){return r.json();});}
function apiDel(url){return fetch(url,{method:"DELETE",headers:authH()}).then(function(r){return r.json();});}
function apiPut(url,body){return fetch(url,{method:"PUT",headers:authH(),body:JSON.stringify(body)}).then(function(r){return r.json();});}

function doLogin(){
  var u=G("lusr").value.trim(),p=G("lpwd").value;
  if(!u||!p)return;
  fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({user:u,pass:p})})
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.ok){
        TOKEN=d.token;ROLE=d.role;MY_USER=u;isAdmin=ROLE==="admin";
        G("loginPage").style.display="none";
        G("appShell").style.display="block";
        G("uName").textContent=u;
        var dot=G("uDot");
        if(isAdmin){dot.className="user-dot ud-admin";dot.textContent="A";}
        else{dot.className="user-dot ud-viewer";dot.textContent="V";}
        if(!isAdmin){G("tab-admin").style.display="none";}
        G("tab-mqtt").style.display="block";
        G("tab-net").style.display="block";
        buildGrid();connectWS();
        setTimeout(fetchInfo,600);setTimeout(fetchState,1000);
        setTimeout(loadSched,1200);setTimeout(loadHistory,1400);
      }else{G("lerr").style.display="block";}
    }).catch(function(){G("lerr").style.display="block";});
}
function doLogout(){
  apiPost("/api/logout",{}).catch(function(){});
  TOKEN="";ROLE="";MY_USER="";
  G("appShell").style.display="none";
  G("loginPage").style.display="flex";
  G("lpwd").value="";
  if(ws){try{ws.close();}catch(e){}ws=null;}
}
G("lpwd").addEventListener("keydown",function(e){if(e.key==="Enter")doLogin();});
G("lusr").addEventListener("keydown",function(e){if(e.key==="Enter")G("lpwd").focus();});
)XHTMLX";
const char APP_15[] PROGMEM = R"XHTMLX(

function showTab(id){
  document.querySelectorAll(".tpage").forEach(function(p){p.classList.remove("act");});
  document.querySelectorAll(".tab").forEach(function(t){t.classList.remove("act");});
  var pg=G("page-"+id),tb=G("tab-"+id);
  if(pg)pg.classList.add("act");
  if(tb)tb.classList.add("act");
  if(id==="hist")loadHistory();
  if(id==="sched")loadSched();
  if(id==="admin"&&isAdmin)loadUsers();
  if(id==="mqtt")loadMqttConfig();
  if(id==="net")loadWifiStatus();
}

setInterval(function(){G("clk").textContent=ts();},1000);

function buildGrid(){
  var g=G("gpioGrid");g.innerHTML="";
  PINS.forEach(function(p){
    var d=document.createElement("div");
    d.id="gc"+p.p;d.className="gc";
    g.appendChild(d);renderCard(p.p);
  });
  populateSchedPins();
}

function drawSpark(pin){
  var data=SPARK[pin];if(!data||data.length<2)return;
  var cv=G("sp"+pin);if(!cv)return;
  var w=cv.offsetWidth||160,h=28;
  cv.width=w;cv.height=h;
  var ctx=cv.getContext("2d");
  ctx.clearRect(0,0,w,h);
  var min=Math.min.apply(null,data),max=Math.max.apply(null,data);
  if(max===min)max=min+1;
  var s=ST[pin];
  var col=s.mode==="pwm"?"#ffd166":s.mode==="adc"?"#c77dff":"#00ffe0";
  ctx.strokeStyle=col;ctx.lineWidth=1.5;ctx.globalAlpha=0.7;
  ctx.shadowColor=col;ctx.shadowBlur=4;
  ctx.beginPath();
  for(var i=0;i<data.length;i++){
    var x=(i/(data.length-1))*w;
    var y=h-((data[i]-min)/(max-min))*(h-4)-2;
    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
  }
  ctx.stroke();
}

var MODES=["none","output","input","pwm","adc"];
function setModeN(pin,n){setMode(pin,MODES[n]);}

function renderCard(pin){
  var p=PINS.filter(function(x){return x.p===pin;})[0];
  var s=ST[pin];var d=G("gc"+pin);
  if(!d||!p)return;
  var mc=s.mode==="output"?"m-out":s.mode==="input"?"m-in":s.mode==="pwm"?"m-pwm":s.mode==="adc"?"m-adc":"";
  d.className="gc "+mc;
  var caps=(p.o?"<span class=\"cap co\">OUT</span>":"")
    +(p.i?"<span class=\"cap ci\">IN</span>":"")
    +(p.pw?"<span class=\"cap cpw\">PWM</span>":"")
    +(p.ad?"<span class=\"cap cad\">ADC</span>":"");
  var mbtns="";
  if(!isAdmin){
    mbtns="<span class=\"vm\">["+s.mode+"]</span>";
  }else{
    if(p.o) mbtns+="<button class=\"mb"+(s.mode==="output"?" ao":"")+"\" onclick=\"setModeN("+pin+",1)\">OUT</button>";
    if(p.i) mbtns+="<button class=\"mb"+(s.mode==="input"?" ai":"")+"\" onclick=\"setModeN("+pin+",2)\">IN</button>";
    if(p.pw)mbtns+="<button class=\"mb"+(s.mode==="pwm"?" ap":"")+"\" onclick=\"setModeN("+pin+",3)\">PWM</button>";
    if(p.ad)mbtns+="<button class=\"mb"+(s.mode==="adc"?" aa":"")+"\" onclick=\"setModeN("+pin+",4)\">ADC</button>";
    mbtns+="<button class=\"mb"+(s.mode==="none"?" ao":"")+"\" onclick=\"setModeN("+pin+",0)\">--</button>";
  }
  var ctrl="<div class=\"none-txt\">Sin configurar</div>";
  if(s.mode==="output"){
    ctrl="<div class=\"out-row\">"
      +"<div class=\"vd"+(s.value?" hi":"")+"\" id=\"vd"+pin+"\">"+(s.value?"HIGH":"LOW")+"</div>"
      +(isAdmin?"<div class=\"tg"+(s.value?" on":"")+"\" id=\"tg"+pin+"\" onclick=\"toggleOut("+pin+")\"></div>":"")
      +"</div>";
  }else if(s.mode==="input"){
    ctrl="<div class=\"in-row\">"
      +"<div class=\"rv"+(s.value?" hi":"")+"\" id=\"rv"+pin+"\">"+(s.value?"HIGH":"LOW")+"</div>"
      +"<button class=\"rbtn\" onclick=\"readIn("+pin+")\">LEER</button>"
      +"</div>";
  }else if(s.mode==="pwm"){
    var pct=Math.round((s.value/255)*100);
    ctrl="<div class=\"pwm-w\">"
      +"<div class=\"pwm-t\"><span class=\"pwmv\" id=\"pwmv"+pin+"\">"+s.value+"</span>"
      +"<span class=\"pwmp\" id=\"pwmp"+pin+"\">"+pct+"%</span></div>"
      +(isAdmin?"<input type=\"range\" min=\"0\" max=\"255\" value=\""+s.value+"\" id=\"sl"+pin+"\" oninput=\"onSlider("+pin+",this.value)\" onchange=\"writePWM("+pin+",this.value)\">":"")
      +"<div class=\"freq-r\">"
      +(isAdmin?"<input class=\"finp\" type=\"number\" id=\"fr"+pin+"\" value=\""+(s.freq||5000)+"\" placeholder=\"Hz\"><button class=\"fbtn\" onclick=\"setFreq("+pin+")\">Hz</button>":"")
)XHTMLX";
const char APP_16[] PROGMEM = R"XHTMLX(
      +"</div>"
      +"<canvas class=\"spark\" id=\"sp"+pin+"\"></canvas>"
      +"</div>";
  }else if(s.mode==="adc"){
    var volt=((s.value/4095)*3.3).toFixed(2);
    var pct2=Math.round((s.value/4095)*100);
    ctrl="<div class=\"adc-w\">"
      +"<div class=\"adc-t\"><span class=\"adcv\" id=\"adcv"+pin+"\">"+s.value+"</span>"
      +"<span class=\"adcvt\" id=\"adcvt"+pin+"\">"+volt+"V</span></div>"
      +"<div class=\"abar\"><div class=\"abf\" id=\"abf"+pin+"\" style=\"width:"+pct2+"%\"></div></div>"
      +"<div class=\"abtns\">"
      +"<button class=\"abtn\" onclick=\"readADC("+pin+")\">LEER</button>"
      +"<button class=\"abtn\" onclick=\"readADCAvg("+pin+")\">AVG</button>"
      +"</div>"
      +"<canvas class=\"spark\" id=\"sp"+pin+"\"></canvas>"
      +"</div>";
  }
  var nm=s.name||p.n;
  var lc=s.lastChange?(s.lastChange+(s.lastUser?" @"+s.lastUser:"")):"";
  d.innerHTML="<div class=\"gh\">"
    +"<div class=\"gn\">"
      +"<span class=\"gnum\">GPIO "+pin+"</span>"
      +"<input class=\"gname\" id=\"gn"+pin+"\" value=\""+hesc(nm)+"\""+(isAdmin?" onblur=\"saveName("+pin+")\"":"")+(isAdmin?" data-pin=\""+pin+"\"":"")+(!isAdmin?" disabled":"")+" title=\"Editar nombre\">"
    +"</div>"
    +"<div class=\"gcaps\">"+caps+(p.r?"<span class=\"wb\">BOOT</span>":"")+"</div>"
    +"</div>"
    +"<div class=\"ms\">"+mbtns+"</div>"
    +"<div class=\"gctrl\">"+ctrl+"</div>"
    +"<div class=\"gfoot\"><span class=\"gfoot-t\">"+lc+"</span></div>";
  setTimeout(function(){drawSpark(pin);},50);
}

function pushSpark(pin,val){
  var a=SPARK[pin];a.push(val);if(a.length>30)a.shift();drawSpark(pin);
}

function setMode(pin,mode){
  apiPost("/api/pin/"+pin+"/mode",{mode:mode})
    .then(function(d){
      if(d.ok){ST[pin].mode=mode;ST[pin].value=0;SPARK[pin]=[];renderCard(pin);log("GPIO"+pin+" -> "+mode,"lc");}
      else log("Error: "+(d.error||"?"),"lr");
    }).catch(function(e){log("setMode: "+e.message,"lr");});
}
function toggleOut(pin){
  if(!isAdmin)return;
  var ns=ST[pin].value?0:1;
  apiPost("/api/pin/"+pin+"/digital",{state:ns})
    .then(function(d){
      if(d.ok){
        ST[pin].value=ns;
        var vd=G("vd"+pin),tg=G("tg"+pin);
        if(vd){vd.textContent=ns?"HIGH":"LOW";vd.className="vd"+(ns?" hi":"");}
        if(tg)tg.className="tg"+(ns?" on":"");
        log("GPIO"+pin+" "+(ns?"HIGH":"LOW"),"lg");
      }
    }).catch(function(e){log("toggle: "+e.message,"lr");});
}
function readIn(pin){
  apiGet("/api/pin/"+pin+"/digital")
    .then(function(d){
      ST[pin].value=d.state;
      var rv=G("rv"+pin);
      if(rv){rv.textContent=d.state?"HIGH":"LOW";rv.className="rv"+(d.state?" hi":"");}
      log("GPIO"+pin+" IN="+(d.state?"HIGH":"LOW"),"lc");
    }).catch(function(e){log("readIn: "+e.message,"lr");});
}
function onSlider(pin,v){
  var vv=G("pwmv"+pin),pp=G("pwmp"+pin);
  if(vv)vv.textContent=v;if(pp)pp.textContent=Math.round((v/255)*100)+"%";
}
function writePWM(pin,duty){
  duty=parseInt(duty);
  apiPost("/api/pin/"+pin+"/pwm",{duty:duty})
    .then(function(d){if(d.ok){ST[pin].value=duty;pushSpark(pin,duty);log("GPIO"+pin+" PWM="+duty,"lw");}})
    .catch(function(e){log("PWM: "+e.message,"lr");});
}
function setFreq(pin){
  var freq=parseInt(G("fr"+pin).value);if(!freq||freq<1)return;
  apiPost("/api/pin/"+pin+"/freq",{freq:freq})
    .then(function(d){if(d.ok){ST[pin].freq=freq;log("GPIO"+pin+" freq="+freq+"Hz","lw");}})
    .catch(function(e){log("freq: "+e.message,"lr");});
}
function readADC(pin){
  apiGet("/api/pin/"+pin+"/adc")
    .then(function(d){updADC(pin,d.raw,d.voltage);pushSpark(pin,d.raw);})
    .catch(function(e){log("ADC: "+e.message,"lr");});
}
function readADCAvg(pin){
  apiGet("/api/pin/"+pin+"/adc/avg")
    .then(function(d){updADC(pin,d.raw,d.voltage);pushSpark(pin,d.raw);})
    .catch(function(e){log("ADCavg: "+e.message,"lr");});
}
function updADC(pin,raw,volt){
  ST[pin].value=raw;
  var pct=Math.round((raw/4095)*100);
  var av=G("adcv"+pin),avt=G("adcvt"+pin),abf=G("abf"+pin);
)XHTMLX";
const char APP_17[] PROGMEM = R"XHTMLX(
  if(av)av.textContent=raw;if(avt)avt.textContent=volt+"V";
  if(abf)abf.style.width=pct+"%";
}
function saveName(pin){
  var inp=G("gn"+pin);if(!inp)return;
  var name=inp.value.trim();if(!name)return;
  ST[pin].name=name;
  apiPost("/api/pin/"+pin+"/name",{name:name})
    .then(function(d){if(d.ok)log("GPIO"+pin+" nombre: "+name,"lc");})
    .catch(function(e){log("name: "+e.message,"lr");});
}
function allOff(){
  apiPost("/api/all/off",{})
    .then(function(){fetchState();log("TODO OFF","lw");})
    .catch(function(e){log("allOff: "+e.message,"lr");});
}
function fetchInfo(){
  apiGet("/api/info")
    .then(function(d){
      G("dIP").textContent=d.ip||"---";G("adIP").textContent=d.ip||"---";
      G("dUp").textContent=fmtUp(d.uptime_s);G("adUp").textContent=fmtUp(d.uptime_s);
      G("dWS").textContent=(d.ws_clients||0)+"";G("adWS").textContent=(d.ws_clients||0)+"";
      G("dNTP").textContent=d.ntp_time||"---";G("adNTP").textContent=d.ntp_time||"---";
    }).catch(function(e){log("info: "+e.message,"lr");});
}
function fetchState(){
  apiGet("/api/state")
    .then(function(d){
      d.pins.forEach(function(p){
        if(ST[p.pin]!==undefined){
          ST[p.pin].mode=p.mode;ST[p.pin].value=p.value;
          if(p.freq)ST[p.pin].freq=p.freq;
          if(p.name)ST[p.pin].name=p.name;
          if(p.last_change)ST[p.pin].lastChange=p.last_change;
          if(p.last_user)ST[p.pin].lastUser=p.last_user;
          renderCard(p.pin);
        }
      });
      log("Estado sincronizado","lc");
    }).catch(function(e){log("state: "+e.message,"lr");});
}
function populateSchedPins(){
  var sel=G("sPin");if(!sel)return;sel.innerHTML="";
  PINS.forEach(function(p){
    var o=document.createElement("option");
    o.value=p.p;o.textContent="GPIO "+p.p+" - "+ST[p.p].name;
    sel.appendChild(o);
  });
}
function updateActVal(){
  var act=G("sAct").value;
  G("sPwmFld").style.display=act==="pwm"?"block":"none";
}
function openSchedModal(ev){
  G("sId").value=ev?ev.id:"";
  G("schedModalT").textContent=ev?"Editar evento":"Nuevo evento";
  G("sDelBtn").style.display=ev?"block":"none";
  if(ev){
    G("sPin").value=ev.pin;G("sAct").value=ev.action;
    G("sPwmV").value=ev.pwm_val||128;
    G("sTime").value=("0"+ev.hour).slice(-2)+":"+("0"+ev.min).slice(-2);
    G("sEn").value=ev.enabled?"1":"0";
    ["dL","dM","dX","dJ","dV","dS","dD"].forEach(function(id){
      var cb=G(id);cb.checked=ev.days.indexOf(parseInt(cb.value))>=0;
    });
  }else{
    G("sAct").value="on";G("sTime").value="08:00";G("sEn").value="1";
    ["dL","dM","dX","dJ","dV","dS","dD"].forEach(function(id){G(id).checked=false;});
  }
  updateActVal();
  G("schedModal").classList.add("show");
}
function closeModal(id){G(id).classList.remove("show");}
function getDays(){
  var d=[];
  ["dL","dM","dX","dJ","dV","dS","dD"].forEach(function(id){
    var cb=G(id);if(cb.checked)d.push(parseInt(cb.value));
  });
  return d;
}
function saveSched(){
  var t=G("sTime").value.split(":");
  var body={pin:parseInt(G("sPin").value),action:G("sAct").value,
    hour:parseInt(t[0]),min:parseInt(t[1]),
    pwm_val:parseInt(G("sPwmV").value),
    days:getDays(),enabled:G("sEn").value==="1"};
  var id=G("sId").value;
  var pr=id?apiPut("/api/schedule/"+id,body):apiPost("/api/schedule",body);
  pr.then(function(d){
    if(d.ok){closeModal("schedModal");loadSched();log("Evento guardado","lg");}
    else log("Error: "+(d.error||"?"),"lr");
  }).catch(function(e){log("sched: "+e.message,"lr");});
}
function delSched(){
  var id=G("sId").value;if(!id)return;
  if(!confirm("Eliminar evento?"))return;
  apiDel("/api/schedule/"+id)
    .then(function(){closeModal("schedModal");loadSched();log("Evento eliminado","lw");})
    .catch(function(e){log("delSched: "+e.message,"lr");});
}
function loadSched(){
  apiGet("/api/schedule")
    .then(function(d){
      SCHED_DATA=d.events||[];
      var body=G("schedBody");
      if(!SCHED_DATA.length){body.innerHTML="<div class=\"empty\">No hay eventos programados</div>";return;}
)XHTMLX";
const char APP_18[] PROGMEM = R"XHTMLX(
      var DAY_N=["D","L","M","X","J","V","S"];
      var html="<table class=\"sched-table\"><thead><tr>"
        +"<th>GPIO</th><th>Accion</th><th>Hora</th><th>Dias</th><th>Estado</th><th></th>"
        +"</tr></thead><tbody>";
      SCHED_DATA.forEach(function(ev,ei){
        var days="";
        for(var i=0;i<=6;i++){
          var on=ev.days.indexOf(i)>=0;
          days+="<span class=\"day-badge "+(on?"day-on":"day-off")+"\">"+DAY_N[i]+"</span>";
        }
        var act=ev.action==="pwm"?"PWM:"+ev.pwm_val:ev.action.toUpperCase();
        html+="<tr>"
          +"<td>GPIO "+ev.pin+"</td>"
          +"<td>"+act+"</td>"
          +"<td>"+("0"+ev.hour).slice(-2)+":"+("0"+ev.min).slice(-2)+"</td>"
          +"<td>"+days+"</td>"
          +"<td><span class=\"sbadge "+(ev.enabled?"s-on":"s-off")+"\">"+(ev.enabled?"ON":"OFF")+"</span></td>"
          +"<td>"+(isAdmin?"<button class=\"icon-btn\" onclick=\"openSchedModal(SCHED_DATA["+ei+"])\">&#9998;</button>":"")+"</td>"
          +"</tr>";
      });
      html+="</tbody></table>";
      body.innerHTML=html;
    }).catch(function(e){log("schedule: "+e.message,"lr");});
}
function loadHistory(){
  apiGet("/api/history")
    .then(function(d){
      var body=G("histBody");
      var events=d.events||[];
      if(!events.length){body.innerHTML="<div class=\"empty\">Sin historial</div>";return;}
      var html="<div class=\"hist-list\">";
      events.slice().reverse().forEach(function(h){
        var dc=h.mode==="output"?"hi-out":h.mode==="input"?"hi-in":h.mode==="pwm"?"hi-pwm":h.mode==="adc"?"hi-adc":"hi-sys";
        html+="<div class=\"hi\">"
          +"<div class=\"hi-dot "+dc+"\"></div>"
          +"<div class=\"hi-body\">"
            +"<div class=\"hi-top\">"
              +"<span class=\"hi-msg\">GPIO "+h.pin+" ["+h.mode+"] = "+h.value+"</span>"
              +"<span class=\"hi-time\">"+h.time+"</span>"
            +"</div>"
            +"<div class=\"hi-meta\"><span class=\"hi-user\">@"+h.user+"</span> &bull; "+h.name+"</div>"
          +"</div>"
          +"</div>";
      });
      html+="</div>";
      body.innerHTML=html;
    }).catch(function(e){log("history: "+e.message,"lr");});
}
function clearHistory(){
  if(!confirm("Borrar historial?"))return;
  apiPost("/api/history/clear",{})
    .then(function(){loadHistory();log("Historial borrado","lw");})
    .catch(function(e){log("clearHist: "+e.message,"lr");});
}
function loadUsers(){
  if(!isAdmin)return;
  apiGet("/api/users")
    .then(function(d){
      var body=G("usersBody");
      var users=d.users||[];
      if(!users.length){body.innerHTML="<div class=\"empty\">Sin usuarios</div>";return;}
      var html="<table class=\"user-table\"><thead><tr><th>Usuario</th><th>Rol</th><th></th></tr></thead><tbody>";
      users.forEach(function(u){
        html+="<tr>"
          +"<td>"+u.name+"</td>"
          +"<td><span class=\"role-badge "+(u.role==="admin"?"r-admin":"r-viewer")+"\">"+(u.role)+"</span></td>"
          +"<td>"+(u.name!==MY_USER?"<button class=\"icon-btn\" onclick=\"delUser("+JSON.stringify(u.name)+")\">&#128465;</button>":"")+"</td>"
          +"</tr>";
      });
      html+="</tbody></table>";
      body.innerHTML=html;
    }).catch(function(e){log("users: "+e.message,"lr");});
}
function openUserModal(){G("nusr").value="";G("npwd").value="";G("nrol").value="admin";G("userModal").classList.add("show");}
function saveUser(){
  var name=G("nusr").value.trim(),pass=G("npwd").value,role=G("nrol").value;
  if(!name||!pass){alert("Completa todos los campos");return;}
  apiPost("/api/users",{name:name,pass:pass,role:role})
    .then(function(d){
      if(d.ok){closeModal("userModal");loadUsers();log("Usuario creado: "+name,"lg");}
      else log("Error: "+(d.error||"?"),"lr");
    }).catch(function(e){log("saveUser: "+e.message,"lr");});
}
function delUser(name){
  if(!confirm("Eliminar usuario "+name+"?"))return;
  apiDel("/api/users/"+name)
    .then(function(d){
      if(d.ok){loadUsers();log("Usuario eliminado: "+name,"lw");}
)XHTMLX";
const char APP_19[] PROGMEM = R"XHTMLX(
      else log("Error: "+(d.error||"?"),"lr");
    }).catch(function(e){log("delUser: "+e.message,"lr");});
}
function resetWifi(){
  if(!confirm("Resetear WiFi? El ESP32 volvera al modo Setup."))return;
  apiGet("/reset").then(function(){log("Reset enviado","lw");})
    .catch(function(e){log("reset: "+e.message,"lr");});
}
function connectWS(){
  if(ws){try{ws.close();}catch(e){}ws=null;}
  pill("pwarn","CONECTANDO");
  try{ws=new WebSocket("ws://"+window.location.hostname+":81");}
  catch(e){log("WS no disponible","lw");return;}
  ws.onopen=function(){
    pill("pon","WS ON");
    log("WebSocket OK","lg");
    ws.send(JSON.stringify({cmd:"AUTH",token:TOKEN}));
    ws.send(JSON.stringify({cmd:"GET_STATE"}));
  };
  ws.onmessage=function(e){
    try{
      var m=JSON.parse(e.data);
      if(m.type==="PIN_CHANGE"&&ST[m.pin]!==undefined){
        ST[m.pin].mode=m.mode;ST[m.pin].value=m.value;
        if(m.freq)ST[m.pin].freq=m.freq;
        if(m.last_change)ST[m.pin].lastChange=m.last_change;
        if(m.last_user)ST[m.pin].lastUser=m.last_user;
        if(m.mode==="pwm"||m.mode==="adc")pushSpark(m.pin,m.value);
        renderCard(m.pin);
        log("WS GPIO"+m.pin+"["+m.mode+"]="+m.value,"lo");
      }else if(m.type==="STATE"){
        m.pins.forEach(function(p){
          if(ST[p.pin]!==undefined){
            ST[p.pin].mode=p.mode;ST[p.pin].value=p.value;
            if(p.freq)ST[p.pin].freq=p.freq;
            if(p.name)ST[p.pin].name=p.name;
            if(p.last_change)ST[p.pin].lastChange=p.last_change;
            if(p.last_user)ST[p.pin].lastUser=p.last_user;
            renderCard(p.pin);
          }
        });
      }else if(m.type==="SCHED_FIRED"){
        log("Scheduler: GPIO"+m.pin+" -> "+m.action,"lp");
        fetchState();
      }
    }catch(x){}
  };
  ws.onclose=function(){pill("poff","OFFLINE");log("WS desconectado","lw");ws=null;setTimeout(connectWS,4000);};
  ws.onerror=function(){pill("perr","ERROR");};
}
setInterval(function(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:"PING"}));},20000);
setInterval(fetchInfo,15000);
document.addEventListener("keydown",function(e){
  if(e.key==="Enter"&&e.target&&e.target.hasAttribute("data-pin"))e.target.blur();
});




var mqttCfg={host:"broker.emqx.io",port:1883,clientId:"",keepAlive:60,
             user:"",pass:"",tls:false,qos:1,
             topicState:"state",topicAlert:"alert",topicHistory:"history",
             intervalState:10000,intervalPing:25000,enabled:false};

function loadMqttConfig(){
  apiGet("/api/mqtt/config").then(function(d){
    mqttCfg=Object.assign(mqttCfg,d);
    G("mqttHost").value        = d.host        || "broker.emqx.io";
    G("mqttPort").value        = d.port        || 1883;
    G("mqttClientId").value    = d.clientId    || "";
    G("mqttKA").value          = d.keepAlive   || 60;
    G("mqttUser").value        = d.user        || "";
    G("mqttPass").value        = d.pass        || "";
    G("mqttTState").value      = d.topicState  || "state";
    G("mqttTAlert").value      = d.topicAlert  || "alert";
    G("mqttTHistory").value    = d.topicHistory|| "history";
    G("mqttQos").value         = d.qos!=null   ? d.qos  : 1;
    G("mqttIntState").value    = d.intervalState  || 10000;
    G("mqttIntPing").value     = d.intervalPing   || 25000;
    var master=G("mqttMasterSw"), tls=G("mqttTlsSw");
    master.classList.toggle("on",!!d.enabled);
    tls.classList.toggle("on",!!d.tls);
    updateMqttStatusPill(d.connected);
    mqttLogLine("Config cargada","lc");
  }).catch(function(e){mqttLogLine("Error cargando config: "+e.message,"lr");});
}

function saveMqttConfig(){
  var payload={
    host:     G("mqttHost").value.trim(),
    port:     parseInt(G("mqttPort").value)||1883,
    clientId: G("mqttClientId").value.trim(),
    keepAlive:parseInt(G("mqttKA").value)||60,
    user:     G("mqttUser").value.trim(),
    pass:     G("mqttPass").value,
    tls:      G("mqttTlsSw").classList.contains("on"),
    qos:      parseInt(G("mqttQos").value),
)XHTMLX";
const char APP_20[] PROGMEM = R"XHTMLX(
    topicState:   G("mqttTState").value.trim()||"state",
    topicAlert:   G("mqttTAlert").value.trim()||"alert",
    topicHistory: G("mqttTHistory").value.trim()||"history",
    intervalState: parseInt(G("mqttIntState").value)||10000,
    intervalPing:  parseInt(G("mqttIntPing").value)||25000,
    enabled: G("mqttMasterSw").classList.contains("on")
  };
  if(!payload.host){mqttLogLine("Host no puede estar vacio","lr");return;}
  mqttLogLine("Guardando...","lc");
  apiPost("/api/mqtt/config",payload).then(function(d){
    if(d.ok){mqttLogLine("Guardado. Reconectando...","lg");setTimeout(checkMqttStatus,2000);}
    else mqttLogLine("Error: "+(d.error||"desconocido"),"lr");
  }).catch(function(e){mqttLogLine("Error: "+e.message,"lr");});
}

function mqttMasterToggle(){
  var sw=G("mqttMasterSw"), en=!sw.classList.contains("on");
  sw.classList.toggle("on",en);
  apiPost("/api/p2p/enable",{enabled:en}).then(function(d){
    mqttLogLine("MQTT "+(en?"activado":"desactivado"),"lg");
    updateMqttStatusPill(false);
    if(en) setTimeout(checkMqttStatus,2500);
  }).catch(function(e){mqttLogLine("Error: "+e.message,"lr");sw.classList.toggle("on",!en);});
}

function mqttToggleTls(){
  var sw=G("mqttTlsSw"), on=sw.classList.contains("on");
  sw.classList.toggle("on",!on);
  if(!on) G("mqttPort").value=8883; else G("mqttPort").value=1883;
}

function mqttDisconnectNow(){
  apiPost("/api/p2p/enable",{enabled:false}).then(function(d){
    G("mqttMasterSw").classList.remove("on");
    updateMqttStatusPill(false);
    mqttLogLine("Desconectado","lr");
  }).catch(function(e){mqttLogLine("Error: "+e.message,"lr");});
}

function checkMqttStatus(){
  apiGet("/api/mqtt/config").then(function(d){
    updateMqttStatusPill(d.connected);
    if(d.connected) mqttLogLine("Conectado al broker","lg");
  }).catch(function(){});
}

function updateMqttStatusPill(connected){
  var pill=G("mqttConPill"), txt=G("mqttConTxt");
  if(!pill) return;
  if(connected){
    pill.className="pill pon"; txt.textContent="CONECTADO";
  } else {
    pill.className="pill poff"; txt.textContent="DESCONECTADO";
  }
}

function mqttLogLine(msg,cls){
  var box=G("mqttLog"); if(!box) return;
  var d=document.createElement("span");
  d.className="le "+(cls||"lc"); d.textContent=ts()+" "+msg;
  box.appendChild(d); box.scrollTop=box.scrollHeight;
  if(box.children.length>60) box.removeChild(box.children[0]);
}

function toggleEye(id, btn){
  var inp=G(id);
  inp.type=inp.type==="password"?"text":"password";
  btn.style.opacity=inp.type==="text"?"1":"0.4";
}

// ── NETWORK TAB ──────────────────────────────────
var netCfg={dhcp:true,ip:"",mask:"255.255.255.0",gw:"",dns1:"8.8.8.8",dns2:"8.8.4.4"};

function loadNetConfig(){
  apiGet("/api/net/config").then(function(d){
    netCfg=Object.assign(netCfg,d);
    // Estado actual (readonly)
    G("netCurIP").textContent   = d.currentIp   || "---";
    G("netCurMask").textContent = d.currentMask || "---";
    G("netCurGW").textContent   = d.currentGw   || "---";
    G("netCurDNS").textContent  = d.currentDns  || "---";
    // DHCP switch
    var dhcp = d.dhcp!==false;
    G("dhcpSw").classList.toggle("on", dhcp);
    G("dhcpDesc").textContent = dhcp
      ? "IP asignada automaticamente por el router"
      : "IP fija configurada manualmente";
    G("staticIpBlock").style.display = dhcp ? "none" : "block";
    // campos IP fija
    if(d.ip)    G("netIP").value   = d.ip;
    if(d.mask)  G("netMask").value = d.mask;
    if(d.gw)    G("netGW").value   = d.gw;
    if(d.dns1)  G("netDNS1").value = d.dns1;
    if(d.dns2)  G("netDNS2").value = d.dns2;
  }).catch(function(e){
    // En modo DEBUG AP no hay WiFi, mostrar valores del AP
    G("netCurIP").textContent = "192.168.4.1 (AP)";
    G("netCurMask").textContent = "255.255.255.0";
    G("netCurGW").textContent   = "---";
    G("netCurDNS").textContent  = "---";
  });
}

function dhcpToggle(){
  var sw=G("dhcpSw"), on=sw.classList.contains("on");
  sw.classList.toggle("on",!on);
  var nowDhcp=!on;
)XHTMLX";
const char APP_21[] PROGMEM = R"XHTMLX(
  G("dhcpDesc").textContent=nowDhcp
    ?"IP asignada automaticamente por el router"
    :"IP fija configurada manualmente";
  G("staticIpBlock").style.display=nowDhcp?"none":"block";
  hideNetMsg();
}

function validateIp(ip){
  return /^(\d{1,3}\.){3}\d{1,3}$/.test(ip)&&
    ip.split(".").every(function(n){return parseInt(n)<=255;});
}

function showNetMsg(msg,ok){
  var d=G("netValidMsg");
  d.style.display="block";
  d.style.background=ok?"rgba(57,255,20,.08)":"rgba(255,68,102,.1)";
  d.style.border="1px solid "+(ok?"rgba(57,255,20,.3)":"rgba(255,68,102,.3)");
  d.style.color=ok?"var(--on)":"var(--err)";
  d.textContent=msg;
}
function hideNetMsg(){var d=G("netValidMsg");if(d)d.style.display="none";}

function saveNetConfig(){
  var dhcp=G("dhcpSw").classList.contains("on");
  var payload={dhcp:dhcp};
  if(!dhcp){
    var ip=G("netIP").value.trim(), mask=G("netMask").value.trim(),
        gw=G("netGW").value.trim(),  dns1=G("netDNS1").value.trim(),
        dns2=G("netDNS2").value.trim();
    if(!validateIp(ip)){showNetMsg("IP invalida: "+ip,false);return;}
    if(!validateIp(mask)){showNetMsg("Mascara invalida: "+mask,false);return;}
    if(!validateIp(gw)){showNetMsg("Gateway invalido: "+gw,false);return;}
    if(dns1&&!validateIp(dns1)){showNetMsg("DNS primario invalido",false);return;}
    payload.ip=ip; payload.mask=mask; payload.gw=gw;
    payload.dns1=dns1||"8.8.8.8"; payload.dns2=dns2||"8.8.4.4";
  }
  apiPost("/api/net/config",payload).then(function(d){
    if(d.ok){
      showNetMsg(dhcp?"DHCP activado. Reinicia para aplicar.":"IP fija guardada. El ESP32 se reconectara...",true);
      setTimeout(loadNetConfig,2500);
    } else {
      showNetMsg("Error: "+(d.error||"desconocido"),false);
    }
  }).catch(function(e){showNetMsg("Error: "+e.message,false);});
}

// ── WIFI TAB ─────────────────────────────────────
var wifiSelNetwork = null;   // red actualmente seleccionada
var wifiRedirectTimer = null;
var wifiNewIPAddr = "";

// Carga estado actual + dispara escaneo automático
function loadWifiStatus(){
  apiGet("/api/wifi/status").then(function(d){
    G("wifiCurSSID").textContent = d.ssid || "Sin conexion";
    G("wifiCurCH").textContent   = d.ch!=null ? "CH "+d.ch : "---";
    G("wifiCurMAC").textContent  = d.mac  || "---";
    // RSSI con color
    var rssi=d.rssi||0;
    G("wifiCurRSSI").textContent = rssi===0?"---":rssi+"dBm";
    G("wifiCurRSSI").style.color = rssi<-75?"var(--warn)":rssi<-55?"#c8d8e8":"var(--on)";
    // IP actual — si loadNetConfig ya la llenó no sobreescribir con "---"
    if(d.ip) G("netCurIP").textContent = d.ip;
    // Pill
    var pill=G("wifiStatPill"), txt=G("wifiStatTxt");
    if(d.connected){
      pill.className="pill pon"; txt.textContent="CONECTADO";
    } else {
      pill.className="pill poff"; txt.textContent="DESCONECTADO";
    }
  }).catch(function(){
    G("wifiCurSSID").textContent="ESP32-GPIO-PRO (AP)";
    G("netCurIP").textContent="192.168.4.1";
    G("wifiCurCH").textContent="CH 6";
    G("wifiCurRSSI").textContent="---";
    G("wifiCurMAC").textContent="---";
    G("wifiStatPill").className="pill pwarn";
    G("wifiStatTxt").textContent="MODO AP";
  });
}

// Escanear redes
function wifiScan(){
  var btn=G("wifiScanBtn");
  btn.disabled=true; btn.textContent="Escaneando...";
  G("wifiScanning").style.display="block";
  G("wifiNetList").style.display="none";
  wifiCancelSel();

  apiPost("/api/wifi/scan",{}).then(function(d){
    G("wifiScanning").style.display="none";
    G("wifiNetList").style.display="flex";
    btn.disabled=false; btn.innerHTML="&#x21BB; ESCANEAR";
    if(!d.networks||d.networks.length===0){
      G("wifiNetList").innerHTML='<div style="font-family:var(--mo);font-size:0.614rem;color:rgba(200,216,232,.2);text-align:center;padding:16px 0">No se encontraron redes</div>';
      return;
    }
    renderNetList(d.networks, d.current);
  }).catch(function(e){
    G("wifiScanning").style.display="none";
    G("wifiNetList").style.display="flex";
    btn.disabled=false; btn.innerHTML="&#x21BB; ESCANEAR";
)XHTMLX";
const char APP_22[] PROGMEM = R"XHTMLX(
    G("wifiNetList").innerHTML='<div style="font-family:var(--mo);font-size:0.614rem;color:var(--err);text-align:center;padding:16px 0">Error: '+e.message+'</div>';
  });
}

// Construir barras de señal según RSSI
function rssiBars(rssi){
  // 4 barras: >-55 todas, >-65 3, >-75 2, resto 1
  var lit = rssi>=-55?4 : rssi>=-65?3 : rssi>=-75?2 : 1;
  var html='<div class="rssi-bars">';
  for(var i=1;i<=4;i++) html+='<div class="rssi-bar'+(i<=lit?' lit':'')+'"></div>';
  return html+'</div>';
}

function renderNetList(networks, currentSSID){
  var html="";
  networks.sort(function(a,b){return (b.rssi||0)-(a.rssi||0);});
  networks.forEach(function(n){
    var isCurrent=n.ssid===currentSSID;
    var isOpen=!n.secured;
    var secLabel=isOpen?"<span class=\"wifi-badge open\">ABIERTA</span>":"<span class=\"wifi-badge secured\">&#128274;</span>";
    var curLabel=isCurrent?"<span class=\"wifi-badge current\">CONECTADA</span>":"";
    var sel=isCurrent?" selected":"";
    var cur=isCurrent?" current":"";
    html+="<div class=\"wifi-net"+sel+"\" onclick=\"wifiSelectNet(this)\" data-ssid=\""+escH(n.ssid)+"\" data-secured=\""+n.secured+"\">"+
      "<div class=\"wifi-net-ssid"+cur+"\">"+escH(n.ssid)+"</div>"+
      "<div class=\"wifi-net-info\">"+curLabel+secLabel+rssiBars(n.rssi||0)+
      "<span style=\"font-family:var(--mo);font-size:0.519rem;color:rgba(200,216,232,.3);min-width:38px;text-align:right\">"+(n.rssi||0)+"dBm</span>"+
      "</div></div>";
  });
  G("wifiNetList").innerHTML=html;
}


function escH(s){return s.split("&").join("&amp;").split(String.fromCharCode(34)).join("&quot;").split("<").join("&lt;");}

// Seleccionar red de la lista
function wifiSelectNet(el, ssid, secured){
  var ssid=el.dataset.ssid; var secured=parseInt(el.dataset.secured);
  // Deselect previo
  document.querySelectorAll(".wifi-net").forEach(function(e){e.classList.remove("selected");});
  el.classList.add("selected");
  wifiSelNetwork={ssid:ssid, secured:secured};
  G("wifiSelSSID").textContent = ssid;
  G("wifiSelSec").textContent  = secured ? "WPA/WPA2 (protegida)" : "Red abierta";
  G("wifiPassRow").style.display = secured ? "flex" : "none";
  G("wifiPass").value = "";
  G("wifiConnMsg").style.display = "none";
  G("wifiProgBar").style.display = "none";
  G("wifiConnectCard").style.display = "block";
  G("wifiNewIpCard").style.display = "none";
  G("wifiConnBtn").disabled = false;
  G("wifiConnBtn").textContent = "GUARDAR Y CONECTAR";
  G("wifiConnectCard").scrollIntoView({behavior:"smooth",block:"nearest"});
}

function wifiCancelSel(){
  wifiSelNetwork=null;
  G("wifiConnectCard").style.display="none";
  G("wifiNewIpCard").style.display="none";
  document.querySelectorAll(".wifi-net").forEach(function(e){e.classList.remove("selected");});
}

// Enviar credenciales y reiniciar
function wifiConnect(){
  if(!wifiSelNetwork) return;
  var pass = G("wifiPass").value;
  if(wifiSelNetwork.secured && pass.length<8){
    wifiShowMsg("La contrasena debe tener al menos 8 caracteres",false); return;
  }
  G("wifiConnBtn").disabled=true;
  G("wifiConnBtn").textContent="Guardando...";
  wifiShowProgress("Guardando credenciales...", 20);

  apiPost("/api/wifi/connect",{ssid:wifiSelNetwork.ssid, pass:pass}).then(function(d){
    if(d.ok){
      wifiShowProgress("Credenciales guardadas. Reiniciando ESP32...", 60);
      wifiNewIPAddr = d.ip || "";
      setTimeout(function(){
        wifiShowProgress("Esperando reconexion...", 80);
        // Tras el reinicio el ESP32 tardará ~5s en conectar
        setTimeout(function(){ wifiShowNewIP(d.ip); }, 6000);
      }, 1500);
    } else {
      wifiShowMsg("Error: "+(d.error||"credenciales no guardadas"),false);
      G("wifiConnBtn").disabled=false;
      G("wifiConnBtn").textContent="GUARDAR Y CONECTAR";
      G("wifiProgBar").style.display="none";
    }
  }).catch(function(e){
    wifiShowMsg("Error de comunicacion: "+e.message, false);
    G("wifiConnBtn").disabled=false;
    G("wifiConnBtn").textContent="GUARDAR Y CONECTAR";
)XHTMLX";
const char APP_23[] PROGMEM = R"XHTMLX(
    G("wifiProgBar").style.display="none";
  });
}

function wifiShowMsg(msg, ok){
  var d=G("wifiConnMsg");
  d.style.display="block";
  d.style.background=ok?"rgba(57,255,20,.08)":"rgba(255,68,102,.1)";
  d.style.border="1px solid "+(ok?"rgba(57,255,20,.3)":"rgba(255,68,102,.3)");
  d.style.color=ok?"var(--on)":"var(--err)";
  d.textContent=msg;
}

function wifiShowProgress(txt, pct){
  G("wifiProgBar").style.display="block";
  G("wifiProgTxt").textContent=txt;
  G("wifiProgFill").style.width=pct+"%";
}

function wifiShowNewIP(ip){
  G("wifiProgFill").style.width="100%";
  G("wifiProgTxt").textContent="Listo.";
  G("wifiConnectCard").style.display="none";
  G("wifiNewIpCard").style.display="block";
  var addr = ip ? "http://"+ip : "http://[nueva-IP]";
  G("wifiNewIP").textContent = addr;
  wifiNewIPAddr = addr;
  G("wifiRedirectBtn").style.display = ip ? "inline-block" : "none";
  // Cuenta regresiva 15s para redirect automático
  if(ip){
    var secs=15;
    G("wifiRedirectCount").textContent="Redirigiendo en "+secs+"s...";
    if(wifiRedirectTimer) clearInterval(wifiRedirectTimer);
    wifiRedirectTimer=setInterval(function(){
      secs--;
      if(secs<=0){clearInterval(wifiRedirectTimer);wifiRedirect();}
      else G("wifiRedirectCount").textContent="Redirigiendo en "+secs+"s...";
    },1000);
  }
  G("wifiNewIpCard").scrollIntoView({behavior:"smooth",block:"nearest"});
}

function wifiRedirect(){
  if(wifiRedirectTimer) clearInterval(wifiRedirectTimer);
  if(wifiNewIPAddr) window.location.href=wifiNewIPAddr;
}

// DEBUG MODE: arranque directo sin login

window.onload=function(){initTheme();buildGrid();connectWS();setTimeout(fetchInfo,800);setTimeout(fetchState,1400);setTimeout(loadSched,1200);setTimeout(loadHistory,1800);};

</script>
</body>
</html>

)XHTMLX";

// Portal
const char PROV_1[] PROGMEM = R"XHTMLX(
<!DOCTYPE html>
<html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPIO PRO Setup</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@700;900&family=Rajdhani:wght@400;600;700&display=swap');
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#03050a;--a:#00ffe0;--a2:#0080ff;--on:#39ff14;--err:#ff4466;--warn:#ffd166;}
body{background:var(--bg);color:#c8d8e8;font-family:'Rajdhani',sans-serif;
  min-height:100vh;display:flex;flex-direction:column;align-items:center;
  justify-content:center;padding:20px 16px;}
body::before{content:'';position:fixed;inset:0;pointer-events:none;
  background:repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(0,255,224,.015) 40px),
    repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(0,255,224,.015) 40px);}
.logo{text-align:center;margin-bottom:20px;position:relative;z-index:1;}
.logo h1{font-family:'Orbitron',sans-serif;font-weight:900;font-size:1.3rem;letter-spacing:.1em;
  background:linear-gradient(90deg,var(--a),var(--a2));-webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.logo p{font-family:'Share Tech Mono',monospace;font-size:.52rem;color:rgba(200,216,232,.22);letter-spacing:.2em;margin-top:3px;}
.card{position:relative;z-index:1;width:100%;max-width:400px;
  background:linear-gradient(145deg,#07101f,#0b1628);border:1px solid rgba(0,255,224,.18);
  border-radius:10px;padding:22px 20px 26px;box-shadow:0 20px 40px rgba(0,0,0,.5);}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;border-radius:10px 10px 0 0;
  background:linear-gradient(90deg,var(--a),var(--a2));box-shadow:0 0 16px var(--a);}
.stit{font-family:'Orbitron',sans-serif;font-size:.6rem;letter-spacing:.16em;
  color:var(--a);text-transform:uppercase;margin-bottom:14px;}
.sep{border:none;border-top:1px solid rgba(255,255,255,.05);margin:14px 0;}
.lbl{font-family:'Share Tech Mono',monospace;font-size:.48rem;letter-spacing:.14em;
  color:rgba(200,216,232,.28);text-transform:uppercase;margin-bottom:4px;}
.nets{display:flex;flex-direction:column;gap:5px;margin-bottom:10px;max-height:150px;overflow-y:auto;}
.ni{background:rgba(0,0,0,.4);border:1px solid rgba(255,255,255,.07);border-radius:4px;
  padding:8px 11px;cursor:pointer;display:flex;align-items:center;justify-content:space-between;
  font-family:'Share Tech Mono',monospace;font-size:.68rem;transition:all .18s;}
.ni:hover,.ni.sel{border-color:rgba(0,255,224,.35);color:var(--a);}
.inp{background:rgba(0,0,0,.5);border:1px solid rgba(255,255,255,.08);color:#c8d8e8;
  font-family:'Share Tech Mono',monospace;font-size:.75rem;padding:9px 12px;
  border-radius:4px;outline:none;width:100%;margin-bottom:10px;transition:border-color .2s;}
.inp:focus{border-color:rgba(0,255,224,.4);}
.pw-w{position:relative;margin-bottom:10px;}
.pw-w .inp{margin-bottom:0;padding-right:40px;}
.eye{position:absolute;right:10px;top:50%;transform:translateY(-50%);
  background:none;border:none;color:rgba(200,216,232,.3);cursor:pointer;font-size:.9rem;}
.btn{font-family:'Rajdhani',sans-serif;font-weight:700;font-size:.7rem;letter-spacing:.16em;
  text-transform:uppercase;padding:10px 16px;border:none;border-radius:4px;
  cursor:pointer;transition:all .18s;width:100%;margin-bottom:7px;}
.bp{background:rgba(0,255,224,.1);color:var(--a);border:1px solid rgba(0,255,224,.3);}
.bs{background:rgba(0,128,255,.1);color:var(--a2);border:1px solid rgba(0,128,255,.28);}
.br{background:rgba(255,68,102,.07);color:var(--err);border:1px solid rgba(255,68,102,.2);font-size:.6rem;}
.hint{font-family:'Share Tech Mono',monospace;font-size:.5rem;color:rgba(200,216,232,.2);margin-top:4px;}
.sb{background:rgba(0,0,0,.4);border:1px solid rgba(255,255,255,.07);border-radius:4px;
  padding:10px 13px;font-family:'Share Tech Mono',monospace;font-size:.62rem;
  line-height:1.8;margin-bottom:10px;}
.ok{color:var(--on);}.wn{color:var(--warn);}
)XHTMLX";
const char PROV_2[] PROGMEM = R"XHTMLX(
.inst{font-family:'Share Tech Mono',monospace;font-size:.54rem;color:rgba(200,216,232,.28);line-height:1.9;margin-top:8px;}
.inst .il{display:flex;gap:8px;margin-bottom:3px;}
.inst .in{color:var(--a);}
</style></head><body>
<div class="logo"><h1>GPIO PRO</h1><p>CONFIGURACION INICIAL</p></div>
<div class="card">
  <div id="s1">
    <div class="stit">1. Red WiFi</div>
    <button class="btn bs" id="btnScan" onclick="scan()">ESCANEAR REDES</button>
    <div class="nets" id="nets" style="margin-top:8px">
      <div style="font-family:'Share Tech Mono',monospace;font-size:.56rem;color:rgba(200,216,232,.18);padding:6px">Presiona Escanear...</div>
    </div>
    <div class="lbl">O SSID MANUAL</div>
    <input class="inp" id="ssid" type="text" placeholder="Nombre de la red WiFi">
    <div class="lbl">Contrasena WiFi</div>
    <div class="pw-w">
      <input class="inp" id="wpass" type="password" placeholder="Contrasena">
      <button class="eye" onclick="tgl('wpass')">&#128065;</button>
    </div>
    <div class="sep"></div>
    <div class="stit">2. Usuario Admin</div>
    <div class="lbl">Nombre de usuario</div>
    <input class="inp" id="admu" type="text" placeholder="admin" value="admin">
    <div class="lbl">Contrasena de acceso</div>
    <div class="pw-w">
      <input class="inp" id="admp" type="password" placeholder="minimo 4 caracteres">
      <button class="eye" onclick="tgl('admp')">&#128065;</button>
    </div>
    <div class="hint">Este usuario tendra rol ADMIN con acceso total</div>
    <div class="sep"></div>
    <div class="stit">3. Zona horaria NTP</div>
    <div class="lbl">Offset UTC (horas)</div>
    <input class="inp" id="tz" type="number" value="-5" min="-12" max="14" placeholder="-5 para Colombia">
    <button class="btn bp" onclick="save()" style="margin-top:6px">GUARDAR Y CONECTAR</button>
    <hr style="border:none;border-top:1px solid rgba(255,255,255,.04);margin:8px 0">
    <button class="btn br" onclick="rst()">BORRAR CONFIG GUARDADA</button>
  </div>
  <div id="s2" style="display:none">
    <div class="stit">Configuracion guardada</div>
    <div class="sb" id="sb"><span class="wn">Guardando y reiniciando...</span></div>
    <div class="inst" id="inst" style="display:none">
      <div class="il"><span class="in">01</span><span>Reconectate a tu WiFi normal</span></div>
      <div class="il"><span class="in">02</span><span>Espera ~15s al ESP32</span></div>
      <div class="il"><span class="in">03</span><span>Abre la IP que muestra el Serial Monitor</span></div>
      <div class="il"><span class="in">04</span><span>Ingresa con el usuario admin que creaste</span></div>
    </div>
  </div>
</div>
<script>
var sel='';
function tgl(id){var i=document.getElementById(id);i.type=i.type==='password'?'text':'password';}
function scan(){
  var btn=document.getElementById('btnScan'),list=document.getElementById('nets');
  btn.disabled=true;btn.textContent='ESCANEANDO...';
  fetch('/scan').then(function(r){return r.json();}).then(function(nets){
    btn.disabled=false;btn.textContent='ESCANEAR REDES';list.innerHTML='';
    if(!nets.length){list.innerHTML='<div style="color:#ff4466;padding:6px;font-family:monospace;font-size:.56rem">Sin redes detectadas</div>';return;}
    nets.forEach(function(n){
      var d=document.createElement('div');d.className='ni';
      var bars=n.rssi>-60?'||||':n.rssi>-75?'|||':n.rssi>-85?'||':'|';
      d.innerHTML='<span>'+n.ssid+'</span><span style="font-size:.56rem;opacity:.4">'+bars+(n.encrypted?' L':'')+' </span>';
      d.onclick=function(){document.querySelectorAll('.ni').forEach(function(e){e.classList.remove('sel');});d.classList.add('sel');sel=n.ssid;document.getElementById('ssid').value=n.ssid;};
      list.appendChild(d);
    });
  }).catch(function(){btn.disabled=false;btn.textContent='ESCANEAR REDES';});
}
function save(){
  var ssid=document.getElementById('ssid').value.trim();
  var wpass=document.getElementById('wpass').value;
  var admu=document.getElementById('admu').value.trim();
)XHTMLX";
const char PROV_3[] PROGMEM = R"XHTMLX(
  var admp=document.getElementById('admp').value;
  var tz=parseInt(document.getElementById('tz').value)||0;
  if(!ssid){alert('Ingresa el nombre de la red WiFi');return;}
  if(!admu||admp.length<4){alert('Usuario y contrasena (min 4 chars) son obligatorios');return;}
  if(!sel)sel=ssid;
  document.getElementById('s1').style.display='none';
  document.getElementById('s2').style.display='block';
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(sel)+'&pass='+encodeURIComponent(wpass)
      +'&admu='+encodeURIComponent(admu)+'&admp='+encodeURIComponent(admp)
      +'&tz='+tz})
    .then(function(r){return r.text();})
    .then(function(msg){
      document.getElementById('sb').innerHTML='<span class="ok">OK: '+msg+'</span>';
      document.getElementById('inst').style.display='block';
    }).catch(function(){document.getElementById('sb').innerHTML='<span style="color:#ff4466">Error al guardar</span>';});
}
function rst(){
  if(!confirm('Borrar toda la configuracion guardada?'))return;
  fetch('/reset').then(function(){alert('Config borrada. Reiniciando en modo Setup.');}).catch(function(){});
}
window.onload=function(){setTimeout(scan,400);};
</script></body></html>
)XHTMLX";

void sendAppPage(){
  WiFiClient cl=server.client();
  cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"));
  cl.print(FPSTR(APP_1)); cl.flush();
  cl.print(FPSTR(APP_2)); cl.flush();
  cl.print(FPSTR(APP_3)); cl.flush();
  cl.print(FPSTR(APP_4)); cl.flush();
  cl.print(FPSTR(APP_5)); cl.flush();
  cl.print(FPSTR(APP_6)); cl.flush();
  cl.print(FPSTR(APP_7)); cl.flush();
  cl.print(FPSTR(APP_8)); cl.flush();
  cl.print(FPSTR(APP_9)); cl.flush();
  cl.print(FPSTR(APP_10)); cl.flush();
  cl.print(FPSTR(APP_11)); cl.flush();
  cl.print(FPSTR(APP_12)); cl.flush();
  cl.print(FPSTR(APP_13)); cl.flush();
  cl.print(FPSTR(APP_14)); cl.flush();
  cl.print(FPSTR(APP_15)); cl.flush();
  cl.print(FPSTR(APP_16)); cl.flush();
  cl.print(FPSTR(APP_17)); cl.flush();
  cl.print(FPSTR(APP_18)); cl.flush();
  cl.print(FPSTR(APP_19)); cl.flush();
  cl.print(FPSTR(APP_20)); cl.flush();
  cl.print(FPSTR(APP_21)); cl.flush();
  cl.print(FPSTR(APP_22)); cl.flush();
  cl.print(FPSTR(APP_23)); cl.flush();
}

void sendProvPage(){
  WiFiClient cl=server.client();
  cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"));
  cl.print(FPSTR(PROV_1)); cl.flush();
  cl.print(FPSTR(PROV_2)); cl.flush();
  cl.print(FPSTR(PROV_3)); cl.flush();
}















// ══════════════════════════════════════════════════
//  UTILIDADES
// ══════════════════════════════════════════════════
int pinIndex(uint8_t pin){
  for(int i=0;i<NUM_PINS;i++) if(PIN_TABLE[i].pin==pin) return i;
  return -1;
}
const char* modeName(PinMode_t m){
  switch(m){
    case PM_OUTPUT: return "output";
    case PM_INPUT:  return "input";
    case PM_PWM:    return "pwm";
    case PM_ADC:    return "adc";
    default:        return "none";
  }
}
PinMode_t modeFromStr(const char* s){
  if(!s) return PM_NONE;
  if(strcmp(s,"output")==0) return PM_OUTPUT;
  if(strcmp(s,"input")==0)  return PM_INPUT;
  if(strcmp(s,"pwm")==0)    return PM_PWM;
  if(strcmp(s,"adc")==0)    return PM_ADC;
  return PM_NONE;
}
String hashPass(const String& pass){
  uint32_t h=2166136261UL;
  for(size_t i=0;i<pass.length();i++){h^=(uint8_t)pass[i];h*=16777619UL;}
  char buf[12]; sprintf(buf,"%08X",h); return String(buf);
}
String genToken(){
  const char* ch="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  String t=""; for(int i=0;i<TOKEN_LEN;i++) t+=ch[esp_random()%62];
  return t;
}
String nowStr(){
  if(!ntpSynced) return String(millis()/1000)+"s";
  time_t now=time(nullptr)+(tzOffset*3600);
  struct tm* t=gmtime(&now);
  char buf[20]; strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",t);
  return String(buf);
}
void addCORS(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,PUT,DELETE,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","Content-Type,X-Token");
}
void sendJSON(int code,const String& b){addCORS();server.send(code,"application/json",b);}
void sendError(int code,const char* m){
  sendJSON(code,String("{\"ok\":false,\"error\":\"")+String(m)+"\"}");
}

// ── getAuth: en DEBUG_MODE acepta el token fijo ──
Session* findSession(const String& token){
  for(int i=0;i<3;i++)
    if(sessions[i].active && String(sessions[i].token)==token
       && millis()<sessions[i].expires) return &sessions[i];
  return nullptr;
}
Session* getAuth(){
  String tok=server.header("X-Token");
  if(!tok.length()) return nullptr;
#if DEBUG_MODE
  // Token fijo: siempre retorna sesion debug admin
  if(tok==String(DEBUG_TOKEN)) return &sessions[0];
#endif
  return findSession(tok);
}
bool requireAuth(bool adminOnly){
  Session* s=getAuth();
  if(!s){sendError(401,"No autorizado");return false;}
  if(adminOnly && strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return false;}
  return true;
}

int findUser(const char* name){
  for(int i=0;i<MAX_USERS;i++)
    if(users[i].active && strcmp(users[i].name,name)==0) return i;
  return -1;
}
void addHist(uint8_t pin,const char* mode,int value,const char* usr,const char* name){
  int i=histHead;
  strncpy(hist[i].time,nowStr().c_str(),19); hist[i].time[19]=0;
  hist[i].pin=pin;
  strncpy(hist[i].mode,mode,7); hist[i].mode[7]=0;
  hist[i].value=value;
  strncpy(hist[i].name,name,23); hist[i].name[23]=0;
  strncpy(hist[i].user,usr,15);  hist[i].user[15]=0;
  histHead=(histHead+1)%MAX_HIST;
  if(histCount<MAX_HIST) histCount++;
  int idx=pinIndex(pin);
  if(idx>=0){
    strncpy(pinState[idx].lastChange,nowStr().c_str(),19);
    strncpy(pinState[idx].lastUser,usr,15);
  }
}
void broadcastPin(int idx,const char* usr){
  DynamicJsonDocument doc(256);
  doc["type"]="PIN_CHANGE"; doc["pin"]=PIN_TABLE[idx].pin;
  doc["mode"]=modeName(pinState[idx].mode); doc["value"]=pinState[idx].value;
  if(pinState[idx].mode==PM_PWM) doc["freq"]=pinState[idx].freq;
  doc["last_change"]=pinState[idx].lastChange; doc["last_user"]=pinState[idx].lastUser;
  String out; serializeJson(doc,out); webSocket.broadcastTXT(out);
}
bool applyMode(int idx,PinMode_t nm){
  uint8_t pin=PIN_TABLE[idx].pin;
  if(pinState[idx].mode==PM_PWM) ledcDetach(pin);
  pinState[idx].mode=nm; pinState[idx].value=0;
  switch(nm){
    case PM_OUTPUT:
      if(!PIN_TABLE[idx].canOut) return false;
      pinMode(pin,OUTPUT); digitalWrite(pin,LOW); break;
    case PM_INPUT:
      if(!PIN_TABLE[idx].canIn) return false;
      pinMode(pin,INPUT); break;
    case PM_PWM:
      if(!PIN_TABLE[idx].canPWM) return false;
      pinState[idx].freq=PWM_FREQ_DEF;
      ledcAttach(pin,PWM_FREQ_DEF,PWM_RES);
      ledcWrite(pin,0); break;
    case PM_ADC:
      if(!PIN_TABLE[idx].canADC) return false;
      pinMode(pin,INPUT); analogSetPinAttenuation(pin,ADC_11db); break;
    default: pinMode(pin,INPUT); break;
  }
  saveGPIOState();
  return true;
}
String buildStateJSON(){
  DynamicJsonDocument doc(4096); doc["ok"]=true;
  JsonArray pins=doc.createNestedArray("pins");
  for(int i=0;i<NUM_PINS;i++){
    JsonObject p=pins.createNestedObject();
    p["pin"]=PIN_TABLE[i].pin; p["mode"]=modeName(pinState[i].mode);
    p["value"]=pinState[i].value; p["name"]=pinState[i].name;
    p["last_change"]=pinState[i].lastChange; p["last_user"]=pinState[i].lastUser;
    if(pinState[i].mode==PM_PWM) p["freq"]=pinState[i].freq;
  }
  String out; serializeJson(doc,out); return out;
}
void execSched(SchedEvent& ev,const char* usr){
  int idx=pinIndex(ev.pin); if(idx<0) return;
  if(strcmp(ev.action,"on")==0){
    if(pinState[idx].mode!=PM_OUTPUT) applyMode(idx,PM_OUTPUT);
    pinState[idx].value=1; digitalWrite(ev.pin,HIGH);
    saveGPIOState();
    addHist(ev.pin,"output",1,usr,pinState[idx].name); broadcastPin(idx,usr);
  }else if(strcmp(ev.action,"off")==0){
    if(pinState[idx].mode!=PM_OUTPUT) applyMode(idx,PM_OUTPUT);
    pinState[idx].value=0; digitalWrite(ev.pin,LOW);
    saveGPIOState();
    addHist(ev.pin,"output",0,usr,pinState[idx].name); broadcastPin(idx,usr);
  }else if(strcmp(ev.action,"pwm")==0){
    if(pinState[idx].mode!=PM_PWM) applyMode(idx,PM_PWM);
    pinState[idx].value=ev.pwm_val; ledcWrite(PIN_TABLE[idx].pin,ev.pwm_val);
    saveGPIOState();
    addHist(ev.pin,"pwm",ev.pwm_val,usr,pinState[idx].name); broadcastPin(idx,usr);
  }else if(strcmp(ev.action,"toggle")==0){
    if(pinState[idx].mode!=PM_OUTPUT) applyMode(idx,PM_OUTPUT);
    int ns=pinState[idx].value?0:1; pinState[idx].value=ns;
    digitalWrite(ev.pin,ns?HIGH:LOW);
    addHist(ev.pin,"output",ns,usr,pinState[idx].name); broadcastPin(idx,usr);
  }
  DynamicJsonDocument doc(128);
  doc["type"]="SCHED_FIRED"; doc["pin"]=ev.pin; doc["action"]=ev.action;
  String out; serializeJson(doc,out); webSocket.broadcastTXT(out);
  Serial.printf("[SCHED] GPIO%d -> %s\n",ev.pin,ev.action);
}


// ══════════════════════════════════════════════════
//  P2P MQTT MODULE
// ══════════════════════════════════════════════════
String getMqttSerial(){
  uint64_t chipid=ESP.getEfuseMac();
  char buf[13];
  sprintf(buf,"%04X%08X",(uint16_t)(chipid>>32),(uint32_t)(chipid&0xFFFFFFFF));
  return String(buf);
}
String mqttT(const String& suffix){ return "gpio/"+deviceSerial+"/"+suffix; }

void mqttPublishState(){
  if(!mqttEnabled||!mqttPubSub.connected()) return;
  if(!mqttTabs.state) return;
  DynamicJsonDocument doc(4096);
  doc["serial"]=deviceSerial;
  doc["uptime"]=(millis()-bootTime)/1000;
  doc["ntp"]=ntpSynced?nowStr():"";
  JsonObject tabsJ=doc.createNestedObject("tabs");
  tabsJ["state"]=mqttTabs.state; tabsJ["control"]=mqttTabs.control;
  tabsJ["history"]=mqttTabs.history; tabsJ["alerts"]=mqttTabs.alerts;
  JsonArray pins=doc.createNestedArray("pins");
  for(int i=0;i<NUM_PINS;i++){
    JsonObject p=pins.createNestedObject();
    p["pin"]=PIN_TABLE[i].pin; p["mode"]=modeName(pinState[i].mode);
    p["value"]=pinState[i].value; p["name"]=pinState[i].name;
    if(pinState[i].mode==PM_PWM) p["freq"]=pinState[i].freq;
  }
  String payload; serializeJson(doc,payload);
  mqttPubSub.publish(mqttT(mqttCfg.topicState).c_str(),payload.c_str(),true);
}

void mqttPublishHistory(){
  if(!mqttEnabled||!mqttPubSub.connected()) return;
  if(!mqttTabs.history) return;
  DynamicJsonDocument doc(4096);
  JsonArray evs=doc.createNestedArray("events");
  int count=min((int)histCount,20);
  for(int i=0;i<count;i++){
    int idx=(histHead-count+i+MAX_HIST)%MAX_HIST;
    JsonObject e=evs.createNestedObject();
    e["time"]=hist[idx].time; e["pin"]=hist[idx].pin;
    e["mode"]=hist[idx].mode; e["value"]=hist[idx].value;
    e["name"]=hist[idx].name; e["user"]=hist[idx].user;
  }
  String payload; serializeJson(doc,payload);
  mqttPubSub.publish(mqttT(mqttCfg.topicHistory).c_str(),payload.c_str(),false);
}

void mqttPublishAlert(uint8_t pin,const char* msg){
  if(!mqttEnabled||!mqttPubSub.connected()) return;
  if(!mqttTabs.alerts) return;
  DynamicJsonDocument doc(256);
  doc["pin"]=pin; doc["msg"]=msg; doc["time"]=nowStr();
  String payload; serializeJson(doc,payload);
  mqttPubSub.publish(mqttT(mqttCfg.topicAlert).c_str(),payload.c_str(),false);
}

void mqttOnMessage(char* topic,byte* payload,unsigned int length){
  String topicStr=String(topic);
  String body="";
  for(unsigned int i=0;i<length;i++) body+=(char)payload[i];
  Serial.printf("[MQTT] <- %s : %s\n",topic,body.c_str());
  DynamicJsonDocument doc(256);
  if(deserializeJson(doc,body)) return;
  // Extraer pin y accion del topic: gpio/{serial}/pin/{N}/{action}
  int p1=topicStr.lastIndexOf('/');
  int p2=topicStr.lastIndexOf('/',p1-1);
  if(p1<0||p2<0) return;
  String action=topicStr.substring(p1+1);
  uint8_t pin=(uint8_t)topicStr.substring(p2+1,p1).toInt();
  int idx=pinIndex(pin); if(idx<0) return;
  if(action=="set"&&mqttTabs.control){
    if(doc.containsKey("state")&&pinState[idx].mode==PM_OUTPUT){
      int st=doc["state"]; pinState[idx].value=st;
      digitalWrite(pin,st?HIGH:LOW);
      addHist(pin,"output",st,"mqtt",pinState[idx].name);
      broadcastPin(idx,"mqtt"); mqttPublishState();
    }else if(doc.containsKey("duty")&&pinState[idx].mode==PM_PWM){
      int duty=(int)doc["duty"]; duty=constrain(duty,0,255);
      pinState[idx].value=duty; ledcWrite(pin,duty);
      addHist(pin,"pwm",duty,"mqtt",pinState[idx].name);
      broadcastPin(idx,"mqtt"); mqttPublishState();
    }
  }else if(action=="mode"&&mqttTabs.control){
    const char* mstr=doc["mode"];
    if(mstr){
      PinMode_t nm=modeFromStr(mstr);
      if(applyMode(idx,nm)){
        addHist(pin,modeName(nm),0,"mqtt",pinState[idx].name);
        broadcastPin(idx,"mqtt"); mqttPublishState();
      }
    }
  }
}

bool mqttConnect(){
  if(!mqttEnabled) return false;
  if(mqttPubSub.connected()) return true;
  if(WiFi.status()!=WL_CONNECTED) return false;
  Serial.printf("[MQTT] Conectando a %s:%d...\n",mqttCfg.host,mqttCfg.port);
  mqttPubSub.setServer(mqttCfg.host, mqttCfg.port);
  mqttPubSub.setKeepAlive(mqttCfg.keepAlive);
  mqttPubSub.setCallback(mqttOnMessage);
  mqttPubSub.setBufferSize(4096);
  // ClientId: usar config o generar desde serial
  String cid = strlen(mqttCfg.clientIdCfg)>0
    ? String(mqttCfg.clientIdCfg)
    : "esp32-gpio-"+deviceSerial;
  mqttClientId = cid;
  String will="{\"online\":false,\"serial\":\""+deviceSerial+"\"}";
  const char* usr = strlen(mqttCfg.user)>0 ? mqttCfg.user : nullptr;
  const char* pwd = strlen(mqttCfg.pass)>0 ? mqttCfg.pass : nullptr;
  bool ok=mqttPubSub.connect(
    cid.c_str(), usr, pwd,
    mqttT("ping").c_str(), 0, true, will.c_str()
  );
  if(ok){
    mqttConnected=true;
    Serial.println(F("[MQTT] Conectado OK"));
    mqttPubSub.subscribe(mqttT("pin/+/set").c_str(),  mqttCfg.qos);
    mqttPubSub.subscribe(mqttT("pin/+/mode").c_str(), mqttCfg.qos);
    String online="{\"online\":true,\"serial\":\""+deviceSerial+"\",\"ip\":\""+deviceIP+"\"}";
    mqttPubSub.publish(mqttT("ping").c_str(),online.c_str(),true);
    mqttPublishState();
  }else{
    mqttConnected=false;
    Serial.printf("[MQTT] Fallo rc=%d\n",mqttPubSub.state());
  }
  return ok;
}

void mqttLoop(){
  if(!mqttEnabled) return;
  bool wasConn=mqttConnected;
  mqttConnected=mqttPubSub.connected();
  if(!mqttConnected){
    if(wasConn) Serial.println(F("[MQTT] Desconexion detectada"));
    static unsigned long lastRetry=0;
    if(millis()-lastRetry>5000){lastRetry=millis();mqttConnect();}
    return;
  }
  mqttPubSub.loop();
  if(millis()-mqttLastPing>(unsigned long)mqttCfg.intervalPing){
    mqttLastPing=millis();
    String msg="{\"online\":true,\"uptime\":"+String((millis()-bootTime)/1000)+"}";
    mqttPubSub.publish(mqttT("ping").c_str(),msg.c_str(),true);
  }
  if(millis()-mqttLastState>(unsigned long)mqttCfg.intervalState){
    mqttLastState=millis(); mqttPublishState();
  }
}

void mqttInit(){
  deviceSerial=getMqttSerial();
  loadMqttCfgNVS();
  loadNetCfgNVS();
  Serial.println("[MQTT] Serial: "+deviceSerial);
}

void mqttSetEnabled(bool en){
  mqttEnabled=en;
  if(!en&&mqttPubSub.connected()){
    mqttPubSub.disconnect(); mqttConnected=false;
    Serial.println(F("[MQTT] Desconectado"));
  } else if(en) mqttConnect();
}

// ── NVS MQTT ─────────────────────────────────────
void saveMqttCfgNVS(){
#if !DEBUG_MODE
  prefs.begin("mqttcfg",false);
  prefs.putString("host",   mqttCfg.host);
  prefs.putInt   ("port",   mqttCfg.port);
  prefs.putString("cid",    mqttCfg.clientIdCfg);
  prefs.putInt   ("ka",     mqttCfg.keepAlive);
  prefs.putString("user",   mqttCfg.user);
  prefs.putString("pass",   mqttCfg.pass);
  prefs.putBool  ("tls",    mqttCfg.tls);
  prefs.putInt   ("qos",    mqttCfg.qos);
  prefs.putString("tst",    mqttCfg.topicState);
  prefs.putString("tal",    mqttCfg.topicAlert);
  prefs.putString("thi",    mqttCfg.topicHistory);
  prefs.putUInt  ("ints",   mqttCfg.intervalState);
  prefs.putUInt  ("intp",   mqttCfg.intervalPing);
  prefs.putBool  ("en",     mqttEnabled);
  prefs.end();
#endif
}
void loadMqttCfgNVS(){
#if !DEBUG_MODE
  prefs.begin("mqttcfg",true);
  strlcpy(mqttCfg.host,       prefs.getString("host","broker.emqx.io").c_str(),64);
  mqttCfg.port              = prefs.getInt   ("port",1883);
  strlcpy(mqttCfg.clientIdCfg,prefs.getString("cid","").c_str(),32);
  mqttCfg.keepAlive         = prefs.getInt   ("ka",60);
  strlcpy(mqttCfg.user,       prefs.getString("user","").c_str(),32);
  strlcpy(mqttCfg.pass,       prefs.getString("pass","").c_str(),32);
  mqttCfg.tls               = prefs.getBool  ("tls",false);
  mqttCfg.qos               = prefs.getInt   ("qos",1);
  strlcpy(mqttCfg.topicState,  prefs.getString("tst","state").c_str(),32);
  strlcpy(mqttCfg.topicAlert,  prefs.getString("tal","alert").c_str(),32);
  strlcpy(mqttCfg.topicHistory,prefs.getString("thi","history").c_str(),32);
  mqttCfg.intervalState     = prefs.getUInt  ("ints",10000);
  mqttCfg.intervalPing      = prefs.getUInt  ("intp",25000);
  mqttEnabled               = prefs.getBool  ("en",false);
  prefs.end();
#endif
}

// ── NVS NET ──────────────────────────────────────
void saveNetCfgNVS(){
#if !DEBUG_MODE
  prefs.begin("netcfg",false);
  prefs.putBool  ("dhcp",  netCfgSt.dhcp);
  prefs.putString("ip",    netCfgSt.ip);
  prefs.putString("mask",  netCfgSt.mask);
  prefs.putString("gw",    netCfgSt.gw);
  prefs.putString("dns1",  netCfgSt.dns1);
  prefs.putString("dns2",  netCfgSt.dns2);
  prefs.end();
#endif
}
void loadNetCfgNVS(){
#if !DEBUG_MODE
  prefs.begin("netcfg",true);
  netCfgSt.dhcp  = prefs.getBool  ("dhcp",true);
  strlcpy(netCfgSt.ip,   prefs.getString("ip",  "").c_str(),16);
  strlcpy(netCfgSt.mask, prefs.getString("mask","255.255.255.0").c_str(),16);
  strlcpy(netCfgSt.gw,   prefs.getString("gw",  "").c_str(),16);
  strlcpy(netCfgSt.dns1, prefs.getString("dns1","8.8.8.8").c_str(),16);
  strlcpy(netCfgSt.dns2, prefs.getString("dns2","8.8.4.4").c_str(),16);
  prefs.end();
#endif
}

void applyStaticIP(){
  if(netCfgSt.dhcp||strlen(netCfgSt.ip)==0) return;
  IPAddress ip,mask,gw,dns1,dns2;
  if(!ip.fromString(netCfgSt.ip)||!mask.fromString(netCfgSt.mask)||!gw.fromString(netCfgSt.gw)) return;
  dns1.fromString(netCfgSt.dns1);
  dns2.fromString(netCfgSt.dns2);
  WiFi.config(ip,gw,mask,dns1,dns2);
  Serial.printf("[NET] IP fija: %s / %s gw %s\n",netCfgSt.ip,netCfgSt.mask,netCfgSt.gw);
}

String netGetConfigJSON(){
  DynamicJsonDocument doc(512);
  doc["dhcp"]=netCfgSt.dhcp;
  doc["ip"]  =netCfgSt.ip;   doc["mask"]=netCfgSt.mask;
  doc["gw"]  =netCfgSt.gw;   doc["dns1"]=netCfgSt.dns1;
  doc["dns2"]=netCfgSt.dns2;
  // Estado actual del WiFi
  if(WiFi.status()==WL_CONNECTED){
    doc["currentIp"]  =WiFi.localIP().toString();
    doc["currentMask"]=WiFi.subnetMask().toString();
    doc["currentGw"]  =WiFi.gatewayIP().toString();
    doc["currentDns"] =WiFi.dnsIP().toString();
  } else {
    doc["currentIp"]  =deviceIP;
    doc["currentMask"]="255.255.255.0";
    doc["currentGw"]  ="---";
    doc["currentDns"] ="---";
  }
  String out; serializeJson(doc,out); return out;
}

String mqttGetFullConfigJSON(){
  DynamicJsonDocument doc(768);
  doc["enabled"]  =mqttEnabled;
  doc["connected"]=mqttConnected;
  doc["serial"]   =deviceSerial;
  doc["host"]     =mqttCfg.host;
  doc["port"]     =mqttCfg.port;
  doc["clientId"] =strlen(mqttCfg.clientIdCfg)>0 ? mqttCfg.clientIdCfg : ("esp32-gpio-"+deviceSerial).c_str();
  doc["keepAlive"]=mqttCfg.keepAlive;
  doc["user"]     =mqttCfg.user;
  doc["pass"]     ="";           // nunca devolver pass al cliente
  doc["tls"]      =mqttCfg.tls;
  doc["qos"]      =mqttCfg.qos;
  doc["topicState"]   =mqttCfg.topicState;
  doc["topicAlert"]   =mqttCfg.topicAlert;
  doc["topicHistory"] =mqttCfg.topicHistory;
  doc["intervalState"]=mqttCfg.intervalState;
  doc["intervalPing"] =mqttCfg.intervalPing;
  doc["webapp"]=String("https://infraestructura-it.github.io/gpio-p2p/gpio_p2p_webapp.html?id=")+deviceSerial;
  JsonObject tabsJ=doc.createNestedObject("tabs");
  tabsJ["state"]=mqttTabs.state; tabsJ["control"]=mqttTabs.control;
  tabsJ["history"]=mqttTabs.history; tabsJ["alerts"]=mqttTabs.alerts;
  String out; serializeJson(doc,out); return out;
}

// Legacy — usado por /api/p2p/config
String mqttGetConfigJSON(){ return mqttGetFullConfigJSON(); }

// ══════════════════════════════════════════════════
//  WIFI — SCAN / STATUS / CONNECT
// ══════════════════════════════════════════════════
String wifiStatusJSON(){
  DynamicJsonDocument doc(256);
  bool conn=(WiFi.status()==WL_CONNECTED);
  doc["connected"]=conn;
  if(conn){
    doc["ssid"]=WiFi.SSID();
    doc["ip"]=WiFi.localIP().toString();
    doc["rssi"]=WiFi.RSSI();
    doc["ch"]=WiFi.channel();
    doc["mac"]=WiFi.macAddress();
  } else {
    doc["ssid"]="";
    doc["ip"]=deviceIP;
    doc["rssi"]=0;
    doc["ch"]=0;
    doc["mac"]=WiFi.macAddress();
  }
  String out; serializeJson(doc,out); return out;
}

String wifiScanJSON(){
  Serial.println(F("[WiFi] Escaneando redes..."));
  int n=WiFi.scanNetworks(false,true); // blocking, incluye hidden
  Serial.printf("[WiFi] Encontradas: %d\n",n);
  DynamicJsonDocument doc(4096);
  doc["current"]= (WiFi.status()==WL_CONNECTED) ? WiFi.SSID() : String("");
  JsonArray arr=doc.createNestedArray("networks");
  for(int i=0;i<n&&i<20;i++){
    JsonObject net=arr.createNestedObject();
    net["ssid"]   = WiFi.SSID(i);
    net["rssi"]   = WiFi.RSSI(i);
    net["ch"]     = WiFi.channel(i);
    net["secured"]= (WiFi.encryptionType(i)!=WIFI_AUTH_OPEN);
    net["bssid"]  = WiFi.BSSIDstr(i);
  }
  WiFi.scanDelete();
  String out; serializeJson(doc,out); return out;
}

void registerWifiRoutes(){
  // GET estado actual
  server.on("/api/wifi/status",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,wifiStatusJSON());
  });
  // POST escanear
  server.on("/api/wifi/scan",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    sendJSON(200,wifiScanJSON());
  });
  // POST guardar credenciales y reiniciar
  server.on("/api/wifi/connect",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(128);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    const char* ssid=doc["ssid"]; const char* pass=doc["pass"];
    if(!ssid||strlen(ssid)==0){sendError(400,"SSID vacio");return;}
    // Guardar en NVS (reusar el mismo mecanismo de loadConfig/saveConfig)
    savedSSID=String(ssid);
    savedPASS=String(pass?pass:"");
#if !DEBUG_MODE
    prefs.begin("wificfg",false);
    prefs.putString("ssid",savedSSID);
    prefs.putString("pass",savedPASS);
    prefs.end();
#endif
    // Intentar conectar para obtener la IP (timeout 8s)
    Serial.printf("[WiFi] Probando conexion a '%s'...\n",ssid);
    WiFi.disconnect(false);
    delay(300);
    WiFi.begin(ssid, pass&&strlen(pass)>0?pass:nullptr);
    int tries=0; String newIP="";
    while(WiFi.status()!=WL_CONNECTED && tries<16){delay(500);tries++;}
    if(WiFi.status()==WL_CONNECTED){
      newIP=WiFi.localIP().toString();
      deviceIP=newIP;
      Serial.println("[WiFi] Conectado: "+newIP);
    } else {
      Serial.println(F("[WiFi] No se pudo conectar con esas credenciales"));
    }
    // Responder con la IP y programar reinicio
    String resp="{\"ok\":true,\"ip\":\""+newIP+"\"}";
    sendJSON(200,resp);
    delay(400);
    ESP.restart();
  });
  // OPTIONS CORS
  server.on("/api/wifi/status",  HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  server.on("/api/wifi/scan",    HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  server.on("/api/wifi/connect", HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  Serial.println(F("[WiFi] Rutas /api/wifi/* registradas"));
}

void registerMqttConfigRoutes(){
  // GET config completa
  server.on("/api/mqtt/config",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,mqttGetFullConfigJSON());
  });
  // POST guardar config
  server.on("/api/mqtt/config",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(512);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    // Aplicar campos
    if(doc.containsKey("host"))     strlcpy(mqttCfg.host,      doc["host"].as<const char*>(),64);
    if(doc.containsKey("port"))     mqttCfg.port             = doc["port"];
    if(doc.containsKey("clientId")) strlcpy(mqttCfg.clientIdCfg,doc["clientId"].as<const char*>(),32);
    if(doc.containsKey("keepAlive"))mqttCfg.keepAlive        = doc["keepAlive"];
    if(doc.containsKey("user"))     strlcpy(mqttCfg.user,      doc["user"].as<const char*>(),32);
    if(doc.containsKey("pass")&&strlen(doc["pass"])>0) strlcpy(mqttCfg.pass,doc["pass"].as<const char*>(),32);
    if(doc.containsKey("tls"))      mqttCfg.tls              = doc["tls"];
    if(doc.containsKey("qos"))      mqttCfg.qos              = doc["qos"];
    if(doc.containsKey("topicState"))   strlcpy(mqttCfg.topicState,  doc["topicState"].as<const char*>(),32);
    if(doc.containsKey("topicAlert"))   strlcpy(mqttCfg.topicAlert,  doc["topicAlert"].as<const char*>(),32);
    if(doc.containsKey("topicHistory")) strlcpy(mqttCfg.topicHistory,doc["topicHistory"].as<const char*>(),32);
    if(doc.containsKey("intervalState"))mqttCfg.intervalState= doc["intervalState"];
    if(doc.containsKey("intervalPing")) mqttCfg.intervalPing = doc["intervalPing"];
    bool en=doc["enabled"]|mqttEnabled;
    saveMqttCfgNVS();
    // Reconectar con nueva config
    if(mqttPubSub.connected()) mqttPubSub.disconnect();
    mqttConnected=false;
    mqttSetEnabled(en);
    sendJSON(200,"{\"ok\":true}");
  });
  server.on("/api/mqtt/config",HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  Serial.println(F("[MQTT] Rutas /api/mqtt/config registradas"));
}

void registerNetRoutes(){
  server.on("/api/net/config",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,netGetConfigJSON());
  });
  server.on("/api/net/config",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(256);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    netCfgSt.dhcp = doc["dhcp"]|true;
    if(!netCfgSt.dhcp){
      if(doc.containsKey("ip"))   strlcpy(netCfgSt.ip,  doc["ip"].as<const char*>(),16);
      if(doc.containsKey("mask")) strlcpy(netCfgSt.mask,doc["mask"].as<const char*>(),16);
      if(doc.containsKey("gw"))   strlcpy(netCfgSt.gw,  doc["gw"].as<const char*>(),16);
      if(doc.containsKey("dns1")) strlcpy(netCfgSt.dns1,doc["dns1"].as<const char*>(),16);
      if(doc.containsKey("dns2")) strlcpy(netCfgSt.dns2,doc["dns2"].as<const char*>(),16);
    }
    saveNetCfgNVS();
    // Aplicar inmediatamente si ya hay WiFi
    if(!netCfgSt.dhcp && WiFi.status()==WL_CONNECTED){
      applyStaticIP();
      deviceIP=WiFi.localIP().toString();
    }
    sendJSON(200,"{\"ok\":true,\"ip\":\""+deviceIP+"\"}");
  });
  server.on("/api/net/config",HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  Serial.println(F("[NET] Rutas /api/net/config registradas"));
}

void registerP2PRoutes(){
  server.on("/api/p2p/config",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,mqttGetConfigJSON());
  });
  server.on("/api/p2p/enable",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(64);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    bool en=doc["enabled"]|false;
    mqttSetEnabled(en);
    mqttEnabled=en; saveMqttCfgNVS();
    sendJSON(200,"{\"ok\":true}");
  });
  server.on("/api/p2p/tabs",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(128);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    if(doc.containsKey("state"))   mqttTabs.state  =(bool)doc["state"];
    if(doc.containsKey("control")) mqttTabs.control=(bool)doc["control"];
    if(doc.containsKey("history")) mqttTabs.history=(bool)doc["history"];
    if(doc.containsKey("alerts"))  mqttTabs.alerts =(bool)doc["alerts"];
    sendJSON(200,"{\"ok\":true}");
  });
  server.on("/api/p2p/config",HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  server.on("/api/p2p/enable",HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  server.on("/api/p2p/tabs",  HTTP_OPTIONS,[](){ addCORS();server.send(204); });
  Serial.println(F("[P2P] Rutas /api/p2p/* registradas"));
}

// ══════════════════════════════════════════════════
//  MODO NORMAL — REST API
// ══════════════════════════════════════════════════
void startNormalMode(){
  const char* hdrs[]={"X-Token"};
  server.collectHeaders(hdrs,1);

  server.on("/",           HTTP_GET, sendAppPage);
  server.on("/index.html", HTTP_GET, sendAppPage);

  // POST /api/login — en DEBUG_MODE devuelve token fijo sin verificar
  server.on("/api/login",HTTP_POST,[](){
#if DEBUG_MODE
    sendJSON(200,"{\"ok\":true,\"token\":\"" DEBUG_TOKEN "\",\"role\":\"admin\"}");
    Serial.println(F("[AUTH] DEBUG login"));
#else
    DynamicJsonDocument doc(256);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    const char* usr=doc["user"]; const char* pwd=doc["pass"];
    if(!usr||!pwd){sendError(400,"Faltan user/pass");return;}
    int ui=findUser(usr);
    if(ui<0){sendError(401,"Usuario no existe");return;}
    if(hashPass(String(pwd))!=String(users[ui].pass)){sendError(401,"Contrasena incorrecta");return;}
    String tok=genToken();
    for(int i=0;i<3;i++){
      if(!sessions[i].active||millis()>=sessions[i].expires){
        tok.toCharArray(sessions[i].token,TOKEN_LEN+1);
        strncpy(sessions[i].user,usr,23);
        strncpy(sessions[i].role,users[ui].role,7);
        sessions[i].expires=millis()+SESSION_MS;
        sessions[i].active=true;
        sendJSON(200,"{\"ok\":true,\"token\":\""+tok+"\",\"role\":\""+String(users[ui].role)+"\"}");
        Serial.println("[AUTH] Login: "+String(usr));
        return;
      }
    }
    sendError(503,"Sin sesiones disponibles");
#endif
  });

  server.on("/api/logout",HTTP_POST,[](){
    String tok=server.header("X-Token");
    for(int i=0;i<3;i++) if(String(sessions[i].token)==tok) sessions[i].active=false;
    sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/info",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    unsigned long up=(millis()-bootTime)/1000;
    DynamicJsonDocument doc(256);
    doc["ok"]=true; doc["ip"]=deviceIP; doc["uptime_s"]=up;
    doc["ws_clients"]=wsClients; doc["ntp_time"]=ntpSynced?nowStr():"Sin NTP";
    doc["ntp_synced"]=ntpSynced;
    String out; serializeJson(doc,out); sendJSON(200,out);
  });

  server.on("/api/state",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,buildStateJSON());
  });

  // NOTA: server.on() NO hace prefix matching en ESP32 WebServer.
  // Las rutas /api/pin/:pin/:action se manejan en onNotFound (abajo).

  server.on("/api/all/off",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    for(int i=0;i<NUM_PINS;i++){
      if(pinState[i].mode==PM_OUTPUT){pinState[i].value=0;digitalWrite(PIN_TABLE[i].pin,LOW);broadcastPin(i,s->user);}
      if(pinState[i].mode==PM_PWM)   {pinState[i].value=0;ledcWrite(PIN_TABLE[i].pin,0);   broadcastPin(i,s->user);}
    }
    sendJSON(200,"{\"ok\":true}");
  });

  // ── SCHEDULER ──
  server.on("/api/schedule",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    DynamicJsonDocument doc(2048); doc["ok"]=true;
    JsonArray evs=doc.createNestedArray("events");
    for(int i=0;i<MAX_SCHED;i++){
      if(!sched[i].active) continue;
      JsonObject e=evs.createNestedObject();
      e["id"]=sched[i].id; e["pin"]=sched[i].pin;
      e["action"]=sched[i].action; e["hour"]=sched[i].hour;
      e["min"]=sched[i].min; e["pwm_val"]=sched[i].pwm_val;
      e["enabled"]=sched[i].enabled;
      JsonArray days=e.createNestedArray("days");
      for(int d=0;d<7;d++) if(sched[i].days&(1<<d)) days.add(d);
    }
    String out; serializeJson(doc,out); sendJSON(200,out);
  });

  server.on("/api/schedule",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(512);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    int slot=-1;
    for(int i=0;i<MAX_SCHED;i++) if(!sched[i].active){slot=i;break;}
    if(slot<0){sendError(507,"Maximo eventos alcanzado");return;}
    sched[slot].active=true; sched[slot].id=(uint8_t)slot;
    sched[slot].pin=(uint8_t)(doc["pin"]|2);
    strncpy(sched[slot].action,doc["action"]|"on",7);
    sched[slot].hour=(uint8_t)(doc["hour"]|8);
    sched[slot].min=(uint8_t)(doc["min"]|0);
    sched[slot].pwm_val=(uint16_t)(doc["pwm_val"]|128);
    sched[slot].enabled=(bool)(doc["enabled"]|true);
    sched[slot].days=0;
    if(doc["days"].is<JsonArray>()){
      for(int d:doc["days"].as<JsonArray>()) sched[slot].days|=(1<<(d&7));
    }
    saveSched();
    sendJSON(200,"{\"ok\":true,\"id\":"+String(slot)+"}");
  });

  server.on("/api/schedule/",HTTP_PUT,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    int id=server.uri().substring(14).toInt();
    if(id<0||id>=MAX_SCHED||!sched[id].active){sendError(404,"Evento no existe");return;}
    DynamicJsonDocument doc(512);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    if(doc.containsKey("pin"))     sched[id].pin=(uint8_t)(int)doc["pin"];
    if(doc.containsKey("action"))  strncpy(sched[id].action,doc["action"],7);
    if(doc.containsKey("hour"))    sched[id].hour=(uint8_t)(int)doc["hour"];
    if(doc.containsKey("min"))     sched[id].min=(uint8_t)(int)doc["min"];
    if(doc.containsKey("pwm_val")) sched[id].pwm_val=(uint16_t)(int)doc["pwm_val"];
    if(doc.containsKey("enabled")) sched[id].enabled=(bool)doc["enabled"];
    if(doc["days"].is<JsonArray>()){
      sched[id].days=0;
      for(int d:doc["days"].as<JsonArray>()) sched[id].days|=(1<<(d&7));
    }
    saveSched(); sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/schedule/",HTTP_DELETE,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    int id=server.uri().substring(14).toInt();
    if(id<0||id>=MAX_SCHED){sendError(404,"No existe");return;}
    sched[id].active=false; saveSched(); sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/schedule",HTTP_OPTIONS,[](){ addCORS(); server.send(204); });
  server.on("/api/schedule/",HTTP_OPTIONS,[](){ addCORS(); server.send(204); });

  // ── HISTORIAL ──
  server.on("/api/history",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    DynamicJsonDocument doc(4096); doc["ok"]=true;
    JsonArray evs=doc.createNestedArray("events");
    for(int i=0;i<histCount;i++){
      int idx=(histHead-histCount+i+MAX_HIST)%MAX_HIST;
      JsonObject e=evs.createNestedObject();
      e["time"]=hist[idx].time; e["pin"]=hist[idx].pin;
      e["mode"]=hist[idx].mode; e["value"]=hist[idx].value;
      e["name"]=hist[idx].name; e["user"]=hist[idx].user;
    }
    String out; serializeJson(doc,out); sendJSON(200,out);
  });

  server.on("/api/history/clear",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    histHead=0; histCount=0; sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/history",HTTP_OPTIONS,[](){ addCORS(); server.send(204); });

  // ── USUARIOS ──
  server.on("/api/users",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(1024); doc["ok"]=true;
    JsonArray arr=doc.createNestedArray("users");
    for(int i=0;i<MAX_USERS;i++){
      if(!users[i].active) continue;
      JsonObject u=arr.createNestedObject();
      u["name"]=users[i].name; u["role"]=users[i].role;
    }
    String out; serializeJson(doc,out); sendJSON(200,out);
  });

  server.on("/api/users",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(256);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    const char* name=doc["name"]; const char* pass=doc["pass"]; const char* role=doc["role"];
    if(!name||!pass||!role){sendError(400,"name,pass,role requeridos");return;}
    if(findUser(name)>=0){sendError(409,"Ya existe");return;}
    int slot=-1;
    for(int i=0;i<MAX_USERS;i++) if(!users[i].active){slot=i;break;}
    if(slot<0){sendError(507,"Maximo usuarios");return;}
    users[slot].active=true;
    strncpy(users[slot].name,name,23);
    hashPass(String(pass)).toCharArray(users[slot].pass,64);
    strncpy(users[slot].role,role,7);
    saveUsers(); sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/users/",HTTP_DELETE,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    String uname=server.uri().substring(11);
    if(uname==String(s->user)){sendError(400,"No puedes eliminarte a ti mismo");return;}
    int ui=findUser(uname.c_str());
    if(ui<0){sendError(404,"No existe");return;}
    users[ui].active=false; saveUsers(); sendJSON(200,"{\"ok\":true}");
  });

  server.on("/api/users",HTTP_OPTIONS,[](){ addCORS(); server.send(204); });
  server.on("/api/users/",HTTP_OPTIONS,[](){ addCORS(); server.send(204); });

  server.on("/reset",HTTP_GET,[](){
    addCORS(); server.send(200,"text/plain","Reset OK");
    delay(500); clearAllConfig(); ESP.restart();
  });

  // ── Routing manual para /api/pin/:pin/:action ──
  // El WebServer de ESP32 NO hace prefix matching, por eso se usa onNotFound.
  server.onNotFound([](){
    String uri=server.uri();
    HTTPMethod method=server.method();
    addCORS();

    // OPTIONS preflight
    if(method==HTTP_OPTIONS){ server.send(204); return; }

    // /api/pin/<pin>/<action>
    if(uri.startsWith("/api/pin/")){
      Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
      bool admin=(strcmp(s->role,"admin")==0);
      String rest=uri.substring(9);           // "2/mode"  "2/adc/avg"
      int sl=rest.indexOf('/');
      if(sl<0){sendError(400,"URL invalida");return;}
      uint8_t pin=(uint8_t)rest.substring(0,sl).toInt();
      String action=rest.substring(sl+1);      // "mode", "name", "adc/avg" ...
      int idx=pinIndex(pin);
      if(idx<0){sendError(400,"Pin no disponible");return;}

      if(method==HTTP_POST){
        DynamicJsonDocument doc(256);
        if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
        if(action=="mode"){
          if(!admin){sendError(403,"Solo admin");return;}
          PinMode_t nm=modeFromStr(doc["mode"]);
          if(!applyMode(idx,nm)){sendError(400,"Modo no soportado");return;}
          addHist(pin,modeName(nm),0,s->user,pinState[idx].name);
          broadcastPin(idx,s->user); mqttPublishState(); sendJSON(200,"{\"ok\":true}");
        }else if(action=="digital"){
          if(!admin){sendError(403,"Solo admin");return;}
          if(pinState[idx].mode!=PM_OUTPUT){sendError(400,"No es OUTPUT");return;}
          int st=doc["state"]|-1; if(st<0||st>1){sendError(400,"state 0 o 1");return;}
          pinState[idx].value=st; digitalWrite(pin,st?HIGH:LOW);
          saveGPIOState();
          addHist(pin,"output",st,s->user,pinState[idx].name);
          broadcastPin(idx,s->user); mqttPublishState(); sendJSON(200,"{\"ok\":true}");
        }else if(action=="pwm"){
          if(!admin){sendError(403,"Solo admin");return;}
          if(pinState[idx].mode!=PM_PWM){sendError(400,"No es PWM");return;}
          int duty=doc["duty"]|-1; if(duty<0||duty>255){sendError(400,"duty 0-255");return;}
          pinState[idx].value=duty; ledcWrite(pin,duty);
          saveGPIOState();
          addHist(pin,"pwm",duty,s->user,pinState[idx].name);
          broadcastPin(idx,s->user); mqttPublishState(); sendJSON(200,"{\"ok\":true}");
        }else if(action=="freq"){
          if(!admin){sendError(403,"Solo admin");return;}
          if(pinState[idx].mode!=PM_PWM){sendError(400,"No es PWM");return;}
          uint32_t freq=doc["freq"]|0; if(freq<1||freq>40000){sendError(400,"freq 1-40000");return;}
          pinState[idx].freq=freq; ledcChangeFrequency(pin,freq,PWM_RES);
          saveGPIOState();
          sendJSON(200,"{\"ok\":true}");
        }else if(action=="name"){
          const char* nm=doc["name"]; if(!nm){sendError(400,"name requerido");return;}
          strncpy(pinState[idx].name,nm,23); saveNames();
          sendJSON(200,"{\"ok\":true}");
        }else sendError(404,"Accion desconocida");

      }else if(method==HTTP_GET){
        if(action=="digital"){
          if(pinState[idx].mode!=PM_INPUT){sendError(400,"No es INPUT");return;}
          int val=digitalRead(pin); pinState[idx].value=val;
          sendJSON(200,"{\"ok\":true,\"state\":"+String(val)+"}");
        }else if(action=="adc"||action=="adc/avg"){
          if(pinState[idx].mode!=PM_ADC){sendError(400,"No es ADC");return;}
          int raw=0;
          if(action=="adc/avg"){long sm=0;for(int k=0;k<16;k++){sm+=analogRead(pin);delay(2);}raw=(int)(sm/16);}
          else raw=analogRead(pin);
          pinState[idx].value=raw;
          float v=(raw/4095.0f)*3.3f; char vb[8]; dtostrf(v,1,2,vb);
          sendJSON(200,"{\"ok\":true,\"raw\":"+String(raw)+",\"voltage\":\""+String(vb)+"\"}");
        }else sendError(404,"Accion desconocida");
      }else{
        sendError(405,"Metodo no permitido");
      }
      return;
    }

    // Cualquier otra ruta no encontrada
    addCORS(); server.send(404,"text/plain","Not found");
  });

  server.begin();
  webSocket.begin();
  webSocket.onEvent(onWSEvent);

  Serial.println(F("\n[OK] GPIO PRO listo:"));
  Serial.println("  App : http://"+deviceIP);
  Serial.println("  WS  : ws://"+deviceIP+":81");
#if DEBUG_MODE
  Serial.println(F("  [!] DEBUG MODE ACTIVO - sin login, sin NVS"));
#endif
}

// ══════════════════════════════════════════════════
//  MODO APROVISIONAMIENTO
// ══════════════════════════════════════════════════
void startProvisionMode(){
  appMode=MODE_PROVISION;
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID,AP_PASS);
  dns.start(DNS_PORT,"*",WiFi.softAPIP());
  server.on("/",HTTP_GET,sendProvPage);
  server.on("/scan",HTTP_GET,[](){
    int n=WiFi.scanNetworks(); String json="[";
    for(int i=0;i<n;i++){
      if(i>0) json+=",";
      json+="{\"ssid\":\""+WiFi.SSID(i)+"\",\"rssi\":"+WiFi.RSSI(i);
      json+=",\"encrypted\":";
      json+=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN?"true":"false");
      json+="}";
    }
    json+="]"; server.send(200,"application/json",json); WiFi.scanDelete();
  });
  server.on("/save",HTTP_POST,[](){
    String ssid=server.arg("ssid"),wpass=server.arg("pass");
    String admu=server.arg("admu"),admp=server.arg("admp");
    int tz=server.arg("tz").toInt();
    if(!ssid.length()||!admu.length()||admp.length()<4){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"Datos incompletos\"}"); return;
    }
    // Intentar conectar al WiFi ANTES de guardar
    Serial.println("[PROV] Intentando conectar a: "+ssid);
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), wpass.c_str());
    int tries=0; bool connected=false;
    while(tries<24){
      delay(500); tries++;
      Serial.print(".");
      if(WiFi.status()==WL_CONNECTED){ connected=true; break; }
    }
    Serial.println();
    if(!connected){
      WiFi.disconnect(true);
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASS);
      server.send(200,"application/json",
        "{\"ok\":false,\"error\":\"No se pudo conectar. Verifica la contrasena e intenta de nuevo.\"}");
      return;
    }
    String newIP = WiFi.localIP().toString();
    Serial.println("[PROV] Conectado! IP: "+newIP);
    // Guardar en NVS
    prefs.begin("cfg",false);
    prefs.putString("ssid",ssid); prefs.putString("pass",wpass);
    prefs.putInt("tz",tz);
    prefs.putString("u0name",admu);
    prefs.putString("u0pass",hashPass(admp));
    prefs.putString("u0role","admin");
    prefs.putInt("ucount",1);
    prefs.end();
    // Responder con la nueva IP antes de reiniciar
    String resp="{\"ok\":true,\"ip\":\""+newIP+"\",\"msg\":\"Conectado! Abre http://"+newIP+"\"}";
    server.send(200,"application/json",resp);
    delay(2000);
    ESP.restart();
  });
  server.on("/reset",HTTP_GET,[](){
    clearAllConfig(); server.send(200,"text/plain","Reset OK");
    delay(1000); ESP.restart();
  });
  server.onNotFound([](){
    server.sendHeader("Location","http://192.168.4.1",true); server.send(302,"text/plain","");
  });
  server.begin();
  Serial.println(F("[AP] ESP32-GPIO-PRO activo"));
  Serial.println(F("  WiFi: ESP32-GPIO-PRO"));
  Serial.println(F("  URL:  http://192.168.4.1"));
}

// ══════════════════════════════════════════════════
//  WEBSOCKET
// ══════════════════════════════════════════════════
void onWSEvent(uint8_t client,WStype_t type,uint8_t* payload,size_t len){
  switch(type){
    case WStype_CONNECTED:
      wsClients++;
      Serial.printf("[WS] #%d conectado\n",client); break;
    case WStype_DISCONNECTED:
      if(wsClients>0) wsClients--;
      Serial.printf("[WS] #%d desconectado\n",client); break;
    case WStype_TEXT:{
      DynamicJsonDocument doc(256);
      if(deserializeJson(doc,(char*)payload)) break;
      const char* cmd=doc["cmd"]; if(!cmd) break;
      if(strcmp(cmd,"GET_STATE")==0){
        DynamicJsonDocument res(4096); res["type"]="STATE";
        JsonArray pins=res.createNestedArray("pins");
        for(int i=0;i<NUM_PINS;i++){
          JsonObject p=pins.createNestedObject();
          p["pin"]=PIN_TABLE[i].pin; p["mode"]=modeName(pinState[i].mode);
          p["value"]=pinState[i].value; p["name"]=pinState[i].name;
          p["last_change"]=pinState[i].lastChange; p["last_user"]=pinState[i].lastUser;
          if(pinState[i].mode==PM_PWM) p["freq"]=pinState[i].freq;
        }
        String out; serializeJson(res,out); webSocket.sendTXT(client,out);
      }else if(strcmp(cmd,"PING")==0){
        webSocket.sendTXT(client,"{\"type\":\"PONG\"}");
      }
      break;
    }
    default: break;
  }
}

// ══════════════════════════════════════════════════
//  SCHEDULER
// ══════════════════════════════════════════════════
void checkScheduler(){
  if(!ntpSynced) return;
  static int lastMin=-1;
  time_t now=time(nullptr)+(tzOffset*3600);
  struct tm* t=gmtime(&now);
  int curMin=t->tm_hour*60+t->tm_min;
  if(curMin==lastMin) return;
  lastMin=curMin;
  uint8_t dayBit=(uint8_t)(1<<t->tm_wday);
  for(int i=0;i<MAX_SCHED;i++){
    if(!sched[i].active||!sched[i].enabled) continue;
    if(!(sched[i].days&dayBit)) continue;
    if(sched[i].hour==t->tm_hour && sched[i].min==t->tm_min)
      execSched(sched[i],"scheduler");
  }
}

// ══════════════════════════════════════════════════
//  PERSISTENCIA
// ══════════════════════════════════════════════════
void loadConfig(){
#if DEBUG_MODE
  // En debug no se lee NVS, se ignora
  Serial.println(F("[DEBUG] NVS ignorado"));
  tzOffset=-5;
#else
  prefs.begin("cfg",true);
  savedSSID=prefs.getString("ssid","");
  savedPASS=prefs.getString("pass","");
  tzOffset=prefs.getInt("tz",-5);
  int uc=prefs.getInt("ucount",0);
  prefs.end();
  prefs.begin("cfg",true);
  for(int i=0;i<min(uc,MAX_USERS);i++){
    String k="u"+String(i);
    users[i].active=true;
    prefs.getString((k+"name").c_str(),"").toCharArray(users[i].name,24);
    prefs.getString((k+"pass").c_str(),"").toCharArray(users[i].pass,64);
    prefs.getString((k+"role").c_str(),"viewer").toCharArray(users[i].role,8);
    Serial.printf("[NVS] User: %s [%s]\n",users[i].name,users[i].role);
  }
  userCount=uc;
  prefs.end();
  Serial.printf("[NVS] SSID:'%s' TZ:%d Users:%d\n",savedSSID.c_str(),tzOffset,uc);
#endif
}

void saveUsers(){
#if DEBUG_MODE
  return; // no persistir en debug
#endif
  prefs.begin("cfg",false);
  int uc=0;
  for(int i=0;i<MAX_USERS;i++){
    if(!users[i].active) continue;
    String k="u"+String(uc);
    prefs.putString((k+"name").c_str(),users[i].name);
    prefs.putString((k+"pass").c_str(),users[i].pass);
    prefs.putString((k+"role").c_str(),users[i].role);
    uc++;
  }
  prefs.putInt("ucount",uc); prefs.end();
}
void saveSched(){
#if DEBUG_MODE
  return;
#endif
  prefs.begin("sched",false); prefs.clear();
  int cnt=0;
  for(int i=0;i<MAX_SCHED;i++){
    if(!sched[i].active) continue;
    String k="s"+String(cnt);
    prefs.putUChar((k+"p").c_str(),sched[i].pin);
    prefs.putString((k+"a").c_str(),sched[i].action);
    prefs.putUChar((k+"h").c_str(),sched[i].hour);
    prefs.putUChar((k+"m").c_str(),sched[i].min);
    prefs.putUChar((k+"d").c_str(),sched[i].days);
    prefs.putUShort((k+"v").c_str(),sched[i].pwm_val);
    prefs.putBool((k+"e").c_str(),sched[i].enabled);
    prefs.putUChar((k+"i").c_str(),sched[i].id);
    cnt++;
  }
  prefs.putInt("cnt",cnt); prefs.end();
}
void loadSched(){
#if DEBUG_MODE
  return;
#endif
  prefs.begin("sched",true);
  int cnt=prefs.getInt("cnt",0);
  for(int i=0;i<min(cnt,MAX_SCHED);i++){
    String k="s"+String(i);
    sched[i].active=true;
    sched[i].pin=prefs.getUChar((k+"p").c_str(),2);
    prefs.getString((k+"a").c_str(),"on").toCharArray(sched[i].action,8);
    sched[i].hour=prefs.getUChar((k+"h").c_str(),8);
    sched[i].min=prefs.getUChar((k+"m").c_str(),0);
    sched[i].days=prefs.getUChar((k+"d").c_str(),0);
    sched[i].pwm_val=prefs.getUShort((k+"v").c_str(),128);
    sched[i].enabled=prefs.getBool((k+"e").c_str(),true);
    sched[i].id=prefs.getUChar((k+"i").c_str(),(uint8_t)i);
  }
  prefs.end();
  Serial.printf("[NVS] Schedules: %d\n",cnt);
}
void saveNames(){
#if DEBUG_MODE
  return;
#endif
  prefs.begin("names",false);
  for(int i=0;i<NUM_PINS;i++)
    prefs.putString(String(PIN_TABLE[i].pin).c_str(),pinState[i].name);
  prefs.end();
}
void loadNames(){
#if DEBUG_MODE
  // Nombres por defecto ya inicializados en setup
  return;
#endif
  prefs.begin("names",true);
  for(int i=0;i<NUM_PINS;i++){
    String def="GPIO "+String(PIN_TABLE[i].pin);
    prefs.getString(String(PIN_TABLE[i].pin).c_str(),def).toCharArray(pinState[i].name,24);
  }
  prefs.end();
}
void clearAllConfig(){
  prefs.begin("cfg",false);       prefs.clear(); prefs.end();
  prefs.begin("sched",false);     prefs.clear(); prefs.end();
  prefs.begin("names",false);     prefs.clear(); prefs.end();
  prefs.begin("gpiostate",false); prefs.clear(); prefs.end();
  prefs.begin("relaycfg",false);  prefs.clear(); prefs.end();
  Serial.println(F("[NVS] Todo borrado"));
}

// ══════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════
void setup(){
  Serial.begin(115200); delay(200); bootTime=millis();
  Serial.println(F("\n============================="));
  Serial.println(F("  ESP32 GPIO PRO v1.0"));
#if DEBUG_MODE
  Serial.println(F("  *** DEBUG MODE ACTIVO ***"));
#endif
  Serial.println(F("============================="));

  memset(pinState,0,sizeof(pinState));
  for(int i=0;i<NUM_PINS;i++){
    pinState[i].freq=PWM_FREQ_DEF;
    if(PIN_TABLE[i].canIn) pinMode(PIN_TABLE[i].pin,INPUT);
    // Nombre por defecto
    String def="GPIO "+String(PIN_TABLE[i].pin);
    def.toCharArray(pinState[i].name,24);
  }
  // nombre especial pin 2
  strncpy(pinState[1].name,"LED onboard",23);

  memset(users,0,sizeof(users));
  memset(sessions,0,sizeof(sessions));
  memset(sched,0,sizeof(sched));
  memset(hist,0,sizeof(hist));
  pinMode(PIN_BOOT,INPUT_PULLUP);

#if DEBUG_MODE
  // ── Sesion debug persistente en slot 0 ──
  strncpy(sessions[0].token, DEBUG_TOKEN, TOKEN_LEN);
  strncpy(sessions[0].user,  "debug",     23);
  strncpy(sessions[0].role,  "admin",     7);
  sessions[0].expires = 0xFFFFFFFFUL; // nunca expira
  sessions[0].active  = true;

  // Arrancar AP directamente, sin leer NVS ni conectar WiFi
  appMode = MODE_NORMAL;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  deviceIP = WiFi.softAPIP().toString();
  dns.start(DNS_PORT,"*",WiFi.softAPIP());
  Serial.println("[DEBUG] AP: "+String(AP_SSID)+" -> http://"+deviceIP);
  startNormalMode();
  mqttInit(); relayInit(); registerP2PRoutes(); registerMqttConfigRoutes(); registerRelayRoutes(); registerNetRoutes(); registerWifiRoutes();

#else
  loadConfig(); loadNames(); loadSched();
  restoreGPIOState();
  if(savedSSID.length()>0 && userCount>0){
    loadNetCfgNVS();
    if(!netCfgSt.dhcp) applyStaticIP();   // IP fija ANTES de conectar
    if(connectWiFi()){
      configTime(0,0,"pool.ntp.org","time.google.com");
      Serial.println(F("[NTP] Sincronizando..."));
      time_t now=time(nullptr); int tries=0;
      while(now<1000000000UL&&tries<20){delay(500);now=time(nullptr);tries++;}
      ntpSynced=(now>1000000000UL);
      Serial.printf("[NTP] %s -> %s\n",ntpSynced?"OK":"Fallo",nowStr().c_str());
      deviceIP=WiFi.localIP().toString();
      appMode=MODE_NORMAL; startNormalMode();
      mqttInit(); relayInit(); registerP2PRoutes(); registerMqttConfigRoutes(); registerRelayRoutes(); registerNetRoutes(); registerWifiRoutes();
      if(mqttEnabled) mqttConnect();
    } else {
      startProvisionMode();
    }
  } else {
    if(userCount==0) Serial.println(F("[PROV] Sin usuarios configurados"));
    startProvisionMode();
  }
#endif
}

void loop(){
  checkResetButton(); checkSerialReset();
  if(appMode==MODE_PROVISION){
    dns.processNextRequest(); server.handleClient();
  }else{
    server.handleClient(); webSocket.loop(); checkScheduler(); mqttLoop(); relayLoop();
#if !DEBUG_MODE
    if(WiFi.status()!=WL_CONNECTED){
      Serial.println(F("[WiFi] Reconectando..."));
      if(connectWiFi()) Serial.println("  URL: http://"+deviceIP);
    }
#endif
  }
}

// ══════════════════════════════════════════════════
//  UTILS
// ══════════════════════════════════════════════════
bool connectWiFi(){
  Serial.println("[WiFi] Conectando '"+savedSSID+"'...");
  WiFi.mode(WIFI_STA); WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
  int t=0;
  while(WiFi.status()!=WL_CONNECTED&&t<40){delay(500);Serial.print(".");t++;}
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    deviceIP=WiFi.localIP().toString();
    Serial.println("[WiFi] OK -> http://"+deviceIP); return true;
  }
  Serial.println(F("[WiFi] Fallo")); return false;
}
void clearWiFiConfig(){clearAllConfig();}
void checkResetButton(){
  static unsigned long t0=0; static bool pressing=false;
  bool low=(digitalRead(PIN_BOOT)==LOW);
  if(low){
    if(!pressing){pressing=true;t0=millis();Serial.println(F("[BTN] Manten 3s..."));}
    else if(millis()-t0>RESET_HOLD_MS){Serial.println(F("[BTN] RESET!"));clearAllConfig();ESP.restart();}
  }else pressing=false;
}
void checkSerialReset(){
  if(Serial.available()){
    char c=Serial.read();
    if(c=='r'||c=='R'){Serial.println(F("[SERIAL] Reset total"));clearAllConfig();ESP.restart();}
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  GPIO STATE — Persistencia NVS
// ══════════════════════════════════════════════════════════════════════════

// Guarda modo, value y freq de todos los pines en NVS
void saveGPIOState(){
#if DEBUG_MODE
  return;
#endif
  prefs.begin("gpiostate", false);
  for(int i=0; i<NUM_PINS; i++){
    String k = "p" + String(PIN_TABLE[i].pin);
    prefs.putUChar((k+"m").c_str(), (uint8_t)pinState[i].mode);
    prefs.putInt  ((k+"v").c_str(), pinState[i].value);
    prefs.putUInt ((k+"f").c_str(), pinState[i].freq);
  }
  prefs.end();
}

// Lee el estado guardado en NVS (solo llena pinState[], no aplica físico)
void loadGPIOState(){
#if DEBUG_MODE
  return;
#endif
  prefs.begin("gpiostate", true);
  for(int i=0; i<NUM_PINS; i++){
    String k = "p" + String(PIN_TABLE[i].pin);
    pinState[i].mode  = (PinMode_t)prefs.getUChar((k+"m").c_str(), (uint8_t)PM_NONE);
    pinState[i].value = prefs.getInt  ((k+"v").c_str(), 0);
    pinState[i].freq  = prefs.getUInt ((k+"f").c_str(), PWM_FREQ_DEF);
  }
  prefs.end();
}

// Aplica físicamente el estado leído de NVS — llamar al final de setup()
void restoreGPIOState(){
  loadGPIOState();
  Serial.println(F("[GPIO] Restaurando estado..."));
  for(int i=0; i<NUM_PINS; i++){
    uint8_t pin = PIN_TABLE[i].pin;
    switch(pinState[i].mode){
      case PM_OUTPUT:
        if(!PIN_TABLE[i].canOut) break;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, pinState[i].value ? HIGH : LOW);
        Serial.printf("  GPIO%d -> OUTPUT %s\n", pin, pinState[i].value ? "HIGH" : "LOW");
        break;
      case PM_PWM:
        if(!PIN_TABLE[i].canPWM) break;
        ledcAttach(pin, pinState[i].freq, PWM_RES);
        ledcWrite(pin, pinState[i].value);
        Serial.printf("  GPIO%d -> PWM duty=%d freq=%dHz\n", pin, pinState[i].value, pinState[i].freq);
        break;
      case PM_INPUT:
        if(!PIN_TABLE[i].canIn) break;
        pinMode(pin, INPUT);
        break;
      case PM_ADC:
        if(!PIN_TABLE[i].canADC) break;
        pinMode(pin, INPUT);
        analogSetPinAttenuation(pin, ADC_11db);
        break;
      default:
        // PM_NONE — no tocar el pin
        break;
    }
  }
  Serial.println(F("[GPIO] Restauración completa"));
}
// ══════════════════════════════════════════════════════════════════════════
//  RELAY - WebSocket P2P (ESP32-S3)
// ══════════════════════════════════════════════════════════════════════════

void relayOnMessage(uint8_t* payload, size_t len){
  String msg = String((char*)payload).substring(0,len);
  Serial.println("[RELAY] RX: "+msg);
  DynamicJsonDocument doc(512);
  if(deserializeJson(doc,msg)) return;
  const char* cmd = doc["cmd"] | "";
  int pinIdx = doc["pin"] | -1;

  if(strcmp(cmd,"on")==0 && pinIdx>=0 && pinIdx<NUM_PINS){
    pinState[pinIdx].value=1;
    digitalWrite(PIN_TABLE[pinIdx].pin, HIGH);
    addHist(pinIdx,"OUTPUT",1,"relay",pinState[pinIdx].name);
    {String _s=buildStateJSON();webSocket.broadcastTXT(_s);}
  } else if(strcmp(cmd,"off")==0 && pinIdx>=0 && pinIdx<NUM_PINS){
    pinState[pinIdx].value=0;
    digitalWrite(PIN_TABLE[pinIdx].pin,LOW);
    addHist(pinIdx,"OUTPUT",0,"relay",pinState[pinIdx].name);
    {String _s=buildStateJSON();webSocket.broadcastTXT(_s);}
  } else if(strcmp(cmd,"pwm")==0 && pinIdx>=0 && pinIdx<NUM_PINS){
    int val = doc["val"] | 0;
    pinState[pinIdx].value=val;
    ledcWrite(PIN_TABLE[pinIdx].pin, val);
    addHist(pinIdx,"PWM",val,"relay",pinState[pinIdx].name);
    {String _s=buildStateJSON();webSocket.broadcastTXT(_s);}
  } else if(strcmp(cmd,"state")==0){
    {String _s=buildStateJSON();wsRelay.sendTXT(_s);}
  }
}

void relayWebSocketEvent(WStype_t type, uint8_t* payload, size_t length){
  switch(type){
    case WStype_CONNECTED:
      relayConnected=true;
      Serial.println(F("[RELAY] Conectado"));
      {
        DynamicJsonDocument auth(128);
        auth["type"]="auth";
        auth["id"]=relayDeviceId;
        auth["token"]=String(relayCfg.token);
        String s; serializeJson(auth,s);
        wsRelay.sendTXT(s);
      }
      relayPublishState();
      break;
    case WStype_DISCONNECTED:
      relayConnected=false;
      Serial.println(F("[RELAY] Desconectado"));
      break;
    case WStype_TEXT:
      relayOnMessage(payload,length);
      break;
    default: break;
  }
}

bool relayConnect(){
  if(!relayEnabled) return false;
  if(relayConnected) return true;
  if(WiFi.status()!=WL_CONNECTED) return false;
  String path="/ws?id="+relayDeviceId+"&role=device";
  Serial.printf("[RELAY] Conectando a %s:%d%s\n",relayCfg.host,relayCfg.port,path.c_str());
  wsRelay.onEvent(relayWebSocketEvent);
  wsRelay.setReconnectInterval(RELAY_RETRY_MS);
  if(relayCfg.tls){
    wsRelay.beginSSL(relayCfg.host, relayCfg.port, path.c_str());
  } else {
    wsRelay.begin(relayCfg.host, relayCfg.port, path.c_str());
  }
  return true;
}

void relayDisconnect(){
  wsRelay.disconnect();
  relayConnected=false;
}

void relayPublishState(){
  if(!relayConnected) return;
  String s=buildStateJSON();
  wsRelay.sendTXT(s);
}

void relayLoop(){
  if(!relayEnabled) return;
  wsRelay.loop();
  if(relayConnected && millis()-relayLastPing>RELAY_PING_MS){
    relayLastPing=millis();
    DynamicJsonDocument doc(64);
    doc["type"]="ping";
    doc["uptime"]=(millis()-bootTime)/1000;
    String s; serializeJson(doc,s);
    wsRelay.sendTXT(s);
  }
}

void relayInit(){
  uint64_t mac=ESP.getEfuseMac();
  char buf[13];
  sprintf(buf,"%04X%08X",(uint16_t)(mac>>32),(uint32_t)mac);
  relayDeviceId=String(buf);
  loadRelayCfgNVS();
  Serial.println("[RELAY] DeviceId: "+relayDeviceId);
  if(relayEnabled) relayConnect();
}

void saveRelayCfgNVS(){
#if !DEBUG_MODE
  prefs.begin("relaycfg",false);
  prefs.putString("host",  relayCfg.host);
  prefs.putInt   ("port",  relayCfg.port);
  prefs.putString("token", relayCfg.token);
  prefs.putBool  ("tls",   relayCfg.tls);
  prefs.putBool  ("ena",   relayCfg.enabled);
  prefs.end();
#endif
}

void loadRelayCfgNVS(){
  strlcpy(relayCfg.host,"",64);
  relayCfg.port=8080;
  strlcpy(relayCfg.token,"",64);
  relayCfg.tls=false;
  relayCfg.enabled=false;
#if !DEBUG_MODE
  prefs.begin("relaycfg",true);
  String h=prefs.getString("host","");
  strlcpy(relayCfg.host,h.c_str(),64);
  relayCfg.port   = prefs.getInt ("port",8080);
  String t=prefs.getString("token","");
  strlcpy(relayCfg.token,t.c_str(),64);
  relayCfg.tls    = prefs.getBool("tls",false);
  relayCfg.enabled= prefs.getBool("ena",false);
  prefs.end();
#endif
  relayEnabled=relayCfg.enabled;
}

String relayGetConfigJSON(){
  DynamicJsonDocument doc(256);
  doc["host"]     = relayCfg.host;
  doc["port"]     = relayCfg.port;
  doc["token"]    = relayCfg.token;
  doc["tls"]      = relayCfg.tls;
  doc["enabled"]  = relayCfg.enabled;
  doc["connected"]= relayConnected;
  doc["deviceId"] = relayDeviceId;
  String s; serializeJson(doc,s); return s;
}

void registerRelayRoutes(){
  server.on("/api/relay/config",HTTP_GET,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    sendJSON(200,relayGetConfigJSON());
  });
  server.on("/api/relay/config",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(256);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    if(doc["host"].is<const char*>()) strlcpy(relayCfg.host,doc["host"],64);
    if(doc["port"].is<int>())        relayCfg.port   = doc["port"];
    if(doc["token"].is<const char*>()) strlcpy(relayCfg.token,doc["token"],64);
    if(doc["tls"].is<bool>())        relayCfg.tls    = doc["tls"];
    if(doc["enabled"].is<bool>()){
      relayCfg.enabled = doc["enabled"];
      relayEnabled = relayCfg.enabled;
    }
    saveRelayCfgNVS();
    sendJSON(200,relayGetConfigJSON());
  });
  server.on("/api/relay/enable",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    if(strcmp(s->role,"admin")!=0){sendError(403,"Solo admin");return;}
    DynamicJsonDocument doc(64);
    if(deserializeJson(doc,server.arg("plain"))){sendError(400,"JSON invalido");return;}
    bool en=doc["enabled"]|false;
    relayCfg.enabled=en; relayEnabled=en;
    saveRelayCfgNVS();
    if(en) relayConnect(); else relayDisconnect();
    sendJSON(200,"{\"ok\":true}");
  });
  server.on("/api/relay/connect",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    relayEnabled=true; relayConnect();
    sendJSON(200,"{\"ok\":true}");
  });
  server.on("/api/relay/disconnect",HTTP_POST,[](){
    Session* s=getAuth(); if(!s){sendError(401,"No autorizado");return;}
    relayDisconnect();
    sendJSON(200,"{\"ok\":true}");
  });
}
