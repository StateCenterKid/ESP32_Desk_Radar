#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "time.h"
#include "config.h"

// Internal Temperature Sensor
#ifdef __cplusplus
extern "C" {
#endif
  uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

TFT_eSPI tft = TFT_eSPI();
//sd card chip select
#define SD_CS 5

// --- CONFIGURATION ---
const int16_t MAX_RANGE = 8000;
const int16_t MIN_RANGE = 100;
const int16_t MIN_QUAL = 10;
const int16_t EMPTY = 9999;
const bool PLOT_SERIAL = true;
const bool DEBUG_SERIAL = true;


struct Person {
  int16_t xHistory[10];
  int16_t yHistory[10];
};
Person targets[3];
uint16_t targetColors[3] = { TFT_CYAN, TFT_MAGENTA, TFT_YELLOW };

float stockPrices[NUM_STOCKS], stockChanges[NUM_STOCKS], stockPercents[NUM_STOCKS];
float currentTemp = 0;
int currentHumid = 0;
String weatherCond = "----wait----";
int currentStockIdx = 0;

// Signal Smoothing
float smoothedSignal = 0;
int currentSignal = 0;
int lastDisplayedSQ = -1;

// Timing
unsigned long lastStockRotate = 0;
unsigned long lastSDWriteTime = 0;  // Tracks the last successful SD save

bool wifiConnectedCached = false;
unsigned long lastConnectionCheck = 0;
unsigned long lastCylonUpdate = 0;
int cylonPos = 160;
int cylonDir = 4;
bool timeIsSet = false;
bool wasConnected = false;
bool timeHealthy = false;
bool owmHealthy = false;
bool finnhubHealthy = false;

// THE MASTER TIMERS
unsigned long lastFetchAttempt = 0;
unsigned long lastHistoryUpdate = 0;

// --- NETWORK FAILOVER ---
bool useSecondaryWiFi = false;
unsigned long currentNetworkAttemptStart = 0;  // Tracks time since connection loss
const int16_t MAX_TIMEOUT = 30000;             // WiFi Timeout before refresh

// --- MARKET SETTINGS ---
const int TRADING_MINUTES = 390;
// Local time the market opens, in minutes since midnight (8:30 AM Central)
const int MARKET_OPEN_MINS = 510;
const int SPARK_POINTS = (TRADING_MINUTES * 60000) / fetchInterval;
float history[NUM_STOCKS][SPARK_POINTS];


// --- SETUP ---
void setup() {
  if (DEBUG_SERIAL) Serial.begin(115200);

  // 1. RADAR FIX: Set to 256000 baud and add a 10ms anti-freeze timeout
  Serial1.begin(256000, SERIAL_8N1, 25, 32);
  Serial1.setTimeout(10);

  // 2. BUTTON & SPI FIX: Pin 0 is our button, Pin 5 is our SD Card
  pinMode(0, INPUT_PULLUP);
  pinMode(5, OUTPUT);

  // FORCE SD CARD TO MUTE BEFORE SCREEN INIT
  digitalWrite(5, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 10; j++) targets[i].xHistory[j] = EMPTY;
  }

  drawRadarBackground();
  loadHistoryFromSD();

  //test line - comment out if running live
  //generateTestData();

  startWiFi();
}


