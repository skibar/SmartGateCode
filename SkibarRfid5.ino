/*
   SKIBAR RFID GATE – Smartgate - SLUTGILTIG VERSION 2026
   Kundversion: 126 – lokal utveckling med räknare för passager
   RÄTT TIDSFIX enligt Stefan
   100 % kompilerbar – testad och klar

   ** Grön blink
   Allt fungerar som det ska: två korta blinkningar varannan sekund.
   Godkänt RFID-kort: fast grönt sken i 2 sekunder.
   Ej godkänt RFID-kort: snabb blinkning i 2 sekunder + buzzer.
   Därefter återgår den till dubbelblink.
   Inget blink, funkar inte

   ** Blå blink
   Släckt → inget WiFi
   Blinkar långsamt → WiFi finns, server saknas
   Fast → WiFi + serverkontakt OK
   Två korta blink → config hämtad
*/

#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Update.h>
#include "MFRC522_I2C.h"
#include "time.h"
#include "SkibarTypes.h"

#ifndef F
#define F(s) ((const __FlashStringHelper*)(s))
#endif

#define I2C_SDA 21
#define I2C_SCL 22
#define MFRC522_ADDR 0x28

// sätt detta när en ny installation ska göras
const int programVersion = 127;
long portSiteId = 1;
long gateId = 103;
int configVersion = 0;

const byte CMD_WAKEUP = 0x00;            // Kommando för att aktivera LPCD (lågpower card detection)
const byte CMD_GET_UID = 0x01;           // Kommando för att hämta UID
const byte CMD_READ_BLOCK = 0x30;        // Kommando för att läsa ett block
const byte CMD_AUTHENTICATE = 0xA0;      // Kommando för autentisering
const byte CMD_WRITE_BLOCK = 0xA1;       // Kommando för att skriva ett block
const byte STATUS_OK = 0x00;             // Statuskod för lyckad operation
const byte STATUS_CARD_DETECTED = 0x01;  // Statuskod för kort detekterat

#define PIN_RELAY_RED 26    // till röda reläet
#define PIN_RELAY_GREEN 27  // till gröna reläet
#define PIN_LED_GREEN 14    // Grön system/heartbeat-LED
#define PIN_LED_BLUE 25     // Blå kommunikations-LED
#define PIN_BUZZER 32       // Summer via NPN-transistor
#define PIN_IR_BARRIER 33   // E3JK-R4M1 signal, INPUT_PULLUP

// ------------------------------------------------------------
// IR / passage settings
// ------------------------------------------------------------
unsigned long RFID_AUTH_TIMEOUT = 10000UL;         // 10 s: giltigt kort väntar på passage
unsigned long IR_DEBOUNCE_MS = 50UL;               // Filter för IR-signalen
unsigned long INVALID_CARD_RED_DURATION = 3500UL;  // Rött efter ogiltigt kort
unsigned long RED_POST_PASSAGE_DURATION = 3000UL;  // Rött efter att obehörig passerat
unsigned long IR_REARM_DELAY_MS = 1000UL;          // Minst 1 s innan ny röd passage tillåts

bool passageAuthorized = false;
unsigned long passageAuthorizedAt = 0;

bool irBlocked = false;
bool irRawState = false;
unsigned long irStateChangedAt = 0;

GateState gateState = GATE_IDLE;
unsigned long gateStateSince = 0;
unsigned long redHoldUntil = 0;

void savePassageCounts();
void countGreenPassage();
void countRedPassage();
void printPassageCounts();
void checkForOtaUpdate();
bool performOtaUpdate();
void printHelp();

const char* gateStateName(GateState state) {
  switch (state) {
    case GATE_IDLE: return "GATE_IDLE";
    case GATE_GREEN_AUTHORIZED: return "GATE_GREEN_AUTHORIZED";
    case GATE_GREEN_PASSING: return "GATE_GREEN_PASSING";
    case GATE_RED_ACTIVE: return "GATE_RED_ACTIVE";
    case GATE_RED_PASSING: return "GATE_RED_PASSING";
    case GATE_REARM_DELAY: return "GATE_REARM_DELAY";
    default: return "GATE_UNKNOWN";
  }
}

void printGateStateDefinitions() {
  Serial.println(F("GateState enum:"));
  Serial.println(F("  0 = GATE_IDLE"));
  Serial.println(F("  1 = GATE_GREEN_AUTHORIZED"));
  Serial.println(F("  2 = GATE_GREEN_PASSING"));
  Serial.println(F("  3 = GATE_RED_ACTIVE"));
  Serial.println(F("  4 = GATE_RED_PASSING"));
  Serial.println(F("  5 = GATE_REARM_DELAY"));
  Serial.println();
}

void setGateState(GateState newState, const char* reason = nullptr) {
  if (gateState == newState) return;

  const GateState oldState = gateState;
  gateState = newState;
  gateStateSince = millis();

  Serial.printf(
    "GateState: %s (%d) -> %s (%d) | IR: %s",
    gateStateName(oldState), (int)oldState,
    gateStateName(newState), (int)newState,
    irBlocked ? "BRUTEN" : "FRI");

  if (reason && reason[0] != '\0') {
    Serial.printf(" | Orsak: %s", reason);
  }
  Serial.println();
}

const String FIRMWARE_VERSION_URL = "https://server.skibar.se/stage/firmware/version.html";
const String FIRMWARE_BINARY_URL = "https://server.skibar.se/stage/firmware/firmware.bin";
const String GATE_CONFIG_BASE_URL = "http://server.skibar.se/stage/api/gates/";

const char* DEFAULT_SSID = "Skibar";          // "Krimarc Mesh";
const char* DEFAULT_PASSWORD = "Enter1000!";  //"88ee88ee88";

// ------------------------------------------------------------
// Lokal TCP-server för SmartGate Admin
// ------------------------------------------------------------
WiFiServer tcpServer(4242);
WiFiClient tcpClient;
bool wifiWasConnected = false;
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 60000UL;  // Hur ofta ska den kolla efter avbrott i WiFi


String tcpInputBuffer;
bool tcpAuthenticated = false;
// Hemlighet får att använda TCP
const char* TCP_ADMIN_TOKEN = "Kopp!auppmedSesamOppnaD1g";
// ------------------------------------------------------------


String IP = "";
String wifidB = "";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "se.pool.ntp.org", 0, 60000);

MFRC522_I2C mfrc522(MFRC522_ADDR, -1);
MFRC522_I2C::MIFARE_Key key;
const byte DATA_BLOCK = 4;

Preferences prefs;
// Beständiga passage-räknare (sparas i Preferences/flash).
uint32_t greenPassageCount = 0;
uint32_t redPassageCount = 0;

void savePassageCounts() {
  prefs.putULong("greenCount", greenPassageCount);
  prefs.putULong("redCount", redPassageCount);
}

void countGreenPassage() {
  greenPassageCount++;
  prefs.putULong("greenCount", greenPassageCount);
  Serial.printf("GRÖN passage registrerad. Totalt gröna: %lu\n", (unsigned long)greenPassageCount);
}

void countRedPassage() {
  redPassageCount++;
  prefs.putULong("redCount", redPassageCount);
  Serial.printf("RÖD passage registrerad. Totalt röda: %lu\n", (unsigned long)redPassageCount);
}

void printPassageCounts() {
  Serial.println(F("\n=== Passageräknare ==="));
  Serial.printf("GreenPassages: %lu\n", (unsigned long)greenPassageCount);
  Serial.printf("RedPassages  : %lu\n", (unsigned long)redPassageCount);
}


time_t currentTime = 0;
int lastOtaCheckDay = -1;

unsigned long redRelayOnTime = 0, greenRelayOnTime = 0;
bool redRelayActive = false, greenRelayActive = false;

char wifiSsid[32] = "";
char wifiPass[64] = "";
char baseUrl[128] = "http://server.skibar.se/stage/DataSetsWS.asmx";

bool cardWriteMode = false;

unsigned long lastSyncMillis = 0;  // Ny variabel för senaste NTP-synk (i millis)

String database = "SkibarV2";
unsigned long relayOnDuration = 5000;
bool UseIRGate = true;  // Finns det en IR gate i systemet

// ------------------------------------------------------------
// Status-LED
// Grön = systemstatus + RFID-resultat
// Blå   = kontakt med omvärlden / nätverk
// ------------------------------------------------------------
enum ExternalStatus {
  EXTERNAL_NO_WIFI,
  EXTERNAL_CONNECTING,
  EXTERNAL_WIFI_ONLY,
  EXTERNAL_SERVER_OK
};

ExternalStatus externalStatus = EXTERNAL_NO_WIFI;
bool serverContactOk = false;
unsigned long blueEventUntil = 0;
byte blueEventFlashes = 0;

// Grön LED: RFID-händelser prioriteras över heartbeat.
enum GreenLedEvent {
  GREEN_EVENT_NONE,
  GREEN_EVENT_APPROVED,
  GREEN_EVENT_REJECTED
};
GreenLedEvent greenLedEvent = GREEN_EVENT_NONE;
unsigned long greenEventUntil = 0;

// ------------------------------------------------------------
// Serverstyrd gate-config
// ------------------------------------------------------------
String getJsonValue(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int keyPos = json.indexOf(search);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + search.length());
  if (colonPos < 0) return "";

  int start = colonPos + 1;
  while (start < (int)json.length() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
    start++;
  }

  bool quoted = start < (int)json.length() && json[start] == '"';
  if (quoted) start++;

  int end = start;
  if (quoted) {
    end = json.indexOf('"', start);
  } else {
    while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\r' && json[end] != '\n') {
      end++;
    }
  }

  if (end < 0) return "";
  String value = json.substring(start, end);
  value.trim();
  return value;
}

bool jsonHasValue(const String& json, const char* key) {
  return json.indexOf(String("\"") + key + "\"") >= 0;
}

bool jsonGetBool(const String& json, const char* key, bool fallback) {
  if (!jsonHasValue(json, key)) return fallback;
  String v = getJsonValue(json, key);
  v.toLowerCase();
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return fallback;
}

unsigned long jsonGetULong(const String& json, const char* key, unsigned long fallback) {
  if (!jsonHasValue(json, key)) return fallback;
  String v = getJsonValue(json, key);
  if (v.length() == 0) return fallback;
  return strtoul(v.c_str(), nullptr, 10);
}

long jsonGetLong(const String& json, const char* key, long fallback) {
  if (!jsonHasValue(json, key)) return fallback;
  String v = getJsonValue(json, key);
  if (v.length() == 0) return fallback;
  return strtol(v.c_str(), nullptr, 10);
}

int jsonGetInt(const String& json, const char* key, int fallback) {
  if (!jsonHasValue(json, key)) return fallback;
  String v = getJsonValue(json, key);
  if (v.length() == 0) return fallback;
  return v.toInt();
}

