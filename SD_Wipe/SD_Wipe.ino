#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

TFT_eSPI tft = TFT_eSPI();
#define SD_CS 5

void setup() {
  Serial.begin(115200);

  // Initialize the screen
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  
  // Header
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 20);
  tft.println("--- SD Wiper Utility ---");
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 60);
  tft.print("Mounting SD Card... ");

  // 1. Mount the SD Card
  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("FAILED");
    tft.setCursor(10, 90);
    tft.println("Check card or pins.");
    return;
  }
  
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("OK");

  // 2. Look for the history file
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 100);
  tft.print("Locating /history.bin... ");

  if (SD.exists("/history.bin")) {
    tft.println("FOUND");
    
    // 3. Delete the file
    tft.setCursor(10, 140);
    tft.print("Deleting file... ");
    
    if (SD.remove("/history.bin")) {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.println("DONE!");
      
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setCursor(10, 200);
      tft.println("SD Card is clean.");
      tft.setCursor(10, 230);
      tft.println("Ready for main program.");
      
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.println("ERROR");
    }
    
  } else {
    // File doesn't exist
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.println("MISSING");
    tft.setCursor(10, 140);
    tft.println("(Already clean!)");
  }
}

void loop() {
  // Nothing to do here! The utility only runs once on boot.
  delay(1000);
}