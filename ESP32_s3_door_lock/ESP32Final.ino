// ============================================================================
// ESP32 Smart Door + Radar System
// This sketch runs on the ESP32-S3 (lock controller) and does the following:
//
// - Connects to Wi-Fi with a static IP and exposes an HTTP web dashboard.
// - Protects the dashboard with a simple password login with session cookie.
// - Shows a live video stream from a separate ESP32-S3-EYE camera
//   (STREAM_URL /stream) and can trigger still snapshots (/capture).
// - Drives a door lock servo (SERVO_PIN) with lock/unlock helpers and an
//   RGB NeoPixel status LED (red = locked, green = unlocked).
// - PIR is used to wake up the radar system
// - Implements a radar module using a panning servo (PAN_SERVO_PIN) and
//   an HC-SR04 ultrasonic sensor (TRIG/ECHO) to detect nearby motion.
//   * RadarTask (FreeRTOS) sweeps the servo, measures distance, and enqueues
//     motion events when an object is detected within DETECT_ON_CM.
//   * NetTask (FreeRTOS) dequeues motion events and:
//       - Notifies a backend via HTTP GET (/motion on 10.117.110.15:8080),
//       - Logs events to Supabase ,
//       - triggers a camera snapshot via SNAPSHOT_URL.
// - Receives face-recognition decisions from the ESP32-S3-EYE on /detect:
//   * "authorized"->   unlock door, log to Supabase, send Telegram alert.
//   * anything else -> lock door, log to Supabase, send Telegram alert "denyed".
// - Supports manual Open/Close buttons on the web UI:
//   * /open and /close endpoints update the servo, log to Supabase,
//     send Telegram notifications, and record entries in an in-memory
//     ring buffer of events.
// - Maintains a ring buffer of recent events (Event[]), and displays them
//   on the /events page with periodic auto-refresh.
// - Uses NTP to keep time in UTC so all logs (Supabase + Telegram + HTML)
//   are timestamped consistently.
//
// In short: this is the main IoT controller for a facial-recognition smart
// door lock with radar-based motion detection, cloud logging, and a secured
// web dashboard on the local network for extra security.
// ==========================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// WIFI configurations ===============================================================================
const char* ssid     = "------";
const char* password = "------";

