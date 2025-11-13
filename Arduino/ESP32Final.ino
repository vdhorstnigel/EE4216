#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_system.h"    // esp_random()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// =================== WiFi ===================
const char* ssid     = "GalaxyZH";
const char* password = "shqt4849";

// Static IP
IPAddress local_IP(10, 117, 110, 218);
IPAddress gateway (10, 117, 110, 197);
IPAddress subnet  (255, 255, 255, 0);
IPAddress primaryDNS  (8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

// =================== Telegram ===================
const char* Telegram_Bot_Token = "8508403841:AAHozwJTKtDI7qsZwpI2k4GbfD3dntZ3T5U";
const char* Telegram_Chat_ID   = "-1003208366727";

// =================== Supabase ===================
const char* SUPABASE_URL         = "https://ljeytidzdxpjsiumsomz.supabase.co";
const char* SUPABASE_API_KEY     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxqZXl0aWR6ZHhwanNpdW1zb216Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjI3MDQ4NjQsImV4cCI6MjA3ODI4MDg2NH0.FIgYIALEQt8Esz7o6RSwy2Dhb6eRvdmOfgsEF5ezioU";
const char* BUCKET               = "camera-images";
const char* TABLE_NAME           = "detection_logs";

// Stream / snapshot endpoints (adjust if your camera differs)
const char* STREAM_URL   = "http://10.117.110.15/stream";
const char* SNAPSHOT_URL = "http://10.117.110.15/capture"; // optional trigger

// =================== Pins & HW ===================
const int BUZZER_PIN     = 5;
const int NEOPIXEL_PIN   = 38;
const int SERVO_PIN      = 14;  // door lock servo
const int PAN_SERVO_PIN  = 13;  // radar pan servo (change to your wiring)
const int TRIG_PIN       = 16;  // HC-SR04 TRIG
const int ECHO_PIN       = 17;  // HC-SR04 ECHO (level shift to 3.3V)
const int PIR_PIN        = 4;   // optional (unused here)

Adafruit_NeoPixel led(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Servo lockServo;
Servo panServo;
WebServer server(80);

// =================== Time (UTC) ===================
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.nist.gov";
static const char* NTP3 = "time.google.com";

String fmtUTC(time_t t) {
  struct tm tm_utc; gmtime_r(&t, &tm_utc);
  char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
  return String(buf) + "Z";
}
String fmtUTCISO(time_t t) {
  struct tm tm_utc; gmtime_r(&t, &tm_utc);
  char buf[25]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
  return String(buf) + "Z";
}
void syncTime() {
  configTime(0, 0, NTP1, NTP2, NTP3);
  Serial.print("Syncing time");
  int retries = 0;
  while (time(nullptr) < 1700000000 && retries < 40) { Serial.print("."); delay(500); retries++; }
  Serial.println();
  Serial.println("Time synced: " + fmtUTC(time(nullptr)));
}

// =================== UI auth ===================
const char* UI_PASS = "supersecret"; // CHANGE ME
String SESSION_TOKEN;

String randToken() {
  char buf[33];
  static const char* hex = "0123456789abcdef";
  for (int i=0;i<32;++i) buf[i] = hex[esp_random() & 0xF];
  buf[32]=0; return String(buf);
}
bool hasSession() {
  if (SESSION_TOKEN.isEmpty()) return false;
  if (!server.hasHeader("Cookie")) return false;
  String cookie = server.header("Cookie");
  int i = cookie.indexOf("session="); if (i<0) return false; i+=8;
  int j = cookie.indexOf(';', i);
  String val = (j<0) ? cookie.substring(i) : cookie.substring(i, j);
  val.trim(); return (val == SESSION_TOKEN);
}
void requireSessionOrLogin() {
  if (!hasSession()) { server.sendHeader("Location", "/login"); server.send(302, "text/plain", "Redirecting"); }
}

// =================== Events ring ===================
struct Event { time_t ts; String from; String label; float score; String raw; };
const size_t EVENT_BUF = 25;
Event events[EVENT_BUF];
size_t evt_head=0, evt_count=0;
void logEvent(const String& from, const String& label, float score, const String& raw) {
  events[evt_head] = { time(nullptr), from, label, score, raw };
  evt_head = (evt_head + 1) % EVENT_BUF;
  if (evt_count < EVENT_BUF) evt_count++;
}

// =================== Door lock helpers ===================
void setLED(int r, int g, int b) { led.setPixelColor(0, led.Color(r,g,b)); led.show(); }
void lockDoor(float)  { Serial.println(" Locked");   setLED(255,0,0);  lockServo.write(65); } //tone(BUZZER_PIN,400,500);
void unlockDoor(float){ Serial.println(" Unlocked"); setLED(0,255,0);  lockServo.write(0); delay(5000); lockDoor(0.0); } //tone(BUZZER_PIN,1000,200);

// =================== Supabase logging ===================
void uploadSupabaseLogTS(String label, float confidence, time_t ts) {
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_NAME;
  String imgURL = String(SUPABASE_URL) + "/storage/v1/object/" + BUCKET + "/Motion_" + String((long)ts) + ".jpg";

  String json = "{"
    "\"object\":\"" + label + "\","
    "\"confidence\":" + String(confidence,2) + ","
    "\"image_url\":\"" + imgURL + "\","
    "\"motion\":\"1\","
    "\"sender_ip\":\"" + WiFi.localIP().toString() + "\""
  "}";

  http.begin(url);
  http.addHeader("Content-Type","application/json");
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_API_KEY));
  http.addHeader("Prefer","return=representation");
  int code = http.POST(json);
  Serial.print("Supabase log -> "); Serial.println(code);
  if (code>0) Serial.println(http.getString());
  http.end();
}

