#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DMD32Plus.h>
#include "fonts/SystemFont5x7.h"
#include "fonts/Arial_black_16.h"

/*----------------------------- WiFi credentials -----------------------------*/
const char *ssid = "ssid";
const char *password = "password";

#define DISPLAYS_ACROSS 1
#define DISPLAYS_DOWN   1
#define PANEL_W (32 * DISPLAYS_ACROSS)
#define PANEL_H (16 * DISPLAYS_DOWN)
#define MAX_CLIENTS 8          // a 9th connection disconnects everyone
#define MAX_TRACK   16         // tracking array size (room for transient overflow)

#define BUZZER_PIN  25         // pulses HIGH 100ms on each client connect
const unsigned long BUZZER_PULSE_MS = 100;

/*------------------------- BRIGHTNESS (LEDC PWM on OE, pot-controlled) ----------*/
volatile uint8_t brightness = 10;      // current level; driven by the potentiometer
#define LEDC_FREQ_HZ   20000   // 20 kHz, above scan rate -> no visible flicker
#define LEDC_RES_BITS  8       // 8-bit -> duty 0..255 maps directly to brightness

#define POT_PIN         35     // ADC1 input-only pin for the potentiometer wiper
#define BRIGHTNESS_MIN  5
#define BRIGHTNESS_MAX  255
const unsigned long POT_READ_MS = 50;   // how often to sample the pot

/*------------------------- GLOBALS -------------------------*/
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);
hw_timer_t *timer = NULL;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

enum Mode { BOOT, PIXEL, SCROLL, RANDOM };
volatile Mode currentMode = BOOT;
int lastBroadcastMode = -1;

unsigned long pixelPhaseStart = 0;
const unsigned long PIXEL_PHASE_MS = 30000;   // 30 seconds

unsigned long randomPhaseStart = 0;
const unsigned long RANDOM_PHASE_MS = 5000;   // 5 seconds
unsigned long lastRandomStep = 0;
const unsigned long RANDOM_STEP_MS = 500;     // how often pixels re-randomize

bool pixelBuffer[PANEL_H][PANEL_W];

struct PixelCmd { uint8_t x; uint8_t y; uint8_t val; };
QueueHandle_t pixelQueue;

struct ScrollMsg { char text[256]; };
QueueHandle_t scrollQueue;

struct WsMsg { char text[200]; };
QueueHandle_t msgQueue;

QueueHandle_t syncQueue;    // client ids needing a full state sync
QueueHandle_t closeQueue;   // client ids to disconnect

// Tracked connected client ids (touched only in the WS event task).
uint32_t clientIds[MAX_TRACK];
int clientIdCount = 0;

char scrollText[256]       = "Hello from ESP32 DMD!";
char activeScrollText[256] = "";
volatile bool clearRequested  = false;
volatile bool invertRequested = false;

// Buzzer pulse state
volatile bool buzzerRequested = false;
unsigned long buzzerStart = 0;
bool buzzerActive = false;

// Pot sampling
unsigned long lastPotRead = 0;

unsigned long lastScrollStep = 0;
const unsigned long SCROLL_STEP_MS = 100;

/*------------------------- DMD SCAN (timer ISR -> task) -------------------------*/
TaskHandle_t scanTaskHandle = NULL;

void IRAM_ATTR triggerScan() {
  BaseType_t hpw = pdFALSE;
  vTaskNotifyGiveFromISR(scanTaskHandle, &hpw);
  if (hpw == pdTRUE) portYIELD_FROM_ISR();
}

void scanTask(void *pv) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ledcWrite(PIN_DMD_nOE, 0);  
    dmd.scanDisplayBySPI();
    ledcWrite(PIN_DMD_nOE, brightness);
  }
}