// Static IP=========================================================================================
IPAddress local_IP(10, 117, 110, 218);
IPAddress gateway (10, 117, 110, 197);
IPAddress subnet  (255, 255, 255, 0);
IPAddress primaryDNS  (8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

// Telegram bot Token and ID =====================================================================
const char* Telegram_Bot_Token = "8508403841:AAHozwJTKtDI7qsZwpI2k4GbfD3dntZ3T5U";
const char* Telegram_Chat_ID   = "-1003208366727";

// Supabase's settings for cloud logging =========================================================
const char* SUPABASE_URL         = "https://ljeytidzdxpjsiumsomz.supabase.co";
const char* SUPABASE_API_KEY     = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxqZXl0aWR6ZHhwanNpdW1zb216Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjI3MDQ4NjQsImV4cCI6MjA3ODI4MDg2NH0.FIgYIALEQt8Esz7o6RSwy2Dhb6eRvdmOfgsEF5ezioU";
const char* BUCKET               = "camera-images";
const char* TABLE_NAME           = "detection_logs";

// this is the stream URL for web embed, snapshot_url for data logging by adding a timezone behind it, as the supabase store pictures and logs in seperate space
const char* STREAM_URL   = "http://10.117.110.15/stream";
const char* SNAPSHOT_URL = "http://10.117.110.15/capture"; 

// PINs configurations ==============================================================================
const int NEOPIXEL_PIN   = 38;  // green for open and red to close 
const int SERVO_PIN      = 14;  // door lock servo
const int PAN_SERVO_PIN  = 13;  // radar pan servo (change to your wiring)
const int TRIG_PIN       = 16;  // HC-SR04 TRIG
const int ECHO_PIN       = 17;  // HC-SR04 ECHO 
const int PIR_PIN        = 4;   // used to wake up the radar

Adafruit_NeoPixel led(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Servo lockServo;
Servo panServo;
WebServer server(80);

// TIME UTC https://arduino.stackexchange.com/questions/91542/what-is-the-ideal-way-to-check-if-time-on-esp8266-via-ntp-is-ready and help with chatgpt
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

// UI that is password protected ===============================================
const char* UI_PASS = "supersecret"; 
String SESSION_TOKEN;


// esp random string generate to use as random token 
String randToken() {
  char buf[33];
  static const char* hex = "0123456789abcdef";
  for (int i=0;i<32;++i) buf[i] = hex[esp_random() & 0xF];
  buf[32]=0; 
  return String(buf);
}

// if not token means no logged in, if no cookie then no session, check session seperated by ;, if no ; means that is last cookie
bool hasSession() {
  if (SESSION_TOKEN.isEmpty()) return false;
  if (!server.hasHeader("Cookie")) return false;
  String cookie = server.header("Cookie");
  int i = cookie.indexOf("session="); if (i<0) return false; i+=8;
  int j = cookie.indexOf(';', i);
  String val = (j<0) ? cookie.substring(i) : cookie.substring(i, j);
  val.trim(); return (val == SESSION_TOKEN);
}

// so if no initial login at main page, go back to login page 
void requireSessionOrLogin() {
  if (!hasSession()) { 
    server.sendHeader("Location", "/login"); 
    server.send(302, "text/plain", "Redirecting"); 
  }
}

// this is for my web logging, the table is log for row is 
struct Event { time_t ts; 
              String from; 
              String label; 
              float score; 
              String raw; };

//i think esp have enough ram but just incase max 25 events 
const size_t EVENT_BUF = 25;
Event events[EVENT_BUF];
size_t evt_head=0, evt_count=0;

// function for log event, first write new event  then time from the UTC timestamp above, then add in the details 
void logEvent(const String& from, const String& label, float score, const String& raw) {
  events[evt_head] = { time(nullptr), from, label, score, raw };

  // if not full add more else overwirte the old one
  evt_head = (evt_head + 1) % EVENT_BUF;
  if (evt_count < EVENT_BUF) evt_count++;
}

// door lock functions =============================================
void setLED(int r, int g, int b) { 
  led.setPixelColor(0, led.Color(r,g,b)); 
  led.show(); 
  }

void lockDoor(float)  { 
  Serial.println(" Locked");   
  setLED(255,0,0); 
  lockServo.write(65); 
  } 

// unlock door then close 5secs later, can be changed easily 
void unlockDoor(float){ 
  Serial.println(" Unlocked"); 
  setLED(0,255,0);  
  lockServo.write(0); 
  delay(5000); 
  lockDoor(0.0); }

// Supabase logging ======================================================================================================


// to log into supabase
void uploadSupabaseLogTS(String label, float confidence, time_t ts) {
  WiFiClientSecure client; 
  client.setInsecure();  // supabase add cert validation for more secure but for now leave it as it is

  HTTPClient http;
  // REST point to post json rows and the other one is for img, the location for img and log are different, just supabase things
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_NAME;
  String imgURL = String(SUPABASE_URL) + "/storage/v1/object/" + BUCKET +
                  "/Motion_" + String((unsigned long)ts) + ".jpg";

  // build json payload, 1 if detected ultrasonic else 0, need to link the path for image using imgURL, so can trace back this snapshot is whose
  String payload;
  if (label == "ultrasonic-motion") {
    payload = String("{") +
      "\"object\":\"" + label + "\"," +
      "\"confidence\":" + String(confidence, 2) + "," +
      "\"image_url\":\"" + imgURL + "\"," +
      "\"motion\":\"1\"," +
      "\"sender_ip\":\"" + WiFi.localIP().toString() + "\"" +
    "}";
  } else {
    payload = String("{") +
      "\"object\":\"" + label + "\"," +
      "\"confidence\":" + String(confidence, 2) + "," +
      "\"motion\":\"0\"," +
      "\"sender_ip\":\"" + WiFi.localIP().toString() + "\"" +
    "}";
  }

  if (!http.begin(client, url)) {
    Serial.println("Supabase begin() failed");
    return;
  }

  //supabase things 
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_API_KEY));
  http.addHeader("Prefer", "return=representation");

  int code = http.POST(payload);
  Serial.printf("Supabase log -> %d\n", code);
  if (code > 0) Serial.println(http.getString());
  http.end();
}

// Standard telegram things==========================================================================
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

// HTTP handlers ================================================================================================

// login page 
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