void saveGateConfigToPreferences() {
  prefs.putLong("gateId", gateId);
  prefs.putLong("portSiteId", portSiteId);
  prefs.putBool("irExist", UseIRGate);
  prefs.putULong("relayOnDuration", relayOnDuration);
  prefs.putULong("rfidAuthTimeout", RFID_AUTH_TIMEOUT);
  prefs.putULong("irDebounceMs", IR_DEBOUNCE_MS);
  prefs.putULong("invalidRedMs", INVALID_CARD_RED_DURATION);
  prefs.putULong("redPostMs", RED_POST_PASSAGE_DURATION);
  prefs.putULong("irRearmMs", IR_REARM_DELAY_MS);
  prefs.putInt("configVersion", configVersion);
}

void applyGateConfig(const GateConfig& cfg) {
  const bool irChanged = (UseIRGate != cfg.irExists);

  gateId = cfg.gateId;
  portSiteId = cfg.siteId;
  UseIRGate = cfg.irExists;
  relayOnDuration = cfg.relayOnDuration;
  RFID_AUTH_TIMEOUT = cfg.rfidAuthTimeout;
  IR_DEBOUNCE_MS = cfg.irDebounceMs;
  INVALID_CARD_RED_DURATION = cfg.invalidCardRedDuration;
  RED_POST_PASSAGE_DURATION = cfg.redPostPassageDuration;
  IR_REARM_DELAY_MS = cfg.irRearmDelay;
  configVersion = cfg.configVersion;

  if (irChanged) {
    passageAuthorized = false;
    passageAuthorizedAt = 0;
    redHoldUntil = 0;
    setGateState(GATE_IDLE, "IR-konfiguration ändrad");
  }

  saveGateConfigToPreferences();
}

bool loadGateConfigFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverContactOk = false;
    Serial.println(F("Config: Ingen WiFi – använder lokal config."));
    return false;
  }

  String gateUrl = GATE_CONFIG_BASE_URL + String(gateId) + "/config.json";
  Serial.printf("Config: hämtar %s\n", gateUrl.c_str());

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(gateUrl);

  int httpCode = http.GET();
  if (httpCode > 0) {
    serverContactOk = true;
  } else {
    serverContactOk = false;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Config: HTTP-fel %d – behåller lokal config.\n", httpCode);
    http.end();
    return false;
  }

  String json = http.getString();
  http.end();

  json.trim();
  if (json.length() < 2 || json[0] != '{') {
    Serial.println(F("Config: Ogiltigt JSON-svar – behåller lokal config."));
    return false;
  }

  long responseGateId = jsonGetLong(json, "gateId", gateId);
  if (responseGateId != gateId) {
    Serial.printf("Config: FEL gateId i svar (%ld), förväntade %ld.\n", responseGateId, gateId);
    return false;
  }

  GateConfig cfg;
  cfg.gateId = gateId;
  cfg.siteId = jsonGetLong(json, "siteId", portSiteId);
  cfg.irExists = jsonGetBool(json, "irExists", UseIRGate);
  cfg.relayOnDuration = jsonGetULong(json, "relayOnDuration", relayOnDuration);
  cfg.rfidAuthTimeout = jsonGetULong(json, "rfidAuthTimeout", RFID_AUTH_TIMEOUT);
  cfg.irDebounceMs = jsonGetULong(json, "irDebounceMs", IR_DEBOUNCE_MS);
  cfg.invalidCardRedDuration = jsonGetULong(json, "invalidCardRedDuration", INVALID_CARD_RED_DURATION);
  cfg.redPostPassageDuration = jsonGetULong(json, "redPostPassageDuration", RED_POST_PASSAGE_DURATION);
  cfg.irRearmDelay = jsonGetULong(json, "irRearmDelay", IR_REARM_DELAY_MS);
  cfg.configVersion = jsonGetInt(json, "configVersion", configVersion);

  Serial.printf(
    "Config: Gate=%ld, Site=%ld, IR=%s, Relay=%lums, Auth=%lums, Red=%lums, Post=%lums, Rearm=%lums, Version=%d\n",
    cfg.gateId, cfg.siteId, cfg.irExists ? "JA" : "NEJ",
    cfg.relayOnDuration, cfg.rfidAuthTimeout, cfg.invalidCardRedDuration,
    cfg.redPostPassageDuration, cfg.irRearmDelay, cfg.configVersion);

  // Samma version innebär att serverkonfigurationen inte har ändrats.
  if (cfg.configVersion == configVersion) {
    Serial.printf("Config version %d oförändrad - behåller lokal config.\n", configVersion);
    return true;
  } else {
    Serial.println(F("Config: sparad i Preferences och aktiv."));
    Serial.println("");
  }

  signalBlueEvent(2);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println(F("\n=== Skibar RFID Gate v2.1 startar ==="));

  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);  // 400kHz
  mfrc522.PCD_Init();
  delay(4);

  byte ver = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.printf("MFRC522 version: 0x%02X\n", ver);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println(F("FEL: Ingen RFID-läsare! Kolla kablar."));
    startupGreenErrorBlink();
  }

  pinMode(PIN_RELAY_RED, OUTPUT);
  pinMode(PIN_RELAY_GREEN, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_IR_BARRIER, INPUT_PULLUP);

  irRawState = (digitalRead(PIN_IR_BARRIER) == HIGH);
  irBlocked = irRawState;
  irStateChangedAt = millis();
  gateStateSince = millis();
  Serial.printf("IR-barriär startläge: %s\n", irBlocked ? "BRUTEN" : "FRI");

  digitalWrite(PIN_RELAY_RED, HIGH);
  digitalWrite(PIN_RELAY_GREEN, HIGH);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_BLUE, LOW);

  prefs.begin("skibar", false);
  greenPassageCount = prefs.getULong("greenCount", 0);
  redPassageCount = prefs.getULong("redCount", 0);
  Serial.printf("Passageräknare laddade: gröna=%lu, röda=%lu\n",
                (unsigned long)greenPassageCount, (unsigned long)redPassageCount);

  // Ladda WiFi-creds först
  prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  if (strlen(wifiSsid) == 0) {
    strcpy(wifiSsid, DEFAULT_SSID);
    strcpy(wifiPass, DEFAULT_PASSWORD);
  }

  // Ladda lagrade värden
  database = prefs.getString("database", "SkibarV2");
  gateId = prefs.getLong("gateId", 103);
  portSiteId = prefs.getLong("portSiteId", 1);
  relayOnDuration = prefs.getULong("relayOnDuration", 5000UL);
  RFID_AUTH_TIMEOUT = prefs.getULong("rfidAuthTimeout", 10000UL);
  IR_DEBOUNCE_MS = prefs.getULong("irDebounceMs", 50UL);
  INVALID_CARD_RED_DURATION = prefs.getULong("invalidRedMs", 3500UL);
  RED_POST_PASSAGE_DURATION = prefs.getULong("redPostMs", 3000UL);
  IR_REARM_DELAY_MS = prefs.getULong("irRearmMs", 1000UL);
  UseIRGate = prefs.getBool("irExist", true);
  configVersion = prefs.getInt("configVersion", 0);
  Serial.printf("Gate ID: %ld | Site ID: %ld | Config version: %d | IR gate: %s\n",
                gateId, portSiteId, configVersion, UseIRGate ? "JA" : "NEJ");

  // Anslut WiFi TIDIGT
  connectWiFi();
  // Starta TCP-servern om Wi-Fi är anslutet.
  if (WiFi.status() == WL_CONNECTED) {
    tcpServer.begin();
    tcpServer.setNoDelay(true);

    wifiWasConnected = true;

    Serial.printf("TCP-server startad på %s:4242\n", WiFi.localIP().toString().c_str());
  }


  // Initiera NTPClient – fix för UDP-connection
  timeClient.begin();

  // Sätt TZ och configTime EFTER WiFi
  configTime(0, 0, "se.pool.ntp.org");
  delay(100);

  // Sätt TZ efter configTime
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  // Synka tid (använd syncTimeFromNtp som har bättre felhantering, istället för tvingande loop)
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeFromNtp();
    lastSyncMillis = millis();  // Sätt initial synktid
  } else {
    // Fallback: Ladda sista sparade tid om ingen WiFi
    currentTime = prefs.getLong("time", time(nullptr));  // Använd current system time som default om ingen sparad
    Serial.printf("Ingen WiFi – använder sparad tid: %s", ctime(&currentTime));
  }

  // Resten som innan...
  prefs.getString("baseUrl", baseUrl, sizeof(baseUrl));
  if (strlen(baseUrl) < 10) strcpy(baseUrl, "http://server.skibar.se/stage/DataSetsWS.asmx");

  if (WiFi.status() == WL_CONNECTED) {
    // Extra synk om behövs
    syncTimeFromNtp();
    lastSyncMillis = millis();
  }
  delay(1500);

  printGateStateDefinitions();

  if (WiFi.status() == WL_CONNECTED) {
    loadGateConfigFromServer();
  }

  checkForOtaUpdate();
  printHelp();

  startupGreenReadyBlink();
  Serial.println(F("Redo för kort!\n"));
}

void syncTimeFromNtp() {
  Serial.print(F("Synkar tid... "));

  // Använd configTime:s inbyggda synk, vänta tills tid är satt
  unsigned long start = millis();
  while (time(nullptr) < 1000000000UL && millis() - start < 10000) {  // Vänta upp till 10 sekunder
    delay(500);
    Serial.print(".");
  }

  if (time(nullptr) < 1000000000UL) {
    Serial.println(F(" misslyckades"));
    return;
  }

  currentTime = time(nullptr);  // UTC epoch
  prefs.putLong("time", currentTime);
  Serial.printf(" UTC: %s", ctime(&currentTime));  // Detta visar lokal tid pga TZ

  // För att visa lokal tid explicit
  struct tm local_tm;
  localtime_r(&currentTime, &local_tm);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local_tm);
  Serial.printf(" Lokal: %s\n", buf);
}

void updateLocalTime() {
  static unsigned long lastMinuteUpdate = 0;
  unsigned long now = millis();

  if (now - lastMinuteUpdate >= 60000) {
    // Kolla om det gått en timme sedan senaste synk
    if (WiFi.status() == WL_CONNECTED && now - lastSyncMillis >= 3600000UL) {  // 3600000 ms = 1 timme
      syncTimeFromNtp();
      lastSyncMillis = now;
    } else {
      // Annars, räkna upp lokalt
      currentTime += 60;                   // Lägg till 60 sekunder (UTC)
      prefs.putLong("time", currentTime);  // Spara uppdaterad tid
      struct tm local_tm;
      localtime_r(&currentTime, &local_tm);
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local_tm);
      //Serial.printf("Uppdaterar tid lokalt: %s\n", buf);
    }
    lastMinuteUpdate = now;
  }
}

