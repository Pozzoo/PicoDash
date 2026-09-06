#include <LiquidCrystal_I2C.h>

#include <Arduino_JSON.h>

#include <ArduinoWiFiServer.h>
#include <BearSSLHelpers.h>
#include <CertStoreBearSSL.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiAP.h>
#include <ESP8266WiFiGeneric.h>
#include <ESP8266WiFiGratuitous.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WiFiSTA.h>
#include <ESP8266WiFiScan.h>
#include <ESP8266WiFiType.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiServer.h>
#include <WiFiServerSecure.h>
#include <WiFiServerSecureBearSSL.h>
#include <WiFiUdp.h>

#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

#include <cmath>
#include <type_traits>

#ifndef STASSID
#define STASSID "yourWiFiSSID"
#define STAPSK "yourWiFiPassword"
#define PIPORT 9991
#endif

const char* ssid = STASSID;
const char* password = STAPSK;

const int piPort = PIPORT;

const int FAN_PIN = D6;
const int BUTTON_PIN = D7;

int lastFanPercent = 0;

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 2000;
const unsigned long LONG_PRESS_MS = 800;


volatile unsigned long btnDownMs = 0;
volatile unsigned long btnUpMs = 0;
volatile bool btnEvent = false;
volatile bool btnPressed = false;
bool longPressFired = false;

bool isBacklightEnabled = true;

void IRAM_ATTR buttonISR();
int screenMode = 1;
int nOfScreenModes = 7;

LiquidCrystal_I2C lcd(0x27,16,2);
ESP8266WiFiMulti WiFiMulti;
ESP8266WebServer server(80);

struct PiMetrics {
  const char* name;
  const char designator;
  float temp;
  float cpu;
  float ramPercent;
  unsigned int ramTotal;
  unsigned int ramUsed;
  float diskPercent;
  unsigned int diskTotal;
  unsigned int diskUsed;
  unsigned int containersRunning;
  unsigned int containersStopped;
  bool online;
  const char* host;
};

PiMetrics pis[2] = {
  { "Raspberry Pi", 'R', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, "192.168.0.2" },
  { "Orange Pi", 'O', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, "192.168.0.3" }
};

void handleMetrics() {
  JSONVar fullMetrics;
  JSONVar arr;

  for (int i = 0; i < 2; i++) {
    JSONVar entry;
    entry["name"] = pis[i].name;
    entry["designator"] = pis[i].designator;
    entry["host"] = pis[i].host;
    entry["temp"] = pis[i].temp;
    entry["cpu"] = pis[i].cpu;
    entry["ram_percent"] = pis[i].ramPercent;
    entry["ram_total"] = pis[i].ramTotal;
    entry["ram_used"] = pis[i].ramUsed;
    entry["disk_percent"] = pis[i].diskPercent;
    entry["disk_total"] = pis[i].diskTotal;
    entry["disk_used"] = pis[i].diskUsed;
    entry["containers_running"] = pis[i].containersRunning;
    entry["containers_stopped"] = pis[i].containersStopped;

    entry["online"] = pis[i].online;

    arr[i] = entry;
  }

  fullMetrics["pis"] = arr;
  server.send(200, "application/json", JSON.stringify(fullMetrics));
}

void handleNotFound() {
  String message = "Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }

  server.send(404, "text/plain", message);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing...");

  pinMode(FAN_PIN, OUTPUT);
  analogWriteFreq(25000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0,0);
  lcd.print("Booting...");

  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connecting to");
  lcd.setCursor(0,1);
  lcd.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    Serial.println("WiFi failed, retrying.");

    lcd.clear();
    lcd.print("WiFi failed");
    lcd.setCursor(0,1);
    lcd.print("retrying.");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("WiFi connected");
  lcd.setCursor(0,1);
  lcd.print(WiFi.localIP());

  server.on("/api/metrics", handleMetrics);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  delay(2000);
  lcd.clear();
}

void IRAM_ATTR buttonISR() {
  btnPressed = (digitalRead(BUTTON_PIN) == LOW);
  if (btnPressed) {
    btnDownMs = millis();
  } else {
    btnUpMs = millis();
  }
  btnEvent = true;
}

template<typename T>
void safeAssign(JSONVar json, const char* key, T& target) {
  if (json.hasOwnProperty(key)) {
    target = (T)(double)json[key];
  } else {
    target = (T)0;
  }
}

