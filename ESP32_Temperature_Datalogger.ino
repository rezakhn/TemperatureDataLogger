/*
 * ESP32-035 Temperature Datalogger
 * Features:
 * - Multiple DS18B20 temperature sensors on OneWire bus
 * - Graphical temperature charts with time ranges
 * - Data table with scroll
 * - Data storage on SPIFFS/LittleFS
 * - Real-time clock with NTP
 * - Battery voltage measurement and percentage
 * - WiFi/Bluetooth status and control
 * - Settings screen
 * - Touch calibration
 * - Dual-core operation
 * - Power management
 */

// Include necessary libraries
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SPIFFS.h>
#include <FS.h>
#include <time.h>
#include <Preferences.h>
#include <driver/adc.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Pin definitions for ESP32-035 board
#define ONE_WIRE_BUS 4      // Pin for DS18B20 sensors
#define BATTERY_ADC_PIN 36  // ADC pin for battery voltage measurement
#define TFT_BL 5            // Backlight control

// Display dimensions
#define TFT_WIDTH 320
#define TFT_HEIGHT 480

// Screen states
enum ScreenState {
  MAIN_SCREEN,
  CHART_SCREEN,
  TABLE_SCREEN,
  SETTINGS_SCREEN
};

// Time range for charts
enum TimeRange {
  ONE_HOUR,
  SIX_HOURS,
  TWENTY_FOUR_HOURS
};

// Global variables
TFT_eSPI tft = TFT_eSPI();
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClient client;
HTTPClient http;

// Preferences for storing settings
Preferences preferences;

// System variables
ScreenState currentScreen = MAIN_SCREEN;
TimeRange currentTimeRange = ONE_HOUR;
bool wifiEnabled = true;
bool bluetoothEnabled = true;
String ssid = "";
String password = "";
int sensorCount = 0;
String sensorNames[10]; // Max 10 sensors
int sampleInterval = 60; // Default 1 minute
float batteryVoltage = 0.0;
int batteryPercentage = 0;
bool deepSleepEnabled = true;
unsigned long lastTouchTime = 0;
bool screenOn = true;

// Data storage
struct TempData {
  time_t timestamp;
  float temperatures[10]; // Max 10 sensors
};

// Circular buffer for storing temperature data
#define DATA_BUFFER_SIZE 1000
TempData dataBuffer[DATA_BUFFER_SIZE];
int dataBufferIndex = 0;
int dataBufferCount = 0;

// Touch calibration values
int touchCalMinX = 380, touchCalMinY = 240, touchCalMaxX = 3800, touchCalMaxY = 3800;

// Core task handles
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;

// Function declarations
void setupSensors();
void readSensors();
void updateDisplay();
void handleTouch();
void updateClock();
void updateBattery();
void connectToWiFi();
void startServer();
void saveSettings();
void loadSettings();
void saveData();
void loadData();
void drawMainScreen();
void drawChartScreen();
void drawTableScreen();
void drawSettingsScreen();
void enterDeepSleep();
void IRAM_ATTR touchISR();

void setup() {
  Serial.begin(115200);
  
  // Disable brownout detection to reduce power consumption
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Initialize display
  tft.init();
  tft.setRotation(3); // Portrait mode
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  
  // Initialize backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    SPIFFS.format();
    if (!SPIFFS.begin(true)) {
      Serial.println("SPIFFS Mount Failed after format");
    }
  }
  
  // Load settings
  loadSettings();
  
  // Initialize sensors
  setupSensors();
  
  // Initialize WiFi
  WiFi.mode(WIFI_STA);
  
  // Connect to WiFi if enabled
  if (wifiEnabled) {
    connectToWiFi();
  }
  
  // Initialize time with NTP
  configTime(0, 0, "pool.ntp.org");
  
  // Initialize ADC for battery measurement
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11); // GPIO36 = ADC1_CHANNEL_0
  
  // Initialize touch
  tft.setTouch(touchCalMinX, touchCalMinY, touchCalMaxX, touchCalMaxY);
  
  // Create tasks for dual-core operation
  xTaskCreatePinnedToCore(
    sensorTask,           // Task function
    "SensorTask",         // Task name
    4096,               // Stack size
    NULL,               // Task parameter
    1,                  // Priority
    &sensorTaskHandle,   // Task handle
    0                    // Core
  );
  
  xTaskCreatePinnedToCore(
    displayTask,          // Task function
    "DisplayTask",        // Task name
    8192,               // Stack size
    NULL,               // Task parameter
    1,                  // Priority
    &displayTaskHandle,  // Task handle
    1                    // Core
  );
  
  Serial.println("Setup complete");
}

