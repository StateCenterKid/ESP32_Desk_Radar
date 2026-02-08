# ESP32_Desk_Radar
An ESP32 - TFT project that uses an LD2450 human presence radar to display people that are behind your desk. Includes api calls to  openweather and Finnhub for weather and stock price information.

🛰️ ESP32 Tactical Radar & Info Dashboard (V3.0)

A high-density telemetry dashboard built for the **Cheap Yellow Display (CYD)**. This project integrates real-time human tracking via the **HLK-LD2450 24GHz Radar** sensor with global weather, stock market data, and a localized tactical HUD.

## 🚀 Features

* **Tactical Radar:** Tracks up to 3 targets simultaneously within an 8-meter range using the LD2450 sensor.
* **Global Sync:** Synchronized clocks and weather for your global teams (Cluj, Pune, Manila).
* **Financial Telemetry:** Live stock ticker with custom sparklines and auto-rotation every 10 seconds.
* **Environmental Data:** Local weather for O'Fallon, MO, featuring dual-unit (F/C) temperature displays.
* **System Health:** Real-time monitoring of WiFi RSSI, Signal Quality, and ESP32 Core temperature.

## 🛠️ Hardware Requirements

* **Display:** ESP32-2432S028R (Cheap Yellow Display / CYD)
* **Sensor:** Hi-Link LD2450 24GHz Radar
* **Connection:** Cat6 cable with RJ45 terminal blocks (Recommended for cord management)

## 📦 Required Libraries

Ensure you have the following libraries installed in your Arduino IDE:

* `TFT_eSPI` (Configured for ILI9341)
* `ArduinoJson` (v6 or higher)
* `HTTPClient` & `WiFi`

## ⚙️ Setup Instructions

1. **Configure Credentials:**
* Rename `config_template.h` to `config.h`.
* Enter your WiFi SSID and Passwords.
* Add your API keys for **Finnhub** and **OpenWeatherMap**.


2. **Wiring:**
* Connect LD2450 TX to ESP32 Pin 32 (RX1).
* Connect LD2450 RX to ESP32 Pin 25 (TX1).
* Power the sensor via 5V and GND.


3. **Compile & Upload:**
* Set your Board to "ESP32 Dev Module."
* Upload `RadarDisplay_V3.ino`.



## 🕹️ Controls

* **WiFi Toggle:** Tap the "WiFi" label in the bottom-left corner to switch between your primary and secondary WiFi networks.
* **Tactical HUD:** The top-right corner displays the raw X/Y coordinates of the primary target.
