/********************************************************************
 *  ESP32-CAM Bear Detector
 *
 *  PURPOSE
 *  -------
 *  This project detects motion in a garden or outdoor area,
 *  captures a photo using an ESP32-CAM, and uploads the image
 *  to an Android phone running the Bear Detector application.
 *
 *  HARDWARE
 *  --------
 *  - ESP32-CAM (AI Thinker)
 *  - PIR motion sensor (GPIO14)
 *  - LDR daylight sensor (GPIO13)
 *  - LED illuminator controlled by MOSFET (GPIO15)
 *  - Android phone acting as Wi-Fi hotspot and image receiver
 *
 *  HOW IT WORKS
 *  ------------
 *  1. The ESP32-CAM normally stays in deep sleep to save power.
 *
 *  2. It wakes up when:
 *     - The PIR sensor detects motion, or
 *     - A periodic timer wake-up occurs.
 *
 *  3. After waking:
 *     - The PIR signal is checked.
 *     - The camera is initialized.
 *     - The LDR determines whether it is day or night.
 *
 *  4. If it is night:
 *     - GPIO15 activates a MOSFET.
 *     - The MOSFET powers IR LEDs.
 *     - The system waits briefly for illumination to stabilize.
 *
 *  5. The ESP32-CAM captures a JPEG image.
 *
 *  6. The ESP32-CAM connects to the Android phone's hotspot.
 *
 *  7. The captured JPEG image is uploaded via HTTP POST to:
 *
 *        http://<ANDROID_IP>:8080/upload
 *
 *  8. After upload:
 *     - Wi-Fi is disconnected.
 *     - The camera is released.
 *     - The ESP32-CAM returns to deep sleep.
 *
 *  POWER SAVING
 *  ------------
 *  Deep sleep is used between events to maximize battery life.
 *  The camera, Wi-Fi, and IR illumination are enabled only when
 *  needed for image capture.
 *
 *  WAKE SOURCES
 *  ------------
 *  - PIR sensor (EXT0 wake-up)
 *  - 60-second timer wake-up
 *
 ********************************************************************/

#include "esp_camera.h"
#include "esp_sleep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ============================================================
//  ★ CONFIGURE THESE THREE LINES FOR YOUR SETUP ★
// ============================================================

// Your Android phone's hotspot name and password
#define HOTSPOT_SSID   "hot"
#define HOTSPOT_PASS   "12345678"

// Android phone's IP on its own hotspot.
// Almost always 192.168.43.1 on Android.
// Open the Bear Detector app → press Start → the app shows the exact IP.
#define ANDROID_IP     "192.168.84.108"

// ============================================================
//  FIXED CONSTANTS  (no need to change)
// ============================================================
#define ANDROID_PORT        8080
#define UPLOAD_PATH         "/upload"
#define UPLOAD_URL          "http://" ANDROID_IP ":" STR(ANDROID_PORT) UPLOAD_PATH

// GPIO
#define PIN_PIR             13
#define PIN_LDR             14
#define PIN_LED_MOSFET      15

// Timing
#define LED_WARMUP_MS       500
#define LED_COOLDOWN_MS     500
#define PIR_HOLD_TIMEOUT_MS 10000
#define PIR_STABILISE_MS    60000
#define IDLE_SLEEP_US       (60LL * 1000000LL)

// Network
#define WIFI_TIMEOUT_MS     20000   // 20 s to associate with hotspot
#define HTTP_TIMEOUT_MS     20000   // 20 s for the upload POST

// Stringify helpers
#define STR2(x) #x
#define STR(x)  STR2(x)

// ============================================================
//  AI THINKER PIN MAP  (do not change)
// ============================================================
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void         initCamera();
bool         isDaylight();
void         waitForPIRLow();
camera_fb_t* captureFrame();
bool         connectHotspot();
void         disconnectWifi();
bool         uploadFrame(camera_fb_t* fb);
void         sleepNormal();    // EXT0 + 60 s timer
void         sleepExt0Only(); // EXT0 only (wait for PIR)
void         blinkFault();

