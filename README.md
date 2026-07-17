# M5stack-AtomM12Camera-Atoms3r
A Streaming project between m5Stackm12 camera and m5stackatoms3r


AtomS3R Wireless Camera Viewer
Stream live video from an M5Stack AtomS3R Camera Kit (M12, OV3660) to the 128×128 LCD of a second AtomS3R — completely wirelessly, with no router, no app, and no PC involved.

The camera runs as its own WiFi access point and serves an MJPEG HTTP stream; the viewer joins it directly and decodes each JPEG straight to the screen. Point-to-point WiFi keeps latency low, and both units recover automatically if either one is power-cycled. Typical performance is 15–25 fps at 128×128.

┌─────────────────┐   WPA2 AP (no router)   ┌─────────────────┐
│ AtomS3R-M12 cam │ ──────────────────────► │ AtomS3R viewer  │
│ OV3660 sensor   │   MJPEG over HTTP       │ 128×128 LCD     │
│ 192.168.4.1     │   http://…/stream       │ 192.168.4.2     │
└─────────────────┘                          └─────────────────┘

Hardware
Unit	Product	Role
Camera	AtomS3R Camera Kit, M12 version (OV3660)	WiFi access point + MJPEG server
Viewer	AtomS3R (0.85" 128×128 LCD)	Stream client + JPEG decoder
How it works
AtomCamAP/AtomCamAP.ino (camera) — brings up the OV3660 with the esp32-camera driver, captures JPEG at 128×128 (quality 12 — matching the viewer's LCD, so no bandwidth is wasted), and serves it with ESP-IDF's HTTP server:
/ — test page (open from any phone/laptop joined to the AP)
/jpg — single snapshot
/stream — MJPEG stream (multipart/x-mixed-replace), sent raw without chunked transfer encoding so simple embedded clients can parse it
AtomViewer/AtomViewer.ino (display) — joins the AP with a static IP (no DHCP wait), opens /stream, parses each multipart frame by its Content-Length, validates the JPEG SOI marker, and draws it with M5GFX's drawJpg(). Reconnects automatically if WiFi or the stream drops; prints fps to the serial monitor.
WiFi credentials (embedded in both sketches)
SSID: AtomCam-Stream
Password: atomcam1234 (WPA2)
Camera: 192.168.4.1 · Viewer: 192.168.4.2
Change them in both .ino files if you like — they just have to match.

Building (Arduino IDE)
Boards manager: install esp32 by Espressif, version ≥ 2.0.7 (needed for FRAMESIZE_128X128), or the M5Stack board package.
Library manager: install M5Unified (viewer only; pulls in M5GFX).
Tools settings for both boards:
Board: M5AtomS3 (or ESP32S3 Dev Module)
PSRAM: OPI PSRAM ← required on the camera (frame buffers live there)
USB CDC On Boot: Enabled
Bring-up
Flash AtomCamAP to the camera. Serial monitor should show AP up: SSID=AtomCam-Stream.
Browser test: join AtomCam-Stream from a phone and open http://192.168.4.1/ — you should see live video before going further.
Flash AtomViewer to the display unit. It joins, connects, and draws.
Either unit can be power-cycled in any order; the viewer retries until the camera reappears.

Gotchas learned the hard way
GPIO18 is the camera power enable on the AtomS3R-M12 and must be driven LOW (then wait ~500 ms) before esp_camera_init() — otherwise the sensor never answers on I²C and init fails with 0x105.
Don't stream with httpd_resp_send_chunk() if an embedded client reads the raw socket: it wraps writes in HTTP chunked transfer encoding, whose framing bytes corrupt the JPEGs. Browsers decode it transparently (so a phone test passes!) while drawJpg() fails on every frame. This repo sends the stream raw via httpd_send() instead.
The OV3660 is mounted upside down; the sketch sets vflip. Tweak set_vflip / set_hmirror in init_camera() if your orientation differs.
If FRAMESIZE_128X128 doesn't compile, your esp32 core is too old — update it, or fall back to FRAMESIZE_QQVGA (160×120, edges clipped).
Both radios run with modem sleep disabled for latency, so the boards run warm. That's normal.
License


MIT — do what you like with it.