// =================== Telegram ===================
String jsonEscape(String s){ s.replace("\\","\\\\"); s.replace("\"","\\\""); s.replace("\n","\\n"); s.replace("\r","\\r"); return s; }
bool sendTelegram(const String& text){
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String("https://api.telegram.org/bot") + Telegram_Bot_Token + "/sendMessage";
  if (!https.begin(client, url)) { Serial.println("TG begin() fail"); return false; }
  https.addHeader("Content-Type","application/json");
  String payload = String("{\"chat_id\":\"") + Telegram_Chat_ID + "\",\"text\":\"" + jsonEscape(text) + "\"}";
  int code = https.POST(payload);
  Serial.printf("Telegram -> %d\n", code);
  if (code>0) Serial.println(https.getString());
  https.end(); return (code==200);
}

// =================== HTTP handlers ===================
void handleLoginForm(){
  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:system-ui;margin:24px;max-width:420px}"
    "form{display:flex;gap:8px}input{flex:1;padding:10px;border-radius:8px;border:1px solid #bbb}"
    "button{padding:10px 14px;border-radius:8px;border:1px solid #999;background:#f6f6f6;cursor:pointer}"
    "button:active{transform:translateY(1px)}</style></head><body>"
    "<h1>Login</h1><form method='POST' action='/login'>"
    "<input type='password' name='p' placeholder='Password' autofocus>"
    "<button type='submit'>Login</button></form></body></html>";
  server.send(200,"text/html",html);
}
void handleLoginPost(){
  if (!server.hasArg("p")) { server.send(400,"text/plain","Bad Request"); return; }
  if (server.arg("p") != UI_PASS) { server.send(401,"text/plain","Wrong password"); return; }
  SESSION_TOKEN = randToken();
  server.sendHeader("Set-Cookie","session="+SESSION_TOKEN+"; HttpOnly; SameSite=Lax");
  server.sendHeader("Location","/"); server.send(302,"text/plain","Logged in");
}
void handleLogout(){
  SESSION_TOKEN="";
  server.sendHeader("Set-Cookie","session=; Max-Age=0; HttpOnly; SameSite=Lax");
  server.sendHeader("Location","/login"); server.send(302,"text/plain","Logged out");
}
void handleRoot(){
  requireSessionOrLogin(); if (!hasSession()) return;
  String html =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:system-ui;margin:16px}.grid{display:grid;grid-template-columns:1fr;gap:16px;max-width:980px}"
    ".card{border:1px solid #ccc;border-radius:12px;padding:12px}img{max-width:100%;height:auto;border-radius:8px;border:1px solid #ddd}"
    "button{padding:10px 14px;border-radius:10px;border:1px solid #999;background:#f6f6f6;cursor:pointer}button:active{transform:translateY(1px)}"
    "#status{font-weight:600}a{color:#06c}</style>"
    "<script>function cmd(p){fetch(p,{method:'POST'}).then(r=>r.text()).then(t=>document.getElementById('status').textContent=t).catch(_=>document.getElementById('status').textContent='Command failed');}</script>"
    "</head><body><h1>ESP32 Smart Door + Radar</h1><p><a href='/logout'>Logout</a></p><div class='grid'>"
    "<div class='card'><h2>Live Stream</h2><p><a href='" + String(STREAM_URL) + "' target='_blank'>Open stream</a></p>"
    "<img src='" + String(STREAM_URL) + "' onerror=\"this.alt='Stream unreachable';\"></div>"
    "<div class='card'><h2>Door Controls</h2><p><button onclick=\"cmd('/open')\">Open</button> <button onclick=\"cmd('/close')\">Close</button></p><p id='status'>Idle</p></div>"
    "<div class='card'><h2>Status</h2><p>IP: " + WiFi.localIP().toString() + "</p><p>UTC: " + fmtUTC(time(nullptr)) + "</p>"
    "<p><a href='/events'>View recent events</a></p></div></div></body></html>";
  server.send(200,"text/html",html);
}
void handleEvents(){
  requireSessionOrLogin(); if (!hasSession()) return;
  String html =
    "<!doctype html><html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'>"
    "<style>body{font-family:system-ui;margin:16px}table{border-collapse:collapse}th,td{border:1px solid #ccc;padding:6px 8px;font-size:14px}</style></head><body>"
    "<h1>Recent /detect events</h1><p><a href='/'>Home</a> | <a href='/logout'>Logout</a></p><table><tr><th>#</th><th>time (UTC)</th><th>from</th><th>label</th><th>score</th><th>raw</th></tr>";
  for (size_t i=0;i<evt_count;++i){
    int idx = (int(evt_head) - 1 - (int)i + (int)EVENT_BUF) % (int)EVENT_BUF;
    const Event& e = events[idx];
    html += "<tr><td>"+String(i+1)+"</td><td>"+fmtUTC(e.ts)+"</td><td>"+e.from+"</td><td>"+e.label+"</td><td>"+String(e.score,2)+"</td><td>"+e.raw+"</td></tr>";
  }
  html += "</table></body></html>";
  server.send(200,"text/html",html);
}
void handleDetection(){
  if (!server.hasArg("plain")) { server.send(400,"text/plain","Invalid Request"); return; }
  String body = server.arg("plain"); body.trim();
  String label=body; float score=0.0f; int k=body.indexOf(',');
  if (k!=-1){ label=body.substring(0,k); score=body.substring(k+1).toFloat(); }
  label.trim(); label.toLowerCase();
  String from = server.client().remoteIP().toString();
  logEvent(from,label,score,body);
  String msg = "Door event\nTime: " + fmtUTC(time(nullptr)) + "\nFrom: " + from + "\nLabel: " + label + "\nScore: " + String(score,2);
  sendTelegram(msg);
  if (label=="authorized") { unlockDoor(score); server.send(200,"text/plain","Access Granted"); }
  else                    { lockDoor(score);   server.send(200,"text/plain","Access Denied"); }
}