void loop() {
  // Main loop runs on core 1, but most work is done by tasks
  delay(100);
}

// Task for reading sensors (runs on core 0)
void sensorTask(void *pvParameters) {
  while (true) {
    readSensors();
    updateBattery();
    delay(sampleInterval * 1000); // Sample interval in seconds
    
    // Check for deep sleep
    if (deepSleepEnabled && (millis() - lastTouchTime > 60000)) { // 1 minute timeout
      enterDeepSleep();
    }
  }
}

// Task for updating display (runs on core 1)
void displayTask(void *pvParameters) {
  while (true) {
    updateDisplay();
    delay(100); // Update display every 100ms
  }
}

void setupSensors() {
  sensors.begin();
  sensorCount = sensors.getDeviceCount();
  
  // Set resolution for DS18B20 sensors
  for (int i = 0; i < sensorCount; i++) {
    sensors.setResolution(i, 12);
  }
  
  Serial.print("Found ");
  Serial.print(sensorCount);
  Serial.println(" DS18B20 sensors");
}

void readSensors() {
  sensors.requestTemperatures();
  
  // Create new temperature data entry
  TempData newData;
  time(&newData.timestamp);
  
  for (int i = 0; i < sensorCount && i < 10; i++) {
    newData.temperatures[i] = sensors.getTempCByIndex(i);
  }
  
  // Add to circular buffer
  dataBuffer[dataBufferIndex] = newData;
  dataBufferIndex = (dataBufferIndex + 1) % DATA_BUFFER_SIZE;
  if (dataBufferCount < DATA_BUFFER_SIZE) {
    dataBufferCount++;
  }
  
  // Save data to SPIFFS
  saveData();
  
  Serial.println("Sensors read and data saved");
}

void updateDisplay() {
  switch (currentScreen) {
    case MAIN_SCREEN:
      drawMainScreen();
      break;
    case CHART_SCREEN:
      drawChartScreen();
      break;
    case TABLE_SCREEN:
      drawTableScreen();
      break;
    case SETTINGS_SCREEN:
      drawSettingsScreen();
      break;
  }
  
  // Handle touch input
  handleTouch();
}