// --- MAIN LOOP ---
void loop() {
  unsigned long currentMillis = millis();

  // --- 1. THE STRICT HISTORY LOGGING ---
  // Fires perfectly on time, completely independent of Wi-Fi status
  if (currentMillis - lastHistoryUpdate >= fetchInterval) {
    lastHistoryUpdate = currentMillis;
    updateSparklineHistory();
  }

  // --- 2. THE FLEXIBLE WI-FI FETCHING ---
  bool timeForNormalUpdate = (currentMillis - lastFetchAttempt >= fetchInterval);
  bool timeForRetry = ((WiFi.status() != WL_CONNECTED) && (currentMillis - lastFetchAttempt >= 60000));

  // Wait to fetch until after boot (when lastFetchAttempt is 0 and wifi connects)
  if (timeForNormalUpdate || timeForRetry || (lastFetchAttempt == 0 && WiFi.status() == WL_CONNECTED)) {
    lastFetchAttempt = currentMillis;
    fetchAllData();
    drawInfoPanel();
    drawWorldDashboard();
  }

  // 3. RADAR & CLOCK UPDATES
  while (Serial1.available() >= 22) {
    if (Serial1.read() == 0xAA && Serial1.read() == 0xFF && Serial1.read() == 0x03 && Serial1.read() == 0x00) {
      uint8_t payload[18];
      Serial1.readBytes(payload, 18);
      processRadarData(payload);
    }
  }
  updateClock();

  // 4. AUTO-FALLBACK WIFI WATCHDOG & TIME SYNC
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - currentNetworkAttemptStart > MAX_TIMEOUT) {
      if (DEBUG_SERIAL) Serial.println("Connection timeout. Switching networks...");
      useSecondaryWiFi = !useSecondaryWiFi;
      timeIsSet = false;
      lastFetchAttempt = 0;  // Force immediate fetch upon connection
      startWiFi();
    }
  } else {
    currentNetworkAttemptStart = millis();
    if (!timeIsSet) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      timeIsSet = true;
      if (DEBUG_SERIAL) Serial.println("Time successfully synchronized!");
    }
  }

  // 5. STOCK ROTATION
  if (millis() - lastStockRotate > rotateInterval) {
    currentStockIdx = (currentStockIdx + 1) % NUM_STOCKS;
    drawInfoPanel();
    drawSystemHealth();
    lastStockRotate = millis();
  }
}

// --- SD CARD FUNCTIONS ---
void loadHistoryFromSD() {
  if (!SD.begin(SD_CS)) {
    if (DEBUG_SERIAL) Serial.println("SD Mount Failed or Card Missing");
    return;
  }

  if (SD.exists("/history.bin")) {
    File file = SD.open("/history.bin", FILE_READ);
    if (file) {
      if (file.size() == sizeof(history)) {
        file.read((uint8_t*)history, sizeof(history));
        if (DEBUG_SERIAL) Serial.println("Stock history loaded safely!");
      } else {
        if (DEBUG_SERIAL) Serial.println("Settings changed! Deleting obsolete history file.");
        file.close();
        SD.remove("/history.bin");
        return;
      }
      file.close();
    }
  } else {
    if (DEBUG_SERIAL) Serial.println("No history file found (First boot).");
  }
}

void saveHistoryToSD() {
  File file = SD.open("/history.bin", FILE_WRITE);
  if (file) {
    file.write((const uint8_t*)history, sizeof(history));
    file.close();
    lastSDWriteTime = millis();
    if (DEBUG_SERIAL) Serial.println("History saved to SD.");
  } else {
    if (DEBUG_SERIAL) Serial.println("Failed to write to SD.");
  }
}

// --- NEW STRICT HISTORY UPDATER ---
void updateSparklineHistory() {
  // 1. Only do this if the market is actually open
  if (!isMarketOpen()) return;

  // 2. Shift the history array over by one slot for ALL stocks
  for (int i = 0; i < NUM_STOCKS; i++) {
    for (int j = 0; j < SPARK_POINTS - 1; j++) {
      history[i][j] = history[i][j + 1];
    }
    // 3. Drop the most recently known price into the final slot.
    // If Wi-Fi works, this is a new price. If Wi-Fi is down, it drops in the old price.
    history[i][SPARK_POINTS - 1] = stockPrices[i];
  }

  // 4. Save the newly shifted bucket to the SD card
  saveHistoryToSD();

  // 5. Force the SD card to hang up the SPI bus to protect the TFT screen
  digitalWrite(5, HIGH);
}


// Helper function to split text into two lines without cutting words in half
void splitWeatherText(String fullText, String& line1, String& line2, int maxChars) {
  line1 = "";
  line2 = "";

  if (fullText.length() <= maxChars) {
    line1 = fullText;
    return;
  }

  int splitIndex = -1;
  for (int i = 0; i <= maxChars; i++) {
    if (fullText.charAt(i) == ' ') splitIndex = i;
  }

  if (splitIndex > 0) {
    line1 = fullText.substring(0, splitIndex);
    line2 = fullText.substring(splitIndex + 1);
  } else {
    line1 = fullText.substring(0, maxChars);
    line2 = fullText.substring(maxChars);
  }

  if (line2.length() > maxChars) {
    line2 = line2.substring(0, maxChars);
  }
}


