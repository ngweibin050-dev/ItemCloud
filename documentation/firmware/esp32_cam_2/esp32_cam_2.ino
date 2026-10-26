// ESP32-CAM → Cloudinary (SIGNED with folder via Apps Script) + Firestore auto-increment (no CF)
// Endpoints: / (viewer), /snap (capture+upload → JSON), /current.json, /jpg, /latest.jpg

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include <time.h>

// Credentials live in secrets.h (git-ignored). Copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"

// Find a quoted string value for a JSON key, allowing spaces after colon.
// Returns true and fills `out` if found: e.g. key "integerValue" -> "123".
bool jsonGetQuotedValue(const String& json, const char* key, String& out) {
  int k = json.indexOf(String("\"") + key + "\"");
  if (k < 0) return false;
  int colon = json.indexOf(':', k);
  if (colon < 0) return false;
  // skip spaces
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length() || json[i] != '\"') return false;
  int start = i + 1;
  int end = json.indexOf('\"', start);
  if (end < 0) return false;
  out = json.substring(start, end);
  return true;
}

/* ===== Wi-Fi ===== */
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* MDNS_NAME = "esp32-cam-2";            // http://esp32-cam-2.local/

/* ===== Cloudinary ===== */
const char* CLD_CLOUD_NAME = SECRET_CLD_CLOUD_NAME;
const char* CLD_API_KEY    = SECRET_CLD_API_KEY;

// Fallback (before first successful upload)
const char* CLD_PUBLIC_ID  = "esp32cam/box2/item";

// Folder-signed Apps Script URL (your latest)
const char* SIGNER_URL = SECRET_SIGNER_BASE_URL "?folder=box2&tags=esp32cam,box2";

/* ===== Firebase (Firestore via REST, Anonymous Auth) ===== */
const char* FB_API_KEY    = SECRET_FB_API_KEY;
const char* FB_PROJECT_ID = SECRET_FB_PROJECT_ID;

String g_idToken;
uint32_t g_tokenObtainedMs = 0;
const uint32_t TOKEN_MAX_AGE_MS = 50UL * 60UL * 1000UL;

/* ===== Camera pins (AI Thinker) ===== */
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

/* ===== Built-in flash control (GPIO 4) ===== */
#define FLASH_PIN 4
#define FLASH_WARMUP_MS 140  // try 140–200ms if you need more light
inline void flash_on()  { digitalWrite(FLASH_PIN, HIGH); }
inline void flash_off() { digitalWrite(FLASH_PIN, LOW); }

WebServer server(80);

/* ---- Local snapshot buffer (/latest.jpg fallback) ---- */
volatile bool camBusy = false;
uint8_t* last_jpg = nullptr;
size_t   last_len = 0;
size_t   last_cap = 0;

/* Cloudinary last uploaded info (for viewer) */
String lastCloudinaryVersion = "";
String lastCloudinaryURL     = "";   // raw secure_url of the newest upload

/* ===== Helpers: Wi-Fi, buffer, time, Cloudinary URL transform ===== */
void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
}

