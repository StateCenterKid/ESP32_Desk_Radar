#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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

// --- CONFIGURATION ---
const int16_t MAX_RANGE = 8000;
const int16_t MIN_RANGE = 100;
const int16_t MIN_QUAL = 2;
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
String weatherDesc = "---";
int currentStockIdx = 0;

// Signal Smoothing
float smoothedSignal = 0;
int currentSignal = 0;
int lastDisplayedSQ = -1;

// Timing
unsigned long lastDataFetch = 0;
const unsigned long fetchInterval = 900000; // 15 mins
unsigned long lastStockRotate = 0;

unsigned long lastCylonUpdate = 0;
int cylonPos = 160;
int cylonDir = 4;
bool timeIsSet = false;
bool wasConnected = false;
bool useSecondaryWiFi = false;

#define SPARK_POINTS 30
float history[NUM_STOCKS][SPARK_POINTS];

// --- SETUP ---
void setup() {
  if (DEBUG_SERIAL) Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, 25, 32);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  uint16_t calData[5] = { 280, 3550, 300, 3500, 6 };
  tft.setTouch(calData);

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 10; j++) targets[i].xHistory[j] = EMPTY;
  }

  drawRadarBackground();

//test line - comment out if running live
//  generateTestData(); // <--- ADD THIS LINE HERE

  startWiFi();
}

// --- MAIN LOOP ---
void loop() {
// 1. TOUCH HANDLING (WiFi Label Toggle - Bottom Left)
  uint16_t t_x = 0, t_y = 0;
  if (tft.getTouch(&t_x, &t_y)) {
    // Target: Bottom Left corner where "WiFi: [SSID]" is drawn
    // X < 120 (covers the label) and Y > 300 (bottom status bar)
    if (t_x > 360 && t_y < 50) {
      useSecondaryWiFi = !useSecondaryWiFi;
      timeIsSet = false;
      lastDataFetch = 0;  // Force immediate re-fetch
      tft.fillScreen(TFT_BLACK);
      drawRadarBackground();
      startWiFi();
      delay(1000); // Debounce to prevent rapid toggling
    }
  }
  while (Serial1.available() >= 22) {
    if (Serial1.read() == 0xAA) {
      if (Serial1.read() == 0xFF && Serial1.read() == 0x03 && Serial1.read() == 0x00) {
        uint8_t payload[18];
        Serial1.readBytes(payload, 18);
        processRadarData(payload);
      }
    }
  }

  updateClock();
  
  if (millis() - lastCylonUpdate > 30) {
    drawCylonScanner();
    lastCylonUpdate = millis();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!timeIsSet) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      timeIsSet = true;
      drawSystemHealth();
    }
    if (millis() - lastDataFetch > fetchInterval || lastDataFetch == 0) {
      fetchAllData();
      lastDataFetch = millis();
      drawInfoPanel();
      drawWorldDashboard();
    }
  }

  if (millis() - lastStockRotate > rotateInterval) {
    currentStockIdx = (currentStockIdx + 1) % NUM_STOCKS;
    drawInfoPanel();
    drawSystemHealth();
    lastStockRotate = millis();
  }
}

// --- RADAR FUNCTIONS ---
void processRadarData(uint8_t* data) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 10; j++) {
      if (targets[i].xHistory[j] != EMPTY) {
        int ex = map(targets[i].xHistory[j], -MAX_RANGE/2, MAX_RANGE/2, 160, 479);
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
        int dx = map(targets[i].xHistory[j], -MAX_RANGE/2, MAX_RANGE/2, 160, 479);
        int dy = map(targets[i].yHistory[j], 0, MAX_RANGE, 0, 160);
        tft.fillCircle(dx, dy, 6 - (j / 2), targetColors[i]);
      }
    }
  }
}

void drawRadarBackground() {
  int minX = 159, maxX = 479, maxY = 160;
  uint16_t circleColor = 0x0140; // Dark Green
  uint16_t gridColor = 0x0560;   // Tactical Green
  
  // 1. DYNAMIC RINGS
  // Inner Ring (1/2 Range)
  // Logic: ( (MAX_RANGE / 2) / MAX_RANGE ) * 160px = 80px
  tft.drawCircle(319, 0, 80, circleColor); 
  
  // Outer Ring (Full Range)
  // Logic: ( MAX_RANGE / MAX_RANGE ) * 160px = 160px
  tft.drawCircle(319, 0, 160, circleColor);

  // 2. GRID LINES
  for (int x = minX; x <= maxX; x += 80) tft.drawLine(x, 0, x, maxY, gridColor);
  for (int y = 0; y <= maxY; y += 80) tft.drawLine(minX, y, maxX, y, gridColor);
  
  // 3. ORIGIN POINT
  tft.fillCircle(319, 0, 5, 0x8000); // Maroon/Red center
}