/*------------------------- HELPERS -------------------------*/
void updateBrightnessFromPot() {
  if (millis() - lastPotRead < POT_READ_MS) return;
  lastPotRead = millis();

  int raw = analogRead(POT_PIN);                 // 0..4095 on ESP32
  uint8_t b = map(raw, 0, 4095, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

  // Only update on a meaningful change to avoid ADC-jitter flicker.
  if (abs((int)b - (int)brightness) >= 2) {
    brightness = b;
  }
}

const char *modeName(Mode m) {
  switch (m) {
    case BOOT:   return "STARTUP";
    case PIXEL:  return "PIXEL DRAWING";
    case SCROLL: return "SCROLLING TEXT";
    case RANDOM: return "RANDOM PIXELS";
  }
  return "UNKNOWN";
}

bool pixelBufferEmpty() {
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      if (pixelBuffer[y][x]) return false;
  return true;
}

void renderPixelBuffer() {
  dmd.clearScreen(true);
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      if (pixelBuffer[y][x])
        dmd.writePixel(x, y, GRAPHICS_NORMAL, 1);
}

void enterBootMode() {
  currentMode = BOOT;
  dmd.clearScreen(true);
  dmd.selectFont(Arial_Black_16);

  String ip = "IP " + WiFi.localIP().toString();
  ip.toCharArray(activeScrollText, sizeof(activeScrollText));

  dmd.drawMarquee(activeScrollText, strlen(activeScrollText), PANEL_W - 1, 0);
  lastScrollStep = millis();
}

void enterScrollMode() {
  currentMode = SCROLL;
  dmd.clearScreen(true);
  dmd.selectFont(Arial_Black_16);

  strncpy(activeScrollText, scrollText, sizeof(activeScrollText) - 1);
  activeScrollText[sizeof(activeScrollText) - 1] = '\0';
  if (strlen(activeScrollText) == 0)
    strcpy(activeScrollText, " ");

  dmd.drawMarquee(activeScrollText, strlen(activeScrollText), PANEL_W - 1, 0);
  lastScrollStep = millis();
}

void enterPixelPhase() {
  if (pixelBufferEmpty()) { enterScrollMode(); return; }
  currentMode = PIXEL;
  renderPixelBuffer();
  pixelPhaseStart = millis();
}

void enterRandomMode() {
  currentMode = RANDOM;
  dmd.clearScreen(true);
  randomPhaseStart = millis();
  lastRandomStep = 0;   // forces an immediate first randomize
}

void stepScroll() {
  if (millis() - lastScrollStep >= SCROLL_STEP_MS) {
    lastScrollStep = millis();
    if (dmd.stepMarquee(-1, 0))
      enterRandomMode();
  }
}

// Flicker every pixel on/off. Does NOT modify pixelBuffer (drawing is preserved).
void stepRandom() {
  if (millis() - lastRandomStep >= RANDOM_STEP_MS) {
    lastRandomStep = millis();
    for (int y = 0; y < PANEL_H; y++)
      for (int x = 0; x < PANEL_W; x++)
        dmd.writePixel(x, y, GRAPHICS_NORMAL, random(2));
  }
}

void drainPixelQueue() {
  PixelCmd cmd;
  while (xQueueReceive(pixelQueue, &cmd, 0) == pdTRUE) {
    if (cmd.x < PANEL_W && cmd.y < PANEL_H) {
      pixelBuffer[cmd.y][cmd.x] = cmd.val;
      if (currentMode == PIXEL)
        dmd.writePixel(cmd.x, cmd.y, GRAPHICS_NORMAL, cmd.val ? 1 : 0);
    }
  }
}

void drainScrollQueue() {
  ScrollMsg s;
  while (xQueueReceive(scrollQueue, &s, 0) == pdTRUE) {
    strncpy(scrollText, s.text, sizeof(scrollText) - 1);
    scrollText[sizeof(scrollText) - 1] = '\0';
  }
}

void drainMsgQueue() {
  WsMsg m;
  while (xQueueReceive(msgQueue, &m, 0) == pdTRUE)
    ws.textAll(m.text);
}

// Push current device state to one client (runs from loop -> no data race).
void sendStateToClient(uint32_t id) {
  ws.text(id, String("MODE:") + modeName(currentMode));
  ws.text(id, String("TEXT:") + scrollText);

  char pix[PANEL_W * PANEL_H + 8];
  int n = 0;
  pix[n++] = 'P'; pix[n++] = 'I'; pix[n++] = 'X'; pix[n++] = ':';
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      pix[n++] = pixelBuffer[y][x] ? '1' : '0';
  pix[n] = '\0';
  ws.text(id, String(pix));
}

void drainSyncQueue() {
  uint32_t id;
  while (xQueueReceive(syncQueue, &id, 0) == pdTRUE)
    sendStateToClient(id);
}

void drainCloseQueue() {
  uint32_t id;
  while (xQueueReceive(closeQueue, &id, 0) == pdTRUE)
    ws.close(id);
}

void queueMsg(const char *m) {
  WsMsg msg;
  strncpy(msg.text, m, sizeof(msg.text) - 1);
  msg.text[sizeof(msg.text) - 1] = '\0';
  xQueueSend(msgQueue, &msg, 0);
}

// Remove an id from the tracking array.
void removeClientId(uint32_t id) {
  for (int i = 0; i < clientIdCount; i++) {
    if (clientIds[i] == id) {
      clientIds[i] = clientIds[--clientIdCount];
      return;
    }
  }
}

/*------------------------- WEBSOCKET -------------------------*/
void handleWsMessage(const String &s) {
  if (s.length() == 0) return;
  char type = s.charAt(0);

  if (type == 'P') {                       // P:x,y,v
    String body = s.substring(s.indexOf(':') + 1);
    int c1 = body.indexOf(',');
    int c2 = body.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) return;
    int x = body.substring(0, c1).toInt();
    int y = body.substring(c1 + 1, c2).toInt();
    int v = body.substring(c2 + 1).toInt();
    if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H) {
      PixelCmd cmd = { (uint8_t)x, (uint8_t)y, (uint8_t)(v ? 1 : 0) };
      xQueueSend(pixelQueue, &cmd, 0);
      char m[80];
      snprintf(m, sizeof(m), "MSG:Server processed pixel drawing: (%d,%d) %s",
               x, y, v ? "ON" : "OFF");
      queueMsg(m);
    }
  } else if (type == 'S') {                // S:text
    ScrollMsg sm;
    String t = s.substring(s.indexOf(':') + 1);
    if (t.length() > 100) t = t.substring(0, 100);   // cap at 100 chars
    t.toCharArray(sm.text, sizeof(sm.text));
    xQueueSend(scrollQueue, &sm, 0);
    char m[200];
    snprintf(m, sizeof(m), "MSG:Server processed scrolling text: \"%s\"", sm.text);
    queueMsg(m);
  } else if (type == 'C') {                // clear
    clearRequested = true;
    queueMsg("MSG:Server processed pixel drawing: canvas cleared");
  } else if (type == 'I') {                // invert
    invertRequested = true;
    queueMsg("MSG:Server processed pixel drawing: inverted");
  }
}