// ============================================================
//  setup()  — runs once per wake
// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n=== ESP32-CAM Bear Detector ==="));

    // Safe GPIO defaults
    pinMode(PIN_LED_MOSFET, OUTPUT);
    digitalWrite(PIN_LED_MOSFET, LOW);
    pinMode(PIN_LDR, INPUT);
    pinMode(PIN_PIR, INPUT);

    // ── Determine why we woke ────────────────────────────────
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_TIMER)
    {
        // 60 s idle timer fired — check if PIR is actually HIGH
        if (digitalRead(PIN_PIR) == LOW)
        {
            Serial.println(F("[WAKE]  Timer wake, PIR LOW → no motion. EXT0-only sleep."));
            sleepExt0Only();
        }
        Serial.println(F("[WAKE]  Timer wake, PIR HIGH → treating as motion."));
    }
    else if (cause == ESP_SLEEP_WAKEUP_EXT0)
    {
        Serial.println(F("[WAKE]  PIR triggered (EXT0)."));
    }
    else
    {
        Serial.println(F("[WAKE]  First boot / manual reset."));
        // Give PIR time to settle after power-on before trusting its output
        if (millis() < (unsigned long)PIR_STABILISE_MS)
        {
            Serial.printf("[PIR]   Uptime %lu ms < stabilisation window. Sleeping.\n", millis());
            sleepNormal();
        }
    }

    // ── 1. Wait for PIR to drop LOW (motion peak passed) ────
    waitForPIRLow();

    // ── 2. Init camera ────────────────────────────────────────
    initCamera();

    // ── 3. LDR → LED → capture ───────────────────────────────
    bool night = !isDaylight();

    if (night)
    {
        digitalWrite(PIN_LED_MOSFET, HIGH);
        Serial.printf("[LED]   IR ON, warming %d ms\n", LED_WARMUP_MS);
        delay(LED_WARMUP_MS);
    }

    camera_fb_t* fb = captureFrame();

    if (night)
    {
        delay(LED_COOLDOWN_MS);
        digitalWrite(PIN_LED_MOSFET, LOW);
        Serial.println(F("[LED]   IR OFF"));
    }

    if (!fb)
    {
        Serial.println(F("[CAM]   Capture failed → sleeping."));
        sleepNormal();
    }

    Serial.printf("[CAM]   %zu bytes captured.\n", fb->len);

Serial.printf("[MEM] Heap=%u  PSRAM=%u\n",
              ESP.getFreeHeap(),
              ESP.getFreePsram());

    // ── 4. Connect to Android hotspot ────────────────────────
    if (!connectHotspot())
    {
        Serial.println(F("[WIFI]  Failed to connect → sleeping."));
        esp_camera_fb_return(fb);
        sleepNormal();
    }

    // ── 5. Upload JPEG to Android ─────────────────────────────
   bool ok = uploadFrame(fb);

esp_camera_fb_return(fb);
esp_camera_deinit();

Serial.printf("[UPLOAD] %s\n", ok ? "OK" : "FAILED");

    // ── 6. Disconnect and sleep ───────────────────────────────
    disconnectWifi();
    sleepNormal();
}

void loop() {}   // never reached

// ============================================================
//  connectHotspot()
//  Connects to the Android phone's mobile hotspot.
//  The phone acts as a Wi-Fi access point; the ESP32 is a client.
// ============================================================
bool connectHotspot()
{
    Serial.printf("[WIFI]  Connecting to hotspot \"%s\" ...\n", HOTSPOT_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - t0 > WIFI_TIMEOUT_MS)
        {
            Serial.println(F("\n[WIFI]  Timeout."));
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
        delay(300);
        Serial.print('.');
    }

    Serial.printf("\n[WIFI]  Connected. ESP32 IP: %s\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI]  Gateway (Android): %s\n",
                  WiFi.gatewayIP().toString().c_str());
    return true;
}

// ============================================================
//  disconnectWifi()
// ============================================================
void disconnectWifi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println(F("[WIFI]  Disconnected."));
}

// ============================================================
//  uploadFrame()
//  POST raw JPEG bytes to Android's /upload endpoint.
// ============================================================
bool uploadFrame(camera_fb_t* fb)
{
     Serial.printf(
        "[DEBUG] fb=%p buf=%p len=%u\n",
        fb,
        fb ? fb->buf : nullptr,
        fb ? fb->len : 0
    );

    HTTPClient http;
    http.begin(UPLOAD_URL);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type",   "image/jpeg");

    int code = http.POST(fb->buf, fb->len);

    if (code > 0)
    {
        Serial.printf("[HTTP]  Response %d: %s\n",
                      code, http.getString().c_str());
        http.end();
        return (code == 200);
    }
    else
    {
        Serial.printf("[HTTP]  Error: %s\n",
                      http.errorToString(code).c_str());
        http.end();
        return false;
    }
}

// ============================================================
//  captureFrame()
// ============================================================
camera_fb_t* captureFrame()
{
    Serial.println(F("[CAM]   Capturing..."));
    // Discard first frame — AEC/AWB need one frame to calibrate exposure
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) { esp_camera_fb_return(fb); fb = nullptr; }
    delay(200);
    fb = esp_camera_fb_get();
    if (!fb) Serial.println(F("[CAM]   *** Frame capture failed ***"));
    return fb;
}

