// =============================================================================
//  MATA TRANSFORMER LOAD MONITOR · v3.0 (Supabase edition)
//  ESP32-S3 N16R8  |  PZEM-004T v3  |  DS18B20  |  SIM900A  |  Supabase REST
//
//  Changes from v2.7:
//   - Removed Firebase_ESP_Client (heavy: auth handshake + 3x redundant writes
//     per sample). Replaced with lightweight HTTPS POST straight to Supabase's
//     PostgREST endpoint via HTTPClient + ArduinoJson — one insert per table.
//   - Voltage/current/frequency are measured by the PZEM-004T's onboard
//     metering IC (pzem.voltage()/current()/frequency()) — there is no
//     Arduino "library" that recomputes these more efficiently than the
//     sensor's own hardware; nothing changed there. What WAS wasteful was
//     the smoothing/filtering code, cleaned up below.
//   - Moving-average filter replaced with a small reusable templated struct
//     instead of two duplicated global arrays + a free function.
//   - Sensor readings now flow through one PZEMReading struct instead of
//     five loose float parameters passed function to function.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SPI.h>
#include <SD.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include "driver/gpio.h"
#include <TinyGPS.h>

// === DEVICE IDENTITY ===
const char *DEVICE_ID = "MATA_001"; // Change to update all Supabase rows automatically

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================
#define RXD2 17
#define TXD2 16
#define SD_CS_PIN 10
#define SD_MOSI_PIN 11
#define SD_MISO_PIN 13
#define SD_SCK_PIN 12
#define DS18B20_PIN 5
#define SIM900_RX_PIN 38
#define SIM900_TX_PIN 39
#define GPS_RX_PIN 20
#define GPS_TX_PIN 21

// =============================================================================
//  OBJECT INSTANCES
// =============================================================================
HardwareSerial pzemSerial(1);
PZEM004Tv30 pzem(pzemSerial, RXD2, TXD2);
SPIClass sdSPI(HSPI);
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
HardwareSerial sim900Serial(2);
HardwareSerial gpsSerial(0);
TinyGPS gps;

// =============================================================================
//  CONFIGURATION
// =============================================================================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

// --- Supabase ---
// Project Settings -> API. Use the "anon" public key here, never the
// service_role key on a device that can be physically accessed.
#ifndef SUPABASE_URL
#define SUPABASE_URL ""
#endif
#ifndef SUPABASE_KEY
#define SUPABASE_KEY ""
#endif

#define TRANSFORMER_KVA 1.0f
#define V_NOMINAL 230.0f
#define FREQ_NOMINAL 60.0f
#define I_FL 4.35f

#define UF_UNDER_PREALERT_MIN 30.0f
#define UF_UNDER_MAX 40.0f
#define UF_OVER_PREALERT_MIN 70.0f
#define UF_OVERLOAD_MIN 80.0f

#define V_UNDERVOLT_WARN 207.0f
#define V_OVERVOLT_WARN 253.0f
#define FREQ_LOW_WARN 59.40f
#define FREQ_HIGH_WARN 60.60f
#define PF_LOW_THRESHOLD 0.85f

// Calibration constants (fitted against a reference meter)
#define CAL_I_SLOPE 0.991522f
#define CAL_I_INTERCEPT -0.018717f
#define CAL_V_SLOPE 0.832805f
#define CAL_V_INTERCEPT 38.562098f

#define DS18B20_RESOLUTION 12
#define TEMP_AMBIENT_C 30.0f
#define TEMP_WARN_ABSOLUTE_C 75.0f
#define TEMP_CRIT_ABSOLUTE_C 85.0f
#define TEMP_RISE_WARN_C 40.0f
#define TEMP_RISE_CRIT_C 55.0f

#define SC_CURRENT_THRESHOLD 15.0f
#define SC_VOLTAGE_COLLAPSE 110.0f
#define SC_CONFIRM_MS 100UL

#define PO_VOLTAGE_THRESHOLD 30.0f
#define PO_CURRENT_MAX 5.0f
#define PO_CONFIRM_MS 3000UL

#define SMS_RECIPIENT "+639942610527"
#define OVERLOAD_SMS_COOLDOWN 300000UL
#define PREALERT_SMS_COOLDOWN 180000UL
#define TEMP_SMS_COOLDOWN 300000UL
#define SC_SMS_COOLDOWN 120000UL
#define PO_SMS_COOLDOWN 300000UL

#define NTP_SERVER "pool.ntp.org"
#define UTC_OFFSET_SEC 28800

