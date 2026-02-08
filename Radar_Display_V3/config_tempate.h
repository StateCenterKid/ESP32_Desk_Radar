// config_template.h - Rename to config.h and fill in your credentials
#ifndef CONFIG_H
#define CONFIG_H

// WiFi Credentials
const char* ssid = "YOUR_WIFI_SSID";         // Primary WiFi
const char* password = "YOUR_WIFI_PASSWORD"; // Primary Password
const char* ssid2 = "YOUR_MOBILE_HOTSPOT";    // Secondary WiFi
const char* password2 = "HOTSPOT_PASSWORD";  // Secondary Password

// API Keys
// Get yours at finnhub.io
const char* finnhubapi = "YOUR_FINNHUB_API_KEY";
// Get yours at openweathermap.org
const char* OWM_API_KEY = "YOUR_OPENWEATHER_API_KEY";

// Weather Location (Local)
const char* weatherCity    = "OFallon"; // Your City
const char* weatherState   = "MO";      // Your State
const char* weatherCountry = "US";      // Your Country Code
const char* localDisplayName = "O'Fallon"; // Visual name for UI

// Timezone Settings (Example: Central Standard Time)
// GMT -6 hours = -21600 seconds
const long  gmtOffset_sec = -21600;

// Daylight Savings (1 hour = 3600 seconds)
const int   daylightOffset_sec = 3600;

const char* ntpServer = "pool.ntp.org";

// World City Definitions
struct WorldCity {
  const char* name;
  long gmtOffset;
  bool hasDST;
  const char* lat;
  const char* lon;
  float temp;
  String desc;
  const char* displayName;
};

// Add your global team locations here
WorldCity worldCities[3] = {
  {"CLUJ",   7200,  true,  "46.77", "23.62", 0, "---", "  Cluj"}, 
  {"PUNE",   19800, false, "18.52", "73.85", 0, "---", "  Pune"},
  {"MANILA", 28800, false, "14.59", "120.98", 0, "---", " Manila"}
};

// Stock Market Settings
// Symbols to track (Finnhub symbols)
const char* stockSymbols[] = {"EMR", "SPY", "NVDA", "DIA", "QQQ"};
const int NUM_STOCKS = sizeof(stockSymbols) / sizeof(stockSymbols[0]);

// Rotation interval (10000 = 10 seconds)
const unsigned long rotateInterval = 10000;

#endif