void pollPi(PiMetrics &pi) {
  WiFiClient client;

  HTTPClient http;

  Serial.println("[HTTP] begin...");
  String url = "http://" + String(pi.host) + ":" + String(piPort) + "/";

  if (http.begin(client, url)) {
    http.setTimeout(2000);

    Serial.println("[HTTP] GET...\n");
    // start connection and send HTTP header
    int httpCode = http.GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      String payload = http.getString();
      Serial.println(payload);

      JSONVar response = JSON.parse(payload);

      if (JSON.typeof(response) == "undefined") {
        Serial.println("Parsing input failed!");
        return;
      }

      pi.online = true;

      safeAssign(response, "cpu", pi.cpu);
      safeAssign(response, "ram_percent", pi.ramPercent);
      safeAssign(response, "ram_used", pi.ramUsed);
      safeAssign(response, "ram_total", pi.ramTotal);
      safeAssign(response, "disk_percent", pi.diskPercent);
      safeAssign(response, "disk_used", pi.diskUsed);
      safeAssign(response, "disk_total", pi.diskTotal);
      safeAssign(response, "containers_running", pi.containersRunning);
      safeAssign(response, "containers_stopped", pi.containersStopped);
      safeAssign(response, "temp", pi.temp);

    } else {
      Serial.printf("[HTTP] GET failed for host: %s\n", pi.host);
      Serial.printf("[HTTP] HTTP code: %d\n", httpCode);
      Serial.printf("[HTTP] full URL: %s\n", url.c_str());
      Serial.println(" ");
      pi.online = false;
    }

    http.end();
  } else {
    Serial.println("[HTTP] Unable to connect");
  }
}

int intLength(int n) {
    if (n == 0) return 1;
    int len = 0;
    n = abs(n);
    while (n > 0) {
        n /= 10;
        len++;
    }
    return len;
}

char* numberSanitizer(float num) {
  static char buffer[3];
  buffer[2] = '\0';

  if (num >= 100) {
    buffer[0] = '9';
    buffer[1] = '9';
    return buffer;
  }

  buffer[0] = (int)((int)num / 10) + '0';
  buffer[1] = ((int)num % 10) + '0';
  return buffer;
}

char* storageUnitSanitizer(unsigned int num) {
  static char buffer[10]; // max "4294.9TB\0"
  const char units[] = "KMGT";

  unsigned long scaled = num;
  unsigned long remainder = 0;
  uint8_t unitIndex = 0;

  while (scaled >= 1000 && unitIndex < 3) {
    remainder = scaled % 1000;
    scaled /= 1000;
    unitIndex++;
  }

  uint8_t frac = (remainder * 10 + 500) / 1000;
  if (frac == 10) {
    scaled++;
    frac = 0;
  }

  char* ptr = buffer;
  ptr += sprintf(ptr, "%lu", scaled);

  if (unitIndex > 0 && frac > 0) {
    ptr += sprintf(ptr, ".%u", frac);
  }

  *ptr++ = units[unitIndex];
  *ptr++ = 'B';
  *ptr = '\0';

  return buffer;
}

char* percentageSanitizer(float num) {
  static char buffer[5];
  buffer[4] = '\0';
  buffer[2] = '.';

  if (num >= 100) {
    buffer[0] = '9';
    buffer[1] = '9';
    buffer[3] = '9';
    return buffer;
  }

  buffer[0] = (int)((int)num / 10) + '0';
  buffer[1] = ((int)num % 10) + '0';
  buffer[3] = (((int)(num * 10) % 10) + '0');
  return buffer;
}

void writeScreenMode1(PiMetrics &pi, int &i) {
  if (!pi.online) {
    lcd.setCursor(0,i);
    lcd.print(pi.designator);
    lcd.print(" OFFLINE       ");

    return;
  }

  lcd.setCursor(4,i);
  lcd.print("");

  lcd.setCursor(5,i);
  lcd.print("");

  lcd.setCursor(8,i);
  lcd.print("");

  lcd.setCursor(9,i);
  lcd.print("");

  lcd.setCursor(13,i);
  lcd.print("");

  lcd.setCursor(14,i);
  lcd.print("");

  lcd.setCursor(15,i);
  lcd.print("");

  lcd.setCursor(0,i);
  lcd.print(pi.designator);
  lcd.print(" C:");
  lcd.print(numberSanitizer(pi.cpu));
  lcd.print(" R:");
  lcd.print(numberSanitizer(pi.ramPercent));
  lcd.print(" T:");
  lcd.print((int) pi.temp);
}

void writeScreenMode2(PiMetrics &pi, int &i) {
  if (!pi.online) {
    lcd.setCursor(0,i);
    lcd.print(pi.designator);
    lcd.print(" OFFLINE       ");

    return;
  }

  lcd.setCursor(7,i);
  lcd.print("     ");

  lcd.setCursor(0,i);
  lcd.print(pi.designator);
  lcd.print(" TEMP: ");
  lcd.print(pi.temp);
  lcd.write(0xDF);
  lcd.print("C");
}