void connectWiFi() {
  Serial.printf("Ansluter till WiFi: %s...", wifiSsid);
  externalStatus = EXTERNAL_CONNECTING;
  bool ledState = false;

  WiFi.begin(wifiSsid, wifiPass);
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    ledState = !ledState;
    digitalWrite(PIN_LED_BLUE, ledState ? HIGH : LOW);
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F(" OK!"));
    externalStatus = EXTERNAL_WIFI_ONLY;
    digitalWrite(PIN_LED_BLUE, LOW);

    String ipStr = WiFi.localIP().toString();
    Serial.printf("IP: %s | Signal: %d dBm\n", ipStr.c_str(), WiFi.RSSI());
    wifidB = String(WiFi.RSSI()) + "dBm";
    IP = ipStr;
  } else {
    Serial.println(F(" misslyckades"));
    externalStatus = EXTERNAL_NO_WIFI;
    serverContactOk = false;
    digitalWrite(PIN_LED_BLUE, LOW);
  }
}

void maintainWiFiConnection() {
  unsigned long now = millis();

  // ----------------------------------------------------------
  // WiFi har tappats
  // ----------------------------------------------------------
  if (WiFi.status() != WL_CONNECTED) {

    if (wifiWasConnected) {
      Serial.println();
      Serial.println(F("WiFi-anslutningen har förlorats!"));

      wifiWasConnected = false;

      // Avsluta eventuell TCP-session
      tcpClient.stop();
      tcpAuthenticated = false;
      tcpInputBuffer = "";

      IP = "";
      wifidB = "";

      serverContactOk = false;
      externalStatus = EXTERNAL_NO_WIFI;

      digitalWrite(PIN_LED_BLUE, LOW);
    }

    // Försök återansluta var 15:e sekund
    if (now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
      lastWifiReconnectAttempt = now;

      Serial.println(F("Försöker återansluta till WiFi..."));
      WiFi.reconnect();
    }

    return;
  }

  // ----------------------------------------------------------
  // WiFi har återkommit
  // ----------------------------------------------------------
  if (!wifiWasConnected) {
    wifiWasConnected = true;

    Serial.println();
    Serial.println(F("WiFi-anslutningen är tillbaka!"));

    IP = WiFi.localIP().toString();
    wifidB = String(WiFi.RSSI()) + "dBm";

    Serial.printf("IP: %s | Signal: %d dBm\n",
                  IP.c_str(), WiFi.RSSI());

    externalStatus = EXTERNAL_WIFI_ONLY;
    serverContactOk = false;

    // Starta TCP-servern igen
    tcpServer.begin();
    tcpServer.setNoDelay(true);

    Serial.println(F("TCP-server startad på port 4242."));

    // Kontrollera kontakten med Skibar-servern igen
    Serial.println(F("Kontrollerar kontakt med Skibar-servern..."));

    if (loadGateConfigFromServer()) {
      Serial.println(F("Serverkontakt återupprättad."));
    } else {
      Serial.println(F("Serverkontakt saknas fortfarande."));
    }
  }
}


void updateStatusLeds() {
  const unsigned long now = millis();

  // ----------------------------------------------------------
  // GRÖN LED:
  //   - Godkänt RFID: fast sken i 2 sekunder
  //   - Ej godkänt RFID: snabb blinkning i 2 sekunder
  //   - Normal drift: dubbelblink varannan sekund
  // ----------------------------------------------------------
  static bool greenOn = false;

  if (greenLedEvent != GREEN_EVENT_NONE) {
    if (now >= greenEventUntil) {
      greenLedEvent = GREEN_EVENT_NONE;
      greenOn = false;
      digitalWrite(PIN_LED_GREEN, LOW);
    } else if (greenLedEvent == GREEN_EVENT_APPROVED) {
      // Godkänt kort: fast grönt i 2 sekunder.
      digitalWrite(PIN_LED_GREEN, HIGH);
      greenOn = true;
    } else {
      // Ej godkänt kort: snabb blinkning under 2 sekunder.
      const unsigned long phase = (now / 150UL) % 2UL;
      greenOn = (phase == 0);
      digitalWrite(PIN_LED_GREEN, greenOn ? HIGH : LOW);
    }
  } else {
    // Normal heartbeat: två korta blinkningar, därefter paus.
    // Mönster upprepas varannan sekund: 100 ms PÅ, 100 ms AV,
    // 100 ms PÅ, sedan AV resten av perioden.
    const unsigned long phase = now % 2000UL;
    greenOn = (phase < 100UL) || (phase >= 200UL && phase < 300UL);
    digitalWrite(PIN_LED_GREEN, greenOn ? HIGH : LOW);
  }

  // ----------------------------------------------------------
  // BLÅ LED: kommunikationsstatus. En lyckad config-hämtning
  // kan dessutom ge två korta blinkningar.
  // ----------------------------------------------------------
  static unsigned long blueLast = 0;
  static bool blueOn = false;
  static byte eventFlashDone = 0;

  if (blueEventFlashes > 0 && now >= blueEventUntil) {
    blueEventFlashes = 0;
    eventFlashDone = 0;
    blueEventUntil = 0;
    blueOn = false;
    digitalWrite(PIN_LED_BLUE, LOW);
    blueLast = now;
  }

  if (blueEventFlashes > 0 && now < blueEventUntil) {
    const unsigned long phase = now - blueLast;
    if (!blueOn && phase >= 150UL) {
      digitalWrite(PIN_LED_BLUE, HIGH);
      blueOn = true;
      blueLast = now;
    } else if (blueOn && phase >= 150UL) {
      digitalWrite(PIN_LED_BLUE, LOW);
      blueOn = false;
      blueLast = now;
      eventFlashDone++;
      if (eventFlashDone >= blueEventFlashes) {
        blueEventFlashes = 0;
        eventFlashDone = 0;
        blueEventUntil = 0;
        blueLast = now;
      }
    }
    return;
  }

  if (blueEventFlashes == 0) {
    if (WiFi.status() != WL_CONNECTED) {
      externalStatus = EXTERNAL_NO_WIFI;
    } else if (!serverContactOk) {
      externalStatus = EXTERNAL_WIFI_ONLY;
    } else {
      externalStatus = EXTERNAL_SERVER_OK;
    }

    switch (externalStatus) {
      case EXTERNAL_NO_WIFI:
        digitalWrite(PIN_LED_BLUE, LOW);
        break;

      case EXTERNAL_CONNECTING:
        if (now - blueLast >= 250UL) {
          blueOn = !blueOn;
          digitalWrite(PIN_LED_BLUE, blueOn ? HIGH : LOW);
          blueLast = now;
        }
        break;

      case EXTERNAL_WIFI_ONLY:
        if (now - blueLast >= 1000UL) {
          blueOn = !blueOn;
          digitalWrite(PIN_LED_BLUE, blueOn ? HIGH : LOW);
          blueLast = now;
        }
        break;

      case EXTERNAL_SERVER_OK:
        digitalWrite(PIN_LED_BLUE, HIGH);
        break;
    }
  }
}

void signalBlueEvent(byte flashes) {
  if (flashes == 0) return;
  blueEventFlashes = flashes;
  blueEventUntil = millis() + ((unsigned long)flashes * 400UL);
}

void signalGreenApproved() {
  greenLedEvent = GREEN_EVENT_APPROVED;
  greenEventUntil = millis() + 2000UL;
}

void signalGreenRejected() {
  greenLedEvent = GREEN_EVENT_REJECTED;
  greenEventUntil = millis() + 2000UL;
}

void startupGreenReadyBlink() {
  for (byte i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(100);
    digitalWrite(PIN_LED_GREEN, LOW);
    delay(100);
  }
}

void startupGreenErrorBlink() {
  digitalWrite(PIN_LED_BLUE, LOW);
  while (true) {
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(200);
    digitalWrite(PIN_LED_GREEN, LOW);
    delay(200);
  }
}

void relayOn(uint8_t pin, const char* name, unsigned long& timer, bool& state) {
  if (pin == PIN_RELAY_RED && greenRelayActive) relayOff(PIN_RELAY_GREEN, "Grönt");
  if (pin == PIN_RELAY_GREEN && redRelayActive) relayOff(PIN_RELAY_RED, "Rött");
  digitalWrite(pin, LOW);
  timer = millis();
  state = true;
  Serial.printf("%s relä → PÅ\n", name);
}

void relayOff(uint8_t pin, const char* name) {
  digitalWrite(pin, HIGH);
  if (pin == PIN_RELAY_RED) redRelayActive = false;
  if (pin == PIN_RELAY_GREEN) greenRelayActive = false;
  Serial.printf("%s relä → AV\n", name);
}

void authorizePassage() {
  passageAuthorized = true;
  passageAuthorizedAt = millis();

  // Krav: grönt ska tändas omedelbart vid godkänt RFID.
  relayOn(PIN_RELAY_GREEN, "Grönt", greenRelayOnTime, greenRelayActive);
  signalGreenApproved();

  if (UseIRGate) {
    setGateState(irBlocked ? GATE_GREEN_PASSING : GATE_GREEN_AUTHORIZED,
                 irBlocked ? "RFID godkänt + IR redan bruten" : "RFID godkänt, väntar på IR");

    if (irBlocked) {
      Serial.println(F("RFID GODKÄNT och IR redan bruten → GRÖN passage"));
    } else {
      Serial.println(F("RFID GODKÄNT → GRÖN tänd, väntar på IR-passage..."));
    }
  } else {
    // Utan IR ska gammalt beteende behållas.
    setGateState(GATE_IDLE, "IR avstängd / reset");
  }
}

void cancelPassageAuthorization(const char* reason) {
  if (passageAuthorized) {
    Serial.printf("RFID-godkännande återställt: %s\n", reason);
  }
  passageAuthorized = false;
  passageAuthorizedAt = 0;
}

void updateIrInput() {
  const unsigned long now = millis();
  const bool rawBlocked = (digitalRead(PIN_IR_BARRIER) == HIGH);

  if (rawBlocked != irRawState) {
    irRawState = rawBlocked;
    irStateChangedAt = now;
  }

  if (now - irStateChangedAt >= IR_DEBOUNCE_MS) {
    irBlocked = irRawState;
  }
}

void startRedForInvalidCard() {
  const unsigned long now = millis();

  relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
  signalGreenRejected();
  redHoldUntil = now + INVALID_CARD_RED_DURATION;
  setGateState(irBlocked ? GATE_RED_PASSING : GATE_RED_ACTIVE,
               irBlocked ? "Ogiltigt RFID + IR bruten" : "Ogiltigt RFID");

  Serial.printf("OGILTIGT RFID → RÖD i minst %lu ms\n", INVALID_CARD_RED_DURATION);
}

void startRedForUnapprovedPassage() {
  const unsigned long now = millis();

  relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
  redHoldUntil = 0;  // Sätts när strålen kommer tillbaka.
  setGateState(GATE_RED_PASSING, "IR-passage utan godkänt RFID");

  Serial.println(F("IR-passage utan godkänt RFID → RÖD under hela passagen"));
}

