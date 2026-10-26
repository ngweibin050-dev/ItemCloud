// ================== FORWARD DECLARATIONS (helps the IDE) ==================
struct Attempts;
struct LedEvent;
struct Box;

// ================== INCLUDES ==================
#include <Arduino.h>
#include <Wire.h>
#include "I2CKeyPad.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>

// ---------- Firebase (email/password — NOT anonymous) ----------
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ===== Serial logging switches =====
#define SERIAL_SHOW_STATUS   0   // [BOX…] status lines
#define SERIAL_SHOW_SENSORS  0   // [DBG] sensor spam
#define SERIAL_SHOW_KEYS     1   // per-keystroke
#define SERIAL_SHOW_SUBMIT   1   // when a code is submitted

// ===================== USER CONFIG =====================
// Credentials live in secrets.h (git-ignored). Copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;

#define FB_API_KEY SECRET_FB_API_KEY
#define FB_DB_URL SECRET_FB_DB_URL
#define FB_PROJECT_ID SECRET_FB_PROJECT_ID  // Firestore unused now (no device writes)

// Real user credentials (Email/Password sign-in)
#define FB_USER_EMAIL SECRET_FB_USER_EMAIL
#define FB_USER_PASSWORD SECRET_FB_USER_PASSWORD

// RTDB paths (base per box)
static const char* PATH_BOX1 = "/UnlockCodes/box1";
static const char* PATH_BOX2 = "/UnlockCodes/box2";

// ================= NTP =================
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.google.com";
bool g_timeReady = false;
uint64_t nowEpochMs() { struct timeval tv; gettimeofday(&tv, nullptr); return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL; }

// ================= Cameras (optional snapshots) =================
String CAM1_SNAP_URL = "http://esp32-cam-1.local/snap";
String CAM2_SNAP_URL = "http://esp32-cam-2.local/snap";
static const uint16_t HTTP_TIMEOUT = 25000;

// ================= I2C Keypads (two buses) =================
#define KP1_SDA  16
#define KP1_SCL  17
#define KP2_SDA  21
#define KP2_SCL  22
#define KEYPAD1_ADDR  0x20
#define KEYPAD2_ADDR  0x21
TwoWire I2C_A = TwoWire(0);
TwoWire I2C_B = TwoWire(1);
I2CKeyPad kpad1(KEYPAD1_ADDR, &I2C_A);
I2CKeyPad kpad2(KEYPAD2_ADDR, &I2C_B);
bool hasKP1=false, hasKP2=false;

// ================= Pins ================= // pin 25 telah rosak
// Box 1
#define B1_RELAY_PIN        32
#define B1_US_TRIG          26
#define B1_US_ECHO          27
#define B1_DOOR_PIN         14
// LED pins SWAPPED: green<->red
#define B1_LED_GREEN        13
#define B1_LED_RED          12

// Box 2
#define B2_RELAY_PIN        33
#define B2_US_TRIG          19
#define B2_US_ECHO          35
#define B2_DOOR_PIN         4
// LED pins SWAPPED: green<->red
#define B2_LED_GREEN        23
#define B2_LED_RED          18

// ================= Electrical behavior =================
#define RELAY_ACTIVE_LOW  true     // true: LOW=locked, HIGH=unlocked
#define IR_ACTIVE_HIGH    false    // door closed = HIGH if true; LOW if false

// Buzzers — change pins to your wiring
#define BZZR_PIN  2

// beep durations (ms)
#define ACTIVE_BUZZER
const uint16_t BEEP_SHORT_MS = 60;
const uint16_t BEEP_SUCCESS_MS[] = {120, 60, 120};  // pattern: on, pause, on
const uint8_t  BEEP_SUCCESS_COUNT = 3;
const uint16_t BEEP_FAIL_MS = 300;

// ================= Timing & Code =================
const uint8_t  CODE_LEN    = 8;
const uint32_t UNLOCK_MS   = 10000;   // unlock window
const uint16_t KEYPAD_MS   = 60;
const uint16_t DEBOUNCE_MS = 50;
static const uint32_t CODE_POLL_MS = 2000;

static const uint32_t SNAP_ONESHOT_DELAY_MS = 40000;
static const uint32_t HOLD_MS       = 5000;   // must see object for ≥ 5s with door closed
static const uint32_t SNAP_MAX_MS   = 8 * 60 * 1000; 

// ================= Ultrasonic =================
const uint16_t SONAR_MS          = 150;
const uint32_t PULSE_TIMEOUT_US  = 30000;  // slightly longer

// ================= Presence thresholds (raised to 25cm) =================
const uint16_t THRESH_ON_CM  = 19;
const uint16_t THRESH_OFF_CM = 20;

// ================= Door debounce =================
const uint16_t DOOR_DEBOUNCE_MS = 40;

// ================= Snapshot =================
const uint32_t SNAP_COOLDOWN_MS = 1500;

// ================= Key map =================
const char KEYMAP[16] = {
  '1','2','3','A',
  '4','5','6','B',
  '7','8','9','C',
  '*','0','#','D'
};

// ================= Data structures (placed BEFORE functions!) =================
const uint8_t ATTEMPT_MAX = 12;
struct Attempts {
  String code[ATTEMPT_MAX];
  bool ok[ATTEMPT_MAX];
  uint8_t count = 0;
  uint8_t head  = 0;
};

struct LedEvent {
  uint8_t  remaining = 0;   // half-cycles
  bool     isGreen   = false;
  uint32_t lastMs    = 0;
  bool     onPhase   = false;
};

struct Box {
  // pins
  int relayPin, usTrig, usEcho, doorPin, ledGreen, ledRed;

  String lastSnapItemId;
  bool   lastSnapOk = false;
  uint8_t  snapTries = 0;

  // sensors
  bool presenceUS = false;
  uint32_t presenceStartMs = 0;
  bool    snapActive  = false; 
  uint32_t snapStartedAtMs = 0;
  uint32_t lastSnapTryMs = 0; 
  String  lastSnapMsg;  
  bool doorClosed = false;
  bool doorOpenedAfterUnlock = false;   // set once door opened after a correct code
  bool waitingForRemoval = false;       // we saw open; now waiting for close+empty
  int  lastDist   = -1;
  int  doorLastRaw = -1;
  uint32_t doorLastEdgeMs = 0;

  bool presence   = false;      // fused presence (US only)
  uint32_t lastSonarMs = 0;

  // unlock window
  bool     unlockingNow = false;
  uint32_t unlockUntilMs = 0;

  // RTDB code
  String   activeCode;
  uint64_t expiredAtMs = 0;
  String   attemptId;            // provided by web in RTDB
  uint32_t lastCodePollMs = 0;
  String   codeStatus = "no_code"; // ok / no_code / fb_err:<msg>

  // keypad
  String   input;
  int      lastIdx = -1;
  int      lastHandledIdx = -1;
  uint32_t lastChangeMs = 0;
  uint32_t lastKeypadScanMs = 0;

  // snapshot
  String   camUrl;
  uint32_t lastSnapMs = 0;
  bool     snapDoneThisPresence = false;
  bool     snapPending = false;
  uint32_t snapSinceMs = 0;
  uint32_t snapTargetAtMs = 0;

  // LEDs
  LedEvent ev;
  uint32_t lastOpenBlinkMs = 0;
  bool     openBlinkOn = false;

  // RTDB base path for this box
  const char* pathBase = nullptr;

  // claims/status lifecycle
  String   status = "pending";   // pending/cancelled/expired/successful/unlocked_window/ready/locked
  bool     claimArmed = false;   // set after correct code; cleared after success
  bool     doorWasOpen = false;  // for edge detection
  uint32_t lastUnlockByCodeMs = 0;
};