// optional GET helpers
void handleOpen(){ requireSessionOrLogin(); if (!hasSession()) return; unlockDoor(1.0); server.send(200,"text/plain","Opened"); }
void handleClose(){requireSessionOrLogin(); if (!hasSession()) return; lockDoor(0.0);   server.send(200,"text/plain","Closed"); }

// =================== Radar config ===================
// Pan servo calibration & limits
const int SERVO_MIN_US       = 950;
const int SERVO_MAX_US       = 2000;
const int SWEEP_MIN_ANGLE    = 15;
const int SWEEP_MAX_ANGLE    = 165;

// Thresholds (enter/exit with hysteresis)
const float DETECT_ON_CM     = 60.0f;
const float DETECT_OFF_CM    = 70.0f;
const int   OUT_RELEASE_COUNT= 3;

// Motion tuning
const int SERVO_STEP_DEG     = 2;
const int SERVO_STEP_INTERVAL= 5;    // ms
const int SERVO_SETTLE_MS    = 50;   // ms after arrive before ping

// Ultrasonic timing
const int  DEGREES_PER_PING  = 6;
const int  PING_GAP_MS       = 60;
const int  ECHO_TIMEOUT_US   = 30000;
const float DIST_MIN_CM      = 5.0f;
const float DIST_MAX_CM      = 300.0f;