void handleIrPassage() {
  if (!UseIRGate) return;

  const unsigned long now = millis();
  // Under kortskrivning ska IR inte skapa passagehändelser.
  // Vi fortsätter ändå läsa sensorn så övergångar efter skrivning inte blir falska.
  updateIrInput();
  if (cardWriteMode) {
    gateStateSince = now;
    return;
  }

  switch (gateState) {
    case GATE_IDLE:
      if (irBlocked) {
        startRedForUnapprovedPassage();
      }
      break;

    case GATE_GREEN_AUTHORIZED:
      if (irBlocked) {
        Serial.println(F("IR bruten efter godkänt RFID → GRÖN passage"));
        setGateState(GATE_GREEN_PASSING, "IR bruten efter godkänt RFID");
      } else if (passageAuthorized && now - passageAuthorizedAt >= RFID_AUTH_TIMEOUT) {
        cancelPassageAuthorization("timeout");
        setGateState(GATE_IDLE, "IR avstängd / reset");
        Serial.println(F("RFID-godkännande timeout → GRÖN passage ej genomförd"));
      }
      break;

    case GATE_GREEN_PASSING:
      // Krav: så länge IR är bruten ska grönt fortsätta lysa.
      if (!greenRelayActive) {
        relayOn(PIN_RELAY_GREEN, "Grönt", greenRelayOnTime, greenRelayActive);
      }

      if (!irBlocked) {
        Serial.println(F("Godkänd passage avslutad → GRÖN av, rearm 1 sekund"));
        countGreenPassage();
        relayOff(PIN_RELAY_GREEN, "Grönt");
        cancelPassageAuthorization("passage genomförd");
        setGateState(GATE_REARM_DELAY, "Godkänd passage avslutad");
      }
      break;

    case GATE_RED_ACTIVE:
      // Ogiltigt kort: rött hålls minst 3,5 s.
      // Om strålen bryts under tiden går vi över till RED_PASSING.
      if (irBlocked) {
        Serial.println(F("IR bruten medan RÖD är aktiv → RÖD under hela passagen"));
        setGateState(GATE_RED_PASSING, "IR bruten under röd aktiv");
      } else if (now >= redHoldUntil) {
        relayOff(PIN_RELAY_RED, "Rött");
        setGateState(GATE_IDLE, "IR avstängd / reset");
      }
      break;

    case GATE_RED_PASSING:
      // Krav: rött ska vara tänt hela tiden medan strålen är bruten.
      if (!redRelayActive) {
        relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
      }

      if (!irBlocked) {
        // Registrera en faktisk obehörig IR-passage en gång när strålen blir fri.
        countRedPassage();
        // Efter passage ska rött fortsätta minst 3 sekunder.
        redHoldUntil = max(redHoldUntil, now + RED_POST_PASSAGE_DURATION);
        Serial.println(F("Röd passage avslutad → RÖD kvar 3 sekunder"));
        setGateState(GATE_RED_ACTIVE, "Obehörig passage avslutad, eftertid");
      }
      break;

    case GATE_REARM_DELAY:
      // Efter godkänd passage får rött inte reagera direkt.
      // Kräver 1 sekund med fri stråle innan systemet återgår till IDLE.
      if (irBlocked) {
        gateStateSince = now;
      } else if (now - gateStateSince >= IR_REARM_DELAY_MS) {
        setGateState(GATE_IDLE, "IR avstängd / reset");
        gateStateSince = now;
        Serial.println(F("IR rearm klar → systemet redo för ny passage"));
      }
      break;
  }
}

String getCardUid() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}



bool authenticateCard() {

  byte status = mfrc522.PCD_Authenticate(
    MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A,
    7,
    &key,
    &(mfrc522.uid));

  if (status != MFRC522_I2C::STATUS_OK) {
    Serial.printf("Auth fel: %s\n", mfrc522.GetStatusCodeName(status));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return false;
  }

  return true;
}

bool authenticateSector() {
  Wire.beginTransmission(MFRC522_ADDR);
  Wire.write(CMD_AUTHENTICATE);        // Använd konstant för autentisering
  Wire.write(DATA_BLOCK / 4 * 4 + 3);  // Trailerblock för sektorn
  Wire.write(key.keyByte, 6);          // Skicka nyckel A
  Wire.endTransmission();
  delay(10);
  Wire.requestFrom(MFRC522_ADDR, 1);
  if (Wire.available()) {
    byte status = Wire.read();
    return status == STATUS_OK;  // Använd konstant för OK-status
  }
  return false;
}

//

bool readCardData(time_t& from, time_t& to, long& orderId, long& siteId, bool leaveCardActive = false) {
  byte buffer[18];
  byte len = 18;

  if (!authenticateCard()) {
    Serial.println(F("❌ Autentisering misslyckades vid läsning"));
    return false;
  }

  if (mfrc522.MIFARE_Read(DATA_BLOCK, buffer, &len) != MFRC522_I2C::STATUS_OK) {
    Serial.println(F("❌ Läsning misslyckades"));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return false;
  }

  // Parsa data (samma som tidigare)
  int y1 = buffer[0] + 2000;
  int m1 = buffer[1];
  int d1 = buffer[2];
  int h1 = buffer[3];
  int min1 = buffer[4];
  int y2 = buffer[5] + 2000;
  int m2 = buffer[6];
  int d2 = buffer[7];
  int h2 = buffer[8];
  int min2 = buffer[9];

  memcpy(&orderId, buffer + 10, 4);
  memcpy(&siteId, buffer + 14, 2);

  struct tm t1 = { 0 }, t2 = { 0 };
  t1.tm_year = y1 - 1900;
  t1.tm_mon = m1 - 1;
  t1.tm_mday = d1;
  t1.tm_hour = h1;
  t1.tm_min = min1;
  t1.tm_sec = 0;
  t1.tm_isdst = -1;

  t2.tm_year = y2 - 1900;
  t2.tm_mon = m2 - 1;
  t2.tm_mday = d2;
  t2.tm_hour = h2;
  t2.tm_min = min2;
  t2.tm_sec = 0;
  t2.tm_isdst = -1;

  from = mktime(&t1);
  to = mktime(&t2);

  Serial.printf("Läser från kort: %04d-%02d-%02d %02d:%02d → %04d-%02d-%02d %02d:%02d | Order:%ld | Site:%ld\n",
                y1, m1, d1, h1, min1, y2, m2, d2, h2, min2, orderId, siteId);

  if (!leaveCardActive) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }

  return true;
}

const char* formatTime(time_t t) {
  static char buf[20];
  struct tm local_tm;
  localtime_r(&t, &local_tm);  // ← Använd localtime_r istället!
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local_tm);
  return buf;
}
String formatTimeDebug(time_t t) {
  struct tm local_tm;
  localtime_r(&t, &local_tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local_tm);
  return String(buf);
}
// const char* formatTime(time_t t) {
//   static char buf[20];
//   strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", localtime(&t));
//   return buf;
// }

void writeCardData(const String& params) {
  String clean = params;
  clean.trim();
  clean.replace(" ", "");
  clean.replace("\t", "");

  String parts[4];
  int idx = 0, startPos = 0;
  for (int i = 0; i <= clean.length() && idx < 4; i++) {
    if (i == clean.length() || clean[i] == ',') {
      parts[idx++] = clean.substring(startPos, i);
      startPos = i + 1;
    }
  }

  if (idx != 4) {
    Serial.println(F("Fel format! Exempel: write 2026-04-20 00:00,2026-05-01 23:59,1234,1"));
    return;
  }

  // Parsa till variabler
  int y1 = parts[0].substring(0, 4).toInt();
  int m1 = parts[0].substring(5, 7).toInt();
  int d1 = parts[0].substring(8, 10).toInt();
  int h1 = parts[0].substring(10, 12).toInt();
  int min1 = parts[0].substring(13, 15).toInt();

  int y2 = parts[1].substring(0, 4).toInt();
  int m2 = parts[1].substring(5, 7).toInt();
  int d2 = parts[1].substring(8, 10).toInt();
  int h2 = parts[1].substring(10, 12).toInt();
  int min2 = parts[1].substring(13, 15).toInt();

  long orderId = parts[2].toInt();
  long siteId = 1;  //parts[3].toInt();

  Serial.printf("Sparar: %04d-%02d-%02d %02d:%02d → %04d-%02d-%02d %02d:%02d | Order:%ld | Site:%ld\n",
                y1, m1, d1, h1, min1, y2, m2, d2, h2, min2, orderId, siteId);

  // === Packa till 16 bytes ===
  byte buf[16] = { 0 };

  // Bytes 0-5:   Start datum+tids (YYYYMMDDHHMM) = 12 tecken men vi tar 6 bytes kompakt
  // Bättre: Använd binary för datum

  // Enkel och pålitlig lösning:
  buf[0] = y1 - 2000;  // år - 2000 (0-255 → räcker till 2255)
  buf[1] = m1;
  buf[2] = d1;
  buf[3] = h1;
  buf[4] = min1;

  buf[5] = y2 - 2000;
  buf[6] = m2;
  buf[7] = d2;
  buf[8] = h2;
  buf[9] = min2;

  // Bytes 10-13: Order ID (4 bytes)
  memcpy(buf + 10, &orderId, 4);

  // Bytes 14-15: Site ID (2 bytes räcker gott)
  memcpy(buf + 14, &siteId, 2);

  // Skriv till kort
  cardWriteMode = true;
  Serial.println(F("Skanna kort för att skriva (10 sekunder)..."));
  unsigned long deadline = millis() + 10000;

  while (millis() < deadline) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      if (authenticateCard() && mfrc522.MIFARE_Write(DATA_BLOCK, buf, 16) == MFRC522_I2C::STATUS_OK) {
        Serial.println(F("✅ Kortet är programmerat!"));
      } else {
        Serial.println(F("❌ Skrivning misslyckades"));
      }
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      break;
    }
    delay(200);
  }
  cardWriteMode = false;
}


bool queryServerAndOpenIfValid(const String& uid, bool canWriteNow = false, bool authorizeForPassage = false) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Ingen WiFi → RFID kan inte verifieras mot servern"));
    return false;
  }

  HTTPClient http;
  http.begin(String(baseUrl) + "?c=" + database);
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader("SOAPAction", "http://tempuri.org/CheckCard");

  String soap = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<soap:Envelope xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
                "xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<soap:Body><CheckCard xmlns=\"http://tempuri.org/\">"
                "<SiteId>"
                + String(portSiteId) + "</SiteId>"
                                       "<GateId>"
                + String(gateId) + "</GateId>"
                                   "<CardID>"
                + uid + "</CardID>"
                        "<Temp>3</Temp><HumId>25</HumId>"
                        "</CheckCard></soap:Body></soap:Envelope>";

  int httpCode = http.POST(soap);
  if (httpCode != 200) {
    Serial.printf("Serverfel (HTTP %d) – passage förblir ej godkänd\n", httpCode);
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();

  int s = resp.indexOf("<CheckCardResult>");
  int e = resp.indexOf("</CheckCardResult>");
  if (s == -1 || e == -1) {
    Serial.println(F("Inget giltigt svar från servern – passage förblir ej godkänd"));
    return false;
  }

  String result = resp.substring(s + 17, e);
  Serial.println("Server svarade: " + result);

  // Parsa resultatet
  int c1 = result.indexOf(',');
  int c2 = result.indexOf(',', c1 + 1);
  int c3 = result.indexOf(',', c2 + 1);

  if (c1 != -1) {
    String orderIdStr = result.substring(0, c1);
    String fromStr = result.substring(c1 + 1, c2);
    String toStr = result.substring(c2 + 1, c3 != -1 ? c3 : result.length());
    String siteIdStr = (c3 != -1) ? result.substring(c3 + 1) : "1";

    // Om vi kan skriva direkt (kortet är fortfarande aktivt)
    if (canWriteNow) {
      if (writeCardNow("write," + fromStr + "," + toStr + "," + orderIdStr + "," + siteIdStr)) {
        Serial.println(F("✅ Kort uppdaterat medan det låg kvar!"));
      }
    }

    if (authorizeForPassage && UseIRGate) {
      authorizePassage();
    } else if (!UseIRGate) {
      // Gamla beteendet när IR inte används: godkänt kort öppnar grönt direkt.
      relayOn(PIN_RELAY_GREEN, "Grönt", greenRelayOnTime, greenRelayActive);
    } else {
      Serial.println(F("Servern godkände kortet (ingen passage startad)."));
    }

    return true;
  } else {
    Serial.println(F("Servern godkände inte kortet."));
    return false;
  }
}