bool triggerSnap(const String& url, String& outItemId, String& outMsg) {
  outItemId = ""; outMsg = "";
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(HTTP_TIMEOUT);
  if (!http.begin(url)) { outMsg = "begin_fail"; return false; }
  int code = http.GET();
  if (code != 200) { outMsg = String("http_") + code; http.end(); return false; }
  String body = http.getString();
  http.end();

  // very light parse; accept either {"ok":true,"itemID":"I019"} or {"ok":true,"itemId":"I019"}
  bool ok = body.indexOf("\"ok\":true") >= 0;
  if (!ok) { outMsg = "ok:false"; return false; }
  int k = body.indexOf("\"itemID\"");
  if (k < 0) k = body.indexOf("\"itemId\"");
  if (k >= 0) {
    int q1 = body.indexOf('"', body.indexOf(':', k) + 1) + 1;
    int q2 = body.indexOf('"', q1);
    if (q1 > 0 && q2 > q1) outItemId = body.substring(q1, q2);
  }
  outMsg = "ok";
  return true;
}


static bool ntpOk = false;

static void syncTimeOnce() {
  Serial.println("Configuring NTP: local 192.168.43.171 + google + pool...");
  configTime(8 * 3600, 0, "192.168.43.171", "time.google.com", "pool.ntp.org");

  unsigned long start = millis();
  const unsigned long timeout = 30UL * 1000UL; // 30 seconds
  unsigned long lastDot = start;

  while (millis() - start < timeout) {
    time_t now = time(nullptr);
    if (now > 1700000000) { // sane epoch check
      ntpOk = true;
      g_timeReady = true;
      Serial.printf("\nNTP sync OK, epoch=%ld\n", now);
      return;
    }
    // print a dot every second so user sees progress
    if (millis() - lastDot >= 1000) {
      Serial.print(".");
      lastDot = millis();
    }
    delay(200);
  }

  Serial.println("\nNTP still not synced (timeout).");
  // leave ntpOk=false so loop() may retry later
}

// ================= Globals =================
Box box1 { .relayPin=B1_RELAY_PIN,.usTrig=B1_US_TRIG,.usEcho=B1_US_ECHO,.doorPin=B1_DOOR_PIN,
           .ledGreen=B1_LED_GREEN,.ledRed=B1_LED_RED,.camUrl=CAM1_SNAP_URL, .pathBase=PATH_BOX1 };
Box box2 { .relayPin=B2_RELAY_PIN,.usTrig=B2_US_TRIG,.usEcho=B2_US_ECHO,.doorPin=B2_DOOR_PIN,
           .ledGreen=B2_LED_GREEN,.ledRed=B2_LED_RED,.camUrl=CAM2_SNAP_URL, .pathBase=PATH_BOX2 };
Attempts attempts1, attempts2;

WebServer server(80);

// Firebase
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---- Admin override polling (RTDB: /unlockOverride/{box}) ----
uint64_t g_lastOverrideAtMsB1 = 0;
uint64_t g_lastOverrideAtMsB2 = 0;
uint32_t g_lastOverridePollMsB1 = 0;
uint32_t g_lastOverridePollMsB2 = 0;

// QR-unlock polling (RTDB: <boxBase>/unlockRequest)
uint64_t g_lastQrSeenAtMsB1 = 0;
uint64_t g_lastQrSeenAtMsB2 = 0;
uint32_t g_lastQrPollMsB1 = 0;
uint32_t g_lastQrPollMsB2 = 0;

// NEW: latch so we only trigger on a rising edge of "triggered"
bool g_lastQrTriggeredB1 = false;
bool g_lastQrTriggeredB2 = false;

// ---------- Buzzer helpers ----------
void buzzerBeepPinOnce(uint8_t pin, uint16_t ms) {
#ifdef ACTIVE_BUZZER
  digitalWrite(pin, HIGH);
  delay(ms);
  digitalWrite(pin, LOW);
#else
  tone(pin, 2000); // 2kHz
  delay(ms);
  noTone(pin);
#endif
}

// non-blocking-ish short beep using millis — but for simplicity we block briefly
void buzzerShort(uint8_t pin) {
  buzzerBeepPinOnce(pin, BEEP_SHORT_MS);
}

void buzzerSuccess(uint8_t pin) {
#ifndef ACTIVE_BUZZER
  // pattern: short beep, short pause, short beep
  tone(pin, 2200); delay(BEEP_SUCCESS_MS[0]); noTone(pin); delay(BEEP_SUCCESS_MS[1]);
  tone(pin, 2200); delay(BEEP_SUCCESS_MS[2]); noTone(pin);
#else
  digitalWrite(pin, HIGH); delay(120); digitalWrite(pin, LOW); delay(60);
  digitalWrite(pin, HIGH); delay(120); digitalWrite(pin, LOW);
#endif
}

void buzzerFail(uint8_t pin) {
#ifdef ACTIVE_BUZZER
  digitalWrite(pin, HIGH); delay(BEEP_FAIL_MS); digitalWrite(pin, LOW);
#else
  tone(pin, 600); delay(BEEP_FAIL_MS); noTone(pin);
#endif
}

// ================= Prototypes =================
void lockNow(Box& b);
void unlockNow(Box& b);
bool readDoorClosedDebounced(Box& b);
void startEventBlink(Box& b, bool green, uint8_t pulses);

// RTDB helpers
static inline bool netReady() { return WiFi.status()==WL_CONNECTED && Firebase.ready(); }
void rtdbSetStatus(const char* base, const char* status);
void rtdbPushAttempt(const char* base, const String& code, bool ok);
void rtdbSetLiveInput(const char* base, const String& input);

// Unified status setter
void setStatus(Box& b, const char* newStatus, bool forceMirror = false, const char* source = "device");

// ================= Helpers =================
inline void setLock(Box& b, bool locked) {
  digitalWrite(b.relayPin,
    RELAY_ACTIVE_LOW ? (locked ? LOW : HIGH)
                     : (locked ? HIGH : LOW));
}
inline void lockNow(Box& b)   { setLock(b, true);  b.unlockingNow=false; b.unlockUntilMs=0; }
inline void unlockNow(Box& b) { setLock(b, false); }

static inline int median3(int a, int b, int c) {
  if (a > b) { int t=a; a=b; b=t; }
  if (b > c) { int t=b; b=c; c=t; }
  if (a > b) { int t=a; a=b; b=t; }
  return b;
}
int readDistanceCmOnce(int trig, int echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long dur = pulseIn(echo, HIGH, PULSE_TIMEOUT_US);
  if (dur == 0) return -1;
  return int((dur * 0.0343f) / 2.0f + 0.5f);
}
int readDistanceCm(int trig, int echo) {
  int a = readDistanceCmOnce(trig, echo); delayMicroseconds(1500);
  int b = readDistanceCmOnce(trig, echo); delayMicroseconds(1500);
  int c = readDistanceCmOnce(trig, echo);
  if (a < 0 && b < 0 && c < 0) return -1;
  if (a < 0) a = b; if (a < 0) a = c;
  if (b < 0) b = a;
  if (c < 0) c = b;
  return median3(a, b, c);
}

bool readDoorClosedDebounced(Box& b) {
  int raw = digitalRead(b.doorPin);
  bool closed = IR_ACTIVE_HIGH ? (raw == HIGH) : (raw == LOW);
  uint32_t now = millis();
  if (b.doorLastRaw == -1) { b.doorLastRaw = raw; b.doorLastEdgeMs = now; return closed; }
  if (raw != b.doorLastRaw) { b.doorLastRaw = raw; b.doorLastEdgeMs = now; }
  if (now - b.doorLastEdgeMs >= DOOR_DEBOUNCE_MS) return closed;
  return b.doorClosed;
}

void applyPresencePolicy(Box& b) {
  if (b.unlockingNow && millis() < b.unlockUntilMs) { unlockNow(b); return; }
  b.unlockingNow=false; b.unlockUntilMs=0;
  if (b.presence) lockNow(b); else unlockNow(b);
}