// ── Intervals (ms) ──
const long RETRY_INTERVAL = 60000;
const long SEND_INTERVAL = 10000;    // 10 sec Supabase logging
const long DISPLAY_INTERVAL = 30000; // 30 sec serial output
const long SMS_POLL_INTERVAL = 5000;
const long SD_WRITE_INTERVAL = 300000; // 5 min SD card logging
const long TEMP_READ_INTERVAL = 1000;
#define GPS_BAUD 9600
#define GPS_SEND_INTERVAL 15000UL

#define FILTER_WINDOW 3 // moving-average filter length

// =============================================================================
//  SMALL REUSABLE TYPES
// =============================================================================

// Fixed-window moving average. Replaces the old vBuffer[]/iBuffer[]/bufIdx/
// bufFull globals + free updateAverage() function with one reusable type —
// O(1) per sample, no heap allocation, no duplicated logic per channel.
template <int N>
struct MovingAverage
{
  float buf[N] = {0};
  uint8_t idx = 0;
  bool full = false;

  float update(float newVal)
  {
    buf[idx] = newVal;
    idx = (idx + 1) % N;
    if (idx == 0)
      full = true;
    if (!full)
      return newVal;
    float sum = 0;
    for (int i = 0; i < N; i++)
      sum += buf[i];
    return sum / N;
  }
};

// One struct instead of (voltage, current, power, freq, tempC) passed
// individually through half a dozen functions.
struct Reading
{
  float voltage = 0;
  float current = 0;
  float power = 0;
  float freq = 0;
  float tempC = NAN;
  float s_va = 0;
  float s_kva = 0;
  float q_var = 0;
  float pf = 0;
  float uf = 0;
  float tempRise = NAN;
  const char *faultType = "NORMAL";
};

// =============================================================================
//  STATE VARIABLES
// =============================================================================
bool sdReady = false;
bool thermoReady = false;
bool sim900Ready = false;
bool wifiReady = false;
bool smsAlertsEnabled = true;

bool ds18b20ConversionPending = false;
unsigned long ds18b20ConvStart = 0;

String currentLogFile = "";
float lastTempC = NAN;
float ambientTempC = TEMP_AMBIENT_C;

bool pzemError = false;
bool ds18b20Error = false;

bool wasOverloaded = false;
bool wasPreAlert = false;
bool wasTempAlert = false;
bool wasShortCircuit = false;
bool wasPowerOutage = false;

unsigned long lastOverloadSMS = 0;
unsigned long lastPreAlertSMS = 0;
unsigned long lastTempAlertSMS = 0;
unsigned long lastSCSMS = 0;
unsigned long lastPOSMS = 0;

unsigned long scDetectStart = 0;
unsigned long poDetectStart = 0;
bool scPending = false;
bool poPending = false;

unsigned long lastSend = 0;
unsigned long lastDisplay = 0;
unsigned long lastSMSPoll = 0;
unsigned long lastSDWrite = 0;
unsigned long lastTempRead = 0;
unsigned long lastSDRetry = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastThermoRetry = 0;

// GPS variables
float gpsLat = 0.0f;
float gpsLng = 0.0f;
bool gpsValid = false;
bool statusOnSent = false;
unsigned long lastGpsSend = 0;
bool gpsHwError = false;
unsigned long lastGpsDataMs = 0;
unsigned long lastGpsAlertSent = 0;

MovingAverage<FILTER_WINDOW> vFilter;
MovingAverage<FILTER_WINDOW> iFilter;