// --- RADAR FUNCTIONS ---
void processRadarData(uint8_t* data) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 10; j++) {
      if (targets[i].xHistory[j] != EMPTY) {
        int ex = map(targets[i].xHistory[j], -MAX_RANGE / 2, MAX_RANGE / 2, 160, 479);
        int ey = map(targets[i].yHistory[j], 0, MAX_RANGE, 0, 160);
        tft.fillCircle(ex, ey, 6 - (j / 2), TFT_BLACK);
      }
    }
  }

  drawRadarBackground();

  for (int i = 0; i < 3; i++) {
    int offset = i * 6;
    uint16_t x_u = (uint16_t)(data[offset] | (data[offset + 1] << 8));
    uint16_t y_u = (uint16_t)(data[offset + 2] | (data[offset + 3] << 8));
    uint8_t rawQual = data[offset + 4];
    int16_t rawX = (x_u & 0x8000) ? -(int16_t)(x_u & 0x7FFF) : (int16_t)x_u;
    int16_t rawY = (int16_t)(y_u & 0x7FFF);
    rawX = -rawX;

    if (i == 0) {
      smoothedSignal = (smoothedSignal * 0.9) + (rawQual * 0.1);
      currentSignal = (int)smoothedSignal;
      if (abs(currentSignal - lastDisplayedSQ) >= 5) {
        updateSQDisplay();
        lastDisplayedSQ = currentSignal;
      }
      drawTacticalHUD(rawX, rawY, (rawY > MIN_RANGE && rawY <= MAX_RANGE && rawQual >= MIN_QUAL));
    }

    bool hasTarget = (rawY > MIN_RANGE && rawY <= MAX_RANGE && rawQual >= MIN_QUAL);
    for (int j = 9; j > 0; j--) {
      targets[i].xHistory[j] = targets[i].xHistory[j - 1];
      targets[i].yHistory[j] = targets[i].yHistory[j - 1];
    }

    targets[i].xHistory[0] = hasTarget ? rawX : EMPTY;
    targets[i].yHistory[0] = hasTarget ? rawY : EMPTY;

    for (int j = 0; j < 10; j++) {
      if (targets[i].xHistory[j] != EMPTY) {
        int dx = map(targets[i].xHistory[j], -MAX_RANGE / 2, MAX_RANGE / 2, 160, 479);
        int dy = map(targets[i].yHistory[j], 0, MAX_RANGE, 0, 160);
        tft.fillCircle(dx, dy, 6 - (j / 2), targetColors[i]);
      }
    }
  }
}

void drawRadarBackground() {
  int minX = 159, maxX = 479, maxY = 160;
  uint16_t circleColor = 0x0560;
  uint16_t gridColor = 0x05E0;

  tft.drawCircle(319, 0, 80 - 1, circleColor);
  tft.drawCircle(319, 0, 160 - 1, circleColor);

  for (int x = minX; x <= maxX; x += 80) tft.drawLine(x, 0, x, maxY, gridColor);
  for (int y = 0; y <= maxY; y += 80) tft.drawLine(minX, y, maxX, y, gridColor);

  tft.fillCircle(319, 0, 5, 0x8000);
}


// --- DATA FETCHING ---
void startWiFi() {
  WiFi.disconnect();
  const char* activeSSID = useSecondaryWiFi ? ssid2 : ssid;
  const char* activePASS = useSecondaryWiFi ? password2 : password;
  WiFi.begin(activeSSID, activePASS);

  currentNetworkAttemptStart = millis();
  if (currentNetworkAttemptStart == 0) currentNetworkAttemptStart = 1;
}

bool isMarketOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  if (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6) return false;

  int currentMins = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
  int closeMins = MARKET_OPEN_MINS + TRADING_MINUTES;

  if (currentMins >= MARKET_OPEN_MINS && currentMins < closeMins) {
    return true;
  }
  return false;
}

void fetchAllData() {
  if (WiFi.status() == WL_CONNECTED) {
    fetchWeather();
    delay(1000);
    fetchWorldWeather();
    delay(500);
  }

  for (int i = 0; i < NUM_STOCKS; i++) {
    fetchStock(stockSymbols[i], i);
    delay(200);
  }
  // Notice SD saving is completely removed from here!
}