static void dumpMem(const char* tag){
  Serial.printf("[%s] heap=%u psram=%u 8bit=%u\n", tag,
    ESP.getFreeHeap(),
    ESP.getFreePsram(),
    heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

static bool ensure_capacity(size_t need) {
  if (last_cap >= need) return true;
  size_t newCap = (need + 4095) & ~((size_t)4095);
  void* p = heap_caps_realloc(last_jpg, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return false;
  last_jpg = (uint8_t*)p;
  last_cap = newCap;
  return true;
}
static bool copy_to_last(const uint8_t* src, size_t len) {
  if (!ensure_capacity(len)) return false;
  memcpy(last_jpg, src, len);
  last_len = len;
  return true;
}

// Inject Cloudinary delivery transforms for snappy viewing
String cldOptimized(const String& raw, const String& opts = "f_auto,q_auto:eco,w_900") {
  int p = raw.indexOf("/image/upload/");
  if (p < 0) return raw;
  String pre  = raw.substring(0, p + 14); // includes trailing '/'
  String rest = raw.substring(p + 14);
  return pre + opts + "/" + rest;
}

// NTP → ISO8601 UTC (Firestore timestamp)
String iso8601_utc_now() {
  time_t nowSec = time(nullptr);
  struct tm tmUtc;
  gmtime_r(&nowSec, &tmUtc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return String(buf);
}

/* ===== Camera init ===== */
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 28;             // higher number = more compression
    config.fb_count     = 1;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 36;
    config.fb_count     = 1;
  }

  if (!last_jpg) {
    last_cap = 256 * 1024;
    last_jpg = (uint8_t*)heap_caps_malloc(last_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!last_jpg) last_jpg = (uint8_t*)heap_caps_malloc(last_cap, MALLOC_CAP_8BIT);
    if (!last_jpg) last_cap = 0;
  }

  return esp_camera_init(&config) == ESP_OK;
}

/* ===== Firebase: Anonymous sign-in ===== */
bool firebaseEnsureToken() {
  if (g_idToken.length() && (millis() - g_tokenObtainedMs) < TOKEN_MAX_AGE_MS) return true;

  wifiEnsure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);

  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=" + String(FB_API_KEY);
  if (!http.begin(url)) { Serial.println("[FB] begin signUp failed"); return false; }

  // Avoid gzip; some Arduino cores do not auto-decompress
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Content-Type", "application/json");

  int code = http.POST("{}");
  String body = http.getString();
  http.end();

  Serial.printf("[FB] signUp HTTP %d\n", code);
  Serial.println("[FB] signUp body:");
  Serial.println(body);

  if (code != 200) return false;

  // Robust parse: "idToken":"..."
  int k = body.indexOf("\"idToken\"");
  if (k < 0) { Serial.println("[FB] idToken key missing"); return false; }
  k = body.indexOf(':', k);
  while (k + 1 < (int)body.length() && body[k + 1] == ' ') k++;
  if (k + 1 >= (int)body.length() || body[k + 1] != '\"') { Serial.println("[FB] idToken value not a string"); return false; }
  int start = k + 2;
  int end = body.indexOf('\"', start);
  if (end < 0) { Serial.println("[FB] idToken closing quote missing"); return false; }

  g_idToken = body.substring(start, end);
  g_tokenObtainedMs = millis();
  Serial.println("[FB] Anonymous auth OK");
  return true;
}

/* ===== Firestore helpers ===== */

// zero-pad: padZeros(7,3) => "007"
String padZeros(long n, int width=3) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%0*ld", width, n);
  return String(buf);
}

// Read counters/item.n (lowercase)
long firestoreCounterRead() {
  if (!firebaseEnsureToken()) { Serial.println("[FB] No token for read"); return -1; }

  String url =
    "https://firestore.googleapis.com/v1/projects/" + String(FB_PROJECT_ID) +
    "/databases/(default)/documents/counters/item?mask.fieldPaths=n";

  HTTPClient http;
  if (!http.begin(url)) { Serial.println("[FB] begin(read) failed"); return -1; }

  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Authorization", "Bearer " + g_idToken);

  int code = http.GET();
  String resp = http.getString();
  http.end();

  Serial.printf("[FB] read counter HTTP %d (len=%d)\n", code, resp.length());
  if (code != 200) { Serial.println(resp); return -1; }

  String val;
  if (jsonGetQuotedValue(resp, "integerValue", val)) return val.toInt();
  if (jsonGetQuotedValue(resp, "doubleValue",  val)) return (long)(val.toDouble() + 0.5);

  Serial.println("[FB] numeric value not found in counter");
  return -1;
}

// Increment counters/item.n and return the new value
bool firestoreIncrementAndGet(long &outN) {
  outN = -1;
  if (!firebaseEnsureToken()) { Serial.println("[FB] No token for inc+get"); return false; }

  String url =
    "https://firestore.googleapis.com/v1/projects/" + String(FB_PROJECT_ID) +
    "/databases/(default)/documents:commit";
  String counterDoc =
    "projects/" + String(FB_PROJECT_ID) +
    "/databases/(default)/documents/counters/item";   // << lowercase

  String body =
    "{"
      "\"writes\":[{"
        "\"transform\":{"
          "\"document\":\"" + counterDoc + "\","
          "\"fieldTransforms\":[{"
            "\"fieldPath\":\"n\","
            "\"increment\":{\"integerValue\":\"1\"}"
          "}]"
        "},"
        "\"currentDocument\":{\"exists\":true}"
      "}]"
    "}";

  HTTPClient http;
  if (!http.begin(url)) { Serial.println("[FB] begin(commit) failed"); return false; }
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Authorization", "Bearer " + g_idToken);

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[FB] commit(increment) HTTP %d\n", code);
  if (code != 200) { Serial.println(resp); return false; }

  int tr = resp.indexOf("\"transformResults\"");
  if (tr >= 0) {
    String val;
    String tail = resp.substring(tr);
    if (jsonGetQuotedValue(tail, "integerValue", val)) { outN = val.toInt(); if (outN > 0) return true; }
    if (jsonGetQuotedValue(tail, "doubleValue",  val)) { outN = (long)(val.toDouble() + 0.5); if (outN > 0) return true; }
  }

  long nowN = firestoreCounterRead();
  if (nowN > 0) { outN = nowN; return true; }
  return false;
}