void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 20000) { delay(400); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nWiFi FAIL");
}

// ---------------- Firebase helpers ----------------
uint64_t toU64(const String& s) { return strtoull(s.c_str(), nullptr, 10); }

bool jsonGetString(FirebaseJson& j, const char* key, String& out) {
  FirebaseJsonData d;
  if (j.get(d, key) && d.type == "string") { out = d.to<String>(); return true; }
  return false;
}
bool jsonGetNumberAsString(FirebaseJson& j, const char* key, String& out) {
  FirebaseJsonData d;
  if (j.get(d, key) && (d.type == "int" || d.type == "double" || d.type == "float" || d.type == "number")) {
    out = d.to<String>(); return true;
  }
  if (j.get(d, key) && d.type == "string") { out = d.to<String>(); return true; }
  return false;
}

void logBoxSerial(const char* name, const Box& b) {
#if SERIAL_SHOW_STATUS
  Serial.printf("[%s] status=%s code=%s expMs=%llu\n",
                name,
                b.status.c_str(),
                b.activeCode.length() ? b.activeCode.c_str() : "(none)",
                (unsigned long long)b.expiredAtMs);
  if (b.codeStatus.startsWith("fb_err:")) {
    Serial.printf("[%s] ERROR: %s\n", name, b.codeStatus.c_str()+7);
  }
#endif
}

// poll-guarded (used in loop)
void fetchCodeIfDue(Box& b, const char* path, const char* tagName) {
  uint32_t now = millis();
  if (now - b.lastCodePollMs < CODE_POLL_MS) return;
  b.lastCodePollMs = now;

  if (!Firebase.ready()) { b.codeStatus = "fb_err:not_ready"; logBoxSerial(tagName, b); return; }
  if (!Firebase.RTDB.getJSON(&fbdo, path)) { b.codeStatus = String("fb_err:") + fbdo.errorReason().c_str(); logBoxSerial(tagName, b); return; }

  FirebaseJson& json = *fbdo.to<FirebaseJson*>();
  String sCode, sExp, sStatus, sAttempt;

  // code
  if (jsonGetString(json, "code", sCode)) { sCode.toUpperCase(); b.activeCode = sCode; }
  else { b.activeCode = ""; }

  // status (read first)
  if (jsonGetString(json, "status", sStatus)) {
    if (sStatus != b.status) setStatus(b, sStatus.c_str(), /*forceMirror*/false, /*source*/"remote");
  } else {
    if (b.status != "pending") setStatus(b, "pending", false, "remote");
  }

  // expiry: prefer expiresAtMs; only fall back to expiredAtMs if the node is *actually* expired
  sExp = "";
  jsonGetNumberAsString(json, "expiresAtMs", sExp);
  if (sExp.length() == 0 && sStatus == "expired") {
    jsonGetNumberAsString(json, "expiredAtMs", sExp);
  }
  b.expiredAtMs = sExp.length() ? toU64(sExp) : 0ULL;

  // attempt id
  if (jsonGetString(json, "attemptId", sAttempt)) b.attemptId = sAttempt;

  b.codeStatus = (b.activeCode.length() ? "ok" : "no_code");
  logBoxSerial(tagName, b);
}

// immediate fetch (used by /fetchcodeboxX)
void fetchCodeNow(Box& b, const char* path, const char* tagName) {
  if (!Firebase.ready()) { b.codeStatus = "fb_err:not_ready"; logBoxSerial(tagName, b); return; }
  if (!Firebase.RTDB.getJSON(&fbdo, path)) { b.codeStatus = String("fb_err:") + fbdo.errorReason().c_str(); logBoxSerial(tagName, b); return; }

  FirebaseJson& json = *fbdo.to<FirebaseJson*>();
  String sCode, sExp, sStatus, sAttempt;

  // code
  if (jsonGetString(json, "code", sCode)) { sCode.toUpperCase(); b.activeCode = sCode; }
  else { b.activeCode = ""; }

  // status (read first)
  if (jsonGetString(json, "status", sStatus)) {
    if (sStatus != b.status) setStatus(b, sStatus.c_str(), /*forceMirror*/false, /*source*/"remote");
  } else {
    if (b.status != "pending") setStatus(b, "pending", false, "remote");
    sStatus = "pending";
  }

  // expiry field handling (same as fetchCodeIfDue)
  sExp = "";
  jsonGetNumberAsString(json, "expiresAtMs", sExp);
  if (sExp.length() == 0 && sStatus == "expired") {
    jsonGetNumberAsString(json, "expiredAtMs", sExp);
  }
  b.expiredAtMs = sExp.length() ? toU64(sExp) : 0ULL;

  // attempt id
  if (jsonGetString(json, "attemptId", sAttempt)) b.attemptId = sAttempt;

  b.codeStatus = (b.activeCode.length() ? "ok" : "no_code");
  logBoxSerial(tagName, b);
}

bool codeIsValid(const Box& b) {
  if (b.activeCode.length() != CODE_LEN) return false;
  if (b.status != "pending") return false;
  if (!g_timeReady || b.expiredAtMs==0) return true;
  return nowEpochMs() <= b.expiredAtMs;  // <- expiry
}

// ---------------- LED handling ----------------
void startEventBlink(Box& b, bool green, uint8_t pulses) {
  b.ev.isGreen   = green;
  b.ev.remaining = pulses * 2;
  b.ev.lastMs    = 0;
  b.ev.onPhase   = false;
  digitalWrite(b.ledGreen, LOW);
  digitalWrite(b.ledRed, LOW);
}
void driveEventBlink(Box& b) {
  if (b.ev.remaining == 0) return;
  uint32_t now = millis();
  uint32_t phaseMs = 120;
  if (b.ev.lastMs == 0 || now - b.ev.lastMs >= phaseMs) {
    b.ev.lastMs = now;
    b.ev.onPhase = !b.ev.onPhase;
    b.ev.remaining--;
    int pinOn = b.ev.isGreen ? b.ledGreen : b.ledRed;
    digitalWrite(b.ledGreen, LOW);
    digitalWrite(b.ledRed, LOW);
    if (b.ev.onPhase) digitalWrite(pinOn, HIGH);
  }
}
void updateStatusLeds(Box& b) {
  if (b.ev.remaining > 0) { driveEventBlink(b); return; }

  uint32_t now = millis();

  if (b.unlockingNow && now < b.unlockUntilMs) {
    digitalWrite(b.ledGreen, HIGH);
    digitalWrite(b.ledRed, LOW);
    return;
  }

  if (!b.doorClosed) {
    if (now - b.lastOpenBlinkMs >= 800) {
      b.lastOpenBlinkMs = now; b.openBlinkOn = !b.openBlinkOn;
    }
    digitalWrite(b.ledGreen, b.openBlinkOn ? HIGH : LOW);
    digitalWrite(b.ledRed, LOW);
    return;
  }

  if (b.presence) {
    digitalWrite(b.ledRed, HIGH);
    digitalWrite(b.ledGreen, LOW);
  } else {
    digitalWrite(b.ledGreen, HIGH);
    digitalWrite(b.ledRed, LOW);
  }
}

// ---------------- RTDB WRITE HELPERS ----------------
void rtdbSetStatus(const char* base, const char* status) {
  if (!netReady()) return;
  FirebaseJson j;
  j.set("status", status);
  Firebase.RTDB.updateNode(&fbdo, base, &j);
}

void rtdbPushAttempt(const char* base, const String& code, bool ok) {
  if (!netReady()) return;
  String path = String(base) + "/attempts";
  FirebaseJson j;
  j.set("code", code);
  j.set("ok", ok);
  j.set("atMs", (double)nowEpochMs());
  Firebase.RTDB.pushJSON(&fbdo, path, &j);
}

void rtdbSetLiveInput(const char* base, const String& input) {
  if (!netReady()) return;
  FirebaseJson j;
  j.set("liveInput", input);
  j.set("liveInputAtMs", (double)nowEpochMs());
  Firebase.RTDB.updateNode(&fbdo, base, &j);
}