// =============================================================================
//  HELPER FUNCTIONS
// =============================================================================
String getISOTimestamp()
{
  struct tm t;
  if (!getLocalTime(&t))
    return "1970-01-01T00:00:00";
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

String getTimestamp()
{
  struct tm t;
  if (!getLocalTime(&t))
    return "1970-01-01 00:00:00";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String getTodayFilename()
{
  struct tm t;
  if (!getLocalTime(&t))
    return "/MATA_unknown.csv";
  char buf[32];
  strftime(buf, sizeof(buf), "/MATA_%Y-%m-%d.csv", &t);
  return String(buf);
}

// Electrical calculations (unchanged — these are plain physics identities,
// not something a library replaces; V/I/F themselves come from the PZEM
// sensor's own metering IC via pzem.voltage()/current()/frequency()).
float calcApparentPower_VA(float v, float i) { return v * i; }
float calcApparentPower_kVA(float v, float i) { return (v * i) / 1000.0f; }
float calcReactivePower_VAR(float s_va, float p_w)
{
  float q2 = s_va * s_va - p_w * p_w;
  return (q2 > 0.0f) ? sqrtf(q2) : 0.0f;
}
float calcPowerFactor(float p_w, float s_va)
{
  if (s_va < 0.5f)
    return 1.0f;
  return constrain(p_w / s_va, 0.0f, 1.0f);
}
float calcUtilizationFactor(float s_kva)
{
  return constrain((s_kva / TRANSFORMER_KVA) * 100.0f, 0.0f, 999.0f);
}
const char *getLoadStatus(float uf)
{
  if (uf < UF_UNDER_PREALERT_MIN)
    return "SEVERE UNDERLOAD";
  if (uf < UF_UNDER_MAX)
    return "UNDERLOAD (PRE-ALERT)";
  if (uf <= UF_OVER_PREALERT_MIN)
    return "NORMAL LOAD";
  if (uf < UF_OVERLOAD_MIN)
    return "OVERLOAD (PRE-ALERT)";
  return "OVERLOADED";
}
bool isPreAlertZone(float uf)
{
  return (uf >= UF_UNDER_PREALERT_MIN && uf < UF_UNDER_MAX) ||
         (uf > UF_OVER_PREALERT_MIN && uf < UF_OVERLOAD_MIN);
}
bool isOverloadZone(float uf) { return uf >= UF_OVERLOAD_MIN; }
const char *getVoltageStatus(float v)
{
  if (v < PO_VOLTAGE_THRESHOLD)
    return "NO SUPPLY";
  if (v < V_UNDERVOLT_WARN)
    return "UNDER-VOLTAGE";
  if (v > V_OVERVOLT_WARN)
    return "OVER-VOLTAGE";
  return "NORMAL";
}
const char *getFreqStatus(float f)
{
  return (f < FREQ_LOW_WARN || f > FREQ_HIGH_WARN) ? "OUT OF BAND" : "NORMAL";
}
const char *getPFStatus(float pf)
{
  return (pf < PF_LOW_THRESHOLD) ? "LOW PF" : "ACCEPTABLE";
}
float getTempRise(float tempC)
{
  return isnan(tempC) ? NAN : tempC - ambientTempC;
}
const char *getTempStatus(float tempC)
{
  if (isnan(tempC))
    return "NO SENSOR";
  float rise = getTempRise(tempC);
  if (tempC >= TEMP_CRIT_ABSOLUTE_C || rise >= TEMP_RISE_CRIT_C)
    return "CRITICAL";
  if (tempC >= TEMP_WARN_ABSOLUTE_C || rise >= TEMP_RISE_WARN_C)
    return "WARNING";
  return "NORMAL";
}

// Fills in every derived field of a Reading from the four raw PZEM values.
void computeDerived(Reading &r)
{
  r.s_va = calcApparentPower_VA(r.voltage, r.current);
  r.s_kva = r.s_va / 1000.0f;
  r.q_var = calcReactivePower_VAR(r.s_va, r.power);
  r.pf = calcPowerFactor(r.power, r.s_va);
  r.uf = calcUtilizationFactor(r.s_kva);
  r.tempRise = getTempRise(r.tempC);
}

// =============================================================================
//  DS18B20 INIT
// =============================================================================
bool initDS18B20()
{
  gpio_reset_pin((gpio_num_t)DS18B20_PIN);
  pinMode(DS18B20_PIN, INPUT);
  delay(100);
  oneWire.reset_search();
  ds18b20.begin();
  ds18b20.setResolution(DS18B20_RESOLUTION);
  ds18b20.setWaitForConversion(true);
  ds18b20.requestTemperatures();
  float probe = ds18b20.getTempCByIndex(0);
  ds18b20.setWaitForConversion(false);
  if (probe == DEVICE_DISCONNECTED_C || isnan(probe))
    return false;
  lastTempC = probe;
  return true;
}

// =============================================================================
//  SD CARD
// =============================================================================
bool sdInit()
{
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, sdSPI, 4000000))
    return false;
  if (SD.cardType() == CARD_NONE)
    return false;
  return true;
}
void sdEnsureHeader(const String &fn)
{
  if (!SD.exists(fn))
  {
    File f = SD.open(fn, FILE_WRITE);
    if (f)
    {
      f.print("\xEF\xBB\xBF");
      f.println("Timestamp,Voltage (V),Volt Status,Current (A),"
                "Active Power P (W),Apparent Power S (VA),S (kVA),"
                "Reactive Power Q (VAR),Power Factor,PF Status,"
                "Frequency (Hz),Freq Status,Utilization Factor (%),Load Status,"
                "Temp (C),Temp Rise (K),Temp Status,Fault Type");
      f.close();
    }
  }
}
void sdAppendReading(const Reading &r)
{
  if (!sdReady)
    return;
  String fn = getTodayFilename();
  if (fn != currentLogFile)
  {
    sdEnsureHeader(fn);
    currentLogFile = fn;
  }
  File f = SD.open(fn, FILE_APPEND);
  if (!f)
  {
    sdReady = false;
    return;
  }
  f.print(getTimestamp());
  f.print(",");
  f.print(r.voltage, 2);
  f.print(",");
  f.print(getVoltageStatus(r.voltage));
  f.print(",");
  f.print(r.current, 3);
  f.print(",");
  f.print(r.power, 2);
  f.print(",");
  f.print(r.s_va, 2);
  f.print(",");
  f.print(r.s_kva, 4);
  f.print(",");
  f.print(r.q_var, 2);
  f.print(",");
  f.print(r.pf, 4);
  f.print(",");
  f.print(getPFStatus(r.pf));
  f.print(",");
  f.print(r.freq, 2);
  f.print(",");
  f.print(getFreqStatus(r.freq));
  f.print(",");
  f.print(r.uf, 2);
  f.print(",");
  f.print(getLoadStatus(r.uf));
  f.print(",");
  if (isnan(r.tempC))
  {
    f.print("N/A,N/A,N/A,");
  }
  else
  {
    f.print(r.tempC, 2);
    f.print(",");
    f.print(r.tempRise, 2);
    f.print(",");
    f.print(getTempStatus(r.tempC));
    f.print(",");
  }
  f.println(r.faultType);
  f.close();
}