// to handle password, correct or not, if correct store token 
void handleLoginPost(){
  if (!server.hasArg("p")) { server.send(400,"text/plain","Bad Request"); return; }
  if (server.arg("p") != UI_PASS) { server.send(401,"text/plain","Wrong password"); return; }
  SESSION_TOKEN = randToken();
  server.sendHeader("Set-Cookie","session="+SESSION_TOKEN+"; HttpOnly; SameSite=Lax");
  server.sendHeader("Location","/"); server.send(302,"text/plain","Logged in");
}

// to logout 
void handleLogout(){
  SESSION_TOKEN="";
  server.sendHeader("Set-Cookie","session=; Max-Age=0; HttpOnly; SameSite=Lax");
  server.sendHeader("Location","/login"); server.send(302,"text/plain","Logged out");
}

// this is the main login page, the very base html diamentions are generated using chatgpt and maded with alterations 
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


// explained above where so if no initial login at main page, go back to login page, refresh every 2 seconds for live update, base html generated by chatgpt with alterations
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

// we put the eye to post the following details 
void handleDetection(){
  if (!server.hasArg("plain")) { 
    server.send(400,"text/plain","Invalid Request"); 
    return; 
    }
  String body = server.arg("plain"); body.trim();
  String label=body; float score=0.0f; int k=body.indexOf(',');
  if (k!=-1){ 
    label=body.substring(0,k); 
    score=body.substring(k+1).toFloat(); 
    }
  label.trim(); 
  label.toLowerCase();

  // ip of eye that made the request
  String from = server.client().remoteIP().toString();
  logEvent(from,label,score,body);

  //ts for tele  
  time_t ts = time(nullptr);
  String msg = "Door event\nTime: " + fmtUTC(ts) + "\nFrom: " + from + "\nLabel: " + label + "\nScore: " + String(score,2);
  sendTelegram(msg);
  if (label=="authorized") { 
    unlockDoor(score); 
    uploadSupabaseLogTS("authorized", score, ts); 
    server.send(200,"text/plain","Access Granted"); }
  else { 
    lockDoor(score);  
    uploadSupabaseLogTS("denied", score, ts); 
    server.send(200,"text/plain","Access Denied"); }
}

// optional GET helpers
// void handleOpen(){ requireSessionOrLogin(); if (!hasSession()) return; unlockDoor(1.0); uploadSupabaseLogTS("Manual-Open", 1, time(nullptr)); sendTelegram("Door event\nTime: " + fmtUTC(time(nullptr)) + "\nLabel: Manual-Open\nScore: 1"); server.send(200,"text/plain","Opened"); }
// void handleClose(){requireSessionOrLogin(); if (!hasSession()) return; lockDoor(0.0); uploadSupabaseLogTS("Manual-Close", 0, time(nullptr)); sendTelegram("Door event\nTime: " + fmtUTC(time(nullptr)) + "\nLabel: Manual-Close\nScore: 0"); server.send(200,"text/plain","Closed"); }


// first protected by login, then show is manually pressed, then update supabase and telegram 
void handleOpen() {
  requireSessionOrLogin(); 
  if (!hasSession()) return;

  time_t ts = time(nullptr);
  String from = server.client().remoteIP().toString();
  logEvent(from, "Manual-Open", 1.0f, "UI button");

  unlockDoor(1.0);
  uploadSupabaseLogTS("Manual-Open", 1, ts);
  sendTelegram("Door event\nTime: " + fmtUTC(ts) + "\nLabel: Manual-Open\nScore: 1");

  server.send(200,"text/plain","Opened");
}

// first protected by login, then show is manually pressed, then update supabase and telegram 
void handleClose() {
  requireSessionOrLogin(); 
  if (!hasSession()) return;

  time_t ts = time(nullptr);
  String from = server.client().remoteIP().toString();

  logEvent(from, "Manual-Close", 0.0f, "UI button");

  lockDoor(0.0);
  uploadSupabaseLogTS("Manual-Close", 0, ts);
  sendTelegram("Door event\nTime: " + fmtUTC(ts) + "\nLabel: Manual-Close\nScore: 0");

  server.send(200,"text/plain","Closed");
}


// Radar config ==================================================================
// since sg90 only 180, following for settings 
const int SERVO_MIN_US = 950;
const int SERVO_MAX_US = 2000;
const int SWEEP_MIN_ANGLE = 15;
const int SWEEP_MAX_ANGLE = 165;