// ============================================================
//  sleepNormal()  — EXT0 (PIR) + 60 s timer
// ============================================================
void sleepNormal()
{
    Serial.println(F("[SLEEP] EXT0 + 60 s timer. Sleeping..."));
    Serial.flush();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_PIR, 1);
    esp_sleep_enable_timer_wakeup(IDLE_SLEEP_US);
    esp_deep_sleep_start();
}

// ============================================================
//  sleepExt0Only()  — wait indefinitely for PIR
// ============================================================
void sleepExt0Only()
{
    Serial.println(F("[SLEEP] EXT0 only. Sleeping until PIR fires..."));
    Serial.flush();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_PIR, 1);
    esp_deep_sleep_start();
}

// ============================================================
//  waitForPIRLow()
// ============================================================
void waitForPIRLow()
{
    if (digitalRead(PIN_PIR) == LOW) return;
    Serial.println(F("[PIR]   Waiting for PIR LOW..."));
    unsigned long t0 = millis();
    while (digitalRead(PIN_PIR) == HIGH)
    {
        if (millis() - t0 > (unsigned long)PIR_HOLD_TIMEOUT_MS)
        {
            Serial.println(F("[PIR]   Timeout, proceeding anyway."));
            break;
        }
        delay(50);
    }
    Serial.printf("[PIR]   LOW after %lu ms.\n", millis() - t0);
}

// ============================================================
//  isDaylight()
// ============================================================
bool isDaylight()
{
    int v = digitalRead(PIN_LDR);
    Serial.printf("[LDR]   %s\n", v == HIGH ? "DAY" : "NIGHT");
    return (v == HIGH);
}

// ============================================================
//  blinkFault()  — blink IR LED then sleep
// ============================================================
void blinkFault()
{
    for (int i = 0; i < 8; i++)
    {
        digitalWrite(PIN_LED_MOSFET, HIGH); delay(150);
        digitalWrite(PIN_LED_MOSFET, LOW);  delay(150);
    }
    sleepNormal();
}

// ============================================================
//  initCamera()
// ============================================================
void initCamera()
{
    Serial.println(F("[CAM]   Initialising..."));

    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = CAM_PIN_D0;
    cfg.pin_d1       = CAM_PIN_D1;
    cfg.pin_d2       = CAM_PIN_D2;
    cfg.pin_d3       = CAM_PIN_D3;
    cfg.pin_d4       = CAM_PIN_D4;
    cfg.pin_d5       = CAM_PIN_D5;
    cfg.pin_d6       = CAM_PIN_D6;
    cfg.pin_d7       = CAM_PIN_D7;
    cfg.pin_xclk     = CAM_PIN_XCLK;
    cfg.pin_pclk     = CAM_PIN_PCLK;
    cfg.pin_vsync    = CAM_PIN_VSYNC;
    cfg.pin_href     = CAM_PIN_HREF;
    cfg.pin_sscb_sda = CAM_PIN_SIOD;
    cfg.pin_sscb_scl = CAM_PIN_SIOC;
    cfg.pin_pwdn     = CAM_PIN_PWDN;
    cfg.pin_reset    = CAM_PIN_RESET;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;

if (psramFound())
{
    cfg.frame_size   = FRAMESIZE_SXGA;
    cfg.jpeg_quality = 6;   // 0=best 63=worst; 6 is visibly sharper
    cfg.fb_count     = 2;   // 2 buffers lets the sensor settle
    Serial.println(F("[CAM]   PSRAM found → SXGA q6"));
}
else
{
    cfg.frame_size   = FRAMESIZE_SVGA;
    cfg.jpeg_quality = 6;
    cfg.fb_count     = 1;
    Serial.println(F("[CAM]   No PSRAM → SVGA q6"));
}

    if (esp_camera_init(&cfg) != ESP_OK)
    {
        Serial.println(F("[CAM]   *** FATAL init failed ***"));
        blinkFault();
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s)
    {
      s->set_brightness(s, 1);   // slight lift helps detail in shadows
    s->set_contrast(s, 1);     // more edge definition
    s->set_saturation(s, 0);
    s->set_sharpness(s, 2);    // range is 0–2 on OV2640
    s->set_denoise(s, 1);
        s->set_gainceiling(s, (gainceiling_t)2);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 0);
        s->set_ae_level(s, 0);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        s->set_colorbar(s, 0);
    }

    Serial.println(F("[CAM]   Ready."));
}