void handleTouch() {
  uint16_t x, y;
  if (tft.getTouch(&x, &y)) {
    lastTouchTime = millis();
    
    // Determine which screen we're on and handle touch accordingly
    switch (currentScreen) {
      case MAIN_SCREEN:
        // Check for icon touches (WiFi, Bluetooth, Settings)
        if (x > 280 && x < 310 && y > 10 && y < 40) { // WiFi icon
          wifiEnabled = !wifiEnabled;
          if (wifiEnabled) {
            WiFi.begin();
          } else {
            WiFi.disconnect();
          }
        } else if (x > 240 && x < 270 && y > 10 && y < 40) { // Bluetooth icon
          bluetoothEnabled = !bluetoothEnabled;
          // Bluetooth control would go here
        } else if (x > 200 && x < 230 && y > 10 && y < 40) { // Settings icon
          currentScreen = SETTINGS_SCREEN;
        } else if (x > 20 && x < 100 && y > TFT_HEIGHT - 60) { // Chart button
          currentScreen = CHART_SCREEN;
        } else if (x > 120 && x < 200 && y > TFT_HEIGHT - 60) { // Table button
          currentScreen = TABLE_SCREEN;
          tableScrollOffset = 0; // Reset scroll when entering table
        } else if (x > 220 && x < 300 && y > TFT_HEIGHT - 60) { // Settings button
          currentScreen = SETTINGS_SCREEN;
        }
        break;
        
      case CHART_SCREEN:
        // Handle chart navigation (zoom, scroll)
        if (x < 50 && y > TFT_HEIGHT - 70 && y < TFT_HEIGHT - 40) { // Previous time range
          currentTimeRange = (TimeRange)((currentTimeRange + 2) % 3); // Wrap around backwards
        } else if (x > TFT_WIDTH - 90 && y > TFT_HEIGHT - 70 && y < TFT_HEIGHT - 40) { // Next time range
          currentTimeRange = (TimeRange)((currentTimeRange + 1) % 3); // Wrap around forwards
        } else if (x > 100 && x < 220 && y > TFT_HEIGHT - 30) { // Back to main screen
          currentScreen = MAIN_SCREEN;
        }
        break;
        
      case TABLE_SCREEN:
        // Handle table navigation
        if (x > 10 && x < 30 && y > TFT_HEIGHT - 50 && y < TFT_HEIGHT - 30) { // Scroll up
          handleTableScroll(-1);
        } else if (x > 10 && x < 30 && y > TFT_HEIGHT - 30 && y < TFT_HEIGHT - 10) { // Scroll down
          handleTableScroll(1);
        } else if (x > 100 && x < 220 && y > TFT_HEIGHT - 30) { // Back to main screen
          currentScreen = MAIN_SCREEN;
        }
        break;
        
      case SETTINGS_SCREEN:
        // Handle settings screen touches
        // Page navigation
        if (y > 30 && y < 60) {
          if (x < TFT_WIDTH/4) settingsPage = 0;
          else if (x < TFT_WIDTH/2) settingsPage = 1;
          else if (x < 3*TFT_WIDTH/4) settingsPage = 2;
          else settingsPage = 3;
        }
        // Back to main screen
        else if (x > 100 && x < 220 && y > TFT_HEIGHT - 30) {
          currentScreen = MAIN_SCREEN;
        }
        // Settings controls based on current page
        else {
          switch(settingsPage) {
            case 0: // General settings
              if (x > 220 && x < 280 && y > 65 && y < 90) { // WiFi toggle
                changeSetting(0, 0); // Toggle
              } else if (x > 220 && x < 280 && y > 95 && y < 120) { // Bluetooth toggle
                changeSetting(1, 0); // Toggle
              } else if (x > 220 && x < 240 && y > 125 && y < 150) { // Sample interval -
                changeSetting(2, -10);
              } else if (x > 260 && x < 280 && y > 125 && y < 150) { // Sample interval +
                changeSetting(2, 10);
              } else if (x > 220 && x < 280 && y > 155 && y < 180) { // Deep sleep toggle
                changeSetting(3, 0); // Toggle
              }
              break;
              
            case 1: // WiFi settings
              if (x > 220 && x < 280 && y > 65 && y < 90) { // Edit SSID
                // This would require a keyboard interface - simplified for now
              } else if (x > 220 && x < 280 && y > 95 && y < 120) { // Edit password
                // This would require a keyboard interface - simplified for now
              } else if (x > 220 && x < 280 && y > 125 && y < 150) { // Hotspot toggle
                wifiEnabled = !wifiEnabled;
                if (wifiEnabled) {
                  WiFi.begin();
                } else {
                  WiFi.disconnect();
                }
                saveSettings();
              }
              break;
              
            case 2: // Sensor settings
              if (x > 220 && x < 240 && y > 65 && y < 90) { // Sensor count -
                changeSetting(0, -1);
              } else if (x > 260 && x < 280 && y > 65 && y < 90) { // Sensor count +
                changeSetting(0, 1);
              } else if (x > 220 && x < 280 && y > 95 && y < 120 && sensorCount > 0) { // Rename sensor
                // This would require a keyboard interface - simplified for now
              }
              break;
              
            case 3: // Calibration settings
              if (x > 10 && x < 150 && y > 200 && y < 230) { // Calibrate touch
                // Touch calibration would go here
              } else if (x > 170 && x < 310 && y > 200 && y < 230) { // Calibrate battery
                // Battery calibration would go here
              }
              break;
          }
        }
        break;
    }
  }
}

void updateClock() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[21];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.println(timeStr);
  }
}

void updateBattery() {
  // Read battery voltage (assuming voltage divider)
  int raw = analogRead(BATTERY_ADC_PIN);
  // Convert to voltage (assuming 3.3V reference and appropriate voltage divider)
  // You may need to calibrate this based on your voltage divider
  float voltage = (raw / 4095.0) * 3.3 * 2.0; // Example for 1:1 divider, adjust as needed
  
  batteryVoltage = voltage;
  
  // Calculate battery percentage (Li-ion 1100mAh)
  // 4.2V = 100%, 3.0V = 0% (approximate)
  batteryPercentage = (int)(((voltage - 3.0) / (4.2 - 3.0)) * 100);
  if (batteryPercentage > 100) batteryPercentage = 100;
  if (batteryPercentage < 0) batteryPercentage = 0;
  
  Serial.printf("Battery: %.2fV (%d%%)\n", batteryVoltage, batteryPercentage);
}

void connectToWiFi() {
  if (ssid.length() > 0 && password.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nFailed to connect to WiFi");
    }
  }
}