// Snabb skrivning medan kortet fortfarande är aktivt
bool writeCardNow(const String& input) {
  Serial.println("→ Försöker skriva direkt till kortet (medan det ligger kvar)");

  // Samma parsing som tidigare (använd den fungerande versionen)
  String parts[5];
  int partIndex = 0;
  int startIndex = 0;
  for (int i = 0; i <= input.length() && partIndex < 5; i++) {
    if (i == input.length() || input[i] == ',') {
      parts[partIndex++] = input.substring(startIndex, i);
      startIndex = i + 1;
    }
  }

  int y1, m1, d1, h1, min1, y2, m2, d2, h2, min2;
  sscanf(parts[1].c_str(), "%d-%d-%d %d:%d", &y1, &m1, &d1, &h1, &min1);
  sscanf(parts[2].c_str(), "%d-%d-%d %d:%d", &y2, &m2, &d2, &h2, &min2);

  long orderId = parts[3].toInt();
  long siteId = 1;  //parts[4].toInt();

  byte buf[16] = { 0 };
  buf[0] = y1 - 2000;
  buf[1] = m1;
  buf[2] = d1;
  buf[3] = h1;
  buf[4] = min1;
  buf[5] = y2 - 2000;
  buf[6] = m2;
  buf[7] = d2;
  buf[8] = h2;
  buf[9] = min2;
  memcpy(buf + 10, &orderId, 4);
  memcpy(buf + 14, &siteId, 2);


  MFRC522_I2C::StatusCode status = (MFRC522_I2C::StatusCode)mfrc522.MIFARE_Write(DATA_BLOCK, buf, 16);

  if (status == MFRC522_I2C::STATUS_OK) {
    Serial.println(F("✅ Kortet uppdaterades direkt!"));
    return true;
  } else {
    Serial.printf("❌ Write fel: %s\n", mfrc522.GetStatusCodeName(status));
    return false;
  }
}

bool wakeUpCard() {
  // Försök 1: Normal wakeup via kommando
  Wire.beginTransmission(MFRC522_ADDR);
  Wire.write(CMD_WAKEUP);
  Wire.endTransmission();
  delay(15);

  Wire.requestFrom(MFRC522_ADDR, 1);
  if (Wire.available() && Wire.read() == STATUS_CARD_DETECTED) {
    Serial.println(F("✅ Kort väckt via CMD_WAKEUP"));
    return true;
  }

  // Försök 2: Standard bibliotek-metod (ofta mer stabil)
  Serial.println(F("Wakeup misslyckades → försöker med PICC_IsNewCardPresent"));
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    Serial.println(F("✅ Kort hittat via bibliotek"));
    return true;
  }

  // Försök 3: Enkel "ping"
  byte ver = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (ver != 0x00 && ver != 0xFF) {
    Serial.println(F("✅ MFRC522 svarar fortfarande"));
    return true;
  }

  Serial.println(F("❌ Kunde inte väcka kortet efter 3 försök"));
  return false;
}


void writeCardDataCardPresent(String input) {
  Serial.println("→ Raw input från server: " + input);

  // Dela upp på kommatecken först (innan vi tar bort mellanslag)
  String parts[5];
  int partIndex = 0;
  int startIndex = 0;

  for (int i = 0; i <= input.length() && partIndex < 5; i++) {
    if (i == input.length() || input[i] == ',') {
      parts[partIndex++] = input.substring(startIndex, i);
      startIndex = i + 1;
    }
  }

  if (partIndex < 4) {
    Serial.println(F("❌ Fel format från servern"));
    return;
  }

  // parts[0] = orderId (eftersom input börjar med "write," i vissa fall, men servern skickar utan)
  // Justera beroende på hur du anropar funktionen
  String orderStr, fromStr, toStr, siteStr;

  if (parts[0] == "write") {
    fromStr = parts[1];
    toStr = parts[2];
    orderStr = parts[3];
    siteStr = "1";  //parts[4];
  } else {
    // Servern skickar direkt: 216478,2026-04-20 00:00,2026-05-10 23:59,...,1
    orderStr = parts[0];
    fromStr = parts[1];
    toStr = parts[2];
    siteStr = "1";  //parts[4];        // hoppa över "Vuxen" etc.
  }

  // Nu parsar vi datumen ordentligt (med mellanslag kvar)
  int y1, m1, d1, h1, min1;
  int y2, m2, d2, h2, min2;

  sscanf(fromStr.c_str(), "%d-%d-%d %d:%d", &y1, &m1, &d1, &h1, &min1);
  sscanf(toStr.c_str(), "%d-%d-%d %d:%d", &y2, &m2, &d2, &h2, &min2);

  long orderId = orderStr.toInt();
  long siteId = siteStr.toInt();

  Serial.printf("Server-write: %04d-%02d-%02d %02d:%02d → %04d-%02d-%02d %02d:%02d | Order:%ld | Site:%ld\n",
                y1, m1, d1, h1, min1, y2, m2, d2, h2, min2, orderId, siteId);

  // === Packa till kort ===
  byte buf[16] = { 0 };
  buf[0] = y1 - 2000;
  buf[1] = m1;
  buf[2] = d1;
  buf[3] = h1;
  buf[4] = min1;
  buf[5] = y2 - 2000;
  buf[6] = m2;
  buf[7] = d2;
  buf[8] = h2;
  buf[9] = min2;
  memcpy(buf + 10, &orderId, 4);
  memcpy(buf + 14, &siteId, 2);

  // Skriv till kort
  Serial.println(F("Försöker skriva till kort..."));
  bool success = false;
  unsigned long timeout = millis() + 10000;

  while (millis() < timeout) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      if (authenticateCard()) {
        MFRC522_I2C::StatusCode status = (MFRC522_I2C::StatusCode)mfrc522.MIFARE_Write(DATA_BLOCK, buf, 16);

        if (status == MFRC522_I2C::STATUS_OK) {
          Serial.println(F("✅ Kortet uppdaterades från server!"));
          success = true;
        } else {
          Serial.printf("❌ Write fel: %s\n", mfrc522.GetStatusCodeName(status));
        }
      } else {
        Serial.println(F("❌ Autentisering misslyckades"));
      }

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      break;
    }
    delay(50);
  }

  if (!success) Serial.println(F("❌ Timeout vid skrivning"));
}

void printSettings(Print& out) {
  out.println(F("\n=== Nuvarande inställningar ==="));
  out.printf("Tid           : %s", ctime(&currentTime));
  out.printf("Gate ID       : %ld\n", gateId);
  out.printf("Site ID       : %ld\n", portSiteId);
  out.printf("Config ver.   : %d\n", configVersion);
  out.printf("GateState     : %s (%d)\n",
             gateStateName(gateState), (int)gateState);
  out.printf("WiFi          : %s\n", wifiSsid);
  out.printf("Password      : %s\n", wifiPass);
  out.printf("WiFi dB       : %s\n", wifidB.c_str());
  out.printf("Local IP      : %s\n", IP.c_str());
  out.printf("Server        : %s\n", baseUrl);
  out.printf("Config URL    : %s%ld/config\n",
             GATE_CONFIG_BASE_URL.c_str(), gateId);
  out.printf("Database      : %s\n", database.c_str());
  out.printf("Led Duration  : %lu\n", relayOnDuration);
  out.printf("RFID timeout  : %lu\n", RFID_AUTH_TIMEOUT);
  out.printf("IR debounce   : %lu\n", IR_DEBOUNCE_MS);
  out.printf("Invalid red   : %lu\n", INVALID_CARD_RED_DURATION);
  out.printf("Red post      : %lu\n", RED_POST_PASSAGE_DURATION);
  out.printf("IR rearm      : %lu\n", IR_REARM_DELAY_MS);
  out.printf("Version       : %d\n", programVersion);
  out.printf("configVersion : %d\n", configVersion);
  out.printf("IR exists     : %d\n", UseIRGate);
  out.printf("Green passages: %lu\n",
             (unsigned long)greenPassageCount);
  out.printf("Red passages  : %lu\n",
             (unsigned long)redPassageCount);
  //out.printf("Green LED pin : %d\n", PIN_LED_GREEN);
  //out.printf("Blue LED pin  : %d\n", PIN_LED_BLUE);
  out.printf("External stat : %s\n",
             externalStatus == EXTERNAL_NO_WIFI ? "NO_WIFI" : externalStatus == EXTERNAL_WIFI_ONLY ? "WIFI_ONLY"
                                                            : externalStatus == EXTERNAL_SERVER_OK ? "SERVER_OK"
                                                                                                   : "CONNECTING");

  out.println(F("============================"));
}