// Radar state (0=SWEEP, 1=HOLD)
volatile int   radar_state = 0;
int   pan_angle       = 90;
int   pan_dir         = +1;
int   last_meas_angle = -1000;
bool  settling        = false;
int   outCount        = 0;
unsigned long lastStepMs=0, arriveMs=0, lastPingMs=0;

// =================== Motion queue (NetTask) ===================
struct MotionEvent { time_t ts; int angle; float dist; };
QueueHandle_t motionQ = nullptr;

// =================== Utilities ===================
int clampAngle(int a){ if (a<SWEEP_MIN_ANGLE) return SWEEP_MIN_ANGLE; if (a>SWEEP_MAX_ANGLE) return SWEEP_MAX_ANGLE; return a; }
String urlEncode(const String& s){
  static const char *hex="0123456789ABCDEF";
  String out; out.reserve(s.length()*3);
  for (size_t i=0;i<s.length();++i){ uint8_t c=(uint8_t)s[i];
    if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=(char)c;
    else { out+='%'; out+=hex[(c>>4)&0xF]; out+=hex[c&0xF]; }
  } return out;
}
void stepToward(int target){
  if (millis() - lastStepMs < (unsigned long)SERVO_STEP_INTERVAL) return;
  lastStepMs = millis();
  target = clampAngle(target);
  int delta = target - pan_angle;
  if (delta>0) pan_angle += (delta > SERVO_STEP_DEG ? SERVO_STEP_DEG : delta);
  else if (delta<0) pan_angle -= ((-delta) > SERVO_STEP_DEG ? SERVO_STEP_DEG : -delta);
  pan_angle = clampAngle(pan_angle);
  panServo.write(pan_angle);
  if (pan_angle == target){ if (!settling){ settling=true; arriveMs=millis(); } }
  else settling=false;
}
long echoUs(uint32_t timeout_us=ECHO_TIMEOUT_US){
  unsigned long now=millis();
  if (now - lastPingMs < (unsigned long)PING_GAP_MS) vTaskDelay(pdMS_TO_TICKS(PING_GAP_MS - (now - lastPingMs)));
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, timeout_us);
  lastPingMs = millis();
  return us; // 0 on timeout
}
float distOnceCm(){
  long us = echoUs();
  if (us<=0) return NAN;
  float cm = (us * 0.0343f) / 2.0f;
  if (cm < DIST_MIN_CM || cm > DIST_MAX_CM) return NAN;
  return cm;
}
float distMedian3(){
  float a=distOnceCm(), b=distOnceCm(), c=distOnceCm();
  float arr[3]={a,b,c};
  for(int i=0;i<3;i++) for(int j=i+1;j<3;j++){
    if(!isnan(arr[j]) && (isnan(arr[i]) || arr[j]<arr[i])){ float t=arr[i]; arr[i]=arr[j]; arr[j]=t; }
  }
  if (!isnan(arr[1])) return arr[1];
  if (!isnan(arr[0])) return arr[0];
  return NAN;
}
bool shouldMeasureNow(){
  if (!settling) return false;
  if (millis() - arriveMs < (unsigned long)SERVO_SETTLE_MS) return false;
  if (abs(pan_angle - last_meas_angle) < DEGREES_PER_PING) return false;
  last_meas_angle = pan_angle; return true;
}

// =================== NetTask: send GET + Supabase ===================
void notifyMotionTS(time_t ts, int angle_deg, float distance_cm){
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String iso = fmtUTCISO(ts);
  String image_url = String(SUPABASE_URL) + "/storage/v1/object/" + BUCKET + "/Motion_" + String((long)ts) + ".jpg";

  String url = "http://10.117.110.15:8080/motion";
  url += "?device=ESP32_RADAR";
  url += "&ts=" + String((long)ts);
  url += "&time=" + iso;
  url += "&angle=" + String(angle_deg);
  url += "&distance_cm=" + String(distance_cm,1);
  url += "&image_url=" + urlEncode(image_url);

  Serial.println(String("[NetTask] GET ")+url);
  if (http.begin(url)){ int code=http.GET(); Serial.printf("[NetTask] -> HTTP %d\n", code);
    if (code>0){ String resp=http.getString(); if (resp.length()) Serial.println(resp); }
    http.end();
  } else Serial.println("[NetTask] begin() failed");

  // Log to Supabase with same ts-based snapshot path
  uploadSupabaseLogTS("ultrasonic-motion", distance_cm, ts);

  // Optional: ping camera snapshot endpoint once
  if (SNAPSHOT_URL && *SNAPSHOT_URL){
    HTTPClient snap;
    if (snap.begin(SNAPSHOT_URL)){ int sc=snap.GET(); Serial.printf("[NetTask] Snapshot -> %d\n", sc); snap.end(); }
  }
}