// Get next ID like I001, I002...
String firestoreNextItemId() {
  long n = -1;
  for (int tries = 0; tries < 3 && n <= 0; ++tries) {
    if (!firestoreIncrementAndGet(n)) {
      Serial.printf("[FB] inc+get attempt %d failed\n", tries + 1);
      delay(150);
    }
  }
  if (n > 0) return "I" + padZeros(n, 3);
  return "";
}

// Create LostItems/<itemID> with your expected fields
// fields: { itemID, imageUrl, createdAt, status:'lost', box }
bool firestoreCreateLostItem(const String& itemId,
                             const String& imageUrl,
                             const String& isoUtc,
                             const String& box /* "" to omit */) {
  if (!firebaseEnsureToken()) { Serial.println("[FB] No token for create"); return false; }

  String endpoint =
    "https://firestore.googleapis.com/v1/projects/" + String(FB_PROJECT_ID) +
    "/databases/(default)/documents/LostItems?documentId=" + itemId;

  // Build fields JSON
  String fields =
    "\"itemID\":{\"stringValue\":\"" + itemId   + "\"},"
    "\"imageUrl\":{\"stringValue\":\"" + imageUrl + "\"},"
    "\"createdAt\":{\"timestampValue\":\"" + isoUtc + "\"},"
    "\"status\":{\"stringValue\":\"lost\"}";
  if (box.length()) fields += ",\"box\":{\"stringValue\":\"" + box + "\"}";

  String json = "{ \"fields\": { " + fields + " } }";

  HTTPClient http;
  if (!http.begin(endpoint)) { Serial.println("[FB] begin(create POST) failed"); return false; }
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + g_idToken);

  int code = http.POST(json);
  String body = http.getString();
  http.end();

  if (code != 200 && code != 201) {
    Serial.printf("[FB] create LostItems/%s HTTP %d\n", itemId.c_str(), code);
    Serial.println(body);
    return false;
  }
  Serial.printf("[FB] Created LostItems/%s\n", itemId.c_str());
  return true;
}

/* ===== Signer: read {signature, timestamp, folder, tags} ===== */
bool getSignature(String& signature, String& timestamp, String& folder, String& tags) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, SIGNER_URL)) {
    Serial.println("[Signer] begin() failed");
    return false;
  }

  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.printf("[Signer] HTTP %d\n", code);
  if (code != 200) { Serial.println(body); return false; }

  int s1 = body.indexOf("\"signature\":\"");
  int s2 = body.indexOf("\"", s1 + 13);
  int t1 = body.indexOf("\"timestamp\":");
  int t2 = body.indexOf(",", t1 + 12); if (t2 < 0) t2 = body.indexOf("}", t1 + 12);
  int f1 = body.indexOf("\"folder\":\"");
  int f2 = body.indexOf("\"", f1 + 10);
  int g1 = body.indexOf("\"tags\":\"");
  int g2 = body.indexOf("\"", g1 + 8);
  if (s1 < 0 || s2 < 0 || t1 < 0 || f1 < 0 || f2 < 0 || g1 < 0 || g2 < 0) return false;

  signature = body.substring(s1 + 13, s2);
  timestamp = body.substring(t1 + 12, t2); timestamp.trim();
  folder    = body.substring(f1 + 10, f2);
  tags      = body.substring(g1 + 8, g2);
  return signature.length() && timestamp.length() && folder.length();
}

