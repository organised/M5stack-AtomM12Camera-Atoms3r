/*
 * AtomViewer — display unit sketch
 * Board: M5Stack AtomS3R (0.85" 128x128 LCD)
 *
 * Joins the camera unit's WiFi access point, opens its MJPEG stream
 * (http://192.168.4.1/stream) and decodes each JPEG straight to the
 * LCD with M5GFX's drawJpg(). Reconnects automatically if either the
 * WiFi link or the HTTP stream drops.
 *
 * Arduino IDE settings:
 *   Board:           "M5AtomS3" (m5stack boards) or "ESP32S3 Dev Module"
 *   PSRAM:           "OPI PSRAM"
 *   USB CDC on boot: "Enabled"
 *   Library:         M5Unified (pulls in M5GFX)
 */

#include <M5Unified.h>
#include <WiFi.h>

// ---------- must match the camera sketch ----------
static const char *AP_SSID     = "AtomCam-Stream";
static const char *AP_PASSWORD = "atomcam1234";

static const IPAddress CAM_IP(192, 168, 4, 1);  // softAP default address
static const uint16_t  CAM_PORT = 80;

// Static IP: skips DHCP so (re)connects are instant and deterministic.
static const IPAddress MY_IP(192, 168, 4, 2);
static const IPAddress GATEWAY(192, 168, 4, 1);
static const IPAddress SUBNET(255, 255, 255, 0);

static WiFiClient stream;

// 128x128 @ quality 12 is typically 3-6 KB; 32 KB leaves huge headroom.
static const size_t JPG_BUF_SIZE = 32768;
static uint8_t jpgBuf[JPG_BUF_SIZE];

static uint32_t frameCount = 0;
static uint32_t fpsWindowStart = 0;

static void showStatus(const char *line1, const char *line2 = "") {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(line1, 64, 54, &fonts::Font2);
  if (line2[0]) M5.Display.drawString(line2, 64, 74, &fonts::Font2);
}

static void connectWiFi() {
  showStatus("Joining", AP_SSID);
  Serial.printf("Connecting to %s", AP_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // no modem sleep -> lower latency
  WiFi.config(MY_IP, GATEWAY, SUBNET);
  WiFi.begin(AP_SSID, AP_PASSWORD);

  // WPA2 4-way handshake + association typically completes well under 5 s.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    if (millis() - start > 15000) {  // camera not up yet? start over
      Serial.println(" timeout, retrying");
      WiFi.disconnect(true);
      delay(500);
      start = millis();
      WiFi.begin(AP_SSID, AP_PASSWORD);
    }
  }
  Serial.printf("\nConnected, IP=%s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static bool openStream() {
  showStatus("Opening", "stream...");
  Serial.println("Opening /stream");

  stream.stop();
  if (!stream.connect(CAM_IP, CAM_PORT, 3000)) {
    Serial.println("TCP connect failed");
    return false;
  }
  stream.setTimeout(3000);
  stream.print("GET /stream HTTP/1.1\r\n"
               "Host: 192.168.4.1\r\n"
               "Connection: keep-alive\r\n\r\n");

  // Read the HTTP status line and check for 200.
  String status = stream.readStringUntil('\n');
  if (status.indexOf("200") < 0) {
    Serial.printf("Bad HTTP status: %s\n", status.c_str());
    stream.stop();
    return false;
  }
  // Skip remaining response headers (up to the blank line); part headers follow.
  while (true) {
    String line = stream.readStringUntil('\n');
    if (line.length() == 0) { stream.stop(); return false; }  // timed out
    if (line == "\r" || line == "") break;
  }
  frameCount = 0;
  fpsWindowStart = millis();
  return true;
}

/*
 * Pull one multipart chunk: skip boundary/part-header lines until we've
 * seen Content-Length and the blank line, then read exactly that many
 * bytes of JPEG. Returns false on any timeout/parse problem so the
 * caller can tear down and reconnect.
 */
static bool readFrame(size_t &jpgLen) {
  int contentLength = -1;
  for (int i = 0; i < 12; i++) {  // boundary + a few headers, generous cap
    String line = stream.readStringUntil('\n');
    if (line.length() == 0 && !stream.connected()) return false;
    line.trim();
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    } else if (line.length() == 0 && contentLength > 0) {
      break;  // blank line after headers: JPEG bytes come next
    }
  }
  if (contentLength <= 0 || (size_t)contentLength > JPG_BUF_SIZE) {
    Serial.printf("Bad frame length: %d\n", contentLength);
    return false;
  }

  size_t got = stream.readBytes(jpgBuf, contentLength);
  if (got != (size_t)contentLength) {
    Serial.printf("Short read: %u/%d\n", got, contentLength);
    return false;
  }
  // Sanity check: JPEG must start with the FF D8 SOI marker. If it doesn't,
  // we're misaligned with the stream (e.g. server framing changed) —
  // reconnect rather than feeding garbage to the decoder.
  if (jpgBuf[0] != 0xFF || jpgBuf[1] != 0xD8) {
    Serial.printf("Not a JPEG (starts %02X %02X %02X %02X) - misaligned stream\n",
                  jpgBuf[0], jpgBuf[1], jpgBuf[2], jpgBuf[3]);
    return false;
  }
  jpgLen = got;
  return true;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(128);
  Serial.begin(115200);

  connectWiFi();
}

void loop() {
  M5.update();

  if (WiFi.status() != WL_CONNECTED) {
    stream.stop();
    connectWiFi();
    return;
  }

  if (!stream.connected()) {
    if (!openStream()) {
      showStatus("Camera", "not found");
      delay(1000);
    }
    return;
  }

  size_t jpgLen;
  if (!readFrame(jpgLen)) {
    Serial.println("Stream lost, reconnecting");
    stream.stop();
    return;
  }

  if (!M5.Display.drawJpg(jpgBuf, jpgLen, 0, 0)) {
    Serial.printf("JPEG decode failed (%u bytes)\n", jpgLen);
    return;  // don't count failed frames in the fps figure
  }

  // FPS to the serial monitor once a second
  frameCount++;
  uint32_t now = millis();
  if (now - fpsWindowStart >= 1000) {
    Serial.printf("%.1f fps\n", frameCount * 1000.0f / (now - fpsWindowStart));
    frameCount = 0;
    fpsWindowStart = now;
  }
}