void NetTask(void*){
  MotionEvent ev;
  for(;;){
    if (xQueueReceive(motionQ, &ev, portMAX_DELAY) == pdTRUE){
      notifyMotionTS(ev.ts, ev.angle, ev.dist);
    }
  }
}

// =================== RadarTask: sweep/hold, enqueue once ===================
void RadarTask(void*){
  bool sentForThisHold = false;
  for(;;){
    if (radar_state == 0){ // SWEEP
      int next = pan_angle + (pan_dir * SERVO_STEP_DEG);
      if (next >= SWEEP_MAX_ANGLE){ next = SWEEP_MAX_ANGLE; pan_dir = -1; }
      if (next <= SWEEP_MIN_ANGLE){ next = SWEEP_MIN_ANGLE; pan_dir = +1; }
      stepToward(next);

      if (shouldMeasureNow()){
        float cm = distMedian3();
        if (!isnan(cm)){
           //Serial.printf("[SWEEP] ang=%d, d=%.1f\n", pan_angle, cm);
          if (cm <= DETECT_ON_CM){
            radar_state = 1;
            sentForThisHold = false;
            outCount = 0;
            // Serial.println("-> HOLD");
          }
        }
      }
    } else { // HOLD
      float cm = distMedian3();
      if (!isnan(cm)){
         Serial.printf("[HOLD ] ang=%d, d=%.1f\n", pan_angle, cm);
        if (!sentForThisHold && cm <= DETECT_ON_CM){
          MotionEvent ev{ time(nullptr), pan_angle, cm };
          xQueueSend(motionQ, &ev, 0);       // one-shot enqueue
          sentForThisHold = true;
        }
        bool out = (cm > DETECT_OFF_CM);
        if (out){
          if (++outCount >= OUT_RELEASE_COUNT){ radar_state=0; /* Serial.println("<- SWEEP"); */ }
        } else outCount = 0;
      } else {
        if (++outCount >= OUT_RELEASE_COUNT){ radar_state=0; /* Serial.println("<- SWEEP NaN"); */ }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1)); // yield
  }
}

// =================== Setup / Loop ===================
void setup(){
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  led.begin(); led.clear(); led.show();

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  lockServo.setPeriodHertz(50);
  bool okLock = lockServo.attach(SERVO_PIN, 544, 2450);
  Serial.printf("lockServo attach=%d\n", okLock);
  delay(300);
  lockDoor(0.0);

  panServo.setPeriodHertz(50);
  bool okPan = panServo.attach(PAN_SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  Serial.printf("panServo attach=%d\n", okPan);
  pan_angle = clampAngle(90);
  panServo.write(pan_angle);
  radar_state = 0; // start sweeping

  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED){ Serial.print("."); delay(400); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  syncTime();

  // headers for cookie
  const char* headerKeys[] = { "Cookie" };
  server.collectHeaders(headerKeys, 1);

  server.on("/login",  HTTP_GET,  handleLoginForm);
  server.on("/login",  HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET,  handleLogout);
  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/events", HTTP_GET,  handleEvents);
  server.on("/open",   HTTP_POST, handleOpen);
  server.on("/close",  HTTP_POST, handleClose);
  server.on("/open",   HTTP_GET,  handleOpen);
  server.on("/close",  HTTP_GET,  handleClose);
  server.on("/detect", HTTP_POST, handleDetection);

  server.begin();
  Serial.println("HTTP server started");

  // Create queue and tasks
  motionQ = xQueueCreate(8, sizeof(MotionEvent));
  // Pin NetTask to Core 0 (WiFi), RadarTask to Core 1 (app)
  xTaskCreatePinnedToCore(NetTask,   "NetTask",   6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(RadarTask, "RadarTask", 4096, nullptr, 2, nullptr, 1);
}

void loop(){
  server.handleClient();
}