/* ===== Signed upload to Cloudinary (uses 'folder') ===== */
bool uploadToCloudinarySigned(const uint8_t* jpg, size_t len, String& secureUrlOut, String& newVersionOut) {
  String sig, ts, folder, tags;
  if (!getSignature(sig, ts, folder, tags)) {
    Serial.println("[CLD] signer failed");
    return false;
  }

  String boundary = "----ESP32SignedBoundary" + String(millis());

  String a_apiKey =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"api_key\"\r\n\r\n" +
    String(CLD_API_KEY) + "\r\n";

  String a_timestamp =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"timestamp\"\r\n\r\n" +
    ts + "\r\n";

  String a_folder =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"folder\"\r\n\r\n" +
    folder + "\r\n";

  String a_tags =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"tags\"\r\n\r\n" +
    tags + "\r\n";

  String a_signature =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"signature\"\r\n\r\n" +
    sig + "\r\n";

  String a_file =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"latest.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = a_apiKey.length() + a_timestamp.length() + a_folder.length() +
                         a_tags.length() + a_signature.length() + a_file.length() +
                         len + tail.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  const char* host = "api.cloudinary.com";
  if (!client.connect(host, 443)) {
    Serial.println("[CLD] connect() failed");
    return false;
  }

  String urlPath = String("/v1_1/") + CLD_CLOUD_NAME + "/image/upload";
  client.print(String("POST ") + urlPath + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + "\r\n");
  client.print("User-Agent: ESP32-CAM\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + contentLength + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(a_apiKey);
  client.print(a_timestamp);
  client.print(a_folder);
  client.print(a_tags);
  client.print(a_signature);
  client.print(a_file);
  client.write(jpg, len);
  client.print(tail);

  // Read response
  uint32_t t0 = millis();
  while (!client.available() && (millis() - t0) < 20000) delay(10);

  // replace the "String resp" accumulation:
  String statusLine;
  while (client.connected() && !client.available()) delay(1);
  while (client.available()) {
    char c = client.read();
    statusLine += c;
    if (statusLine.endsWith("\r\n")) break;
  }
  if (statusLine.indexOf(" 200 ") < 0 && statusLine.indexOf(" 201 ") < 0) { client.stop(); return false; }

  // skip headers
  int crlfCount = 0;
  while (client.connected()) {
    if (!client.available()) { delay(1); continue; }
    char c = client.read();
    if (c == '\r') continue;
    if (c == '\n') { if (++crlfCount == 2) break; }
    else crlfCount = 0;
  }

  // read just enough of the JSON body to extract fields
  String body;
  while (client.connected() || client.available()) {
    while (client.available()) body += (char)client.read();
    if (body.length() > 4096) break; // safety cap
    delay(1);
  }
  client.stop();

  // parse secure_url and version
  int su1 = body.indexOf("\"secure_url\":\"");
  if (su1 >= 0) {
    int su2 = body.indexOf("\"", su1 + 14);
    if (su2 > su1) secureUrlOut = body.substring(su1 + 14, su2);
  }
  int v1 = body.indexOf("\"version\":");
  if (v1 >= 0) {
    int v2 = body.indexOf(",", v1 + 10); if (v2 < 0) v2 = body.indexOf("}", v1 + 10);
    if (v2 > v1) { newVersionOut = body.substring(v1 + 10, v2); newVersionOut.trim(); }
  }
  return secureUrlOut.length() > 0;

}

/* ===== HTTP Handlers ===== */
void handleCurrentJson() {
  String raw;
  if (lastCloudinaryURL.length()) {
    raw = lastCloudinaryURL;
  } else {
    raw = "https://res.cloudinary.com/" + String(CLD_CLOUD_NAME) + "/image/upload/";
    if (lastCloudinaryVersion.length()) raw += "v" + lastCloudinaryVersion + "/";
    raw += CLD_PUBLIC_ID; raw += ".jpg";
  }
  String turl = cldOptimized(raw);
  String body = "{\"url\":\"" + raw + "\",\"turl\":\"" + turl + "\"}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", body);
}