// --- DATA FETCHING ---
void startWiFi() {
  WiFi.disconnect();
  const char* activeSSID = useSecondaryWiFi ? ssid2 : ssid;
  const char* activePASS = useSecondaryWiFi ? password2 : password;
  WiFi.begin(activeSSID, activePASS);
}

void fetchAllData() {
  // 1. Local Weather
  fetchWeather();
  delay(1000); // 1 second gap

  // 2. Global Weather
  fetchWorldWeather();
  delay(500);

  // 3. Stocks (Finnhub is generally more lenient than OWM)
  for (int i = 0; i < NUM_STOCKS; i++) {
    fetchStock(stockSymbols[i], i);
    delay(200); 
  }
}

void fetchWorldWeather() {
  HTTPClient http;
  for (int i = 0; i < 3; i++) {
    String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(worldCities[i].lat) + 
                 "&lon=" + String(worldCities[i].lon) + "&units=imperial&appid=" + String(OWM_API_KEY);
    
    http.setTimeout(5000); // 5 second timeout for slow connections
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, http.getString());
      worldCities[i].temp = doc["main"]["temp"];
      worldCities[i].desc = doc["weather"][0]["main"].as<String>();
    } else if (DEBUG_SERIAL) {
      Serial.printf("OWM Error (%s): %d\n", worldCities[i].name, httpCode);
    }
    http.end();
    delay(500); // Increased delay to 0.5 seconds between global pings
  }
}

void fetchWeather() {
  HTTPClient http;
  
  // Cleaned up URL construction
  // Format: q=City,State,Country
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
               String(weatherCity) + "," + 
               String(weatherState) + "," + 
               String(weatherCountry) + 
               "&units=imperial&appid=" + String(OWM_API_KEY);

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
      weatherDesc = doc["weather"][0]["main"].as<String>();
      
      if (DEBUG_SERIAL) Serial.printf("Local Weather: %.1fF, %s\n", currentTemp, weatherDesc.c_str());
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
  HTTPClient http;
  String url = "https://finnhub.io/api/v1/quote?symbol=" + symbol + "&token=" + String(finnhubapi);
  http.begin(url);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, http.getString());
    
    float newPrice = doc["c"];
    stockPrices[idx] = newPrice;
    stockChanges[idx] = doc["d"];
    stockPercents[idx] = doc["dp"];

    // --- NEW LOGIC: Save to History ---
    // 1. Shift everything to the left
    for (int j = 0; j < SPARK_POINTS - 1; j++) {
      history[idx][j] = history[idx][j+1];
    }
    // 2. Add new price to the end
    history[idx][SPARK_POINTS - 1] = newPrice;
    
  } else {
    if(DEBUG_SERIAL) Serial.printf("Stock HTTP Error: %d\n", httpCode);
  }
  http.end();
}