// ---------------- Status setter (avoid echoing remote updates) ----------------
void setStatus(Box& b, const char* newStatus, bool forceMirror, const char* source) {
  bool changed = (b.status != String(newStatus));
  b.status = newStatus;

  // Only mirror to RTDB when change originates from device/web UI actions,
  // NOT when we just read a value from RTDB ("remote").
  if ((changed || forceMirror) && String(source) != "remote") {
    rtdbSetStatus(b.pathBase, newStatus);
  }
}

// Poll /UnlockCodes/<boxId>/unlockRequest and open UNLOCK_MS when method=="qr_scan" and triggered==true
void pollQrUnlock(Box& b,
              const char* boxId,               // "box1" or "box2"
              uint64_t& lastSeenAt,            // last-seen timestamp used to dedupe timestamped requests
              uint32_t& lastPollMs) {          // poll guard timestamp
  // Throttle polling locally
  if (millis() - lastPollMs < 700) return;

  if (!Firebase.ready()) {
    Serial.println("[QR] Firebase not ready");
    // do NOT update lastPollMs so we'll retry soon
    return;
  }

  String path = String("/UnlockCodes/") + boxId + "/unlockRequest";

  // Perform RTDB fetch
  if (!Firebase.RTDB.getJSON(&fbdo, path)) {
    Serial.printf("[QR] RTDB.getJSON(%s) failed: %s\n", path.c_str(), fbdo.errorReason().c_str());
    // do NOT update lastPollMs so we'll retry quickly
    return;
  }

  // Successful read -> update poll timestamp so we don't hammer on success
  lastPollMs = millis();

  // Optional raw payload debug (helpful to inspect actual RTDB reply)
  String raw = fbdo.payload();
  Serial.printf("[QR RAW] %s => %s\n", path.c_str(), raw.c_str());

  FirebaseJson* j = fbdo.to<FirebaseJson*>();
  FirebaseJsonData dMethod, dTrig, dTs;

  // --- method (string) ---
  if (!j->get(dMethod, "method")) {
    Serial.printf("[QR] %s: no method field\n", boxId);
    return;
  }
  String method = dMethod.to<String>();
  method.toLowerCase();

  // --- triggered (robust parsing) ---
  bool triggered = false;
  bool haveTriggered = false;
  if (j->get(dTrig, "triggered")) {
    haveTriggered = true;
    String rawTrig = dTrig.to<String>();
    Serial.printf("[QR DBG] %s: triggered rawType=%s rawVal=%s\n", boxId, dTrig.type.c_str(), rawTrig.c_str());

    // handle boolean-like types
    String ttype = dTrig.type;
    ttype.toLowerCase();
    if (ttype == "bool" || ttype == "boolean") {
      triggered = dTrig.to<bool>();
    } else if (ttype == "int" || ttype == "double" || ttype == "float" || ttype == "number") {
      triggered = (dTrig.to<int>() != 0);
    } else {
      // string fallback (case-insensitive)
      rawTrig.toLowerCase();
      triggered = (rawTrig == "true" || rawTrig == "1" || rawTrig == "yes");
    }
  } else {
    Serial.printf("[QR DBG] %s: no triggered field present\n", boxId);
  }

  // --- timestamp parsing (try multiple keys) ---
  uint64_t reqAt = 0;
  const char* tsKeys[] = { "requestedAt", "requestedAtMs", "atMs", "processedAt" };
  for (int k = 0; k < (int)(sizeof(tsKeys)/sizeof(tsKeys[0])); ++k) {
    if (j->get(dTs, tsKeys[k])) {
      String rawTs = dTs.to<String>();
      Serial.printf("[QR DBG] %s: found ts key=%s type=%s val=%s\n", boxId, tsKeys[k], dTs.type.c_str(), rawTs.c_str());
      String ttype = dTs.type; ttype.toLowerCase();
      if (ttype == "int" || ttype == "double" || ttype == "float" || ttype == "number") {
        reqAt = (uint64_t)dTs.to<double>();
      } else {
        // string -> parse to unsigned long long
        reqAt = strtoull(rawTs.c_str(), nullptr, 10);
      }
      break;
    }
  }

  Serial.printf("[QR DBG] %s: method='%s' triggered=%s reqAt=%llu lastSeenAt=%llu\n",
                boxId, method.c_str(),
                (haveTriggered ? (triggered ? "true" : "false") : "missing"),
                (unsigned long long)reqAt,
                (unsigned long long)lastSeenAt);

  // --- latch pointer selection (rising-edge fallback) ---
  bool *pLatch = nullptr;
  if (String(boxId) == "box1") pLatch = &g_lastQrTriggeredB1;
  else if (String(boxId) == "box2") pLatch = &g_lastQrTriggeredB2;

  // --- decide whether to open ---
  bool shouldOpen = false;

  // Prefer timestamp-based dedupe when a timestamp exists and is newer than lastSeenAt
  if (reqAt != 0 && reqAt > lastSeenAt) {
    if (method == "qr_scan" && (haveTriggered ? triggered : true)) {
      shouldOpen = true;
      lastSeenAt = reqAt; // consume timestamp
    }
  } else {
    // No valid/new timestamp: fallback to boolean rising-edge detection if 'triggered' is present
    if (haveTriggered && triggered && method == "qr_scan" && pLatch && !(*pLatch)) {
      shouldOpen = true;
      lastSeenAt = nowEpochMs(); // set some non-zero time so future duplicates are avoided
    }
  }

  if (shouldOpen) {
    b.unlockingNow = true;
    b.unlockUntilMs = millis() + UNLOCK_MS;
    unlockNow(b);
    if (pLatch) *pLatch = true; // latch to avoid re-opening on same boolean 'true'
    Serial.printf("[QR] %s OPEN for %lu ms (lastSeenAt=%llu)\n", boxId, (unsigned long)UNLOCK_MS, (unsigned long long)lastSeenAt);
    return;
  }

  // If the remote cleared triggered (boolean -> false), clear our latch so next true can be accepted
  if (pLatch && haveTriggered && !triggered && *pLatch) {
    *pLatch = false;
    Serial.printf("[QR] %s latch reset (remote cleared triggered)\n", boxId);
  }

  // Nothing to do
  // Serial.printf("[QR DBG] %s: no action\n", boxId);
}



// Poll /unlockOverride/{boxId} and open a temporary unlock window if present.
void pollUnlockOverride(Box& b,
                        const char* boxId,
                        uint64_t& lastSeenAt,
                        uint32_t& lastPollMs) {
  if (millis() - lastPollMs < 700) return;
  lastPollMs = millis();
  if (!Firebase.ready()) return;

  String path = String("/unlockOverride/") + boxId;  // <-- leading slash
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  FirebaseJson* j = fbdo.to<FirebaseJson*>();
  FirebaseJsonData dAt, dSec;
  if (!j->get(dAt, "atMs") || !j->get(dSec, "seconds")) return;

  uint64_t atMs = 0;
  if (dAt.type == "int" || dAt.type == "double" || dAt.type == "float" || dAt.type == "number")
    atMs = (uint64_t)dAt.to<double>();
  else if (dAt.type == "string")
    atMs = strtoull(dAt.to<String>().c_str(), nullptr, 10);
  else
    return;

  int seconds = 0;
  if (dSec.type == "int" || dSec.type == "double" || dSec.type == "float" || dSec.type == "number")
    seconds = dSec.to<int>();
  else if (dSec.type == "string")
    seconds = dSec.to<String>().toInt();
  else
    return;

  if (atMs == 0 || atMs <= lastSeenAt) return;
  if (seconds < 1) seconds = 1;
  if (seconds > 30) seconds = 30;

  b.unlockingNow  = true;
  b.unlockUntilMs = millis() + (uint32_t)seconds * 1000U;
  unlockNow(b);
  lastSeenAt = atMs;

  // optional mirror
  FirebaseJson mirror;
  mirror.set("status", "override");
  mirror.set("atMs", (double)nowEpochMs());
  mirror.set("seconds", seconds);
  Firebase.RTDB.updateNode(&fbdo, (String("/unlock/") + boxId + "/window").c_str(), &mirror);
}

