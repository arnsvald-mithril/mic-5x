/*
 * MIC-5X — 5-parameter indoor microclimate monitor
 * Firmware v1.1 (matches Datasheet Rev. C.1, §7 Firmware Summary)
 *
 * MCU:     ESP-12E / ESP-12F (ESP8266), bare module on custom 2-layer PCB
 * Sensors: DHT22 (T/RH), BMP280 (P/T, I2C), BH1750 (lux, I2C),
 *          custom nephelometric dust channel (TSAL6200 + BPW34 + LM358 TIA -> TOUT/A0)
 * Display: SSD1306 128x64 OLED (I2C)
 * Output:  HTTP server on port 80 ("/" HTML page, "/json" API)
 *
 * Design rules implemented (see datasheet):
 *  - Non-blocking loop: millis() scheduling, no delay() in steady state
 *  - DHT22 retried up to 3x, out-of-range readings rejected, last valid kept
 *  - Dust: burst of 16 ADC samples averaged (~x4 white-noise reduction);
 *    burst is taken OUTSIDE web-client handling to avoid the known ESP8266
 *    Wi-Fi-TX-vs-TOUT corruption
 *  - Temperature cross-check DHT22 vs BMP280 (warn if |dT| > 2 degC)
 *  - Pressure trend over ~30 min, threshold 0.3 hPa (rising/falling/stable)
 *  - Dust temperature compensation via BMP280 (linear tempco, §5.2)
 *
 * Libraries (Arduino IDE -> Library Manager):
 *  - Adafruit BMP280 Library (+ Adafruit Unified Sensor, Adafruit BusIO)
 *  - BH1750 (Christopher Laws)
 *  - DHT sensor library (Adafruit)
 *  - Adafruit SSD1306 + Adafruit GFX
 *
 * Board settings: "Generic ESP8266 Module", Flash 4MB, CPU 80 MHz.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <BH1750.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* ============================ CONFIG ================================== */
/* !! Pin map: verify against the KiCad netlist of your PCB revision.
 * The values below are the conventional ESP-12E assignment used during
 * bring-up; the datasheet (Rev. C.1) does not pin these down.            */
#define PIN_I2C_SDA     4      // GPIO4  — SDA (BMP280, BH1750, SSD1306)
#define PIN_I2C_SCL     5      // GPIO5  — SCL
#define PIN_DHT         14     // GPIO14 — DHT22 single-wire data
#define PIN_DUST_ADC    A0     // TOUT via R8/R14 divider (~1 V full scale)

const char* WIFI_SSID   = "YOUR_SSID";
const char* WIFI_PASS   = "YOUR_PASSWORD";
const char* AP_SSID     = "MIC-5X";        // fallback AP if STA join fails
const char* AP_PASS     = "mic5xsetup";

const uint32_t SENSOR_PERIOD_MS   = 30000; // poll cycle (datasheet: 30 s)
const uint32_t WIFI_JOIN_TIMEOUT  = 20000;

/* Dust channel calibration (§5.1–5.2 of the datasheet).
 * V_clean: TIA output with clean air (dark/offset + stray light), mV at TOUT.
 * K_MV_PER_UGM3: slope from two-point calibration vs reference instrument.
 * DUST_TEMPCO: fractional signal drift per degC, compensated via BMP280 T.
 * Defaults are placeholders — MUST be calibrated per unit (§8).           */
float V_CLEAN_MV      = 90.0f;
float K_MV_PER_UGM3   = 0.45f;
float DUST_TEMPCO     = 0.004f;   // 0.4 %/degC, referenced to +25 degC

const float ADC_FULL_SCALE_MV = 1000.0f;  // ESP8266 TOUT ~0..1 V
const int   DUST_BURST_N      = 16;

/* ============================ GLOBALS ================================= */
ESP8266WebServer server(80);
Adafruit_BMP280  bmp;                       // I2C 0x76/0x77
BH1750           lux;                       // I2C 0x23
DHT              dht(PIN_DHT, DHT22);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);  // I2C 0x3C