// Thresholds for distance 
const float DETECT_ON_CM     = 60.0f;
const float DETECT_OFF_CM    = 70.0f;
const int OUT_RELEASE_COUNT= 3;

// Motion tuning
const int SERVO_STEP_DEG = 2;
const int SERVO_STEP_INTERVAL= 25;  
const int SERVO_SETTLE_MS = 50;   

// Ultrasonic timing
const int DEGREES_PER_PING  = 6;
const int PING_GAP_MS = 60;
const int ECHO_TIMEOUT_US  = 30000;
const float DIST_MIN_CM = 5.0f;
const float DIST_MAX_CM = 300.0f;

// Radar state (0=SWEEP, 1=HOLD)
volatile int   radar_state = 0;
int   pan_angle      = 90;
int   pan_dir         = +1;
int   last_meas_angle = -1000;
bool  settling        = false;
int   outCount        = 0;
unsigned long lastStepMs=0, arriveMs=0, lastPingMs=0;


// Motion queue, freeRTOs for message queue 
struct MotionEvent { time_t ts; int angle; float dist; };
QueueHandle_t motionQ = nullptr;

// for the radar sweep======================================

// min max of servo sweep
int clampAngle(int a){ 
  if (a<SWEEP_MIN_ANGLE) return SWEEP_MIN_ANGLE; 
  if (a>SWEEP_MAX_ANGLE) return SWEEP_MAX_ANGLE; 
  return a; 
  }

// to make it URL safe, so url is valid
String urlEncode(const String& s){
  static const char *hex="0123456789ABCDEF";
  String out; out.reserve(s.length()*3);
  for (size_t i=0;i<s.length();++i){ uint8_t c=(uint8_t)s[i];
    if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=(char)c;
    else { out+='%'; out+=hex[(c>>4)&0xF]; out+=hex[c&0xF]; }
  } return out;
}

// bounce between min and max with a fixed step and interval
void sweepStepSimple() {
  static int dir = +1;
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  if (now - lastStep < (unsigned long)SERVO_STEP_INTERVAL) return;
  lastStep = now;

  pan_angle += dir * SERVO_STEP_DEG;
  if (pan_angle >= SWEEP_MAX_ANGLE) { pan_angle = SWEEP_MAX_ANGLE; dir = -1; }
  if (pan_angle <= SWEEP_MIN_ANGLE) { pan_angle = SWEEP_MIN_ANGLE; dir = +1; }
  panServo.write(pan_angle);
}

// Simple one-shot distance reading
float readDistanceCmSimple() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long us = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (us <= 0) return NAN;

  float cm = (us* 0.0343f) / 2.0f;
  if (cm < DIST_MIN_CM || cm > DIST_MAX_CM) return NAN;
  return cm;
}

// when motion is detected====================================================================================
//what this does is we put the EYE to snapshot when get request is sent, for url put the time of it to retreive it in the supabase
// then it log event to supabase 
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

  // ping camera snapshot endpoint once
  if (SNAPSHOT_URL && *SNAPSHOT_URL){
    HTTPClient snap;
    if (snap.begin(SNAPSHOT_URL)){ 
      int sc=snap.GET(); 
      Serial.printf("[NetTask] Snapshot -> %d\n", sc); snap.end(); 
    }
  }
}

// RTOs pin to core 0 to block motionQ
void NetTask(void*){
  MotionEvent ev;
  // oh this is interesting but for(;;) is inifinite loop which wait till event happens
  for(;;){
    if (xQueueReceive(motionQ, &ev, portMAX_DELAY) == pdTRUE){
      notifyMotionTS(ev.ts, ev.angle, ev.dist);
    }
  }
}