void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    uint32_t id = client->id();
    buzzerRequested = true;            // pulse the buzzer on connect
    if (clientIdCount < MAX_TRACK) clientIds[clientIdCount++] = id;

    // Over the limit -> disconnect ALL clients (including this new one).
    if (clientIdCount > MAX_CLIENTS) {
      for (int i = 0; i < clientIdCount; i++) {
        ws.text(clientIds[i], "BYE");
        xQueueSend(closeQueue, &clientIds[i], 0);
      }
      clientIdCount = 0;
      return;
    }

    xQueueSend(syncQueue, &id, 0);

  } else if (type == WS_EVT_DISCONNECT) {
    removeClientId(client->id());

  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      String s;
      s.reserve(len + 1);
      for (size_t i = 0; i < len; i++) s += (char)data[i];
      handleWsMessage(s);
    }
  }
}

/*------------------------- WEB PAGE -------------------------*/
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 DMD Control</title>
<style>
  body{font-family:system-ui,Arial,sans-serif;margin:0;padding:16px;background:#111;color:#eee;
       display:flex;flex-direction:column;align-items:center}
  .wrap{width:100%;max-width:680px;text-align:center}
  h1{font-size:18px;margin:0 0 12px}
  .card{background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:14px;margin-bottom:14px;text-align:center}
  .mode{font-weight:700;color:#4ade80}
  #grid{display:grid;grid-template-columns:repeat(32,1fr);gap:1px;max-width:640px;margin:0 auto;background:#000;
        border:2px solid #444;border-radius:6px;padding:2px;touch-action:manipulation}
  .cell{aspect-ratio:1;background:#222;border-radius:1px;cursor:pointer}
  .cell.on{background:#ff0000;box-shadow:0 0 3px #ff0000}
  .cell:hover,.cell.on:hover{background:#ffb066}
  input[type=text]{padding:8px;border-radius:6px;border:1px solid #444;background:#000;color:#eee;flex:1;min-width:120px;max-width:360px}
  button{padding:8px 14px;border:0;border-radius:6px;background:#3b82f6;color:#fff;cursor:pointer;font-weight:600}
  button:hover{background:#60a5fa}
  button.alt{background:#444}
  button.alt:hover{background:#5a5a5a}
  #msg{margin-top:10px;min-height:20px;color:#fbbf24;font-size:14px}
  #conn{font-size:12px;color:#888;margin-top:4px}
  .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;justify-content:center}
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP32 32x16 DMD Control</h1>

  <div class="card">
    Current DMD mode: <span id="mode" class="mode">...</span>
    <div id="conn">connecting...</div>
  </div>

  <div class="card">
    <strong>Pixel drawing</strong>
    <p style="font-size:13px;color:#aaa;margin:8px 0">Tap a cell to toggle that pixel on the panel.</p>
    <div id="grid"></div>
    <div class="row" style="margin-top:12px">
      <button class="alt" onclick="clearGrid()">Clear</button>
      <button class="alt" onclick="invertGrid()">Invert</button>
    </div>
  </div>

  <div class="card">
    <strong>Scrolling text</strong>
    <div style="font-size:12px;color:#aaa;margin-top:6px"><span id="charcount">0</span>/100 characters</div>
    <div class="row" style="margin-top:8px">
      <input id="txt" type="text" maxlength="100" placeholder="Type text to scroll...">
      <button onclick="sendScroll()">Send</button>
    </div>
  </div>

  <div id="msg"></div>
</div>

<script>
  const W=32,H=16;
  const grid=document.getElementById('grid');
  const cells=[];
  for(let y=0;y<H;y++){for(let x=0;x<W;x++){
    const c=document.createElement('div');
    c.className='cell';c.dataset.x=x;c.dataset.y=y;
    c.addEventListener('click',()=>toggle(c));
    grid.appendChild(c);cells.push(c);
  }}

  const txt=document.getElementById('txt');
  const charcount=document.getElementById('charcount');
  txt.addEventListener('input',()=>{
    if(txt.value.length>100)txt.value=txt.value.slice(0,100);
    charcount.textContent=txt.value.length;
  });

  let ws, kicked=false;
  function connect(){
    kicked=false;
    ws=new WebSocket('ws://'+location.host+'/ws');
    ws.onopen=()=>{document.getElementById('conn').textContent='connected';};
    ws.onclose=()=>{
      if(kicked){document.getElementById('conn').textContent='disconnected - max clients reached refresh page';return;}
      document.getElementById('conn').textContent='disconnected - retrying';
      setTimeout(connect,1000);
    };
    ws.onmessage=e=>{
      const d=e.data;
      if(d==='BYE'){kicked=true;ws.close();return;}
      if(d.startsWith('MODE:'))document.getElementById('mode').textContent=d.slice(5);
      else if(d.startsWith('MSG:'))showMsg(d.slice(4));
      else if(d.startsWith('TEXT:')){txt.value=d.slice(5);charcount.textContent=txt.value.length;}
      else if(d.startsWith('PIX:'))applyPix(d.slice(4));
    };
  }
  connect();

  function send(s){if(ws&&ws.readyState===1)ws.send(s);else showMsg('Not connected');}
  function toggle(c){const on=c.classList.toggle('on');send('P:'+c.dataset.x+','+c.dataset.y+','+(on?1:0));}
  function sendScroll(){
    let v=document.getElementById('txt').value;
    if(v.length>100){showMsg('User entered more than 100 characters');v=v.slice(0,100);}
    send('S:'+v);
  }
  function clearGrid(){cells.forEach(c=>c.classList.remove('on'));send('C');}
  function invertGrid(){cells.forEach(c=>c.classList.toggle('on'));send('I');}
  function applyPix(s){for(let i=0;i<cells.length&&i<s.length;i++){
    if(s[i]==='1')cells[i].classList.add('on');else cells[i].classList.remove('on');}}

  let msgTimer;
  function showMsg(m){
    const el=document.getElementById('msg');
    el.textContent=m;
    clearTimeout(msgTimer);
    msgTimer=setTimeout(()=>{el.textContent='';},5000);
  }
</script>
</body>
</html>
)rawliteral";

/*------------------------- SETUP -------------------------*/
void setup() {
  Serial.begin(115200);

  pinMode(PIN_OTHER_SPI_nCS, OUTPUT);
  digitalWrite(PIN_OTHER_SPI_nCS, HIGH);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  xTaskCreatePinnedToCore(scanTask, "dmdScan", 4096, NULL,
                          configMAX_PRIORITIES - 1, &scanTaskHandle, 1);

  timer = timerBegin(1000000L);
  timerAttachInterrupt(timer, &triggerScan);
  timerAlarm(timer, 1000, true, 0);
  dmd.clearScreen(true);

  analogReadResolution(12);                      // 0..4095
  analogSetPinAttenuation(POT_PIN, ADC_11db);    // full ~0..3.3V range
  ledcAttach(PIN_DMD_nOE, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcWrite(PIN_DMD_nOE, brightness);

  randomSeed(esp_random());   // seed RNG for random mode

  pixelQueue  = xQueueCreate(256, sizeof(PixelCmd));
  scrollQueue = xQueueCreate(4,   sizeof(ScrollMsg));
  msgQueue    = xQueueCreate(16,  sizeof(WsMsg));
  syncQueue   = xQueueCreate(12,  sizeof(uint32_t));
  closeQueue  = xQueueCreate(12,  sizeof(uint32_t));
  memset(pixelBuffer, 0, sizeof(pixelBuffer));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("DMD web UI: http://");
  Serial.println(WiFi.localIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send_P(200, "text/html", INDEX_HTML);
  });
  server.onNotFound([](AsyncWebServerRequest *req){
    req->send(404, "text/plain", "Not found");
  });
  server.begin();

  enterBootMode();   // scroll the IP once before the normal cycle
}

/*------------------------- LOOP -------------------------*/
void loop() {
  ws.cleanupClients(MAX_TRACK);
  updateBrightnessFromPot();
  drainPixelQueue();
  drainScrollQueue();
  drainMsgQueue();
  drainSyncQueue();
  drainCloseQueue();

  // Buzzer pulse (non-blocking): HIGH for BUZZER_PULSE_MS on client connect.
  if (buzzerRequested) {
    buzzerRequested = false;
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerStart = millis();
    buzzerActive = true;
  }
  if (buzzerActive && millis() - buzzerStart >= BUZZER_PULSE_MS) {
    buzzerActive = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  if (clearRequested) {
    clearRequested = false;
    memset(pixelBuffer, 0, sizeof(pixelBuffer));
    if (currentMode == PIXEL) dmd.clearScreen(true);
  }

  if (invertRequested) {
    invertRequested = false;
    for (int y = 0; y < PANEL_H; y++)
      for (int x = 0; x < PANEL_W; x++)
        pixelBuffer[y][x] = !pixelBuffer[y][x];
    if (currentMode == PIXEL) renderPixelBuffer();
  }

  switch (currentMode) {
    case BOOT:
      if (millis() - lastScrollStep >= SCROLL_STEP_MS) {
        lastScrollStep = millis();
        if (dmd.stepMarquee(-1, 0))   // IP has fully scrolled off
          enterPixelPhase();
      }
      break;
    case PIXEL:
      if (pixelBufferEmpty() || millis() - pixelPhaseStart >= PIXEL_PHASE_MS)
        enterScrollMode();
      break;
    case SCROLL:
      stepScroll();
      break;
    case RANDOM:
      stepRandom();
      if (millis() - randomPhaseStart >= RANDOM_PHASE_MS)
        enterPixelPhase();
      break;
  }

  if ((int)currentMode != lastBroadcastMode) {
    lastBroadcastMode = (int)currentMode;
    ws.textAll(String("MODE:") + modeName(currentMode));
  }
}