struct Readings {
  float tDht    = NAN;   // degC
  float rh      = NAN;   // %
  float tBmp    = NAN;   // degC
  float pHpa    = NAN;   // hPa
  float luxVal  = NAN;   // lx
  float dustMv  = NAN;   // raw averaged TOUT, mV
  float dustUg  = NAN;   // compensated estimate, ug/m3 (relative, §8)
  bool  tMismatch = false;
  char  pTrend  = 'S';   // 'R' rising / 'F' falling / 'S' stable
} rd;

uint32_t lastPoll = 0;
bool bmpOk = false, luxOk = false, oledOk = false;

/* Pressure trend: ring buffer of 1 sample/minute, compare now vs 30 min ago */
const uint8_t P_HIST_N = 30;
float    pHist[P_HIST_N];
uint8_t  pHistIdx = 0, pHistFill = 0;
uint32_t lastPHist = 0;
const float P_TREND_THRESHOLD = 0.3f;      // hPa (datasheet §7)

/* ============================ SENSORS ================================= */
bool readDHT() {
  for (uint8_t i = 0; i < 3; i++) {        // retry up to 3x (datasheet)
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    // range validation; reject NaN and out-of-spec values
    if (!isnan(t) && !isnan(h) && t > -40 && t < 80 && h >= 0 && h <= 100) {
      rd.tDht = t; rd.rh = h;
      return true;                          // last valid retained otherwise
    }
    delay(50);                              // brief retry gap only on failure
  }
  return false;
}

void readBMP() {
  if (!bmpOk) return;
  rd.tBmp = bmp.readTemperature();
  rd.pHpa = bmp.readPressure() / 100.0f;
}

void readLux() {
  if (!luxOk) return;
  float v = lux.readLightLevel();
  if (v >= 0) rd.luxVal = v;
}

/* Dust: 16-sample burst on TOUT, averaged. Called from the sensor tick,
 * never from an HTTP handler, so it does not overlap Wi-Fi TX (§7 caveat). */
void readDust() {
  uint32_t acc = 0;
  for (int i = 0; i < DUST_BURST_N; i++) {
    acc += analogRead(PIN_DUST_ADC);        // 10-bit, 0..1023
    delayMicroseconds(300);                 // few-ms total burst
  }
  float raw = (float)acc / DUST_BURST_N;
  rd.dustMv = raw * ADC_FULL_SCALE_MV / 1023.0f;

  // Temperature compensation via BMP280 (§5.2), referenced to +25 degC
  float t = bmpOk && !isnan(rd.tBmp) ? rd.tBmp : 25.0f;
  float vComp = rd.dustMv / (1.0f + DUST_TEMPCO * (t - 25.0f));

  float ug = (vComp - V_CLEAN_MV) / K_MV_PER_UGM3;
  rd.dustUg = ug > 0 ? ug : 0;
}

void updatePressureTrend() {
  if (isnan(rd.pHpa)) return;
  if (millis() - lastPHist >= 60000UL || pHistFill == 0) {
    lastPHist = millis();
    pHist[pHistIdx] = rd.pHpa;
    pHistIdx = (pHistIdx + 1) % P_HIST_N;
    if (pHistFill < P_HIST_N) pHistFill++;
  }
  if (pHistFill < P_HIST_N) { rd.pTrend = 'S'; return; }
  float oldest = pHist[pHistIdx];           // ~30 min ago
  float d = rd.pHpa - oldest;
  rd.pTrend = (d >  P_TREND_THRESHOLD) ? 'R'
            : (d < -P_TREND_THRESHOLD) ? 'F' : 'S';
}

void pollSensors() {
  readDHT();
  readBMP();
  readLux();
  readDust();
  updatePressureTrend();
  // Redundancy cross-check (datasheet §7)
  rd.tMismatch = (!isnan(rd.tDht) && !isnan(rd.tBmp) &&
                  fabsf(rd.tDht - rd.tBmp) > 2.0f);
  updateOled();
}

/* ============================ OLED ==================================== */
void updateOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("MIC-5X  %s\n", WiFi.isConnected()
              ? WiFi.localIP().toString().c_str() : "AP mode");
  oled.printf("T: %.1fC  RH: %.0f%%\n", rd.tDht, rd.rh);
  oled.printf("P: %.1f hPa %c\n", rd.pHpa,
              rd.pTrend == 'R' ? '^' : rd.pTrend == 'F' ? 'v' : '-');
  oled.printf("Lux: %.0f\n", rd.luxVal);
  oled.printf("Dust: %.0f ug/m3\n", rd.dustUg);
  if (rd.tMismatch) oled.print("! T sensors differ");
  oled.display();
}