// RadarTask===================================
void RadarTask(void*) {
  pinMode(PIR_PIN, INPUT);

  bool radarAwake = false;   // false = sleeping, waiting for PIR
  unsigned long lastActiveMs = 0;   // last time
  bool sentForThisHold = false;   // ensure only one MotionEvent per HOLD

  for (;;) {
    unsigned long now = millis();

    // radar sleep mode till pir high====================
    if (!radarAwake) {
      // Park radar and reset state
      pan_angle = clampAngle(90);
      panServo.write(pan_angle);
      radar_state = 0;
      outCount = 0;

      int pir = digitalRead(PIR_PIN);
      if (pir == HIGH) {
        // PIR triggered then wake up 
        radarAwake = true;
        lastActiveMs = now;    // to last 10 seconds
        sentForThisHold = false;
      } else {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
    }

    // when the radar wakes up 
    if (radar_state == 0) {
      // start the sweeping written above 
      sweepStepSimple();
      float cm = readDistanceCmSimple();
      if (!isnan(cm) && cm <= DETECT_ON_CM) {
        radar_state = 1;      // go to HOLD
        sentForThisHold = false;
        outCount = 0;
        lastActiveMs = now;    // caz i want to reset timer back when presence detected 
      }
    } else {
      // HOLD status, for facial recognition and snapshot 
      float cm = readDistanceCmSimple();
      if (!isnan(cm)) {
        // if close then consider there 
        if (cm <= DETECT_OFF_CM) {
          lastActiveMs = now;          // keep extending the 10s window
        }

        if (!sentForThisHold && cm <= DETECT_ON_CM) {
          MotionEvent ev{ time(nullptr), pan_angle, cm };
          xQueueSend(motionQ, &ev, 0);   // one-shot enqueue
          sentForThisHold = true;
        }

        bool out = (cm > DETECT_OFF_CM);
        if (out) {
          if (++outCount >= OUT_RELEASE_COUNT) {
            radar_state = 0;          // lost target then go back to sweep
          }
        } else {
          outCount = 0;
        }
      } else {
        // if no reading 
        if (++outCount >= OUT_RELEASE_COUNT) {
          radar_state = 0;
        }
      }
    }

    // timer set 10 seconds 
    if (radarAwake && (now - lastActiveMs >= 10000UL)) {   
      radarAwake = false;     // go back to PIR sleep
      continue;
    }
    
    // RTOs task blocker 
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}


// =================== Setup / Loop ===================
void setup(){
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  led.begin(); led.clear(); led.show();

  // pwm for servo settings this is following is quite interesting, im allocating 4 pwm channel but using 2
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  // this set one for door lock 
  lockServo.setPeriodHertz(50);
  bool okLock = lockServo.attach(SERVO_PIN, 544, 2450);
  Serial.printf("lockServo attach=%d\n", okLock);
  delay(300);
  lockDoor(0.0);

  // set one for radar servo 
  panServo.setPeriodHertz(50);
  bool okPan = panServo.attach(PAN_SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  Serial.printf("panServo attach=%d\n", okPan);
  pan_angle = clampAngle(90);
  panServo.write(pan_angle);
  radar_state = 0; // start sweeping 

  // this is so i can hace two pwm for servo without issue and safely 

  // wifi config
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED){ Serial.print("."); delay(400); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  //time set up from the top 
  syncTime();

  // headers for cookie
  const char* headerKeys[] ={ "Cookie" };
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
  // Pin NetTask to Core 0, RadarTask to Core 1
  xTaskCreatePinnedToCore(NetTask,   "NetTask",   6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(RadarTask, "RadarTask", 8192, nullptr, 2, nullptr, 1);
}

void loop(){
  server.handleClient();



  // char buffer[500];
  // vTaskGetRunTimeStats(buffer);
  // Serial.println("--- FreeRTOS Runtime Stats ---");
  // Serial.println(buffer);
  // Serial.println("------------------------------");
  // delay(5000);
}

// chatgpt is used to help debug when attempting to add the RTOs which infact is something that is not needed in the first place
// initially it was thought that the esp32 is overloading due to a) lack off cpu and b) lack of power, but infact is the buzzer pwm!

// following is the thought process and how it was debuged
// the radar is unable to function properly when it is merge with the door lock but functioning properly when using 2 seperate esp32
// At that point of time it was thought that it was due to a) lack off cpu and b) lack of power
// but after implementing RTOs and checking the Runtime Stats, we notice that it is consuming less than 20%
// And after using multimeter checking each part, the power flow is doing great 
// thus our attention turns towards the pwm since the issue is the movement of the servo, where there is jittering 
// after reseaching and test runs, we found out that it is due to the buzzer's pwm affecting the pwm of the servo. 
// Though at that point we didnt had time to implement the buzzer
// since the servo uses 50khz which is the ESP32PWM timer, it is possible to use LEDC channel 4 for buzzer since they are seperate 