// ---------------- Services ----------------
void recordAttempt(Attempts& A, const String& code, bool ok) {
  A.code[A.head] = code;
  A.ok[A.head]   = ok;
  A.head = (A.head + 1) % ATTEMPT_MAX;
  if (A.count < 255) A.count++;
}

// schedule snapshot precisely when door is closed AND presence is true for HOLD_MS
// schedule snapshot precisely when door is closed AND presence is true for HOLD_MS
void fusePresenceAndMaybeSnap(Box& b, bool usNow) {
  if (usNow != b.presence) b.presence = usNow;
  uint32_t now = millis();

  if (b.doorClosed && b.presence) {
    if (b.presenceStartMs == 0) b.presenceStartMs = now;

    // Arm ONCE per continuous presence period
    if (!b.snapActive && !b.snapDoneThisPresence && (now - b.presenceStartMs) >= HOLD_MS) {
      b.snapActive       = true;
      b.snapStartedAtMs  = now;
      b.snapTries        = 0;
      b.lastSnapMsg      = "armed";
      b.snapTargetAtMs   = b.presenceStartMs + SNAP_ONESHOT_DELAY_MS;
      if (b.snapTargetAtMs < now) b.snapTargetAtMs = now + 200;
    }
  } else {
    // Presence/door lost → reset *everything*, including the done flag
    b.presenceStartMs    = 0;
    b.snapActive         = false;
    b.snapTries          = 0;
    b.snapTargetAtMs     = 0;
    b.snapDoneThisPresence = false;   // <— allow arming next time presence returns
  }
}

void serviceSnapshotLoop(Box& b, const String& cameraSnapUrl, const char* tagName) {
  if (!b.snapActive) return;

  uint32_t now = millis();
  if (!b.doorClosed || !b.presence) { 
    b.snapActive = false; b.snapTries = 0; b.snapTargetAtMs = 0; 
    return; 
  }
  if (now - b.snapStartedAtMs > SNAP_MAX_MS) { 
    b.snapActive = false; b.snapTries = 0; b.snapTargetAtMs = 0; 
    return; 
  }

  // Only one shot per presence
  if (b.snapTries >= 1) { b.snapActive = false; return; }

  if (now < b.snapTargetAtMs) return;

  // Take the single photo
  String itemId, msg;
  bool ok = triggerSnap(cameraSnapUrl, itemId, msg);

  b.lastSnapTryMs   = now;
  b.lastSnapMsg     = msg;
  b.lastSnapOk      = ok;
  b.lastSnapItemId  = itemId;

  Serial.printf("[%s] one-shot snap: %s (item=%s)\n", tagName, msg.c_str(), itemId.c_str());

  // Permanently disarm for this continuous presence interval
  b.snapTries            = 1;
  b.snapActive           = false;
  b.snapTargetAtMs       = 0;
  b.snapDoneThisPresence = true;   // <— key line
}

void serviceSensors(Box& b) {
  if (millis() - b.lastSonarMs >= SONAR_MS) {
    b.lastSonarMs = millis();
    int d = readDistanceCm(b.usTrig, b.usEcho);
    b.lastDist = d;

    bool newUS = b.presenceUS;
    if (d >= 0) {
      if (!b.presenceUS && d <= THRESH_ON_CM)       newUS = true;
      else if (b.presenceUS && d >= THRESH_OFF_CM)  newUS = false;
    }
    if (newUS != b.presenceUS) b.presenceUS = newUS;

    bool prevClosed = b.doorClosed;
    b.doorClosed = readDoorClosedDebounced(b);
    bool justOpened = (!b.doorClosed && prevClosed);
    bool justClosed = (b.doorClosed && !prevClosed);

    // ----- success state machine -----
    if (b.claimArmed) {
      if (justOpened && (b.unlockingNow || b.doorOpenedAfterUnlock)) {
        b.doorOpenedAfterUnlock = true;
      }
      if (justClosed && b.doorOpenedAfterUnlock && !b.presence) {
        setStatus(b, "successful", /*force*/true, "device");
        b.claimArmed = false;
        b.doorOpenedAfterUnlock = false;
        b.waitingForRemoval = false;
        b.activeCode = "";
        b.expiredAtMs = 0;
        unlockNow(b);
      }
    }

    fusePresenceAndMaybeSnap(b, b.presenceUS);
  }

  updateStatusLeds(b);
}

// new: core submit that accepts an explicit input
void onSubmitWith(Box& b, Attempts& A, const String& rawInput) {
  b.input = rawInput;
  b.input.toUpperCase();

  bool ok = (b.input.length()==CODE_LEN && b.activeCode.length()==CODE_LEN &&
             b.input == b.activeCode && codeIsValid(b));

  if (ok) {
    b.unlockingNow  = true;
    b.unlockUntilMs = millis() + UNLOCK_MS;
    unlockNow(b);
    b.claimArmed = true;
    b.doorOpenedAfterUnlock = false;
    b.waitingForRemoval = true;
    setStatus(b, "pending", /*forceMirror*/true, "device"); // stays pending
  }

  // New BUZZER code (single shared buzzer)
  if (ok) {
    buzzerSuccess(BZZR_PIN);
  } else {
    buzzerFail(BZZR_PIN);
  }


  recordAttempt(A, b.input, ok);
  rtdbPushAttempt(b.pathBase, b.input, ok);

  b.input = "";
  rtdbSetLiveInput(b.pathBase, b.input);
}

// Keep the keypad path: '#' submits the current buffer
void onSubmit(Box& b, Attempts& A) {
  onSubmitWith(b, A, b.input);
  #if SERIAL_SHOW_SUBMIT
    Serial.printf("[KEYPAD] Box %s submit=%s\n",
                  (b.pathBase == PATH_BOX1 ? "1" : "2"),
                  b.input.c_str());
  #endif
}

void serviceKeypad(Box& b, I2CKeyPad& kp, Attempts& A) {
  uint32_t now = millis();
  if (now - b.lastKeypadScanMs < KEYPAD_MS) return;
  b.lastKeypadScanMs = now;

  int idx = kp.getKey(); // -1 if none
  if (idx != b.lastIdx) { b.lastChangeMs = now; b.lastIdx = idx; }
  if (idx == -1) { b.lastHandledIdx = -1; return; }

  if ((now - b.lastChangeMs) >= DEBOUNCE_MS && idx != b.lastHandledIdx) {
    b.lastHandledIdx = idx;
    char k = KEYMAP[idx];

    startEventBlink(b, /*green*/false, /*pulses*/1);

    // --- beep on every keypress ---
    buzzerShort(BZZR_PIN);

    bool changed=false;
    if ((k>='0' && k<='9') || (k>='A' && k<='D')) {
      if (b.input.length() < CODE_LEN) { b.input += k; changed=true; }
    } else if (k=='*') {
      b.input = ""; changed=true;
    } else if (k=='#') {
      // If currently in an unlocked window, allow immediate lock when user presses '#'
      if (b.unlockingNow) {
        // Immediately end unlock window and lock hardware
        b.unlockingNow = false;
        b.unlockUntilMs = 0;
        lockNow(b);
        setStatus(b, "locked", /*forceMirror*/true, "device");
        // optional: buzz to confirm
        buzzerShort(BZZR_PIN);
      } else {
        // otherwise treat '#' as submit
        onSubmit(b, A);
      }
    }

    if (changed) {
      rtdbSetLiveInput(b.pathBase, b.input);
    }

    #if SERIAL_SHOW_KEYS
      Serial.printf("[KEYPAD] Box %s key=%c\n",
                    (b.pathBase == PATH_BOX1 ? "1" : "2"), k);
    #endif
  }
}