// =============================================================================
//  SIM900A
// =============================================================================
bool sim900SendCmd(const char *cmd, const char *expected, unsigned long ms = 2000)
{
  sim900Serial.println(cmd);
  unsigned long start = millis();
  String r = "";
  while (millis() - start < ms)
  {
    while (sim900Serial.available())
      r += (char)sim900Serial.read();
    if (r.indexOf(expected) != -1)
      return true;
    delay(10);
  }
  return false;
}
void sim900FlushRx()
{
  while (sim900Serial.available())
    sim900Serial.read();
}
bool sim900Init()
{
  sim900FlushRx();
  for (int i = 0; i < 3; i++)
  {
    if (sim900SendCmd("AT", "OK", 1500))
      goto ready;
    delay(500);
  }
  return false;
ready:
  sim900SendCmd("AT+CMGF=1", "OK", 1000);
  sim900SendCmd("AT+CSCS=\"GSM\"", "OK", 1000);
  sim900SendCmd("AT+CNMI=0,0,0,0,0", "OK", 1000);
  return true;
}
bool sim900SendSMS(const char *number, const String &message)
{
  if (!sim900Ready)
    return false;
  sim900FlushRx();
  String cmd = String("AT+CMGS=\"") + number + "\"";
  sim900Serial.println(cmd);
  unsigned long s = millis();
  bool gotPrompt = false;
  while (millis() - s < 4000)
  {
    if (sim900Serial.find(">"))
    {
      gotPrompt = true;
      break;
    }
    delay(10);
  }
  if (!gotPrompt)
    return false;
  sim900Serial.print(message);
  delay(80);
  sim900Serial.write(0x1A);
  s = millis();
  while (millis() - s < 15000)
  {
    while (sim900Serial.available())
    {
      String resp = sim900Serial.readString();
      if (resp.indexOf("+CMGS") != -1)
        return true;
      if (resp.indexOf("ERROR") != -1)
        return false;
    }
    delay(20);
  }
  return false;
}
String buildReadingsSMS(const Reading &r,
                        bool isOverload, bool isPreAlert, bool isTempAlert,
                        bool isShortCircuit, bool isPowerOutage)
{
  bool anyAlert = isOverload || isPreAlert || isTempAlert || isShortCircuit || isPowerOutage;
  String msg = anyAlert ? "==[MATA TLM ALERT]==\n" : "==[MATA TLM STATUS]==\n";
  msg += getTimestamp() + "\n";
  msg += "--- ELECTRICAL ---\n";
  msg += "V:  " + String(r.voltage, 1) + " V (" + getVoltageStatus(r.voltage) + ")\n";
  msg += "I:  " + String(r.current, 2) + " A\n";
  msg += "P:  " + String(r.power, 1) + " W\n";
  msg += "S:  " + String(r.s_kva, 3) + " kVA\n";
  msg += "Q:  " + String(r.q_var, 1) + " VAR\n";
  msg += "PF: " + String(r.pf, 3) + " (" + getPFStatus(r.pf) + ")\n";
  msg += "Hz: " + String(r.freq, 1) + " (" + getFreqStatus(r.freq) + ")\n";
  msg += "UF: " + String(r.uf, 1) + "% (" + getLoadStatus(r.uf) + ")\n";
  msg += "--- THERMAL ---\n";
  if (!isnan(r.tempC))
  {
    msg += "T:    " + String(r.tempC, 1) + " C\n";
    msg += "Rise: " + String(r.tempRise, 1) + " K (" + getTempStatus(r.tempC) + ")\n";
  }
  else
    msg += "T: N/A (sensor error)\n";
  if (isOverload)
    msg += "--- ALERT ---\n!! OVERLOAD: UF=" + String(r.uf, 1) + "% (>=80%)\nReduce connected loads immediately.";
  if (isPreAlert)
    msg += "--- ALERT ---\n!! PRE-ALERT: " + String(r.uf, 1) + "% - approaching limit.";
  if (isTempAlert)
    msg += "--- ALERT ---\n!! HIGH TEMP: " + String(r.tempC, 1) + " C\nCheck ventilation.";
  if (isShortCircuit)
    msg += "--- FAULT ---\n!! SHORT CIRCUIT !!\nI=" + String(r.current, 1) + "A V=" + String(r.voltage, 1) + "V\nDe-energise & inspect.";
  if (isPowerOutage)
    msg += "--- FAULT ---\n!! POWER OUTAGE !!\nV=" + String(r.voltage, 1) + "V I=" + String(r.current, 2) + "A\nCheck upstream breakers.";
  return msg;
}
void handleIncomingSMS(const Reading &r)
{
  if (!sim900Ready)
    return;
  // Full command parser for ACTIVATE/DEACTIVATE/STATUS/HELP goes here in production.
}