void saveSettings() {
  preferences.begin("temp_logger", false);
  preferences.putBool("wifi_enabled", wifiEnabled);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.putInt("sensor_count", sensorCount);
  
  for (int i = 0; i < sensorCount; i++) {
    String key = "sensor_" + String(i);
    preferences.putString(key.c_str(), sensorNames[i]);
  }
  
  preferences.putInt("sample_interval", sampleInterval);
  preferences.putBool("deep_sleep", deepSleepEnabled);
  preferences.putInt("touch_cal_min_x", touchCalMinX);
  preferences.putInt("touch_cal_min_y", touchCalMinY);
  preferences.putInt("touch_cal_max_x", touchCalMaxX);
  preferences.putInt("touch_cal_max_y", touchCalMaxY);
  preferences.end();
}

void loadSettings() {
  preferences.begin("temp_logger", true);
  wifiEnabled = preferences.getBool("wifi_enabled", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  sensorCount = preferences.getInt("sensor_count", 0);
  
  for (int i = 0; i < sensorCount; i++) {
    String key = "sensor_" + String(i);
    sensorNames[i] = preferences.getString(key.c_str(), "Sensor " + String(i+1));
  }
  
  sampleInterval = preferences.getInt("sample_interval", 60);
  deepSleepEnabled = preferences.getBool("deep_sleep", true);
  touchCalMinX = preferences.getInt("touch_cal_min_x", 380);
  touchCalMinY = preferences.getInt("touch_cal_min_y", 240);
  touchCalMaxX = preferences.getInt("touch_cal_max_x", 3800);
  touchCalMaxY = preferences.getInt("touch_cal_max_y", 3800);
  preferences.end();
}

void saveData() {
  File file = SPIFFS.open("/temp_data.txt", FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open temp_data.txt for writing");
    return;
  }
  
  // Write the latest data entry
  time_t timestamp = dataBuffer[(dataBufferIndex - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE].timestamp;
  char timeStr[21];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&timestamp));
  
  file.print(timeStr);
  file.print(",");
  
  for (int i = 0; i < sensorCount; i++) {
    file.print(dataBuffer[(dataBufferIndex - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE].temperatures[i]);
    if (i < sensorCount - 1) file.print(",");
  }
  file.println();
  
  file.close();
}

void loadData() {
  File file = SPIFFS.open("/temp_data.txt", FILE_READ);
  if (!file) {
    Serial.println("Failed to open temp_data.txt for reading");
    return;
  }
  
  dataBufferCount = 0;
  dataBufferIndex = 0;
  
  while (file.available() && dataBufferCount < DATA_BUFFER_SIZE) {
    String line = file.readStringUntil('\n');
    line.trim();
    
    if (line.length() == 0) continue;
    
    // Parse timestamp and temperatures
    int commaIndex = line.indexOf(',');
    String timeStr = line.substring(0, commaIndex);
    
    // Convert time string to time_t
    struct tm tmstruct = {0};
    sscanf(timeStr.c_str(), "%d-%d-%d %d:%d:%d", 
           &tmstruct.tm_year, &tmstruct.tm_mon, &tmstruct.tm_mday,
           &tmstruct.tm_hour, &tmstruct.tm_min, &tmstruct.tm_sec);
    tmstruct.tm_year -= 1900;  // Year since 1900
    tmstruct.tm_mon--;         // Month is 0-based
    
    time_t timestamp = mktime(&tmstruct);
    
    // Parse temperatures
    TempData data;
    data.timestamp = timestamp;
    
    int tempIndex = 0;
    int start = commaIndex + 1;
    while (start < line.length() && tempIndex < 10) {
      int end = line.indexOf(',', start);
      if (end == -1) end = line.length();
      
      String tempStr = line.substring(start, end);
      data.temperatures[tempIndex] = tempStr.toFloat();
      
      start = end + 1;
      tempIndex++;
    }
    
    // Add to buffer
    dataBuffer[dataBufferIndex] = data;
    dataBufferIndex = (dataBufferIndex + 1) % DATA_BUFFER_SIZE;
    dataBufferCount++;
  }
  
  file.close();
  
  Serial.printf("Loaded %d data points\n", dataBufferCount);
}

void drawMainScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw header with clock, battery, WiFi, Bluetooth
  tft.fillRect(0, 0, TFT_WIDTH, 50, TFT_DARKGREY);
  
  // Time and date
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[21];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(timeStr, 10, 15, 2);
  }
  
  // Battery icon and percentage
  int batteryX = TFT_WIDTH - 60;
  tft.drawRect(batteryX, 10, 30, 15, TFT_WHITE);
  tft.fillRect(batteryX + 30, 12, 3, 11, TFT_WHITE); // Battery tip
  int batteryWidth = (30 * batteryPercentage) / 100;
  tft.fillRect(batteryX + 2, 12, batteryWidth, 11, 
               batteryPercentage > 20 ? TFT_GREEN : TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.drawNumber(batteryPercentage, batteryX + 35, 15, 1);
  
  // WiFi icon
  int wifiX = TFT_WIDTH - 110;
  if (WiFi.status() == WL_CONNECTED) {
    // Draw WiFi connected icon (simplified)
    tft.fillCircle(wifiX, 20, 5, TFT_BLUE);
    tft.fillCircle(wifiX, 20, 3, TFT_BLACK);
  } else {
    // Draw WiFi disconnected icon
    tft.drawCircle(wifiX, 20, 5, TFT_RED);
    tft.drawLine(wifiX - 3, 20 - 3, wifiX + 3, 20 + 3, TFT_RED);
  }
  
  // Bluetooth icon
  int btX = TFT_WIDTH - 150;
  if (bluetoothEnabled) {
    // Draw Bluetooth enabled icon (simplified)
    tft.fillTriangle(btX - 3, 15, btX + 3, 20, btX - 3, 25, TFT_BLUE);
  } else {
    // Draw Bluetooth disabled icon
    tft.fillTriangle(btX - 3, 15, btX + 3, 20, btX - 3, 25, TFT_RED);
  }
  
  // Settings icon
  int settingsX = TFT_WIDTH - 190;
  tft.fillCircle(settingsX, 20, 8, TFT_WHITE);
  tft.fillCircle(settingsX, 20, 5, TFT_BLACK);
  
  // Draw sensor readings
  int startY = 70;
  for (int i = 0; i < sensorCount && i < 5; i++) { // Show max 5 sensors on main screen
    tft.setTextColor(TFT_WHITE);
    tft.drawString(sensorNames[i], 10, startY + i * 60, 2);
    
    float temp = 0.0;
    if (dataBufferCount > 0) {
      int idx = (dataBufferIndex - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE;
      temp = dataBuffer[idx].temperatures[i];
    }
    
    tft.setTextColor(TFT_YELLOW);
    tft.drawFloat(temp, 1, 100, startY + i * 60, 4);
    tft.drawString("°C", 160, startY + i * 60, 2);
  }
  
  // Draw navigation buttons
  tft.fillRect(20, TFT_HEIGHT - 60, 80, 40, TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Chart", 35, TFT_HEIGHT - 50, 2);
  
  tft.fillRect(120, TFT_HEIGHT - 60, 80, 40, TFT_GREEN);
  tft.drawString("Table", 135, TFT_HEIGHT - 50, 2);
  
  tft.fillRect(220, TFT_HEIGHT - 60, 80, 40, TFT_ORANGE);
  tft.drawString("Settings", 225, TFT_HEIGHT - 50, 2);
}

void drawChartScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw header
  tft.fillRect(0, 0, TFT_WIDTH, 30, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Temperature Chart", 10, 8, 2);
  
  // Draw chart area
  int chartX = 10;
  int chartY = 40;
  int chartWidth = TFT_WIDTH - 20;
  int chartHeight = TFT_HEIGHT - 120;
  tft.drawRect(chartX, chartY, chartWidth, chartHeight, TFT_WHITE);
  
  // Draw axis labels
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Time", chartX + chartWidth/2 - 20, chartY + chartHeight + 5, 1);
  tft.setTextRotation(90);
  tft.drawString("Temp (°C)", chartX - 25, chartY + chartHeight/2 - 25, 1);
  tft.setTextRotation(0);
  
  // Determine time range for chart
  time_t now;
  time(&now);
  time_t startTime;
  
  switch(currentTimeRange) {
    case ONE_HOUR:
      startTime = now - 3600; // 1 hour ago
      break;
    case SIX_HOURS:
      startTime = now - 6*3600; // 6 hours ago
      break;
    case TWENTY_FOUR_HOURS:
      startTime = now - 24*3600; // 24 hours ago
      break;
  }
  
  // Find min/max temperatures in the time range for scaling
  float minTemp = 1000, maxTemp = -1000;
  int dataPoints = 0;
  
  for (int i = 0; i < dataBufferCount; i++) {
    int idx = (dataBufferIndex - i - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE;
    if (dataBuffer[idx].timestamp >= startTime && dataBuffer[idx].timestamp <= now) {
      for (int s = 0; s < sensorCount; s++) {
        if (dataBuffer[idx].temperatures[s] != DEVICE_DISCONNECTED_C) {
          if (dataBuffer[idx].temperatures[s] < minTemp) minTemp = dataBuffer[idx].temperatures[s];
          if (dataBuffer[idx].temperatures[s] > maxTemp) maxTemp = dataBuffer[idx].temperatures[s];
        }
      }
      dataPoints++;
    }
  }
  
  // Add some padding to temperature range
  if (minTemp != 1000 && maxTemp != -1000) {
    float tempRange = maxTemp - minTemp;
    if (tempRange < 0.1) tempRange = 0.1; // Prevent division by zero
    minTemp -= tempRange * 0.1;
    maxTemp += tempRange * 0.1;
  } else {
    minTemp = 0;
    maxTemp = 50; // Default range
  }
  
  // Draw grid lines and labels
  for (int i = 0; i <= 5; i++) {
    int y = chartY + chartHeight - (i * chartHeight / 5);
    tft.drawFastHLine(chartX, y, chartWidth, TFT_DARKGREY);
    
    float temp = minTemp + (maxTemp - minTemp) * (i / 5.0);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString(String(temp, 1), chartX + 2, y - 8, 1);
  }
  
  // Draw temperature lines for each sensor
  int colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_PINK};
  
  for (int s = 0; s < sensorCount && s < 8; s++) {
    // Draw sensor name in legend
    tft.setTextColor(colors[s]);
    tft.drawString(sensorNames[s], chartX + 10 + s * 70, chartY + chartHeight + 25, 1);
    
    // Draw temperature line
    time_t prevTime = 0;
    float prevTemp = 0;
    
    for (int i = 0; i < dataBufferCount; i++) {
      int idx = (dataBufferIndex - i - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE;
      if (dataBuffer[idx].timestamp >= startTime && dataBuffer[idx].timestamp <= now) {
        float temp = dataBuffer[idx].temperatures[s];
        
        if (temp != DEVICE_DISCONNECTED_C) {
          // Calculate position on chart
          int x = chartX + chartWidth - ((now - dataBuffer[idx].timestamp) * chartWidth) / (now - startTime);
          int y = chartY + chartHeight - ((temp - minTemp) * chartHeight) / (maxTemp - minTemp);
          
          // Draw line segment
          if (prevTime != 0) {
            int prevX = chartX + chartWidth - ((now - prevTime) * chartWidth) / (now - startTime);
            int prevY = chartY + chartHeight - ((prevTemp - minTemp) * chartHeight) / (maxTemp - minTemp);
            tft.drawLine(prevX, prevY, x, y, colors[s]);
          }
          
          prevTime = dataBuffer[idx].timestamp;
          prevTemp = temp;
        } else {
          prevTime = 0; // Reset line when sensor is disconnected
        }
      }
    }
  }
  
  // Draw time range selector
  tft.fillRect(10, TFT_HEIGHT - 70, 80, 30, TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("<", 40, TFT_HEIGHT - 60, 4);
  
  String rangeStr;
  switch (currentTimeRange) {
    case ONE_HOUR: rangeStr = "1H"; break;
    case SIX_HOURS: rangeStr = "6H"; break;
    case TWENTY_FOUR_HOURS: rangeStr = "24H"; break;
  }
  tft.fillRect(100, TFT_HEIGHT - 70, 120, 30, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(rangeStr, 145, TFT_HEIGHT - 60, 4);
  
  tft.fillRect(TFT_WIDTH - 90, TFT_HEIGHT - 70, 80, 30, TFT_BLUE);
  tft.drawString(">", TFT_WIDTH - 60, TFT_HEIGHT - 60, 4);
  
  // Draw back button
  tft.fillRect(100, TFT_HEIGHT - 30, 120, 30, TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Back", 145, TFT_HEIGHT - 22, 2);
}

// Variables for table scrolling
int tableScrollOffset = 0;
int maxTableScroll = 0;

void drawTableScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw header
  tft.fillRect(0, 0, TFT_WIDTH, 30, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Temperature Data", 10, 8, 2);
  
  // Calculate maximum scroll based on data
  maxTableScroll = max(0, dataBufferCount - 10); // 10 rows visible at a time
  
  // Draw table headers
  tft.drawRect(0, 30, TFT_WIDTH, 30, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Time", 5, 38, 2);
  
  int colWidth = (TFT_WIDTH - 80) / max(1, sensorCount);
  for (int i = 0; i < sensorCount && i < 6; i++) { // Max 6 sensors in table
    String header = sensorNames[i];
    if (header.length() > 8) header = header.substring(0, 8);
    tft.drawString(header, 70 + i * colWidth, 38, 1);
  }
  
  // Draw table content
  for (int row = 0; row < 10; row++) { // Show 10 rows at a time
    int y = 60 + row * 30;
    tft.drawFastHLine(0, y, TFT_WIDTH, TFT_DARKGREY);
    
    int dataIndex = tableScrollOffset + row;
    if (dataIndex < dataBufferCount) {
      int idx = (dataBufferIndex - dataIndex - 1 + DATA_BUFFER_SIZE) % DATA_BUFFER_SIZE;
      struct tm *timeinfo = localtime(&dataBuffer[idx].timestamp);
      char timeStr[20];
      strftime(timeStr, sizeof(timeStr), "%m/%d %H:%M", timeinfo);
      tft.drawString(timeStr, 5, y + 8, 1);
      
      for (int i = 0; i < sensorCount && i < 6; i++) {
        if (dataBuffer[idx].temperatures[i] != DEVICE_DISCONNECTED_C) {
          String tempStr = String(dataBuffer[idx].temperatures[i], 1);
          tft.drawString(tempStr, 70 + i * colWidth, y + 8, 1);
        } else {
          tft.drawString("---", 70 + i * colWidth, y + 8, 1);
        }
      }
    }
  }
  
  // Draw scroll indicators
  if (tableScrollOffset > 0) {
    tft.fillTriangle(10, TFT_HEIGHT - 50, 20, TFT_HEIGHT - 40, 30, TFT_HEIGHT - 50, TFT_WHITE);
  }
  if (tableScrollOffset < maxTableScroll) {
    tft.fillTriangle(10, TFT_HEIGHT - 30, 20, TFT_HEIGHT - 40, 30, TFT_HEIGHT - 30, TFT_WHITE);
  }
  
  // Draw back button
  tft.fillRect(100, TFT_HEIGHT - 30, 120, 30, TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Back", 145, TFT_HEIGHT - 22, 2);
}

// Handle table scrolling
void handleTableScroll(int direction) {
  tableScrollOffset += direction;
  if (tableScrollOffset < 0) tableScrollOffset = 0;
  if (tableScrollOffset > maxTableScroll) tableScrollOffset = maxTableScroll;
}

// Settings screen variables
int settingsPage = 0; // 0=General, 1=WiFi, 2=Sensors, 3=Calibration
int selectedSensor = 0;
String tempSensorName = "";

void drawSettingsScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw header
  tft.fillRect(0, 0, TFT_WIDTH, 30, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Settings", 10, 8, 2);
  
  // Draw page navigation
  tft.fillRect(0, 30, TFT_WIDTH/4, 30, settingsPage == 0 ? TFT_BLUE : TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("General", 10, 38, 1);
  
  tft.fillRect(TFT_WIDTH/4, 30, TFT_WIDTH/4, 30, settingsPage == 1 ? TFT_BLUE : TFT_DARKGREY);
  tft.drawString("WiFi", TFT_WIDTH/4 + 20, 38, 1);
  
  tft.fillRect(TFT_WIDTH/2, 30, TFT_WIDTH/4, 30, settingsPage == 2 ? TFT_BLUE : TFT_DARKGREY);
  tft.drawString("Sensors", TFT_WIDTH/2 + 15, 38, 1);
  
  tft.fillRect(3*TFT_WIDTH/4, 30, TFT_WIDTH/4, 30, settingsPage == 3 ? TFT_BLUE : TFT_DARKGREY);
  tft.drawString("Calib", 3*TFT_WIDTH/4 + 15, 38, 1);
  
  // Draw settings based on current page
  switch(settingsPage) {
    case 0: // General settings
      tft.setTextColor(TFT_WHITE);
      tft.drawString("WiFi: " + String(wifiEnabled ? "ON" : "OFF"), 10, 70, 2);
      tft.drawString("Bluetooth: " + String(bluetoothEnabled ? "ON" : "OFF"), 10, 100, 2);
      tft.drawString("Sample Interval: " + String(sampleInterval) + "s", 10, 130, 2);
      tft.drawString("Deep Sleep: " + String(deepSleepEnabled ? "ON" : "OFF"), 10, 160, 2);
      
      // Draw option toggles
      tft.fillRect(220, 65, 60, 25, wifiEnabled ? TFT_GREEN : TFT_RED);
      tft.fillRect(220, 95, 60, 25, bluetoothEnabled ? TFT_GREEN : TFT_RED);
      tft.fillRect(220, 155, 60, 25, deepSleepEnabled ? TFT_GREEN : TFT_RED);
      
      // Sample interval selector
      tft.fillRect(220, 125, 20, 25, TFT_DARKGREY);
      tft.drawString("-", 225, 130, 2);
      tft.fillRect(260, 125, 20, 25, TFT_DARKGREY);
      tft.drawString("+", 265, 130, 2);
      break;
      
    case 1: // WiFi settings
      tft.setTextColor(TFT_WHITE);
      tft.drawString("SSID: " + ssid, 10, 70, 2);
      tft.drawString("Password: " + String(password.length(), DEC) + " chars", 10, 100, 2);
      tft.drawString("Hotspot: " + String(wifiEnabled ? "ON" : "OFF"), 10, 130, 2);
      
      // Draw edit buttons
      tft.fillRect(220, 65, 60, 25, TFT_BLUE);
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Edit", 235, 70, 1);
      
      tft.fillRect(220, 95, 60, 25, TFT_BLUE);
      tft.drawString("Edit", 235, 100, 1);
      
      tft.fillRect(220, 125, 60, 25, wifiEnabled ? TFT_GREEN : TFT_RED);
      break;
      
    case 2: // Sensor settings
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Sensors: " + String(sensorCount), 10, 70, 2);
      
      // Draw sensor list
      for (int i = 0; i < sensorCount && i < 6; i++) {
        int y = 100 + i * 30;
        tft.drawString((i == selectedSensor ? ">" : " ") + sensorNames[i], 10, y, 2);
      }
      
      // Draw sensor controls
      tft.fillRect(220, 65, 20, 25, TFT_DARKGREY);
      tft.drawString("-", 225, 70, 2);
      tft.fillRect(260, 65, 20, 25, TFT_DARKGREY);
      tft.drawString("+", 265, 70, 2);
      
      if (sensorCount > 0) {
        tft.fillRect(220, 95, 60, 25, TFT_BLUE);
        tft.drawString("Rename", 230, 100, 1);
      }
      break;
      
    case 3: // Calibration settings
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Touch Cal: MinX:" + String(touchCalMinX), 10, 70, 1);
      tft.drawString("MinY:" + String(touchCalMinY), 10, 90, 1);
      tft.drawString("MaxX:" + String(touchCalMaxX), 10, 110, 1);
      tft.drawString("MaxY:" + String(touchCalMaxY), 10, 130, 1);
      
      tft.drawString("Battery Cal: " + String(batteryVoltage, 2) + "V", 10, 160, 1);
      
      // Draw calibration buttons
      tft.fillRect(10, 200, 140, 30, TFT_BLUE);
      tft.drawString("Calibrate Touch", 20, 208, 1);
      
      tft.fillRect(170, 200, 140, 30, TFT_BLUE);
      tft.drawString("Calibrate Batt", 180, 208, 1);
      break;
  }
  
  // Draw back button
  tft.fillRect(100, TFT_HEIGHT - 30, 120, 30, TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Back", 145, TFT_HEIGHT - 22, 2);
}

// Function to handle settings changes
void changeSetting(int setting, int value) {
  switch(settingsPage) {
    case 0: // General settings
      switch(setting) {
        case 0: // WiFi toggle
          wifiEnabled = !wifiEnabled;
          if (wifiEnabled) {
            WiFi.begin();
          } else {
            WiFi.disconnect();
          }
          break;
        case 1: // Bluetooth toggle
          bluetoothEnabled = !bluetoothEnabled;
          // Bluetooth control would go here
          break;
        case 2: // Sample interval
          sampleInterval += value;
          if (sampleInterval < 10) sampleInterval = 10; // Minimum 10 seconds
          if (sampleInterval > 600) sampleInterval = 600; // Maximum 10 minutes
          break;
        case 3: // Deep sleep toggle
          deepSleepEnabled = !deepSleepEnabled;
          break;
      }
      break;
      
    case 2: // Sensor settings
      switch(setting) {
        case 0: // Change sensor count
          sensorCount += value;
          if (sensorCount < 0) sensorCount = 0;
          if (sensorCount > 10) sensorCount = 10;
          break;
        case 1: // Select sensor
          selectedSensor += value;
          if (selectedSensor < 0) selectedSensor = 0;
          if (selectedSensor >= sensorCount) selectedSensor = sensorCount - 1;
          break;
      }
      break;
  }
  
  saveSettings();
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  // Configure touch interrupt to wake up
  // This is a simplified version - actual implementation would depend on your touch controller
  // esp_sleep_enable_ext0_wakeup(GPIO_NUM_X, 1); // Enable wakeup on touch
  
  // Or use timer wakeup
  esp_sleep_enable_timer_wakeup(10 * 1000000); // Wake up after 10 seconds if needed
  
  esp_deep_sleep_start();
}

void IRAM_ATTR touchISR() {
  lastTouchTime = millis();
}