void enforceUnlockWindowEnd(Box& b) {
  if (b.unlockingNow && millis() >= b.unlockUntilMs) {
    b.unlockingNow = false;
    b.unlockUntilMs = 0;
    applyPresencePolicy(b); // only lock HW according to presence
    // (no setStatus() here – RTDB stays "pending")
  }
}

void enforceExpiry(Box& b) {
  if (!g_timeReady || b.expiredAtMs == 0) return;
  if (b.status != "pending") return;
  if (nowEpochMs() > b.expiredAtMs) {
    setStatus(b, "expired", /*forceMirror*/true, "device");
  }
}

// ---------------- Web UI helpers ----------------
String humanDoor(const Box& b){ return b.doorClosed ? "Closed" : "Open"; }
String humanPresence(const Box& b){ return b.presence ? "Present" : "None"; }
String humanValidity(const Box& b){
  if (b.activeCode.length() != CODE_LEN) return "—";
  if (!g_timeReady || b.expiredAtMs==0) return "Unknown (no NTP)";
  long long left = (long long)b.expiredAtMs - (long long)nowEpochMs();
  if (left <= 0) return "Expired";
  long s = left/1000; long mm=s/60, ss=s%60;
  char buf[24]; snprintf(buf,sizeof(buf),"%ld:%02ld left",mm,ss);
  return String(buf);
}
String statusText(const Box& b) {
  bool unlocked = (b.unlockingNow && millis() < b.unlockUntilMs);
  if (!b.doorClosed) return "Door: Open";
  if (b.presence && !unlocked) return "Door: Closed • Item: Present • Locked";
  if (b.presence &&  unlocked) return "Door: Closed • Item: Present • UNLOCKED";
  if (!b.presence && unlocked) return "Door: Closed • Item: None • UNLOCKED";
  return "Door: Closed • Item: None • Ready";
}
String attemptsJson(const Attempts& A) {
  String j = "[";
  uint8_t printed=0;
  for (int i=0;i<ATTEMPT_MAX;i++){
    int idx=(A.head-1-i+ATTEMPT_MAX)%ATTEMPT_MAX;
    if (A.code[idx].length()==0) continue;
    if (printed++) j += ",";
    j += "{\"code\":\"" + A.code[idx] + "\",\"ok\":" + String(A.ok[idx]?"true":"false") + "}";
  }
  j += "]";
  return j;
}
String boxJson(const Box& b, const Attempts& A){
  String j="{";
  j += "\"statusRaw\":\""  + b.status + "\",";        
  j += "\"statusText\":\"" + statusText(b) + "\",";   
  j += "\"code\":\"" + (b.activeCode.length()? b.activeCode : String("—")) + "\",";
  j += "\"codeStatus\":\"" + b.codeStatus + "\",";
  j += "\"validity\":\"" + humanValidity(b) + "\",";
  j += "\"door\":\"" + humanDoor(b) + "\",";
  j += "\"presence\":\"" + humanPresence(b) + "\",";
  j += "\"distanceCm\":" + String(b.lastDist) + ",";
  j += "\"attempts\":" + attemptsJson(A) + ",";  // <-- add comma

  // snap object
  j += "\"snap\":{";
  j += "\"active\":" + String(b.snapActive ? "true":"false") + ",";
  j += "\"armedForMs\":" + String(b.snapActive ? (millis() - b.snapStartedAtMs) : 0) + ",";
  j += "\"lastAgeMs\":" + String(b.lastSnapTryMs ? (millis() - b.lastSnapTryMs) : 0) + ",";
  j += "\"lastMsg\":\"" + (b.lastSnapMsg.length() ? b.lastSnapMsg : String("-")) + "\",";
  j += "\"lastOk\":" + String(b.lastSnapOk ? "true":"false") + ",";
  j += "\"lastItemId\":\"" + (b.lastSnapItemId.length() ? b.lastSnapItemId : String("")) + "\"";
  j += "}";

  j += "}";                     // <-- close top-level object
  return j;
}


String boxOnlyJson(const Box& b, const Attempts& A) { return boxJson(b, A); }
String buildStateJson() {
  String j="{";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"timeReady\":" + String(g_timeReady?"true":"false") + ",";
  j += "\"box1\":" + boxJson(box1, attempts1) + ",";
  j += "\"box2\":" + boxJson(box2, attempts2);
  j += "}";
  return j;
}