// =============================================================================
//  WIFI
// =============================================================================
bool tryConnectWiFi()
{
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int r = 0;
  while (WiFi.status() != WL_CONNECTED && r++ < 20)
    delay(500);
  if (WiFi.status() == WL_CONNECTED)
  {
    configTime(UTC_OFFSET_SEC, 0, NTP_SERVER);
    struct tm t;
    for (int n = 0; n < 20; n++)
    {
      if (getLocalTime(&t))
        break;
      delay(100);
    }
    return true;
  }
  return false;
}

// =============================================================================
//  SUPABASE (PostgREST over HTTPS)
// =============================================================================
// One shared function for every table write. `prefer` controls PostgREST's
// response/merge behaviour, e.g. "return=minimal" for a plain insert, or
// "resolution=merge-duplicates,return=minimal" for an upsert.
bool supabaseRequest(const char *method, const String &path, const String &body,
                     const char *prefer = "return=minimal")
{
  if (!wifiReady || WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure client;
  client.setInsecure(); // NOTE: for production, pin Supabase's root CA instead.

  HTTPClient https;
  String url = String(SUPABASE_URL) + path;
  if (!https.begin(client, url))
    return false;

  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", prefer);

  int code = strcmp(method, "PATCH") == 0 ? https.PATCH(body) : https.POST(body);
  bool ok = (code >= 200 && code < 300);
  if (!ok)
    Serial.printf("[Supabase] %s %s -> HTTP %d\n", method, path.c_str(), code);
  https.end();
  return ok;
}

// Insert one row into `readings`. Replaces the old triple-write to
// /readings, /latest, /faults — the dashboard now just queries
// "order by ts desc limit 1" for the latest value instead of needing a
// separate always-overwritten node.
bool sendToSupabase(const Reading &r)
{
  StaticJsonDocument<640> doc;
  doc["device_id"] = DEVICE_ID;
  doc["ts"] = getISOTimestamp();
  doc["voltage_v"] = r.voltage;
  doc["voltage_status"] = getVoltageStatus(r.voltage);
  doc["current_a"] = r.current;
  doc["active_power_w"] = r.power;
  doc["apparent_power_va"] = r.s_va;
  doc["apparent_power_kva"] = r.s_kva;
  doc["reactive_power_var"] = r.q_var;
  doc["power_factor"] = r.pf;
  doc["pf_status"] = getPFStatus(r.pf);
  doc["frequency_hz"] = r.freq;
  doc["freq_status"] = getFreqStatus(r.freq);
  doc["utilization_pct"] = r.uf;
  doc["load_status"] = getLoadStatus(r.uf);
  doc["fault_type"] = r.faultType;
  doc["sms_alerts_enabled"] = smsAlertsEnabled;
  if (!isnan(r.tempC))
  {
    doc["temperature_c"] = r.tempC;
    doc["temp_rise_k"] = r.tempRise;
    doc["temp_status"] = getTempStatus(r.tempC);
  }
  String body;
  serializeJson(doc, body);
  return supabaseRequest("POST", "/rest/v1/readings", body, "return=minimal");
}

bool sendDeviceStatus(const char *statusMsg)
{
  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["status"] = statusMsg;
  doc["last_seen"] = getISOTimestamp();
  String body;
  serializeJson(doc, body);
  // Upsert: one row per device, overwritten on every call.
  return supabaseRequest("POST", "/rest/v1/device_status?on_conflict=device_id", body,
                         "resolution=merge-duplicates,return=minimal");
}

// =============================================================================
//  ALERT LOGIC (unchanged behaviour, now Supabase-backed)
// =============================================================================
void checkShortCircuitAndOutage(Reading &r)
{
  unsigned long now = millis();
  bool scCondition = (r.current >= SC_CURRENT_THRESHOLD) && (r.voltage <= SC_VOLTAGE_COLLAPSE);
  if (scCondition)
  {
    if (!scPending)
    {
      scPending = true;
      scDetectStart = now;
    }
    if (now - scDetectStart >= SC_CONFIRM_MS && (!wasShortCircuit || (now - lastSCSMS >= SC_SMS_COOLDOWN)))
    {
      r.faultType = "SHORT_CIRCUIT";
      if (sim900Ready && smsAlertsEnabled)
        sim900SendSMS(SMS_RECIPIENT, buildReadingsSMS(r, false, false, false, true, false));
      sendToSupabase(r);
      sdAppendReading(r);
      lastSCSMS = now;
      wasShortCircuit = true;
    }
  }
  else
  {
    scPending = false;
    wasShortCircuit = false;
  }

  bool poCondition = (r.voltage <= PO_VOLTAGE_THRESHOLD) && (r.current <= PO_CURRENT_MAX) && !scCondition;
  if (poCondition)
  {
    if (!poPending)
    {
      poPending = true;
      poDetectStart = now;
    }
    if (now - poDetectStart >= PO_CONFIRM_MS && (!wasPowerOutage || (now - lastPOSMS >= PO_SMS_COOLDOWN)))
    {
      r.faultType = "POWER_OUTAGE";
      if (sim900Ready && smsAlertsEnabled)
        sim900SendSMS(SMS_RECIPIENT, buildReadingsSMS(r, false, false, false, false, true));
      sendToSupabase(r);
      sdAppendReading(r);
      lastPOSMS = now;
      wasPowerOutage = true;
    }
  }
  else
  {
    poPending = false;
    wasPowerOutage = false;
  }

  if (!scCondition && !poCondition)
    r.faultType = "NORMAL";
}

void checkOverloadAndPreAlert(Reading &r)
{
  unsigned long now = millis();
  bool isOver = isOverloadZone(r.uf);
  if (isOver && (!wasOverloaded || (now - lastOverloadSMS >= OVERLOAD_SMS_COOLDOWN)))
  {
    if (sim900Ready && smsAlertsEnabled)
      sim900SendSMS(SMS_RECIPIENT, buildReadingsSMS(r, true, false, false, false, false));
    lastOverloadSMS = now;
    wasOverloaded = true;
  }
  else if (!isOver)
    wasOverloaded = false;

  bool isPre = isPreAlertZone(r.uf) && !isOver;
  if (isPre && (!wasPreAlert || (now - lastPreAlertSMS >= PREALERT_SMS_COOLDOWN)))
  {
    if (sim900Ready && smsAlertsEnabled)
      sim900SendSMS(SMS_RECIPIENT, buildReadingsSMS(r, false, true, false, false, false));
    lastPreAlertSMS = now;
    wasPreAlert = true;
  }
  else if (!isPre)
    wasPreAlert = false;

  bool isTempHigh = !isnan(r.tempC) && (r.tempC >= TEMP_WARN_ABSOLUTE_C || r.tempRise >= TEMP_RISE_WARN_C);
  if (isTempHigh && (!wasTempAlert || (now - lastTempAlertSMS >= TEMP_SMS_COOLDOWN)))
  {
    if (sim900Ready && smsAlertsEnabled)
      sim900SendSMS(SMS_RECIPIENT, buildReadingsSMS(r, false, false, true, false, false));
    lastTempAlertSMS = now;
    wasTempAlert = true;
  }
  else if (!isTempHigh)
    wasTempAlert = false;
}

// Serial output
void printReadingsToSerial(const Reading &r)
{
  Serial.printf("[%s] V:%.1f I:%.2f P:%.1f S:%.3fkVA UF:%.1f%% %s | T:%.1f %s\n",
                getTimestamp().c_str(), r.voltage, r.current, r.power, r.s_kva, r.uf,
                getLoadStatus(r.uf), r.tempC, getTempStatus(r.tempC));
}
void printErrorToSerial()
{
  static bool lastErrState = false;
  bool err = pzemError || ds18b20Error;
  if (err != lastErrState)
  {
    if (err)
      Serial.println("[ERROR] PZEM or DS18B20 failure - check wiring");
    lastErrState = err;
  }
}

// =============================================================================
//  GPS
// =============================================================================
void readGPS()
{
  bool anyData = false;
  while (gpsSerial.available())
  {
    anyData = true;
    lastGpsDataMs = millis();
    char c = gpsSerial.read();
    if (gps.encode(c))
    {
      float flat, flon;
      unsigned long age;
      gps.f_get_position(&flat, &flon, &age);
      if (flat != TinyGPS::GPS_INVALID_F_ANGLE && flon != TinyGPS::GPS_INVALID_F_ANGLE && age != TinyGPS::GPS_INVALID_AGE)
      {
        gpsLat = flat;
        gpsLng = flon;
        gpsValid = true;
        gpsHwError = false;
      }
    }
  }
  if (!anyData && (millis() - lastGpsDataMs > 10000))
  {
    gpsHwError = true;
    gpsValid = false;
  }
  else if (anyData)
  {
    gpsHwError = false;
  }
}
void sendGPSToSupabase()
{
  if (!gpsValid || !wifiReady)
    return;
  unsigned long now = millis();
  if (now - lastGpsSend < GPS_SEND_INTERVAL)
    return;
  lastGpsSend = now;

  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["ts"] = getISOTimestamp();
  doc["latitude"] = gpsLat;
  doc["longitude"] = gpsLng;
  String body;
  serializeJson(doc, body);
  supabaseRequest("POST", "/rest/v1/gps_log", body);
}
void checkGPSHealth()
{
  unsigned long now = millis();
  if (!gpsValid && !gpsHwError && (now - lastGpsDataMs > 300000))
  {
    if (now - lastGpsAlertSent > 3600000)
    {
      lastGpsAlertSent = now;
      String msg = "GPS WARNING: No satellite fix for 5 minutes.\nCheck antenna.";
      if (sim900Ready && smsAlertsEnabled)
        sim900SendSMS(SMS_RECIPIENT, msg);
      Serial.println("[GPS] No fix - alert sent.");
    }
  }
  else if (gpsHwError && (now - lastGpsAlertSent > 3600000))
  {
    lastGpsAlertSent = now;
    String msg = "GPS HARDWARE ERROR: No data received.\nCheck wiring.";
    if (sim900Ready && smsAlertsEnabled)
      sim900SendSMS(SMS_RECIPIENT, msg);
    Serial.println("[GPS] Hardware error - alert sent.");
  }

  static unsigned long lastStatusWrite = 0;
  if (wifiReady && (now - lastStatusWrite > 60000))
  {
    lastStatusWrite = now;
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["gps_valid"] = gpsValid;
    doc["gps_hardware_error"] = gpsHwError;
    doc["last_data_ms"] = lastGpsDataMs;
    doc["ts"] = getISOTimestamp();
    if (gpsValid)
    {
      doc["latitude"] = gpsLat;
      doc["longitude"] = gpsLng;
    }
    String body;
    serializeJson(doc, body);
    supabaseRequest("POST", "/rest/v1/gps_status?on_conflict=device_id", body,
                    "resolution=merge-duplicates,return=minimal");
  }
}

// =============================================================================
//  SETUP
// =============================================================================
void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=========================================");
  Serial.println("     MATA TRANSFORMER MONITOR v3.0      ");
  Serial.println("   Backend: Supabase REST               ");
  Serial.println("=========================================");

  sdReady = sdInit();
  if (sdReady)
  {
    String fn = getTodayFilename();
    sdEnsureHeader(fn);
    currentLogFile = fn;
    Serial.println("[SD] OK");
  }
  else
    Serial.println("[SD] FAIL");

  thermoReady = initDS18B20();
  Serial.println(thermoReady ? "[DS18B20] OK" : "[DS18B20] FAIL");

  sim900Serial.begin(9600, SERIAL_8N1, SIM900_RX_PIN, SIM900_TX_PIN);
  sim900Ready = sim900Init();
  Serial.println(sim900Ready ? "[SIM900] OK" : "[SIM900] FAIL");

  wifiReady = tryConnectWiFi();
  Serial.println(wifiReady ? "[WiFi] OK" : "[WiFi] FAIL");

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  lastGpsDataMs = millis();
  Serial.println("[GPS] Init OK");

  if (!statusOnSent && wifiReady)
  {
    statusOnSent = sendDeviceStatus("Mata Meter Turned On");
    Serial.println(statusOnSent ? "[STATUS] Device ON sent" : "[STATUS] Device ON send FAILED");
  }

  Serial.println("\nSystem ready - logging every 5 minutes.\n");
  delay(1000);
}

