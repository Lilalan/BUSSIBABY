#include <Arduino.h>
#include <ArduinoJson.h>

#include <SPI.h>

#include <WiFi.h>
#include <HTTPClient.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>

#include "credentials.h"

#include <driver/rtc_io.h>
#include <esp_sleep.h>

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(26, 25, 33, 27)); // CS, DC, RST, Busy // GDEH0154D67

WiFiClientSecure client; // or WiFiClientSecure for HTTPS
HTTPClient http;

// WLAN-Daten, die den Deep Sleep überleben (RTC-RAM).
// Mit Kanal und BSSID entfällt der WLAN-Scan beim Aufwachen.
RTC_DATA_ATTR uint8_t wifiBssid[6];
RTC_DATA_ATTR int32_t wifiChannel = 0;
RTC_DATA_ATTR bool wifiCacheValid = false;

unsigned long seconds = 0;
String InitialInfo = "Initial :)";

const char *root_ca = CERTIFICATE;
const char *pb_website = PUBLICTRANSPORT_WEBSITE;

const int BUTTON_PIN = 32;

// 3 Minuten in Millisekunden (3 * 60 * 1000) als "Arbeitszeit"
const unsigned long WAKE_DURATION = 180000;
// Alle 30 Sekunden die Kommandos bearbeiten
const unsigned long WORK_INTERVAL = 30000;
// Maximale Wartezeit auf die WLAN-Verbindung
const unsigned long WIFI_TIMEOUT = 10000;
unsigned long wakeStartTime = 0;
unsigned long lastWorkTime = 0;

void updateTime(unsigned long seconds);
void infoToScreen(String InitialInfo);
void goToSleep();
void holeBusDaten();
void zeigeDatenVeraltet();
void wifiStarten();
bool wifiWarten();

void setup()
{
  Serial.begin(115200);

  // Nur ein Aufwachen durch den Taster (Pin 32) startet die Arbeitszeit.
  // Bei Kaltstart oder Reset direkt wieder in den Deep Sleep.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0)
  {
    Serial.println("\nKein Tastendruck erkannt - zurueck in den Deep Sleep.");
    goToSleep();
  }

  // Den Startzeitpunkt des Wachseins festhalten
  Serial.println("\n--- ESP32 AUFGEWECKT (Taster) ---");
  wakeStartTime = millis();
  // Erste Bearbeitung sofort beim Aufwachen
  lastWorkTime = wakeStartTime - WORK_INTERVAL;

  // WLAN verbindet sich im Hintergrund, während das Display initialisiert wird
  wifiStarten();
  client.setCACert(root_ca);

  // Display Connection
  display.init(115200, true, 2, false); // USE THIS for Waveshare boards with "clever" reset circuit, 2ms reset pulse

  wifiWarten();

  Serial.println("Die 3-Minuten-Arbeitszeit laeuft jetzt.");
}

void loop()
{
  // Aktuelle Zeit abfragen
  unsigned long currentMillis = millis();

  // Prüfen, ob die 3 Minuten abgelaufen sind -> zurück in den Deep Sleep
  if (currentMillis - wakeStartTime >= WAKE_DURATION) {
    zeigeDatenVeraltet(); // Letzte Anzeige ist danach nicht mehr aktuell
    goToSleep();
  }

  // Alle 30 Sekunden die Kommandos bearbeiten
  if (currentMillis - lastWorkTime >= WORK_INTERVAL) {
    lastWorkTime = currentMillis;
    holeBusDaten();
  }

  // Alle 10 Sekunden ein Lebenszeichen
  static unsigned long lastMessage = 0;
  if (currentMillis - lastMessage >= 10000) {
    lastMessage = currentMillis;
    long verbleibendeSekunden = (WAKE_DURATION - (currentMillis - wakeStartTime)) / 1000;
    Serial.print("ESP32 ist aktiv. Verbleibende Zeit: ");
    Serial.print(verbleibendeSekunden);
    Serial.println(" Sekunden.");
  }

  delay(100);
}

// Startet die WLAN-Verbindung, ohne auf sie zu warten.
// Mit gespeichertem Kanal und BSSID wird nicht gescannt.
void wifiStarten()
{
  WiFi.mode(WIFI_STA);
  if (wifiCacheValid) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, wifiChannel, wifiBssid, true);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// Wartet auf die WLAN-Verbindung und merkt sich Kanal und BSSID.
// Schlägt die Verbindung fehl, wird beim nächsten Versuch wieder gescannt.
bool wifiWarten()
{
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiChannel = WiFi.channel();
    memcpy(wifiBssid, WiFi.BSSID(), 6);
    wifiCacheValid = true;
    return true;
  }

  Serial.println("[WiFi] Verbindung fehlgeschlagen.");
  wifiCacheValid = false;
  return false;
}