void handleRoot() {
  String raw;
  if (lastCloudinaryURL.length()) {
    raw = lastCloudinaryURL;
  } else {
    raw = "https://res.cloudinary.com/" + String(CLD_CLOUD_NAME) + "/image/upload/";
    if (lastCloudinaryVersion.length()) raw += "v" + lastCloudinaryVersion + "/";
    raw += CLD_PUBLIC_ID; raw += ".jpg";
  }
  String imgUrl = cldOptimized(raw);

  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM Latest</title>"
    "<style>body{font-family:Arial;margin:16px}"
    "button{padding:8px 12px;border:1px solid #888;border-radius:6px;background:#f6f6f6;cursor:pointer}"
    "img{max-width:100%;height:auto;border:1px solid #ccc;border-radius:8px}</style>"
    "<script>"
    "async function refreshBase(){try{const r=await fetch('/current.json',{cache:'no-store'});"
    "if(!r.ok)return;const j=await r.json();const el=document.getElementById('img');"
    "const base=(j.turl||j.url);if(el&&base&&el.dataset.base!==base){el.dataset.base=base;el.src=base+'?t='+Date.now();}}catch(e){}}"
    "async function snap(){try{const r=await fetch('/snap',{cache:'no-store'});const j=await r.json();"
    "const base=(j.turl||j.url);if(base){const el=document.getElementById('img');el.dataset.base=base;el.src=base+'?t='+Date.now();}}catch(e){alert('Snap failed');}}"
    "setInterval(()=>{refreshBase();const el=document.getElementById('img');if(el) el.src=el.dataset.base+'?t='+Date.now();},4000);"
    "</script>"
    "</head><body>"
    "<h2>ESP32-CAM → Cloudinary (folder-signed) → Firestore auto-increment</h2>"
    "<p><button onclick='snap()'>Capture & Upload Now</button></p>"
    "<img id='img' data-base='" + imgUrl + "' src='" + imgUrl + "'>"
    "<p style='font-size:12px;color:#666'>Local fallback: <a href='/latest.jpg'>/latest.jpg</a></p>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void handleJpg() { // direct capture (no upload)
  if (camBusy) { server.send(429, "text/plain", "Busy"); return; }
  camBusy = true;

  // Turn flash on for the capture
  flash_on();
  delay(FLASH_WARMUP_MS);

  camera_fb_t* fb = esp_camera_fb_get();

  // Always turn flash off right after we grab a frame
  flash_off();

  if (!fb || fb->format != PIXFORMAT_JPEG) {
    if (fb) esp_camera_fb_return(fb);
    camBusy = false;
    server.send(503, "text/plain", "Camera fail");
    return;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(fb->buf, fb->len);
  client.flush();
  esp_camera_fb_return(fb);
  camBusy = false;
}

void handleSnap() {
  if (camBusy) { server.send(429, "text/plain", "Busy"); return; }
  camBusy = true;

  flash_on();
  delay(FLASH_WARMUP_MS);
  camera_fb_t* fb = esp_camera_fb_get();
  flash_off();

  if (!fb || fb->format != PIXFORMAT_JPEG) {
    if (fb) esp_camera_fb_return(fb);
    camBusy = false;
    server.send(503, "text/plain", "Snap fail");
    return;
  }

  bool copied = copy_to_last(fb->buf, fb->len);
  esp_camera_fb_return(fb);

  String secureUrl, newVer;
  bool okUpload = copied
      ? uploadToCloudinarySigned(last_jpg, last_len, secureUrl, newVer)  // <<< use the copy
      : false;

  if (okUpload) {
    lastCloudinaryURL     = secureUrl;
    lastCloudinaryVersion = newVer;

    String viewUrl = cldOptimized(secureUrl);
    wifiEnsure();
    String iso = iso8601_utc_now();

    // === auto-increment: I001, I002, ...
    String nextId = firestoreNextItemId();
    if (nextId.length()) {
      const String boxName = "box2";                  // <- set your box id/name
      firestoreCreateLostItem(nextId, viewUrl, iso, boxName);
    } else {
      Serial.println("[FB] Failed to get next item id; skipping LostItems create.");
    }

    String body = "{\"ok\":true,\"url\":\"" + secureUrl + "\",\"turl\":\"" + viewUrl + "\"}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", body);
    Serial.printf("Uploaded. url=%s\n", lastCloudinaryURL.c_str());
  } else {
    if (!copied) { server.send(507, "text/plain", "Insufficient memory"); camBusy = false; return; }
    server.send(500, "text/plain", "Captured OK but upload failed.");
  }

  camBusy = false;
}

void handleLatestJpg() { // local fallback
  if (!last_jpg || !last_len) { server.send(404, "text/plain", "No local image"); return; }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(last_len);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  size_t wrote = client.write(last_jpg, last_len);
  client.flush();
  if (wrote != last_len) {
    Serial.printf("Warn: wrote %u/%u bytes\n", (unsigned)wrote, (unsigned)last_len);
  }
}

/* ===== Setup / Loop ===== */
void setup() {
  Serial.begin(115200);
  delay(200);

  if (!initCamera()) {
    Serial.println("Camera init failed");
    while (true) delay(1000);
  }
  Serial.println("Camera init OK");

  // Flash pin
  pinMode(FLASH_PIN, OUTPUT);
  flash_off();

  wifiEnsure();

  // NTP (UTC)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
  } else {
    Serial.println("mDNS start failed");
  }

  server.on("/",             handleRoot);
  server.on("/jpg",          HTTP_GET, handleJpg);
  server.on("/snap",         HTTP_GET, handleSnap);
  server.on("/latest.jpg",   HTTP_GET, handleLatestJpg);
  server.on("/current.json", HTTP_GET, handleCurrentJson);
  server.begin();
  Serial.println("HTTP: /  /jpg  /snap  /latest.jpg  /current.json");
}

void loop() {
  server.handleClient();
}