// =============================================================================
//  MAIN LOOP
// =============================================================================
void loop()
{
  unsigned long now = millis();
  readGPS();
  sendGPSToSupabase();
  checkGPSHealth();

  // Retries
  if (!sdReady && (now - lastSDRetry >= RETRY_INTERVAL))
  {
    lastSDRetry = now;
    sdReady = sdInit();
    if (sdReady)
    {
      String fn = getTodayFilename();
      sdEnsureHeader(fn);
      currentLogFile = fn;
      Serial.println("[SD] Recovered.");
    }
  }
  if (!wifiReady && (now - lastWiFiRetry >= RETRY_INTERVAL))
  {
    lastWiFiRetry = now;
    wifiReady = tryConnectWiFi();
  }
  if (!thermoReady && (now - lastThermoRetry >= RETRY_INTERVAL))
  {
    lastThermoRetry = now;
    thermoReady = initDS18B20();
    if (thermoReady)
    {
      ds18b20Error = false;
      Serial.println("[DS18B20] Recovered.");
    }
  }

  // DS18B20 async (non-blocking conversion)
  if (thermoReady)
  {
    if (!ds18b20ConversionPending && (now - lastTempRead >= TEMP_READ_INTERVAL))
    {
      ds18b20.requestTemperatures();
      ds18b20ConversionPending = true;
      ds18b20ConvStart = now;
    }
    if (ds18b20ConversionPending && (now - ds18b20ConvStart >= 750))
    {
      ds18b20ConversionPending = false;
      lastTempRead = now;
      float t = ds18b20.getTempCByIndex(0);
      if (t == DEVICE_DISCONNECTED_C || isnan(t))
      {
        ds18b20Error = true;
        lastTempC = NAN;
        thermoReady = false;
        Serial.println("[DS18B20] Read error - retry later.");
      }
      else
      {
        ds18b20Error = false;
        lastTempC = t;
      }
    }
  }
  else
  {
    ds18b20Error = true;
    lastTempC = NAN;
  }

  // Read PZEM (raw voltage/current/power/frequency come straight from the
  // sensor's onboard metering IC — no recomputation needed here)
  float rawV = pzem.voltage(), rawI = pzem.current(), power = pzem.power(), freq = pzem.frequency();
  bool pzemOK = !isnan(rawV) && !isnan(rawI) && !isnan(power) && !isnan(freq);
  pzemError = !pzemOK;

  if (pzemOK)
  {
    float avgV = vFilter.update(rawV);
    float avgI = iFilter.update(rawI);

    // Voltage fix: only calibrate if raw voltage is meaningful AC input,
    // otherwise report a clean 0V instead of a phantom offset.
    Reading r;
    r.voltage = (avgV > 0.5f) ? max(0.0f, avgV * CAL_V_SLOPE + CAL_V_INTERCEPT) : 0.0f;
    r.current = max(0.0f, avgI * CAL_I_SLOPE + CAL_I_INTERCEPT);
    r.power = power;
    r.freq = freq;
    r.tempC = lastTempC;
    computeDerived(r);

    checkShortCircuitAndOutage(r); // may overwrite r.faultType

    if (now - lastDisplay >= DISPLAY_INTERVAL)
    {
      lastDisplay = now;
      printReadingsToSerial(r);
    }

    // Only log if voltage is realistic (>10V) or there's a genuine fault,
    // to avoid phantom zero-voltage rows while still capturing real outages.
    bool validToLog = (r.voltage > 10.0f) || (strcmp(r.faultType, "POWER_OUTAGE") == 0);
    if (validToLog)
    {
      if (wifiReady && (now - lastSend >= SEND_INTERVAL))
      {
        lastSend = now;
        sendToSupabase(r);
      }
      if (sdReady && (now - lastSDWrite >= SD_WRITE_INTERVAL))
      {
        lastSDWrite = now;
        sdAppendReading(r);
      }
    }

    if (sim900Ready && (now - lastSMSPoll >= SMS_POLL_INTERVAL))
    {
      lastSMSPoll = now;
      handleIncomingSMS(r);
    }
    checkOverloadAndPreAlert(r);
  }
  else if (now - lastDisplay >= DISPLAY_INTERVAL)
  {
    lastDisplay = now;
    printErrorToSerial();
  }
}