// ---------------- Web UI (HTML) ----------------
const char* HTML_PAGE = R"HTML(
<!doctype html><html><head><meta charset="utf-8"/>
<title>ESP32 Center</title><meta name="viewport" content="width=device-width,initial-scale=1"/>
<style>
:root{--ok:#0a7a00;--warn:#9a6a00;--err:#a50000;--bg:#fafafa;--card:#fff;--mut:#666}
body{font-family:system-ui,Arial;max-width:920px;margin:18px auto;padding:0 12px;background:var(--bg)}
.hdr{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:10px}
.pill{background:#eee;border-radius:999px;padding:4px 10px}
.pill.ok{background:#c8f7c5}.pill.warn{background:#ffe7a6}.pill.err{background:#ffc9c9}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.card{background:var(--card);border:1px solid #e5e5e5;border-radius:12px;padding:12px;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.row{display:flex;align-items:center;justify-content:space-between;margin:6px 0}
.badge{padding:2px 8px;border-radius:999px;font-size:12px}
.badge.ok{background:#e8f7e6;color:var(--ok)} .badge.warn{background:#fff3d9;color:var(--warn)}
.badge.err{background:#ffe3e3;color:var(--err)}
.kv{color:#222}.kv small{color:var(--mut)}
ul{margin:6px 0 0 16px} li.ok{color:var(--ok)} li.bad{color:var(--err)}
.mono{font-family:ui-monospace,Consolas,monospace}
.h3{margin:0 0 6px 0}
hr{border:none;border-top:1px solid #eee;margin:10px 0}
.api{margin-top:12px;font-size:14px}
.api a{display:inline-block;margin:3px 8px 0 0}
</style></head><body>
<h2>ESP32 Center — Status</h2>
<div class="hdr">
  <span id="ip" class="pill"></span>
  <span id="ntp" class="pill"></span>
  <span id="fb" class="pill"></span>
</div>
<div class="grid">
  <div class="card">
    <h3 class="h3">Box 1</h3>
    <div class="row"><div class="kv">Status</div><div id="b1_status" class="badge"></div></div>
    <div class="row"><div class="kv">Unlock Code</div><div><span id="b1_code" class="mono"></span> <small id="b1_valid" class="mono"></small></div></div>
    <div class="row"><div class="kv">Fetch</div><div><small id="b1_codeStatus"></small></div></div>
    <hr/>
    <div class="row"><div class="kv">Door</div><div id="b1_door" class="mono"></div></div>
    <div class="row"><div class="kv">Item</div><div id="b1_presence" class="mono"></div></div>
    <div class="row"><div class="kv">Distance</div><div id="b1_dist" class="mono"></div></div>
    <div class="row"><div class="kv">Snap</div><div id="b1_snap" class="mono"></div></div>
    <hr/>
    <div>Attempts</div>
    <ul id="b1_attempts"></ul>
    <hr/>
    <div class="row">
      <div class="kv">Type Code (test)</div>
      <div>
        <input id="b1_entry" class="mono" maxlength="8" style="width:9ch;padding:2px 6px" placeholder="XXXXXXXX"/>
        <button id="b1_send" style="margin-left:6px">Submit</button>
      </div>
    </div>
  </div>
  <div class="card">
    <h3 class="h3">Box 2</h3>
    <div class="row"><div class="kv">Status</div><div id="b2_status" class="badge"></div></div>
    <div class="row"><div class="kv">Unlock Code</div><div><span id="b2_code" class="mono"></span> <small id="b2_valid" class="mono"></small></div></div>
    <div class="row"><div class="kv">Fetch</div><div><small id="b2_codeStatus"></small></div></div>
    <hr/>
    <div class="row"><div class="kv">Door</div><div id="b2_door" class="mono"></div></div>
    <div class="row"><div class="kv">Item</div><div id="b2_presence" class="mono"></div></div>
    <div class="row"><div class="kv">Distance</div><div id="b2_dist" class="mono"></div></div>
    <div class="row"><div class="kv">Snap</div><div id="b2_snap" class="mono"></div></div>
    <hr/>
    <div>Attempts</div>
    <ul id="b2_attempts"></ul>
    <hr/>
    <div class="row">
      <div class="kv">Type Code (test)</div>
      <div>
        <input id="b2_entry" class="mono" maxlength="8" style="width:9ch;padding:2px 6px" placeholder="XXXXXXXX"/>
        <button id="b2_send" style="margin-left:6px">Submit</button>
      </div>
    </div>

  </div>
</div>

<div class="api">
  <b>Quick API:</b>
  <a href="/fetchcodebox1">fetchcodebox1</a>
  <a href="/fetchcodebox2">fetchcodebox2</a>
  <a href="/unlockbox1">unlockbox1</a>
  <a href="/unlockbox2">unlockbox2</a>
  <a href="/lockbox1">lockbox1</a>
  <a href="/lockbox2">lockbox2</a>
  <a href="/statebox1">statebox1</a>
  <a href="/statebox2">statebox2</a>
  <a href="/cancelbox1">cancelbox1</a>
  <a href="/cancelbox2">cancelbox2</a>
</div>

<script>
function statusBadge(el, text){
  const t = String(text || '').toLowerCase();
  el.textContent = text;
  let cls = 'badge ';
  if (t.includes('successful') || t.includes('ready')) cls += 'ok';
  else if (t.includes('pending') || t.includes('open') || t.includes('unlocked')) cls += 'warn';
  else if (t.includes('cancelled') || t.includes('expired') || t.includes('error')) cls += 'err';
  else if (t.includes('present')) cls += 'warn';
  else cls += 'ok';
  el.className = cls;
}

function fetchBadgeText(st){
  if(st==='ok') return 'OK';
  if(st==='no_code') return 'No code';
  if(st.startsWith('fb_err:')) return 'Error: ' + st.slice(7);
  return st;
}

async function enterCode(box, code){
  const c = String(code || '').trim().toUpperCase();
  if (!/^[0-9A-D]{8}$/.test(c)) {
    alert('Please enter 8 chars: 0-9 or A-D'); 
    return;
  }
  const path = box===1 ? '/enterbox1' : '/enterbox2';
  const url = `${path}?code=${encodeURIComponent(c)}`;
  try {
    const r = await fetch(url, { method: 'POST', cache: 'no-store' });
    if (!r.ok) throw new Error(await r.text());
    // no-op; the live state will refresh on next tick()
  } catch(e) {
    alert('Submit failed: ' + e.message);
  }
}

function hookEntryControls(){
  const b1i = document.getElementById('b1_entry');
  const b1b = document.getElementById('b1_send');
  const b2i = document.getElementById('b2_entry');
  const b2b = document.getElementById('b2_send');

  if (b1i && b1b) {
    b1b.onclick = () => enterCode(1, b1i.value);
    b1i.addEventListener('keydown', e => { if (e.key==='Enter') b1b.click(); });
  }
  if (b2i && b2b) {
    b2b.onclick = () => enterCode(2, b2i.value);
    b2i.addEventListener('keydown', e => { if (e.key==='Enter') b2b.click(); });
  }
}

async function tick(){
  try{
    const r = await fetch('/state.json',{cache:'no-store'});
    const s = await r.json();
    document.getElementById('ip').textContent = 'IP: ' + s.ip;
    const ntp=document.getElementById('ntp');
    ntp.textContent = 'NTP: ' + (s.timeReady ? 'OK' : 'syncing...');
    ntp.className='pill ' + (s.timeReady ? 'ok' : 'warn');
    const fb=document.getElementById('fb');
    fb.textContent = 'Firebase: Ready';
    fb.className='pill ok';

    const render=(b, pre)=>{
      statusBadge(document.getElementById(pre+'_status'), b.statusRaw || b.statusText);

      // ✅ show code AND expose the helpful sentence as a tooltip
      const codeEl = document.getElementById(pre+'_code');
      codeEl.textContent = b.code || '—';
      codeEl.title = b.statusText;

      document.getElementById(pre+'_valid').textContent = b.validity ? '('+b.validity+')' : '';
      const cs = fetchBadgeText(b.codeStatus);
      const csEl = document.getElementById(pre+'_codeStatus');
      csEl.textContent = cs;
      csEl.style.color = cs.startsWith('Error') ? '#a50000' : '#222';
      document.getElementById(pre+'_door').textContent = b.door;
      document.getElementById(pre+'_presence').textContent = b.presence;
      document.getElementById(pre+'_dist').textContent = (b.distanceCm>=-1? b.distanceCm+' cm' : '—');
      const ul=document.getElementById(pre+'_attempts'); ul.innerHTML='';
      const n=b.attempts.length;
      b.attempts.forEach((a,i)=>{
        const li=document.createElement('li');
        li.className=a.ok?'ok':'bad';
        li.textContent=(n-i)+'. '+a.code+' '+(a.ok?'(correct)':'(wrong)');
        ul.appendChild(li);
      });
      const snapEl = document.getElementById(pre+'_snap');
      if (snapEl && b.snap){
        const age = b.snap.lastAgeMs && b.snap.lastAgeMs > 0
          ? `, ${(b.snap.lastAgeMs/1000).toFixed(1)}s ago`
          : '';
        const txt = b.snap.active
          ? `ARMED… ${b.snap.armedForMs} ms`
          : `last=${b.snap.lastMsg}${b.snap.lastItemId ? ' ('+b.snap.lastItemId+')' : ''}${age}`;
        snapEl.textContent = txt;
      }
    };
    render(s.box1,'b1'); render(s.box2,'b2');
  }catch(e){
    const fb=document.getElementById('fb');
    fb.textContent = 'Firebase: Not ready';
    fb.className='pill warn';
  }
  setTimeout(tick, 700);
}
tick();
hookEntryControls();
</script>
</body></html>
)HTML";

// ---------------- Web server handlers ----------------
void handleRoot(){ server.send(200,"text/html",HTML_PAGE); }
void handleState(){ server.send(200,"application/json",buildStateJson()); }

bool devicePresent(TwoWire& bus, byte addr) { bus.beginTransmission(addr); return (bus.endTransmission() == 0); }

void webInit() {
  server.on("/", handleRoot);
  server.on("/state.json", handleState);

  // Immediate fetch
  server.on("/fetchcodebox1", [](){ fetchCodeNow(box1, PATH_BOX1, "BOX1"); server.send(200, "application/json", boxOnlyJson(box1, attempts1)); });
  server.on("/fetchcodebox2", [](){ fetchCodeNow(box2, PATH_BOX2, "BOX2"); server.send(200, "application/json", boxOnlyJson(box2, attempts2)); });

  // Force unlock window (UNLOCK_MS)
  server.on("/unlockbox1", [](){
    box1.unlockingNow = true;
    box1.unlockUntilMs = millis() + UNLOCK_MS;
    unlockNow(box1);
    server.send(200, "text/plain", "OK: box1 unlocked for 5s");
  });
  server.on("/unlockbox2", [](){
    box2.unlockingNow = true;
    box2.unlockUntilMs = millis() + UNLOCK_MS;
    unlockNow(box2);
    server.send(200, "text/plain", "OK: box2 unlocked for 5s");
  });

  // Web submit code (query param ?code=XXXXXXXX)
  server.on("/enterbox1", HTTP_ANY, [](){
    String code = server.hasArg("code") ? server.arg("code") : String();
    if (code.length() == 0) { server.send(400, "text/plain", "Missing ?code="); return; }
    onSubmitWith(box1, attempts1, code);
    server.send(200, "application/json", boxOnlyJson(box1, attempts1));
  });

  server.on("/enterbox2", HTTP_ANY, [](){ 
    String code = server.hasArg("code") ? server.arg("code") : String();
    if (code.length() == 0) { server.send(400, "text/plain", "Missing ?code="); return; }
    onSubmitWith(box2, attempts2, code);
    server.send(200, "application/json", boxOnlyJson(box2, attempts2));
  });

  // Force lock (immediate)
  server.on("/lockbox1", [](){ lockNow(box1); setStatus(box1, "locked", /*forceMirror*/true, "device"); server.send(200, "text/plain", "OK: box1 locked"); });
  server.on("/lockbox2", [](){ lockNow(box2); setStatus(box2, "locked", /*forceMirror*/true, "device"); server.send(200, "text/plain", "OK: box2 locked"); });

  // Single-box JSON
  server.on("/statebox1", [](){ server.send(200, "application/json", boxOnlyJson(box1, attempts1)); });
  server.on("/statebox2", [](){ server.send(200, "application/json", boxOnlyJson(box2, attempts2)); });

  server.on("/cancelbox1", [](){
    setStatus(box1, "cancelled", /*forceMirror*/true, "device");
    server.send(200, "text/plain", "OK: box1 cancelled");
  });

  server.on("/cancelbox2", [](){
    setStatus(box2, "cancelled", /*forceMirror*/true, "device");
    server.send(200, "text/plain", "OK: box2 cancelled");
  });

  // in webInit()
  server.on("/snapbox1", [](){
    String itemId, msg; bool ok = triggerSnap(CAM1_SNAP_URL, itemId, msg);
    box1.lastSnapMsg = msg; box1.lastSnapOk = ok; box1.lastSnapItemId = itemId; box1.lastSnapTryMs = millis();
    String r = String("{\"ok\":") + (ok?"true":"false") +
              ",\"msg\":\"" + msg + "\",\"itemId\":\"" + itemId + "\"}";
    server.send(ok?200:500, "application/json", r);
  });
  server.on("/snapbox2", [](){
    String itemId, msg; bool ok = triggerSnap(CAM2_SNAP_URL, itemId, msg);
    box2.lastSnapMsg = msg; box2.lastSnapOk = ok; box2.lastSnapItemId = itemId; box2.lastSnapTryMs = millis();
    String r = String("{\"ok\":") + (ok?"true":"false") +
              ",\"msg\":\"" + msg + "\",\"itemId\":\"" + itemId + "\"}";
    server.send(ok?200:500, "application/json", r);
  });

  server.begin();
  if (MDNS.begin("esp32-center")) MDNS.addService("http","tcp",80);
}

// ---------------- Setup / Loop ----------------
void setupBoxPins(Box& b){
  pinMode(b.relayPin, OUTPUT);
  pinMode(b.usTrig, OUTPUT);
  pinMode(b.usEcho, INPUT);
  pinMode(b.doorPin,  INPUT_PULLUP);
  pinMode(b.ledGreen, OUTPUT);
  pinMode(b.ledRed,   OUTPUT);
  digitalWrite(b.ledGreen, LOW);
  digitalWrite(b.ledRed, LOW);
  lockNow(b);
}

void initFirebase(){
  config.api_key = FB_API_KEY;
  config.database_url = FB_DB_URL;
  auth.user.email = FB_USER_EMAIL;
  auth.user.password = FB_USER_PASSWORD;

  fbdo.setBSSLBufferSize(4096, 1024);
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  config.timeout.serverResponse = 10 * 1000;

  Serial.println("Firebase init...");
  Serial.print("Waiting for token");
  unsigned long t0 = millis();
  while (auth.token.uid.length() == 0 && millis() - t0 < 20000) { Serial.print("."); delay(300); }
  Serial.println();
  if (auth.token.uid.length()) Serial.printf("Signed in as uid: %s\n", auth.token.uid.c_str());
  else Serial.println("Warning: token not ready yet (will retry while running).");
}

void detectKeypads(){
  hasKP1 = devicePresent(I2C_A, KEYPAD1_ADDR);
  hasKP2 = devicePresent(I2C_B, KEYPAD2_ADDR);
  Serial.printf("[I2C] KP1@0x%02X on (%d,%d): %s,  KP2@0x%02X on (%d,%d): %s\n",
    KEYPAD1_ADDR, KP1_SDA, KP1_SCL, hasKP1?"OK":"missing",
    KEYPAD2_ADDR, KP2_SDA, KP2_SCL, hasKP2?"OK":"missing");
}

// simple sensor debug to Serial (every ~0.5s)
uint32_t g_dbgLast = 0;
void debugSensors() {
#if SERIAL_SHOW_SENSORS
  static uint32_t g_dbgLast = 0;
  if (millis() - g_dbgLast < 500) return;
  g_dbgLast = millis();
  Serial.printf("[DBG] B1 rawDoor=%d closed=%d dist=%dcm | B2 rawDoor=%d closed=%d dist=%dcm\n",
    digitalRead(box1.doorPin), box1.doorClosed, box1.lastDist,
    digitalRead(box2.doorPin), box2.doorClosed, box2.lastDist);
#endif
}


void setup() {
  Serial.begin(115200);
  delay(100);

  setupBoxPins(box1);
  setupBoxPins(box2);

  I2C_A.begin(KP1_SDA, KP1_SCL); I2C_A.setClock(100000);
  I2C_B.begin(KP2_SDA, KP2_SCL); I2C_B.setClock(100000);
  kpad1.begin(); kpad2.begin();
  detectKeypads();

  wifiConnect();
  initFirebase();
  webInit();

  // --- init single shared buzzer ---
  pinMode(BZZR_PIN, OUTPUT);
#ifdef ACTIVE_BUZZER
  digitalWrite(BZZR_PIN, LOW);
#else
  noTone(BZZR_PIN);
#endif

  Serial.println(F("Center ready: 2 boxes, email/password RTDB, LEDs swapped, keypad red blip, door-open success, snapshots, web UI + HTTP API. (No device Firestore writes.)"));
}

void loop() {
  wifiConnect();

  if (!ntpOk) {
    syncTimeOnce();   // only retry until time is set
  }

  fetchCodeIfDue(box1, PATH_BOX1, "BOX1");
  fetchCodeIfDue(box2, PATH_BOX2, "BOX2");

  enforceExpiry(box1);
  enforceExpiry(box2);

  serviceSensors(box1);
  serviceSensors(box2);

  applyPresencePolicy(box1);
  applyPresencePolicy(box2);

  // drive the loop (each box has its own camera URL)
  serviceSnapshotLoop(box1, CAM1_SNAP_URL, "BOX1");
  serviceSnapshotLoop(box2, CAM2_SNAP_URL, "BOX2");

  if (hasKP1) serviceKeypad(box1, kpad1, attempts1);
  if (hasKP2) serviceKeypad(box2, kpad2, attempts2);

  enforceUnlockWindowEnd(box1);
  enforceUnlockWindowEnd(box2);

  // 🔊 Listen for admin override commands from RTDB
  pollUnlockOverride(box1, "box1", g_lastOverrideAtMsB1, g_lastOverridePollMsB1);
  pollUnlockOverride(box2, "box2", g_lastOverrideAtMsB2, g_lastOverridePollMsB2);

  pollQrUnlock(box1, "box1", g_lastQrSeenAtMsB1, g_lastQrPollMsB1);
  pollQrUnlock(box2, "box2", g_lastQrSeenAtMsB2, g_lastQrPollMsB2);

  server.handleClient();

  debugSensors();
}
