/*
 * AtomCamAP — camera unit sketch
 * Board: M5Stack AtomS3R Camera Kit, M12 version (OV3660)
 *
 * Runs the ESP32-S3 as a standalone WiFi Access Point (no router needed)
 * and serves the camera as an MJPEG HTTP stream:
 *
 *   http://192.168.4.1/        - info page
 *   http://192.168.4.1/jpg     - single JPEG snapshot (browser test)
 *   http://192.168.4.1/stream  - MJPEG stream (what the AtomS3R viewer uses)
 *
 * Arduino IDE settings:
 *   Board:        "M5AtomS3" (m5stack boards) or "ESP32S3 Dev Module"
 *   PSRAM:        "OPI PSRAM"   <- required, frame buffers live in PSRAM
 *   USB CDC on boot: "Enabled"  (for Serial monitor)
 *   Requires arduino-esp32 core >= 2.0.7 (for FRAMESIZE_128X128).
 *   If FRAMESIZE_128X128 doesn't compile, use FRAMESIZE_QQVGA instead.
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ---------- WiFi credentials (the viewer sketch must match these) ----------
static const char *AP_SSID     = "AtomCam-Stream";
static const char *AP_PASSWORD = "atomcam1234";   // WPA2, min 8 chars
static const int   AP_CHANNEL  = 6;
static const int   AP_MAX_CONN = 2;

// ---------- AtomS3R-CAM / AtomS3R-M12 pin map (from M5Stack camera_pins.h) ----------
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   21
#define SIOD_GPIO_NUM   12
#define SIOC_GPIO_NUM   9
#define Y9_GPIO_NUM     13
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     17
#define Y6_GPIO_NUM     4
#define Y5_GPIO_NUM     48
#define Y4_GPIO_NUM     46
#define Y3_GPIO_NUM     42
#define Y2_GPIO_NUM     3
#define VSYNC_GPIO_NUM  10
#define HREF_GPIO_NUM   14
#define PCLK_GPIO_NUM   40
#define POWER_GPIO_NUM  18   // must be pulled LOW to power the camera

static httpd_handle_t stream_httpd = NULL;

// ---------- MJPEG stream handler ----------
#define PART_BOUNDARY "atomframe"
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_FMT     = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  // Send the response raw with httpd_send() rather than the
  // httpd_resp_send_chunk() API: the chunk API wraps every write in HTTP
  // chunked transfer encoding, whose framing bytes corrupt the stream for
  // simple clients (like the AtomViewer) that read the socket directly.
  static const char RESP_HDR[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace;boundary=" PART_BOUNDARY "\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Connection: close\r\n"
      "\r\n";
  if (httpd_send(req, RESP_HDR, sizeof(RESP_HDR) - 1) < 0) return ESP_FAIL;

  char part_hdr[64];
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Frame capture failed");
      return ESP_FAIL;
    }

    size_t hlen = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART_FMT, fb->len);
    bool ok = httpd_send(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) >= 0
           && httpd_send(req, part_hdr, hlen) >= 0
           && httpd_send(req, (const char *)fb->buf, fb->len) >= 0;
    esp_camera_fb_return(fb);

    if (!ok) return ESP_FAIL;  // client disconnected; httpd closes the socket
  }
}

// ---------- single snapshot, handy for testing in a browser ----------
static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  static const char page[] =
      "<html><body style='font-family:sans-serif'>"
      "<h2>AtomS3R Camera</h2>"
      "<p><a href='/jpg'>Snapshot</a> | <a href='/stream'>Stream</a></p>"
      "<img src='/stream' width='256' height='256'>"
      "</body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void start_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 4;

  httpd_uri_t uri_index  = { .uri = "/",       .method = HTTP_GET, .handler = index_handler,  .user_ctx = NULL };
  httpd_uri_t uri_jpg    = { .uri = "/jpg",    .method = HTTP_GET, .handler = jpg_handler,    .user_ctx = NULL };
  httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &uri_index);
    httpd_register_uri_handler(stream_httpd, &uri_jpg);
    httpd_register_uri_handler(stream_httpd, &uri_stream);
  }
}

static bool init_camera() {
  // AtomS3R-M12 hardware quirk: GPIO18 must go LOW before the sensor will
  // answer on I2C. Do this first, then give the sensor time to wake.
  pinMode(POWER_GPIO_NUM, OUTPUT);
  digitalWrite(POWER_GPIO_NUM, LOW);
  delay(500);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_128X128;  // matches the AtomS3R's 128x128 LCD
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;  // always serve the freshest frame

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  // OV3660 tuning (same tweaks the stock CameraWebServer example applies)
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // OV3660 modules are mounted upside down
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!psramFound()) {
    Serial.println("PSRAM not found - enable OPI PSRAM in Tools menu!");
  }

  if (!init_camera()) {
    Serial.println("Rebooting in 3s...");
    delay(3000);
    ESP.restart();
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONN);
  esp_wifi_set_ps(WIFI_PS_NONE);  // no modem sleep -> lower latency

  start_server();

  Serial.println();
  Serial.printf("AP up:  SSID=%s  PASS=%s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Stream: http://%s/stream\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  // Everything runs in the HTTP server tasks; just report client count.
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("Stations connected: %d\n", WiFi.softAPgetStationNum());
  }
}