void writeScreenMode3(PiMetrics &pi) {
  if (!pi.online) {
    lcd.setCursor(0,0);
    lcd.print(pi.designator);
    lcd.print(" OFFLINE       ");

    return;
  }

  lcd.setCursor(0,0);
  lcd.print(pi.designator);
  lcd.print(" RAM: ");
  lcd.print(percentageSanitizer(pi.ramPercent));
  lcd.print("%");

  lcd.setCursor(1,1);
  lcd.print(storageUnitSanitizer(pi.ramUsed));
  lcd.print("/");
  lcd.print(storageUnitSanitizer(pi.ramTotal));
}

void writeScreenMode4(PiMetrics &pi) {
  if (!pi.online) {
    lcd.setCursor(0,0);
    lcd.print(pi.designator);
    lcd.print(" OFFLINE       ");

    return;
  }

  lcd.setCursor(0,0);
  lcd.print(pi.designator);
  lcd.print(" STORAGE: ");
  lcd.print(percentageSanitizer(pi.diskPercent));
  lcd.print("%");

  lcd.setCursor(1,1);
  lcd.print(storageUnitSanitizer(pi.diskUsed));
  Serial.println(storageUnitSanitizer(pi.diskUsed));
  lcd.print("/");
  lcd.print(storageUnitSanitizer(pi.diskTotal));
}

void writeScreenMode5(PiMetrics &pi, int &i) {
  if (!pi.online) {
    lcd.setCursor(0,i);
    lcd.print(pi.designator);
    lcd.print(" OFFLINE       ");

    return;
  }

  lcd.setCursor(9,i);
  lcd.print("  ");

  lcd.setCursor(14,i);
  lcd.print("  ");

  lcd.setCursor(0,i);
  lcd.print(pi.designator);
  lcd.print(" DKR: U:");
  lcd.print(numberSanitizer(pi.containersRunning));
  lcd.print(" D:");
  lcd.print(numberSanitizer(pi.containersStopped));
}

void screenModeSelector() {
  switch(screenMode) {
    case 2:
      for (int i = 0; i < 2; i++) {
        writeScreenMode2(pis[i], i);
      }
      break;

    case 3:
      writeScreenMode3(pis[0]);
      break;

    case 4:
      writeScreenMode3(pis[1]);
      break;

    case 5:
      writeScreenMode4(pis[0]);
      break;

    case 6:
      writeScreenMode4(pis[1]);
      break;

    case 7:
      for (int i = 0; i < 2; i++) {
        writeScreenMode5(pis[i], i);
      }
      break;

    default:
      for (int i = 0; i < 2; i++) {
        writeScreenMode1(pis[i], i);
      }
      break;
  }
}

int tempToDuty(float temp) {
  const float T_MIN = 40.0;
  const float T_MAX = 85.0;
  const int DUTY_MIN = 10;
  const int DUTY_MAX = 99;

  if (temp <= T_MIN) return DUTY_MIN;
  if (temp >= T_MAX) return DUTY_MAX;

  float ratio = (temp - T_MIN) / (T_MAX - T_MIN);
  float eased = ratio * ratio;
  return DUTY_MIN + eased * (DUTY_MAX - DUTY_MIN);
}

void onSinglePress() {
  if (screenMode <= nOfScreenModes) {
    screenMode++;
  } else {
    screenMode = 1;
  }

  lcd.clear();
}

void onLongPress() {
  if (isBacklightEnabled) {
    lcd.noBacklight();
    isBacklightEnabled = false;
  } else {
    lcd.backlight();
    isBacklightEnabled = true;
  }
}

void checkButton() {
  noInterrupts();
  bool pressed = btnPressed;
  unsigned long downMs = btnDownMs;
  unsigned long upMs = btnUpMs;
  bool event = btnEvent;
  btnEvent = false;
  interrupts();

  if (!event) return;

  if (pressed) {
    longPressFired = false;
  } else if (downMs > 0) {
    unsigned long duration = upMs - downMs;
    if (duration >= LONG_PRESS_MS) {
      if (!longPressFired) {
        longPressFired = true;
        onLongPress();
      }
    } else {
      onSinglePress();
    }
  }
}

void loop() {
  if (WiFi.isConnected()) {
    unsigned long now = millis();
    if (now - lastPoll >= POLL_INTERVAL) {
      for (int i = 0; i < 2; i++) {
        pollPi(pis[i]);
      }

      screenModeSelector();

      bool atLeastOneOnline = (pis[0].online || pis[1].online);

      int maxTemp = max(pis[0].temp, pis[1].temp);

      int targetPercent = tempToDuty(atLeastOneOnline? maxTemp : 50);
      if (abs(targetPercent - lastFanPercent) >= 5) {
        analogWrite(FAN_PIN, map(targetPercent, 0, 100, 0, 1023));
        lastFanPercent = targetPercent;
      }
    }

    server.handleClient();
    checkButton();
  }
}