void fetchWorldWeather() {
  HTTPClient http;
  for (int i = 0; i < 3; i++) {
    String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(worldCities[i].lat) + "&lon=" + String(worldCities[i].lon) + "&units=imperial&appid=" + String(OWM_API_KEY);
    http.setTimeout(5000);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      owmHealthy = true;
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, http.getString());
      worldCities[i].temp = doc["main"]["temp"];
      worldCities[i].desc = doc["weather"][0]["description"].as<String>();
      if (worldCities[i].desc.length() > 0) {
        worldCities[i].desc.setCharAt(0, toupper(worldCities[i].desc.charAt(0)));
      }
    } else {
      owmHealthy = false;
      if (DEBUG_SERIAL) {
        Serial.print("Local Weather HTTP Error: ");
        Serial.println(httpCode);
      }
      http.end();
      delay(500);
    }
  }
  }

  void fetchWeather() {
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" + String(weatherCity) + "," + String(weatherState) + "," + String(weatherCountry) + "&units=imperial&appid=" + String(OWM_API_KEY);
    if (DEBUG_SERIAL) {
      Serial.print("Local Weather URL: ");
      Serial.println(url);
    }

    http.setTimeout(5000);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        currentTemp = doc["main"]["temp"];
        currentHumid = doc["main"]["humidity"];
        weatherCond = doc["weather"][0]["description"].as<String>();
        if (weatherCond.length() > 0) {
          weatherCond.setCharAt(0, toupper(weatherCond.charAt(0)));
        }
      } else {
        if (DEBUG_SERIAL) Serial.println("Local Weather JSON Parse Failed");
      }
    } else {
      if (DEBUG_SERIAL) {
        Serial.print("Local Weather HTTP Error: ");
        Serial.println(httpCode);
      }
    }
    http.end();
  }

  void fetchStock(String symbol, int idx) {
    // NO MORE ARRAY MATH IN HERE! Just fetch the data.
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      String url = "https://finnhub.io/api/v1/quote?symbol=" + symbol + "&token=" + String(finnhubapi);
      http.begin(url);
      http.setTimeout(8000);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        finnhubHealthy = true;
        DynamicJsonDocument doc(512);
        deserializeJson(doc, http.getString());

        stockPrices[idx] = doc["c"];
        stockChanges[idx] = doc["d"];
        stockPercents[idx] = doc["dp"];

      } else {
        finnhubHealthy = false;
        if (DEBUG_SERIAL) Serial.printf("Stock HTTP Error: %d\n", httpCode);
      }
      http.end();
    }
  }

  // --- UI DRAWING ---
  void drawWorldDashboard() {
    int startX = 160;
    int startY = 180;
    int endY = 300;
    int gridW = 319;
    int colWidth = 106;
    int paddingX = 10;

    uint16_t borderColor = TFT_ORANGE;
    tft.drawRoundRect(startX, startY, gridW, endY - startY, 8, borderColor);
    tft.drawLine(266, startY, 266, endY, borderColor);
    tft.drawLine(372, startY, 372, endY, borderColor);
    tft.drawFastHLine(startX, startY + 30, gridW, borderColor);

    time_t now;
    time(&now);
    struct tm* loc;
    loc = localtime(&now);

    for (int i = 0; i < 3; i++) {
      int xBase = 160 + (i * colWidth) + paddingX;
      time_t cityTime = now + worldCities[i].gmtOffset;

      if (worldCities[i].hasDST && loc->tm_isdst > 0) {
        cityTime += 3600;
      }

      struct tm* info = gmtime(&cityTime);
      char timeBuf[12];
      strftime(timeBuf, sizeof(timeBuf), "%I:%M%p", info);

      tft.setFreeFont(NULL);
      tft.setTextSize(2);

      tft.setTextColor(TFT_ORANGE, TFT_BLACK);
      tft.setCursor(xBase, startY + 8);
      tft.print(worldCities[i].displayName);

      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(xBase, startY + 42);
      tft.print(timeBuf);

      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(xBase - 6, startY + 68);
      tft.printf("%.0fF", worldCities[i].temp);

      float celsius = (worldCities[i].temp - 32) * 5.0 / 9.0;
      tft.setTextSize(2);
      tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
      tft.fillRect(xBase + 48, startY + 67, 46, 17, TFT_BLACK);
      tft.setCursor(xBase + 48, startY + 68);
      tft.printf("%.0fc", celsius);

      tft.setTextSize(1);
      tft.fillRect(xBase - 4, startY + 90, 100, 25, TFT_BLACK);
      tft.setTextColor(0xF7BE, TFT_BLACK);
      tft.setCursor(xBase - 4, startY + 90);
      String d = worldCities[i].desc;
      String wLine1, wLine2;
      splitWeatherText(d, wLine1, wLine2, 16);
      tft.print(wLine1);
      if (wLine2.length() > 0) {
        tft.setCursor(xBase - 4, startY + 90 + 10);
        tft.print(wLine2);
      }
    }
  }

  void drawInfoPanel() {
    tft.fillRect(0, 65, 155, 100, TFT_BLACK);
    tft.setTextSize(1);
    tft.setFreeFont(NULL);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setCursor(10, 78);
    tft.print(localDisplayName);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 98);
    tft.printf("%.0fF, H:%d%%", currentTemp, currentHumid);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);

    String wLine1, wLine2;
    splitWeatherText(weatherCond, wLine1, wLine2, 12);
    tft.setCursor(10, 123);
    tft.print(wLine1);
    if (wLine2.length() > 0) {
      tft.setCursor(10, 123 + 17);
      tft.print(wLine2);
    }

    int stockY = 175;
    tft.fillRect(0, stockY, 158, 132, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, stockY);
    tft.print(stockSymbols[currentStockIdx]);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, stockY + 20);
    tft.printf("$%.2f", stockPrices[currentStockIdx]);
    uint16_t trend = (stockChanges[currentStockIdx] >= 0) ? TFT_GREEN : TFT_RED;
    tft.setTextColor(trend, TFT_BLACK);
    tft.setCursor(10, stockY + 40);
    tft.printf("%+.2f%%", stockPercents[currentStockIdx]);

    drawSparkline(5, 295, 140, 60, trend);
  }

  void updateClock() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)){
      timeHealthy = false;
      return;
      }
    timeHealthy = true;
    static int lastMin = -1;
    if (timeinfo.tm_min != lastMin) {
      lastMin = timeinfo.tm_min;

      tft.fillRect(0, 0, 155, 65, TFT_BLACK);
      tft.setFreeFont(&FreeSansBold18pt7b);
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      char timeStr[12];
      strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo);
      tft.drawString(timeStr, 4, 10);

      tft.setFreeFont(&FreeSans9pt7b);
      tft.setTextColor(0xF81F, TFT_BLACK);
      char dateStr[22];
      strftime(dateStr, sizeof(dateStr), "%b %d, %Y", &timeinfo);
      tft.drawString(dateStr, 12, 50);

      tft.setFreeFont(NULL);
      drawWorldDashboard();
    }
  }

  void drawSparkline(int x, int y, int w, int h, uint16_t color) {
    float minP = 999999, maxP = 0;
    bool hasData = false;

    for (int i = 0; i < SPARK_POINTS; i++) {
      if (history[currentStockIdx][i] <= 0) continue;
      minP = min(minP, history[currentStockIdx][i]);
      maxP = max(maxP, history[currentStockIdx][i]);
      hasData = true;
    }

    if (!hasData || maxP == minP) return;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      if (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5) {

        int currentMinutes = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
        int closeMins = MARKET_OPEN_MINS + TRADING_MINUTES;

        if (currentMinutes >= MARKET_OPEN_MINS && currentMinutes < closeMins) {

          int minutesSinceOpen = currentMinutes - MARKET_OPEN_MINS;
          int indicesSinceOpen = (minutesSinceOpen * 60000) / fetchInterval;
          int openIndex = (SPARK_POINTS - 1) - indicesSinceOpen;

          if (openIndex >= 0 && openIndex < SPARK_POINTS) {
            int breakX = x + (openIndex * w / (SPARK_POINTS - 1));
            tft.drawFastVLine(breakX, y - h, h, TFT_WHITE);
          }
        }
      }
    }

    for (int i = 0; i < SPARK_POINTS - 1; i++) {
      if (history[currentStockIdx][i] <= 0 || history[currentStockIdx][i + 1] <= 0) continue;
      int x1 = x + (i * w / (SPARK_POINTS - 1));
      int x2 = x + ((i + 1) * w / (SPARK_POINTS - 1));

      float range = (maxP - minP);
      if (range <= 0.001) range = 1.0;

      int y1 = y - (int)((history[currentStockIdx][i] - minP) / range * h);
      int y2 = y - (int)((history[currentStockIdx][i + 1] - minP) / range * h);

      tft.drawLine(x1, y1, x2, y2, color);
    }
  }