// --- UI DRAWING ---
void drawWorldDashboard() {
  int startX = 160;
  int startY = 180;   
  int endY = 300;     
  int gridW = 319;    
  int colWidth = 106; 
  int paddingX = 10; 

  // 1. DRAW BOX & GRID
  uint16_t borderColor = TFT_ORANGE;
  tft.drawRoundRect(startX, startY, gridW, endY - startY, 8, borderColor); 
  tft.drawLine(266, startY, 266, endY, borderColor); 
  tft.drawLine(372, startY, 372, endY, borderColor); 
  tft.drawFastHLine(startX, startY + 30, gridW, borderColor);

  // 2. TIME MATH
  time_t now;
  time(&now); // 'now' is ALWAYS UTC in the Unix world
  
  // We need local info only to see if the LOCAL system thinks we are in DST
  struct tm *loc;
  loc = localtime(&now); 

  for (int i = 0; i < 3; i++) {
    int xBase = 160 + (i * colWidth) + paddingX;
    
    // Unix 'now' is UTC. We simply add the city offset.
    time_t cityTime = now + worldCities[i].gmtOffset;
    
    // Handle DST for the target city
    // If we are currently in DST in Missouri, we assume Cluj (hasDST=true) 
    // is also in its Summer Time (+1 hour).
    if (worldCities[i].hasDST && loc->tm_isdst > 0) {
      cityTime += 3600; 
    }

    // gmtime converts the calculated timestamp into a readable struct without 
    // applying any local ESP32 timezone settings.
    struct tm *info = gmtime(&cityTime);
    char timeBuf[12];
    strftime(timeBuf, sizeof(timeBuf), "%I:%M%p", info);
    
    // 3. DRAWING
    tft.setFreeFont(NULL); 
    tft.setTextSize(2);

    // Header: Display Name (e.g., "  Cluj")
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setCursor(xBase, startY + 8);
    tft.print(worldCities[i].displayName);

    // Corrected Time
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(xBase, startY + 42);
    tft.print(timeBuf);

// --- Temperature Row ---
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(xBase, startY + 68);
    tft.printf("%.0fF", worldCities[i].temp);

    // --- NEW: Celsius Equivalent ---
    // Calculate C, set to Size 1, and dim the color
    float celsius = (worldCities[i].temp - 32) * 5.0 / 9.0;
    tft.setTextSize(1);
    tft.setTextColor(0xF7BE, TFT_BLACK); // Dimmer grey/blue
    
    // We nudge the X position by 42 pixels to sit right after the "00F" text
    tft.setCursor(xBase + 52, startY + 75); 
    tft.printf("%.0fC", celsius);
    
    // Conditions
    tft.setTextSize(1);
    tft.setTextColor(0xF7BE, TFT_BLACK);
    tft.setCursor(xBase, startY + 92);
    String d = worldCities[i].desc;
    if (d.length() > 14) d = d.substring(0, 14);
    tft.print(d);
  }
}

void drawInfoPanel() {
  // 1. Clear Weather Area (Y=65 to Y=145)
  // Height increased to 80 to cover the new name line and the bottom shift
  tft.fillRect(0, 65, 150, 80, TFT_BLACK); 

  // --- LOCAL CITY NAME ---
  tft.setTextSize(1);
  tft.setFreeFont(NULL); // Ensure system font for consistency
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(10, 78); 
  tft.print(localDisplayName);

  // --- WEATHER DATA (Shifted down) ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  
  // Temperature & Humidity (Moved from 85 to 90)
  tft.setCursor(10, 98);
  tft.printf("%.0fF, H:%d%%", currentTemp, currentHumid);
  
  // Weather Condition (Moved from 105 to 115)
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 123); 
  tft.print(weatherDesc);
  
  // 2. Draw Stocks (Remains at Y=175)
  int stockY = 175;
  tft.fillRect(0, stockY, 150, 132, TFT_BLACK);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, stockY); tft.print(stockSymbols[currentStockIdx]);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, stockY + 20); tft.printf("$%.2f", stockPrices[currentStockIdx]);
  
  uint16_t trend = (stockChanges[currentStockIdx] >= 0) ? TFT_GREEN : TFT_RED;
  tft.setTextColor(trend, TFT_BLACK);
  tft.setCursor(10, stockY + 40); tft.printf("%+.2f%%", stockPercents[currentStockIdx]);

  // Redraw Sparkline (Y=280)
  drawSparkline(5, 280, 130, 35, trend); 
}

void updateClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  static int lastMin = -1;
  
  if (timeinfo.tm_min != lastMin) {
    lastMin = timeinfo.tm_min;
    
    tft.fillRect(0, 0, 150, 65, TFT_BLACK);
    
    // Using internal GFX Font pointers (usually enabled by default in TFT_eSPI)
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
    
    tft.setFreeFont(NULL); // Always reset to standard font
    drawWorldDashboard();
  }
}