void holeBusDaten()
{
  // Bei Verbindungsabbruch neu verbinden
  if (WiFi.status() != WL_CONNECTED) {
    wifiStarten();
    wifiWarten();
  }

  if (WiFi.status() == WL_CONNECTED)
  {

    HTTPClient http;

    Serial.print("[HTTP] begin...\n");
    http.useHTTP10(true);
    http.begin(client, pb_website); // HTTPS

    Serial.print("[HTTP] GET...\n");
    // start connection and send HTTP header
    int httpCode = http.GET();

    // httpCode will be negative on error
    if (httpCode > 0)
    {
      // HTTP header has been send and Server response header has been handled
      Serial.printf("[HTTP] GET... code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK)
      {
        // Parse response
        JsonDocument doc;
        deserializeJson(doc, http.getStream());

        // serializeJsonPretty(doc, Serial);
        bool success = doc["success"];
        String BusInfo;

        for (int i = 0; i < 4; i++)
        {
          const char *Fahrten_Fahrt_LinienText = doc["Fahrten"]["Fahrt"][i]["LinienText"]; // "K" "A"
          //String Linientext = String(Fahrten_Fahrt_LinienText);
          const char *Fahrten_Fahrt_RichtungsText = doc["Fahrten"]["Fahrt"][i]["RichtungsText"]; //"Kranebitten" "Wiesengasse" "Rum"
          int Fahrten_Fahrt_AbfahrtInMinuten = doc["Fahrten"]["Fahrt"][i]["AbfahrttInMinuten"]; //"Abfahrt in min"
          //Serial.println(String(Fahrten_Fahrt_LinienText) + ", " + String(Fahrten_Fahrt_RichtungsText) + ", " + String(Fahrten_Fahrt_AbfahrtInMinuten));

          BusInfo = BusInfo + "\n" + String(Fahrten_Fahrt_LinienText) + " " + String(Fahrten_Fahrt_RichtungsText)[0] + String(Fahrten_Fahrt_RichtungsText)[1] + String(Fahrten_Fahrt_RichtungsText)[2] + String(Fahrten_Fahrt_RichtungsText)[3] + " " + String(Fahrten_Fahrt_AbfahrtInMinuten);
          Serial.println(BusInfo);
          //Serial.println(" Status Pin 4 : " + buttonState);
        }
        infoToScreen(BusInfo);
        BusInfo = "";
      }
    }
    else
    {
      Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}

// Zeigt vor dem Deep Sleep einen Hinweis statt der veralteten Busdaten.
// E-Paper behält das Bild auch ohne Strom.
void zeigeDatenVeraltet()
{
  // Die Schrift hat nur ASCII-Zeichen, deshalb "ue" statt "ü"
  const char *zeilen[] = {"Zum Starten", "Taster", "druecken.", "Bussi, Baby!"};
  const int ANZAHL_ZEILEN = 4;
  display.setRotation(1);
  display.setFont(&FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < ANZAHL_ZEILEN; i++)
    {
      int16_t tbx, tby; uint16_t tbw, tbh;
      display.getTextBounds(zeilen[i], 0, 0, &tbx, &tby, &tbw, &tbh);
      uint16_t x = ((display.width() - tbw) / 2) - tbx;
      display.setCursor(x, 40 + i * 36);
      display.print(zeilen[i]);
    }
  }
  while (display.nextPage());
}

void infoToScreen(String RawBusInfo)
{
  const char * BusInfo = RawBusInfo.c_str();
  display.setRotation(1);
  display.setFont(&FreeMonoBold18pt7b);
  display.setTextColor(GxEPD_BLACK);
  int16_t tbx, tby; uint16_t tbw, tbh;
  display.getTextBounds(BusInfo, 0, 0, &tbx, &tby, &tbw, &tbh);
  // center the bounding box by transposition of the origin:
  uint16_t x = ((display.width() - tbw) / 2) - tbx;
  uint16_t y = ((display.height() - tbh) / 2) - tby;
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(x, y);
    display.print(BusInfo);
  }
  while (display.nextPage());
}

void goToSleep() {
  Serial.println("\n3 Minuten sind um. Bereite Deep Sleep vor...");

  // Warten, falls der Taster im Moment des Einschlafens noch gedrückt ist
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(20);
  }
  delay(100); // Entprellen

  // Aufwach-Trigger einrichten (Wacht auf bei LOW / GND) - der einzige Wecker
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);

  // Internen Pull-Up für die Schlafphase aktivieren (wichtig für GPIO 33)
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);

  Serial.println("Gehe jetzt in den Deep Sleep... Zzz...");
  delay(100); // Dem Seriellen Monitor Zeit zum Senden geben
  esp_deep_sleep_start();
}