void processSerialCommand() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0) return;

  Serial.println("→ " + input);

  int spaceIndex = input.indexOf(' ');
  String command = (spaceIndex != -1 ? input.substring(0, spaceIndex) : input);
  command.toLowerCase();
  String params = (spaceIndex != -1 ? input.substring(spaceIndex + 1) : "");

  if (command == "write") {
    writeCardData(params);
  } else if (command == "relay") {
    int p1 = params.indexOf(',');
    int p2 = params.indexOf(',', p1 + 1);
    if (p1 == -1) {
      Serial.println(F("Använd: relay <red/green>,<on/off>"));
      return;
    }
    String r = params.substring(0, p1);
    String a = params.substring(p1 + 1);
    if (r == "red" && a == "on") relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
    if (r == "red" && a == "off") relayOff(PIN_RELAY_RED, "Rött");
    if (r == "green" && a == "on") relayOn(PIN_RELAY_GREEN, "Grönt", greenRelayOnTime, greenRelayActive);
    if (r == "green" && a == "off") relayOff(PIN_RELAY_GREEN, "Grönt");
  } else if (command == "setgatenr") {
    long newGateId = params.toInt();
    if (newGateId > 0) {
      gateId = newGateId;
      prefs.putLong("gateId", gateId);
      configVersion = 0;
      prefs.putInt("configVersion", configVersion);
      Serial.printf("Gate ID uppdaterat till: %ld\n", gateId);
      Serial.printf("Config hämtas vid nästa omstart från: %s%ld/config\n", GATE_CONFIG_BASE_URL.c_str(), gateId);
    } else {
      Serial.println(F("Använd: setgatenr <GateNr>"));
    }
  } else if (command == "setport") {
    long id = params.toInt();
    if (id > 0) {
      portSiteId = id;
      prefs.putLong("portSiteId", id);
      Serial.printf("portSiteId = %ld\n", id);
    }
  } else if (command == "setwifi") {
    int c = params.indexOf(',');
    if (c == -1) {
      Serial.println(F("Använd: setwifi <SSID>,<lösenord>"));
      return;
    }
    String s = params.substring(0, c);
    String p = params.substring(c + 1);
    strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
    strncpy(wifiPass, p.c_str(), sizeof(wifiPass) - 1);
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    Serial.println(F("WiFi sparat → startar om..."));
    delay(1000);
    ESP.restart();
  } else if (command == "setbaseurl") {
    if (params.length() > 0) {
      strncpy(baseUrl, params.c_str(), sizeof(baseUrl) - 1);
      prefs.putString("baseUrl", baseUrl);
      Serial.printf("baseUrl uppdaterad till: %s\n", baseUrl);
    } else {
      Serial.println(F("Använd: setbaseurl <url>"));
    }
  } else if (command == "synctime") {
    syncTimeFromNtp();
    lastSyncMillis = millis();  // Uppdatera synktid efter manuell synk
  } else if (command == "ota") {
    Serial.println(F("Manuell OTA-kontroll..."));
    checkForOtaUpdate();
  } else if (command == "setdate") {
    struct tm tm = { 0 };
    if (sscanf(params.c_str(), "%d-%d-%d %d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) == 5) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      currentTime = mktime(&tm);
      prefs.putLong("time", currentTime);
      Serial.printf("Datum och tid satt manuellt till: %s", ctime(&currentTime));
    } else {
      Serial.println(F("Fel format: setdate YYYY-MM-DD HH:MM"));
    }
  } else if (command == "getcardinfo") {
    int c1 = params.indexOf(',');
    if (c1 == -1) {
      Serial.println(F("Använd: getcardinfo CardID,SiteID"));
      return;
    }
    String cardId = params.substring(0, c1);
    long testSiteId = params.substring(c1 + 1).toInt();
    Serial.printf("Frågar servern om kort %s (SiteID %ld)...\n", cardId.c_str(), testSiteId);
    queryServerAndOpenIfValid(cardId);
  } else if (command == "testwifi") {
    Serial.println(F("\n=== Testar WiFi-anslutning ==="));
    Serial.printf("SSID: %s\n", wifiSsid);
    Serial.printf("Lösenord: %s\n", wifiPass);
    WiFi.disconnect();
    delay(500);
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("WiFi fungerar!"));
      syncTimeFromNtp();
      lastSyncMillis = millis();
    } else {
      Serial.println(F("WiFi fungerar INTE – kolla SSID/lösen eller räckvidd"));
      Serial.println(F("Kräver 2.4Gz"));
    }
  } else if (command == "scanwifi") {
    Serial.println("Skannar WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    Serial.printf("Hittade %d nätverk:\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf(
        "%2d: %-32s RSSI:%4d  CH:%2d\n",
        i,
        WiFi.SSID(i).c_str(),
        WiFi.RSSI(i),
        WiFi.channel(i));
    }
  } else if (command == "setdatabase") {
    if (params.length() > 0) {
      database = params;
      prefs.putString("database", params);
      Serial.println("Database uppdaterad till: " + params);
    } else {
      Serial.println(F("Använd: setdatabase <nytt namn>"));
    }
    // Programversion – skrivskyddad.
  } else if (command == "setversion") {
    tcpClient.printf(
      "ERR Programversionen är skrivskyddad: %d\n",
      programVersion);
  } else if (command == "erasecard" || command == "clearcard" || command == "wipecard") {
    eraseCard();
  } else if (command == "setreldur") {
    unsigned long newDur = params.toInt();
    if (newDur > 0) {
      relayOnDuration = newDur;
      prefs.putULong("relayOnDuration", newDur);
      Serial.printf("Relä-duration uppdaterad till: %lu ms\n", newDur);
    } else {
      Serial.println(F("Använd: setreldur <nytt värde i ms>"));
    }
  } else if (command == "setauthtime") {
    unsigned long newTimeout = params.toInt();

    if (newTimeout > 0 && newTimeout <= 60000) {
      RFID_AUTH_TIMEOUT = newTimeout;
      prefs.putULong("rfidAuthTimeout", RFID_AUTH_TIMEOUT);

      Serial.printf(
        "RFID timeout uppdaterad till: %lu ms\n",
        RFID_AUTH_TIMEOUT);
    } else {
      Serial.println(
        "Använd: setauthtime <millisekunder> (1-60000)");
    }
  } else if (command == "irexist") {
    bool newIRexist = params.toInt();
    UseIRGate = newIRexist;
    prefs.putBool("irExist", newIRexist);

    // Nollställ IR-tillstånd när läget ändras.
    passageAuthorized = false;
    passageAuthorizedAt = 0;
    setGateState(GATE_IDLE, "irexist kommando");
    redHoldUntil = 0;

    if (UseIRGate) {
      updateIrInput();
      Serial.println(F("IR gate aktiverad."));
    } else {
      Serial.println(F("IR gate avstängd – tidigare RFID-beteende används."));
    }

    Serial.printf("IR gate finns i systemet: %d\n", UseIRGate);
  } else if (command == "getcounts") {
    printPassageCounts();
  } else if (command == "clearcounts") {
    if (params == "confirm") {
      greenPassageCount = 0;
      redPassageCount = 0;
      savePassageCounts();
      Serial.println(F("Passageräknarna har nollställts."));
      printPassageCounts();
    } else {
      Serial.println(F("För att nollställa båda räknarna, skriv: clearcounts confirm"));
    }
  } else if (command == "getsettings") {
    Serial.println(F("\n=== Nuvarande inställningar ==="));
    Serial.printf("Tid           : %s", ctime(&currentTime));
    Serial.printf("Gate ID       : %ld\n", gateId);
    Serial.printf("Site ID       : %ld\n", portSiteId);
    Serial.printf("Config ver.   : %d\n", configVersion);
    Serial.printf("GateState     : %s (%d)\n", gateStateName(gateState), (int)gateState);
    Serial.printf("WiFi          : %s\n", wifiSsid);
    Serial.printf("Password      : %s\n", wifiPass);
    Serial.printf("WiFi dB       : %s\n", wifidB);
    Serial.printf("Local IP      : %s\n", IP.c_str());

    Serial.printf("Server        : %s\n", baseUrl);
    Serial.printf("Config URL    : %s%ld/config\n", GATE_CONFIG_BASE_URL.c_str(), gateId);
    Serial.printf("Database      : %s\n", database.c_str());
    Serial.printf("Led Duration  : %lu\n", relayOnDuration);
    Serial.printf("RFID timeout  : %lu\n", RFID_AUTH_TIMEOUT);
    Serial.printf("IR debounce   : %lu\n", IR_DEBOUNCE_MS);
    Serial.printf("Invalid red   : %lu\n", INVALID_CARD_RED_DURATION);
    Serial.printf("Red post      : %lu\n", RED_POST_PASSAGE_DURATION);
    Serial.printf("IR rearm      : %lu\n", IR_REARM_DELAY_MS);
    Serial.printf("Version       : %d\n", programVersion);
    Serial.printf("configVersion : %d\n", configVersion);
    Serial.printf("IR exists     : %d\n", UseIRGate);
    Serial.printf("Green passages: %lu\n", (unsigned long)greenPassageCount);
    Serial.printf("Red passages  : %lu\n", (unsigned long)redPassageCount);
    Serial.printf("Green LED pin : %d\n", PIN_LED_GREEN);
    Serial.printf("Blue LED pin  : %d\n", PIN_LED_BLUE);
    Serial.printf("External stat : %s\n",
                  externalStatus == EXTERNAL_NO_WIFI ? "NO_WIFI" : externalStatus == EXTERNAL_WIFI_ONLY ? "WIFI_ONLY"
                                                                 : externalStatus == EXTERNAL_SERVER_OK ? "SERVER_OK"
                                                                                                        : "CONNECTING");
    Serial.println(F("============================\n"));
  } else printHelp();
}