void drawSparkline(int x, int y, int w, int h, uint16_t color) {
  float minP = 999999, maxP = 0;
  bool hasData = false;

  // 1. Find Min/Max for Scaling
  for (int i = 0; i < SPARK_POINTS; i++) {
    if (history[currentStockIdx][i] <= 0) continue;
    minP = min(minP, history[currentStockIdx][i]);
    maxP = max(maxP, history[currentStockIdx][i]);
    hasData = true;
  }

  // If no valid data or flat line, skip drawing
  if (!hasData || maxP == minP) return;

  // --- NEW LOGIC: Dynamic Market Open Line ---
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Check if it's a weekday (1=Mon ... 5=Fri)
    if (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5) {
      
      // Calculate minutes since midnight for NOW
      int currentMinutes = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
      
      // Market Open Target: 8:30 AM = 510 minutes
      int openMinutes = (8 * 60) + 30; 

      // Calculate minutes per graph point based on fetchInterval
      // e.g., 900000ms / 60000 = 15 minutes per point
      int minutesPerPoint = fetchInterval / 60000;
      if (minutesPerPoint < 1) minutesPerPoint = 1; // Safety check

      // Calculate how many "slots" ago the market opened
      int minutesSinceOpen = currentMinutes - openMinutes;
      int indicesSinceOpen = minutesSinceOpen / minutesPerPoint;

      // The far RIGHT of the graph (SPARK_POINTS - 1) is "Now"
      // Subtract indicesSinceOpen to find the "Open" position
      int openIndex = (SPARK_POINTS - 1) - indicesSinceOpen;

      // Only draw if the line falls within the visible graph (0 to 29)
      if (openIndex >= 0 && openIndex < SPARK_POINTS) {
        int breakX = x + (openIndex * w / (SPARK_POINTS - 1));
        tft.drawFastVLine(breakX, y - h, h, 0x4208); // Dark Grey divider
      }
    }
  }
  // -------------------------------------------

  // 2. Draw the Sparkline
  for (int i = 0; i < SPARK_POINTS - 1; i++) {
    // Skip if either point is invalid (0)
    if (history[currentStockIdx][i] <= 0 || history[currentStockIdx][i + 1] <= 0) continue;

    // Map X coordinates
    int x1 = x + (i * w / (SPARK_POINTS - 1));
    int x2 = x + ((i + 1) * w / (SPARK_POINTS - 1));

    // Map Y coordinates (Inverted: Higher price = Lower pixel Y)
    // Added safety for divide-by-zero if maxP == minP (handled above, but good practice)
    int range = (maxP - minP);
    if (range == 0) range = 1; 

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
  tft.print("WiFi:"); tft.print(WiFi.SSID());
  tft.setCursor(125, yPos);
  tft.print("RSSI:"); tft.print(WiFi.RSSI());
  tft.setCursor(245, yPos);
  tft.print("SQ:"); updateSQDisplay();
  float chipTemp = (temprature_sens_read() - 32) / 1.8;
  tft.setCursor(400, yPos);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.print("Core:"); tft.printf("%.1fC", chipTemp);
}

void updateSQDisplay() {
  int xPos = 245, yPos = 310;
  tft.fillRect(xPos + 18, yPos, 30, 10, TFT_BLACK);
  if (currentSignal > 45) tft.setTextColor(TFT_GREEN, TFT_BLACK);
  else if (currentSignal >= MIN_QUAL) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  else tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(xPos + 18, yPos);
  tft.print(currentSignal); tft.print("%");
}

void drawTacticalHUD(int16_t rawX, int16_t rawY, bool hasTarget) {
  // Positioning for the Top-Right Corner of the grid
  const int hudX = 425; 
  const int hudY = 10;
  
  if (hasTarget) {
    // Clear the small area for the two lines of text
    tft.fillRect(hudX - 5, hudY - 2, 55, 30, TFT_BLACK);
    
    tft.setTextSize(1);
    tft.setTextColor(0x05E0, TFT_BLACK); // Tactical Green
    
    // Line 1: X Coordinate
    tft.setCursor(hudX, hudY);
    tft.printf("X: %d", rawX);
    
    // Line 2: Y Coordinate
    tft.setCursor(hudX, hudY + 15);
    tft.printf("Y: %d", rawY);
  } else {
    // If no target, clear the corner
    tft.fillRect(hudX - 5, hudY - 2, 55, 30, TFT_BLACK);
  }
}

void drawCylonScanner() {
  int yPos = 310, minX = 160, maxX = 320 - 25;
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillRect(minX, yPos, (maxX - minX) + 25, 10, TFT_BLACK);
    tft.fillRect(cylonPos, yPos + 2, 25, 6, 0x8000);
    tft.fillRect(cylonPos + 7, yPos + 1, 11, 8, TFT_RED);
    cylonPos += cylonDir;
    if (cylonPos <= minX || cylonPos >= maxX) cylonDir *= -1;
  }
}
//test set for troubleshooting
void generateTestData() {
  for (int i = 0; i < NUM_STOCKS; i++) {
    for (int j = 0; j < SPARK_POINTS; j++) {
      // Generate a float between 150.00 and 160.00
      float randomCent = (float)random(0, 1000) / 100.0; 
      history[i][j] = 150.0 + randomCent;
    }
    // Also set the current price to match the last point so the text matches the line
    stockPrices[i] = history[i][SPARK_POINTS - 1];
    stockChanges[i] = 1.50; // Fake positive change
    stockPercents[i] = 1.0; // Fake 1% up
  }
}