/* ============================ WEB ===================================== */
const char* trendStr() {
  return rd.pTrend == 'R' ? "rising" : rd.pTrend == 'F' ? "falling" : "stable";
}

void handleRoot() {
  char buf[1200];
  snprintf(buf, sizeof(buf),
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='30'>"          // auto-refresh 30 s
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MIC-5X</title><style>"
    "body{font-family:system-ui,sans-serif;background:#fff;color:#123;"
    "max-width:560px;margin:24px auto;padding:0 16px}"
    "h1{font-size:20px;border-bottom:2px solid #1b3a5c}"
    "table{width:100%%;border-collapse:collapse}"
    "td{padding:8px 6px;border-bottom:1px solid #dde}"
    "td:last-child{text-align:right;font-weight:600}"
    ".w{color:#b05a52}</style></head><body>"
    "<h1>MIC-5X &mdash; Indoor Microclimate</h1><table>"
    "<tr><td>Temperature (DHT22)</td><td>%.1f &deg;C</td></tr>"
    "<tr><td>Humidity</td><td>%.0f %%</td></tr>"
    "<tr><td>Temperature (BMP280)</td><td>%.1f &deg;C</td></tr>"
    "<tr><td>Pressure</td><td>%.1f hPa (%s)</td></tr>"
    "<tr><td>Illuminance</td><td>%.0f lx</td></tr>"
    "<tr><td>Dust (relative)</td><td>%.0f &micro;g/m&sup3;</td></tr>"
    "<tr><td>Dust raw</td><td>%.0f mV</td></tr></table>"
    "%s<p style='color:#789;font-size:12px'>Auto-refresh 30 s &middot; "
    "<a href='/json'>/json</a></p></body></html>",
    rd.tDht, rd.rh, rd.tBmp, rd.pHpa, trendStr(), rd.luxVal,
    rd.dustUg, rd.dustMv,
    rd.tMismatch ? "<p class='w'>Warning: DHT22/BMP280 differ &gt;2&deg;C</p>" : "");
  server.send(200, "text/html", buf);
}

void handleJson() {
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"t_dht\":%.1f,\"rh\":%.0f,\"t_bmp\":%.1f,\"p_hpa\":%.1f,"
    "\"p_trend\":\"%s\",\"lux\":%.0f,\"dust_ugm3\":%.0f,"
    "\"dust_mv\":%.0f,\"t_mismatch\":%s,\"uptime_s\":%lu}",
    rd.tDht, rd.rh, rd.tBmp, rd.pHpa, trendStr(), rd.luxVal,
    rd.dustUg, rd.dustMv, rd.tMismatch ? "true" : "false",
    (unsigned long)(millis() / 1000));
  server.send(200, "application/json", buf);
}

/* ============================ SETUP/LOOP ============================== */
void setup() {
  Serial.begin(115200);
  Serial.println(F("\nMIC-5X fw v1.1"));

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  dht.begin();
  bmpOk  = bmp.begin(0x76) || bmp.begin(0x77);
  luxOk  = lux.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.printf("BMP280:%d BH1750:%d OLED:%d\n", bmpOk, luxOk, oledOk);

  if (bmpOk)
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,   // temperature
                    Adafruit_BMP280::SAMPLING_X16,  // pressure
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_TIMEOUT)
    delay(250);
  if (WiFi.status() != WL_CONNECTED) {          // fallback AP for demo/setup
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("AP mode: http://%s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("STA: http://%s\n", WiFi.localIP().toString().c_str());
  }

  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.begin();

  pollSensors();                                // first reading immediately
  lastPoll = millis();
}

void loop() {
  server.handleClient();                        // stays responsive (no delay())
  if (millis() - lastPoll >= SENSOR_PERIOD_MS) {
    lastPoll = millis();
    pollSensors();                              // ADC burst outside HTTP handling
  }
}