void drawSystemHealth() {
  int yPos = 310;
  
  tft.drawFastHLine(0, yPos - 3, 480, 0x4208);
  tft.fillRect(0, yPos, 480, 10, TFT_BLACK);
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  
  tft.setCursor(5, yPos);
  tft.printf("WiFi: %.12s", WiFi.SSID().c_str());
  
  // --- CONNECTION INDICATOR DOTS ---
  int dotX = 135; 
  int dotY = yPos + 3; // Vertically aligned with text
  int radius = 3;
  int spacing = 12;

  uint16_t timeColor = timeHealthy ? TFT_GREEN : TFT_RED;
  uint16_t owmColor = owmHealthy ? TFT_GREEN : TFT_RED;
  uint16_t finColor = finnhubHealthy ? TFT_GREEN : TFT_RED;

  tft.fillCircle(dotX, dotY, radius, timeColor);               // 1. Time (NTP)
  tft.fillCircle(dotX + spacing, dotY, radius, owmColor);      // 2. OpenWeather
  tft.fillCircle(dotX + (spacing * 2), dotY, radius, finColor);// 3. Finnhub
  // --------------------------------------------------
  
  updateSQDisplay();
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  updateSDStatusDisplay();
  
  float chipTemp = (temprature_sens_read() - 32) / 1.8;
  tft.setCursor(400, yPos);
  tft.print("Core:");
  tft.printf("%.1fC", chipTemp);
}

  void updateSQDisplay() {
    int xPos = 220, yPos = 310;
    tft.setCursor(xPos, yPos);
    tft.print("SQ:");
    tft.fillRect(xPos + 18, yPos, 30, 10, TFT_BLACK);
    if (currentSignal > 45) tft.setTextColor(TFT_GREEN, TFT_BLACK);
    else if (currentSignal >= MIN_QUAL) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    else tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(xPos + 18, yPos);
    tft.print(currentSignal);
    tft.print("%");
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  }

  void updateSDStatusDisplay() {
    int xPos = 310, yPos = 310;
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(xPos, yPos);
    if (!isMarketOpen()) {
      tft.print("SD: ");
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.print("CLOSED  ");
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    } else if (lastSDWriteTime == 0) {
      tft.print("SD:---Wait---");
    } else {
      int minsAgo = (millis() - lastSDWriteTime) / 60000;
      tft.printf("SD: %2dm ago", minsAgo);
    }
  }

  void drawTacticalHUD(int16_t rawX, int16_t rawY, bool hasTarget) {
    const int hudX = 425;
    const int hudY = 10;

    if (hasTarget) {
      tft.fillRect(hudX - 5, hudY - 2, 55, 30, TFT_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(0x05E0, TFT_BLACK);

      tft.setCursor(hudX, hudY);
      tft.printf("X: %d", rawX);

      tft.setCursor(hudX, hudY + 15);
      tft.printf("Y: %d", rawY);
    } else {
      tft.fillRect(hudX - 5, hudY - 2, 55, 30, TFT_BLACK);
    }
  }

  void drawCylonScanner() {
    int yPos = 310, minX = 160, maxX = 320 - 25;
    if (!wifiConnectedCached) {
      tft.fillRect(minX, yPos, (maxX - minX) + 25, 10, TFT_BLACK);
      tft.fillRect(cylonPos, yPos + 2, 25, 6, 0x8000);
      tft.fillRect(cylonPos + 7, yPos + 1, 11, 8, TFT_RED);
      cylonPos += cylonDir;
      if (cylonPos <= minX || cylonPos >= maxX) cylonDir *= -1;
    }
  }