void processTcpClient() {
  // Acceptera en ny klient om ingen är ansluten.
  if (!tcpClient || !tcpClient.connected()) {
    tcpClient.stop();

    WiFiClient newClient = tcpServer.available();

    if (newClient) {
      tcpClient = newClient;
      tcpClient.setNoDelay(true);

      tcpAuthenticated = false;
      tcpInputBuffer = "";

      Serial.println(F("TCP-klient ansluten."));
      tcpClient.println("SmartGate TCP - skicka AUTH <token>");
    }
  }

  if (!tcpClient || !tcpClient.connected()) {
    return;
  }

  while (tcpClient.available()) {
    char c = (char)tcpClient.read();

    if (c == '\r') {
      continue;
    }

    if (c != '\n') {
      if (tcpInputBuffer.length() < 256) {
        tcpInputBuffer += c;
      } else {
        tcpInputBuffer = "";
        tcpClient.println("ERR command too long");
      }
      continue;
    }

    String input = tcpInputBuffer;
    tcpInputBuffer = "";
    input.trim();

    if (input.length() == 0) {
      continue;
    }

    // Autentisering krävs före kommandon.
    if (!tcpAuthenticated) {
      String expected = String("AUTH ") + TCP_ADMIN_TOKEN;

      if (input == expected) {
        tcpAuthenticated = true;
        tcpClient.println("OK AUTHENTICATED");
        Serial.println(F("TCP-klient autentiserad."));
      } else {
        tcpClient.println("ERR AUTH");
        tcpClient.flush();
        tcpClient.stop();
        tcpAuthenticated = false;
        Serial.println(F("TCP-autentisering misslyckades."));
        return;
      }

      continue;
    }

    // Dela upp raden utan att ändra versaler i parametrarna.
    int spaceIndex = input.indexOf(' ');

    String command =
      (spaceIndex >= 0) ? input.substring(0, spaceIndex) : input;

    String params =
      (spaceIndex >= 0) ? input.substring(spaceIndex + 1) : "";

    command.toLowerCase();
    params.trim();

    Serial.println("TCP kommando: " + command);


    // Lokal OTA: OTA_BEGIN <storlek> <md5>
    if (command == "ota_begin") {
      int separator = params.indexOf(' ');

      if (separator <= 0) {
        tcpClient.println(
          "ERR Använd: OTA_BEGIN <storlek> <md5>");
        tcpClient.flush();
        return;
      }

      String sizeText = params.substring(0, separator);
      String md5 = params.substring(separator + 1);

      sizeText.trim();
      md5.trim();
      // Kontrollera filstorleken.
      char* endPtr = nullptr;
      unsigned long parsedSize = strtoul(
        sizeText.c_str(),
        &endPtr,
        10);

      if (endPtr == sizeText.c_str() || *endPtr != '\0' || parsedSize == 0) {
        tcpClient.println("ERR OTA_INVALID_SIZE");
        tcpClient.flush();
        return;
      }

      // MD5 ska bestå av exakt 32 hexadecimala tecken.
      bool validMd5 = (md5.length() == 32);

      for (size_t i = 0; validMd5 && i < md5.length(); i++) {
        if (!isHexadecimalDigit(md5[i])) {
          validMd5 = false;
        }
      }

      if (!validMd5) {
        tcpClient.println("ERR OTA_INVALID_MD5");
        tcpClient.flush();
        return;
      }

      Serial.printf(
        "Lokal OTA begärd, storlek: %lu bytes\n",
        parsedSize);

      receiveLocalOta((size_t)parsedSize, md5);

      // Avsluta hanteringen av detta TCP-anrop.
      return;
    }
    // Befintliga kommandon fortsätter här.
    if (command == "getsettings") {
      printSettings(tcpClient);
      tcpClient.flush();
    }
    // Läs inställningar.
    if (command == "getsettings") {
      printSettings(tcpClient);
      tcpClient.flush();
    }

    // Gate ID.
    else if (command == "setgatenr") {
      long newGateId = params.toInt();

      if (newGateId > 0) {
        gateId = newGateId;
        prefs.putLong("gateId", gateId);

        configVersion = 0;
        prefs.putInt("configVersion", configVersion);

        tcpClient.printf("Gate ID uppdaterat till: %ld\n", gateId);
        Serial.printf("Gate ID uppdaterat till: %ld\n", gateId);
      } else {
        tcpClient.println("ERR Använd: setgatenr <GateNr>");
      }
    }

    // Site ID.
    else if (command == "setport") {
      long id = params.toInt();

      if (id > 0) {
        portSiteId = id;
        prefs.putLong("portSiteId", id);

        tcpClient.printf("portSiteId = %ld\n", id);
      } else {
        tcpClient.println("ERR Använd: setport <nummer>");
      }
    }

    // Serveradress.
    else if (command == "setbaseurl") {
      if (params.length() > 0 && params.length() < sizeof(baseUrl)) {
        strncpy(baseUrl, params.c_str(), sizeof(baseUrl) - 1);
        baseUrl[sizeof(baseUrl) - 1] = '\0';

        prefs.putString("baseUrl", baseUrl);

        tcpClient.printf("baseUrl uppdaterad till: %s\n", baseUrl);
      } else {
        tcpClient.println("ERR Ogiltig serveradress");
      }
    }

    // Databasnamn.
    else if (command == "setdatabase") {
      if (params.length() > 0) {
        database = params;
        prefs.putString("database", database);

        tcpClient.println("Database uppdaterad.");
      } else {
        tcpClient.println("ERR Använd: setdatabase <namn>");
      }
    }


    // Programversion är skrivskyddad.
    else if (command == "setversion") {
      tcpClient.printf(
        "ERR Programversionen är skrivskyddad: %d\n",
        programVersion);
    }

    // Reläduration.
    else if (command == "setreldur") {
      unsigned long newDuration = params.toInt();

      if (newDuration > 0) {
        relayOnDuration = newDuration;
        prefs.putULong("relayOnDuration", newDuration);

        tcpClient.printf("Relä-duration uppdaterad till: %lu ms\n",
                         newDuration);
      } else {
        tcpClient.println("ERR Ogiltig reläduration");
      }
    }

    // IR-inställning.
    else if (command == "irexist") {
      int value = params.toInt();

      if (value == 0 || value == 1) {
        UseIRGate = (value == 1);
        prefs.putBool("irExist", UseIRGate);

        passageAuthorized = false;
        passageAuthorizedAt = 0;
        setGateState(GATE_IDLE, "TCP irexist kommando");
        redHoldUntil = 0;

        if (UseIRGate) {
          updateIrInput();
        }

        tcpClient.printf("IR gate finns i systemet: %d\n", value);
      } else {
        tcpClient.println("ERR Använd: irexist 0 eller irexist 1");
      }
    }

    // Datum och tid.
    else if (command == "setdate") {
      struct tm tm = { 0 };

      if (sscanf(params.c_str(),
                 "%d-%d-%d %d:%d",
                 &tm.tm_year,
                 &tm.tm_mon,
                 &tm.tm_mday,
                 &tm.tm_hour,
                 &tm.tm_min)
          == 5) {

        tm.tm_year -= 1900;
        tm.tm_mon -= 1;

        currentTime = mktime(&tm);
        prefs.putLong("time", currentTime);

        tcpClient.printf("Datum och tid satt: %s",
                         ctime(&currentTime));
      } else {
        tcpClient.println("ERR Format: setdate YYYY-MM-DD HH:MM");
      }
    }

    // Wi-Fi-inställning. Detta kommando startar om ESP32.
    else if (command == "setwifi") {
      int commaIndex = params.indexOf(',');

      if (commaIndex <= 0) {
        tcpClient.println("ERR Använd: setwifi <SSID>,<lösenord>");
        continue;
      }

      String ssid = params.substring(0, commaIndex);
      String password = params.substring(commaIndex + 1);

      if (ssid.length() >= sizeof(wifiSsid) || password.length() >= sizeof(wifiPass)) {
        tcpClient.println("ERR SSID eller lösenord är för långt");
        continue;
      }

      strlcpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid));
      strlcpy(wifiPass, password.c_str(), sizeof(wifiPass));

      prefs.putString("ssid", wifiSsid);
      prefs.putString("pass", wifiPass);

      tcpClient.println("WiFi sparat. SmartGate startar om.");
      tcpClient.flush();

      Serial.println(F("Wi-Fi-inställningar uppdaterade via TCP."));
      delay(1000);
      ESP.restart();
    }

    else {
      tcpClient.println("ERR Okänt eller ej tillåtet TCP-kommando");
    }

    tcpClient.flush();
  }
}

bool receiveLocalOta(size_t imageSize, const String& md5) {
  if (!tcpAuthenticated || !tcpClient.connected()) {
    return false;
  }

  if (imageSize == 0) {
    tcpClient.println("ERR OTA_INVALID_SIZE");
    return false;
  }

  // Starta uppdatering av själva applikationen.
  if (!Update.begin(imageSize, U_FLASH)) {
    tcpClient.printf("ERR OTA_BEGIN: %s\n",
                     Update.errorString());
    return false;
  }

  // Kontrollera filens MD5 när uppdateringen slutförs.
  if (!Update.setMD5(md5.c_str())) {
    Update.abort();
    tcpClient.println("ERR OTA_INVALID_MD5");
    return false;
  }

  tcpClient.println("READY");
  tcpClient.flush();

  uint8_t buffer[1024];
  size_t received = 0;
  unsigned long lastDataAt = millis();

  while (received < imageSize) {
    if (!tcpClient.connected()) {
      Update.abort();
      return false;
    }

    int availableBytes = tcpClient.available();

    if (availableBytes > 0) {
      size_t remaining = imageSize - received;
      size_t toRead = min(
        (size_t)availableBytes,
        min(sizeof(buffer), remaining));

      int count = tcpClient.read(buffer, toRead);

      if (count > 0) {
        lastDataAt = millis();

        size_t written = Update.write(
          buffer,
          (size_t)count);

        if (written != (size_t)count) {
          Update.abort();
          tcpClient.println("ERR OTA_WRITE");
          tcpClient.flush();
          return false;
        }

        received += written;
      }
    } else {
      // Avbryt om inga nya data kommer inom 30 sekunder.
      if (millis() - lastDataAt > 30000UL) {
        Update.abort();
        tcpClient.println("ERR OTA_TIMEOUT");
        tcpClient.flush();
        return false;
      }

      delay(1);
    }
  }

  // Slutför och verifiera OTA-installationen.
  if (!Update.end()) {
    tcpClient.printf("ERR OTA_END: %s\n",
                     Update.errorString());
    tcpClient.flush();
    return false;
  }

  if (!Update.isFinished()) {
    tcpClient.println("ERR OTA_NOT_FINISHED");
    tcpClient.flush();
    return false;
  }

  tcpClient.println("OK OTA");
  tcpClient.flush();

  Serial.println(
    F("Lokal OTA klar. ESP32 startar om."));

  delay(500);
  tcpClient.stop();
  ESP.restart();

  return true;
}


void printHelp() {
  Serial.println(F(
    "\n"
    "╔══════════════════════════════════════════════════════════╗\n"
    "║              SKIBAR RFID GATE – HJÄLP                    ║\n"
    "╚══════════════════════════════════════════════════════════╝\n"
    "\n"
    " Alla kommandon avslutas med ENTER.\n"
    " Kommandon kan skrivas med stora eller små bokstäver.\n"
    "\n"

    "┌──────────────────────────────────────────────────────────┐\n"
    "│ RFID / KORT                                              │\n"
    "├──────────────────────────────────────────────────────────┤\n"
    "│ write YYYY-MM-DD HH:MM,YYYY-MM-DD HH:MM,ORDER,1          │\n"
    "│      Programmerar ett nytt RFID-kort.                    │\n"
    "│      Håll kortet mot läsaren när programmet ber om det.  │\n"
    "│                                                          │\n"
    "│ erasecard                                                │\n"
    "│ clearcard                                                │\n"
    "│ wipecard                                                 │\n"
    "│      Raderar kortets lagrade data.                       │\n"
    "│                                                          │\n"
    "│ getcardinfo CARDID,SITEID                                │\n"
    "│      Frågar servern om ett specifikt kort.               │\n"
    "└──────────────────────────────────────────────────────────┘\n"
    "\n"

    "┌──────────────────────────────────────────────────────────┐\n"
    "│ RELÄER / LAMPOR                                          │\n"
    "├──────────────────────────────────────────────────────────┤\n"
    "│ relay red,on                                             │\n"
    "│ relay red,off                                            │\n"
    "│ relay green,on                                           │\n"
    "│ relay green,off                                          │\n"
    "│      Manuell test av röd eller grön lampa/relä.          │\n"
    "└──────────────────────────────────────────────────────────┘\n"
    "\n"

    "┌──────────────────────────────────────────────────────────┐\n"
    "│ WIFI                                                     │\n"
    "├──────────────────────────────────────────────────────────┤\n"
    "│ setwifi SSID,LÖSENORD                                    │\n"
    "│      Sparar nytt WiFi och startar om enheten.            │\n"
    "│                                                          │\n"
    "│ testwifi                                                 │\n"
    "│      Testar anslutningen med sparade uppgifter.          │\n"
    "│                                                          │\n"
    "│ scanwifi                                                 │\n"
    "│      Visar tillgängliga WiFi-nät och kanal.              │\n"
    "│      ESP32 använder 2,4 GHz WiFi.                        │\n"
    "└──────────────────────────────────────────────────────────┘\n"
    "\n"

    "┌──────────────────────────────────────────────────────────┐\n"
    "│ SYSTEM / KONFIGURATION                                   │\n"
    "├──────────────────────────────────────────────────────────┤\n"
    "│ setgatenr NUMMER                                         │\n"
    "│      Anger vilket GateNr som används i config-URL:n.     │\n"
    "│                                                          │\n"
    "│ setport NUMMER                                           │\n"
    "│      Anger anläggningens Site ID.                        │\n"
    "│                                                          │\n"
    "│ setdatabase NAMN                                         │\n"
    "│      Anger vilken databas systemet använder.             │\n"
    "│                                                          │\n"
    "│ setreldur MILLISEKUNDER                                  │\n"
    "│      Anger hur länge en lampa ska vara aktiv.            │\n"
    "│                                                          │\n"
    "│ setbaseurl URL                                           │\n"
    "│      Ändrar adressen till servern.                       │\n"
    "│                                                          │\n"
    "│ getsettings                                              │\n"
    "│      Visar aktuell konfiguration och räknare.            │\n"
    "│ getcounts                                                │\n"
    "│      Visar antal registrerade gröna/röda passager.       │\n"
    "│ clearcounts confirm                                      │\n"
    "│      Nollställer båda passageräknarna.                   │\n"
    "└──────────────────────────────────────────────────────────┘\n"
    "\n"

    "┌──────────────────────────────────────────────────────────┐\n"
    "│ TID / UPPDATERING                                        │\n"
    "├──────────────────────────────────────────────────────────┤\n"
    "│ synctime                                                 │\n"
    "│      Hämtar aktuell tid från NTP.                        │\n"
    "│                                                          │\n"
    "│ setdate YYYY-MM-DD HH:MM                                 │\n"
    "│      Ställer klockan manuellt.                           │\n"
    "│                                                          │\n"
    "│ ota                                                      │\n"
    "│      Kontrollerar manuellt efter ny firmware.            │\n"
    "│                                                          │\n"
    "│ irexist 1 / irexist 0                                    │\n"
    "│      Aktiverar eller avaktiverar IR-gate.                │\n"
    "│      1 = IR används, 0 = gammalt RFID-beteende.          │\n"
    "└──────────────────────────────────────────────────────────┘\n"
    "\n"

    "╔══════════════════════════════════════════════════════════╗\n"
    "║ EXEMPEL                                                  ║\n"
    "╠══════════════════════════════════════════════════════════╣\n"
    "║ write 2026-12-20 08:00,2027-01-05 23:59,98765,1          ║\n"
    "║ setwifi MittWifi,hemligtlosenord                         ║\n"
    "║ setport 12                                               ║\n"
    "║ setreldur 5000                                           ║\n"
    "║ setdate 2026-12-01 14:30                                 ║\n"
    "║ relay green,on                                           ║\n"
    "║ irexist 0                                                ║\n"
    "╚══════════════════════════════════════════════════════════╝\n"
    "\n"
    " SKIBAR RFID GATE är redo.\n"));
}

void eraseCard() {
  Serial.println(F("\n=== Raderar kortet ==="));
  Serial.println(F("Håll kortet mot läsaren inom 10 sekunder..."));

  unsigned long deadline = millis() + 10000;
  bool cardFound = false;

  while (millis() < deadline) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      cardFound = true;
      Serial.printf("UID: %s\n", getCardUid().c_str());

      if (!authenticateCard()) {
        Serial.println(F("❌ Autentisering misslyckades"));
        break;
      }

      // Radera block 4
      byte empty[16] = { 0 };

      // Fix: Explicit typkonvertering
      MFRC522_I2C::StatusCode status = (MFRC522_I2C::StatusCode)mfrc522.MIFARE_Write(DATA_BLOCK, empty, 16);

      if (status == MFRC522_I2C::STATUS_OK) {
        Serial.println(F("✅ Kortet är nu raderat (block 4 nollställt)"));
      } else {
        Serial.printf("❌ Fel vid radering: %s\n", mfrc522.GetStatusCodeName(status));
      }

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      break;
    }

    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(100);
    digitalWrite(PIN_LED_GREEN, LOW);
    delay(100);
  }

  if (!cardFound) {
    Serial.println(F("❌ Ingen kort hittades inom 10 sekunder"));
  }
}

void checkForOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("OTA: Ingen WiFi-anslutning."));
    return;
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(FIRMWARE_VERSION_URL);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("OTA: Kunde inte hämta versionsfil. HTTP: %d\n", httpCode);
    http.end();
    return;
  }

  String latestVersion = http.getString();
  http.end();

  latestVersion.trim();

  int latest = latestVersion.toInt();

  Serial.printf("Installerad version: %d\n", programVersion);
  Serial.printf("Tillgänglig version: %d\n", latest);

  if (latest <= programVersion) {
    Serial.println(F("OTA: Ingen ny version tillgänglig."));
    return;
  }

  Serial.println(F("OTA: Ny version finns. Startar uppdatering..."));

  if (performOtaUpdate()) {
    Serial.println(F("OTA: Uppdatering klar. Startar om ESP32..."));
    delay(1500);
    ESP.restart();
  } else {
    Serial.println(F("OTA: Uppdateringen misslyckades."));
  }
}

bool performOtaUpdate() {
  HTTPClient http;
  http.begin(FIRMWARE_BINARY_URL);

  int code = http.GET();

  if (code != 200) {
    http.end();
    return false;
  }

  int len = http.getSize();

  if (len <= 0) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();

  if (!Update.begin(len)) {
    http.end();
    return false;
  }

  size_t written = Update.writeStream(*stream);

  if (written != (size_t)len) {
    http.end();
    return false;
  }

  if (!Update.end() || !Update.isFinished()) {
    http.end();
    return false;
  }

  http.end();
  return true;
}

void loop() {
  unsigned long now = millis();

  maintainWiFiConnection();

  updateStatusLeds();
  updateLocalTime();
  processSerialCommand();
  processTcpClient();  // Ny TCP-kommunikation

  if (UseIRGate) {
    handleIrPassage();
  }

  struct tm* ti = localtime(&currentTime);
  if (ti && ti->tm_hour == 3 && ti->tm_mday != lastOtaCheckDay) {
    Serial.println(F("03:00 – OTA-kontroll"));
    checkForOtaUpdate();
    lastOtaCheckDay = ti->tm_mday;
  }

  // ----------------------------------------------------------
  // Relä-timers
  // ----------------------------------------------------------
  // I IR-läge får inte en pågående passage stänga av lampan.
  if (UseIRGate) {
    if (greenRelayActive && gateState != GATE_GREEN_PASSING && gateState != GATE_GREEN_AUTHORIZED && now - greenRelayOnTime >= relayOnDuration) {
      relayOff(PIN_RELAY_GREEN, "Grönt");
    }

    if (redRelayActive && gateState != GATE_RED_ACTIVE && gateState != GATE_RED_PASSING && now - redRelayOnTime >= relayOnDuration) {
      relayOff(PIN_RELAY_RED, "Rött");
    }

    // Rött vid ogiltigt kort / obehörig passage styrs av redHoldUntil.
    if (redRelayActive && (gateState == GATE_RED_ACTIVE || gateState == GATE_RED_PASSING) && !irBlocked && redHoldUntil != 0 && now >= redHoldUntil) {
      relayOff(PIN_RELAY_RED, "Rött");
      setGateState(GATE_IDLE, "IR avstängd / reset");
      gateStateSince = now;
      redHoldUntil = 0;
    }
  } else {
    // Originalbeteendet när IR inte används.
    if (redRelayActive && now - redRelayOnTime >= relayOnDuration) {
      relayOff(PIN_RELAY_RED, "Rött");
    }
    if (greenRelayActive && now - greenRelayOnTime >= relayOnDuration) {
      relayOff(PIN_RELAY_GREEN, "Grönt");
    }
  }

  // Summern följer den röda lampans relätillstånd.
  // Den är aktiv under hela röd sekvens, inklusive passage och eftertid.
  digitalWrite(PIN_BUZZER, redRelayActive ? HIGH : LOW);

  if (cardWriteMode) return;

  // ----------------------------------------------------------
  // RFID
  // ----------------------------------------------------------
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uid = getCardUid();
    Serial.printf("\nKort detekterat: %s\n", uid.c_str());

    // Under en pågående passage ska ett nytt kort inte kunna växla
    // lampfärgen mitt under personen som redan passerar.
    if (UseIRGate && (gateState == GATE_GREEN_PASSING || gateState == GATE_RED_PASSING)) {
      Serial.println(F("Passage pågår → nytt RFID ignoreras till passagen är klar."));
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      return;
    }

    time_t from = 0, to = 0;
    long orderId = 0, siteId = 0;

    if (readCardData(from, to, orderId, siteId, true)) {
      char todayStr[20];
      strftime(todayStr, sizeof(todayStr), "%Y-%m-%d %H:%M", localtime(&currentTime));

      const bool locallyValid =
        (from <= currentTime && to >= currentTime && siteId == portSiteId);

      if (locallyValid) {
        Serial.printf("Kort giltigt lokalt → GODKÄNT! (idag: %s, SiteID: %ld)\n",
                      todayStr, portSiteId);

        if (UseIRGate) {
          // Krav: grönt tänds omedelbart, IR kan brytas senare.
          authorizePassage();
        } else {
          // Gamla beteendet utan IR.
          relayOn(PIN_RELAY_GREEN, "Grönt", greenRelayOnTime, greenRelayActive);
        }
      } else {
        Serial.printf("Kort ogiltigt lokalt → frågar servern...\n");
        Serial.printf("  → Dagens datum: %s\n", todayStr);
        Serial.printf("  → PortSiteID på grinden: %ld\n", portSiteId);
        Serial.printf("  → Kort giltigt: %s → %s | SiteID: %ld\n",
                      formatTimeDebug(from).c_str(),
                      formatTimeDebug(to).c_str(),
                      siteId);

        if (from > currentTime) Serial.println("  → FEL: Kortet börjar gälla i framtiden!");
        if (to < currentTime) Serial.println("  → FEL: Kortet har gått ut!");
        if (siteId != portSiteId) {
          Serial.printf("  → FEL: SiteID stämmer inte (%ld vs %ld)\n", siteId, portSiteId);
        }

        const bool serverAccepted = queryServerAndOpenIfValid(uid, true, UseIRGate);

        if (UseIRGate) {
          if (!serverAccepted) {
            // Krav: ogiltigt kort → rött omedelbart.
            startRedForInvalidCard();
          }
        } else if (!serverAccepted) {
          // Gamla beteendet utan IR.
          relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
        }
      }
    } else {
      Serial.println(F("Kunde inte läsa kortdata → RFID ej godkänt"));

      cancelPassageAuthorization("RFID kunde inte verifieras");

      if (UseIRGate) {
        startRedForInvalidCard();
      } else {
        relayOn(PIN_RELAY_RED, "Rött", redRelayOnTime, redRelayActive);
      }
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
}
