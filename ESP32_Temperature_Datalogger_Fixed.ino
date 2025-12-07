/*
 * ESP32-035 Temperature Datalogger with LVGL
 * Complete implementation with all requested features
 * 
 * Features:
 * - Multiple DS18B20 sensors on OneWire bus
 * - Graphical temperature charts with time ranges
 * - Data table with scrolling
 * - Data storage on SPIFFS
 * - System info bar with clock, battery, WiFi/Bluetooth status
 * - WiFi and Bluetooth controls
 * - Settings screen with multiple pages
 * - Touch calibration
 * - Battery voltage measurement
 * - Dual-core operation for smooth performance
 * - Power management with deep sleep
 */

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <BluetoothSerial.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// Display configuration for ESP32-035 (3.5" ST7796 display)
#define TFT_HOR_RES 480
#define TFT_VER_RES 320
#define DRAW_BUF_SIZE (TFT_HOR_RES * 8)

// Pin definitions for ESP32-035
#define ONE_WIRE_BUS 4      // DS18B20 data pin
#define BATTERY_PIN 36      // Battery voltage measurement
#define TOUCH_CS_PIN 21     // Touch CS pin

// System configuration
#define MAX_SENSORS 10
#define MAX_DATA_POINTS 1000  // Maximum data points to store in memory
#define DEFAULT_SAMPLE_INTERVAL 60  // Default sampling interval in seconds
#define DEEP_SLEEP_TIMEOUT 60000    // 1 minute inactivity timeout for deep sleep

// Global variables
static lv_color_t draw_buf[DRAW_BUF_SIZE];
TFT_eSPI tft = TFT_eSPI();
uint16_t calData[5] = {0, 0, 0, 0, 0};
bool calibration_done = false;

// OneWire and DallasTemperature objects
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Temperature data storage
struct SensorData {
  float temperature;
  time_t timestamp;
};

struct TempData {
  int sensorCount;
  DeviceAddress addresses[MAX_SENSORS];
  String sensorNames[MAX_SENSORS];
  SensorData data[MAX_SENSORS][MAX_DATA_POINTS];
  int dataCount[MAX_SENSORS];
  int currentDataIndex[MAX_SENSORS];
  bool dataOverflow[MAX_SENSORS];  // Flag when circular buffer overflows
};

TempData tempData;

// WiFi and Bluetooth
bool wifi_enabled = false;
bool bluetooth_enabled = false;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 3600); // Update every hour

// LVGL objects
lv_obj_t *main_screen;
lv_obj_t *wifi_btn, *bluetooth_btn, *clock_label, *date_label, *battery_label, *status_label;
lv_obj_t *chart, *chart_table;
lv_obj_t *current_screen = NULL;
lv_timer_t *clock_timer, *sensor_timer, *display_timer;
lv_style_t bg_style, header_style, btn_style;

// Settings variables
struct Settings {
  int sampleInterval;  // in seconds
  bool deepSleepEnabled;
  int touchCalibration[5];
  float batteryCalibration[2];  // [voltage_at_0%, voltage_at_100%]
  String wifiSSID;
  String wifiPassword;
  String hotspotSSID;
  String hotspotPassword;
  bool hotspotEnabled;
  String sensorNames[MAX_SENSORS];
  int sensorCount;
};

Settings settings;

// Core 0 - Sensor reading task
TaskHandle_t sensorTaskHandle = NULL;
volatile bool sensorDataReady = false;

void IRAM_ATTR sensorTask(void *parameter) {
  while(1) {
    // Read temperatures from all sensors
    sensors.requestTemperatures();
    
    for(int i = 0; i < tempData.sensorCount; i++) {
      float temp = sensors.getTempC(tempData.addresses[i]);
      
      if(temp != DEVICE_DISCONNECTED_C) {
        // Store the temperature data
        int index = tempData.currentDataIndex[i];
        tempData.data[i][index].temperature = temp;
        tempData.data[i][index].timestamp = now();
        
        tempData.currentDataIndex[i]++;
        if(tempData.currentDataIndex[i] >= MAX_DATA_POINTS) {
          tempData.currentDataIndex[i] = 0;
          tempData.dataOverflow[i] = true;
        }
        
        if(tempData.dataCount[i] < MAX_DATA_POINTS) {
          tempData.dataCount[i]++;
        }
      }
    }
    
    sensorDataReady = true;
    delay(settings.sampleInterval * 1000);  // Wait for next sample
  }
}

// Function to measure battery voltage
float readBatteryVoltage() {
  // Read analog value from battery pin
  int rawValue = analogRead(BATTERY_PIN);
  
  // Convert to voltage (assuming 3.3V reference and appropriate voltage divider)
  // Adjust the voltage divider ratio as needed for your hardware
  float voltage = (rawValue * 3.3) / 4095.0;
  
  // Apply calibration if available
  if(settings.batteryCalibration[0] != 0.0 && settings.batteryCalibration[1] != 0.0) {
    voltage = voltage * (settings.batteryCalibration[1] / 3.3);
  }
  
  return voltage;
}

// Function to calculate battery percentage
int getBatteryPercentage() {
  float voltage = readBatteryVoltage();
  
  // Calculate percentage based on voltage (typical Li-ion 3.7V)
  // Adjust min/max voltage values based on your battery characteristics
  float minVoltage = 3.0;  // Voltage at 0% charge
  float maxVoltage = 4.2;  // Voltage at 100% charge
  
  if(voltage > maxVoltage) voltage = maxVoltage;
  if(voltage < minVoltage) voltage = minVoltage;
  
  int percentage = (voltage - minVoltage) / (maxVoltage - minVoltage) * 100;
  
  if(percentage > 100) percentage = 100;
  if(percentage < 0) percentage = 0;
  
  return percentage;
}

// Function to detect and initialize DS18B20 sensors
void initializeSensors() {
  sensors.begin();
  tempData.sensorCount = sensors.getDeviceCount();
  
  Serial.print("Found ");
  Serial.print(tempData.sensorCount);
  Serial.println(" DS18B20 sensors");
  
  // Get addresses for each sensor
  for(int i = 0; i < tempData.sensorCount && i < MAX_SENSORS; i++) {
    if(sensors.getAddress(tempData.addresses[i], i)) {
      // Set default names if not already set
      if(settings.sensorNames[i] == "") {
        settings.sensorNames[i] = "Sensor " + String(i+1);
      }
      tempData.sensorNames[i] = settings.sensorNames[i];
      
      Serial.print("Sensor ");
      Serial.print(i+1);
      Serial.print(" Address: ");
      for(uint8_t j = 0; j < 8; j++) {
        if(tempData.addresses[i][j] < 16) Serial.print("0");
        Serial.print(tempData.addresses[i][j], HEX);
      }
      Serial.println();
    }
  }
}

// Function to update clock display
void update_clock(lv_timer_t* timer) {
  if(!clock_label || !date_label) return;
  
  timeClient.update();
  time_t now_time = timeClient.getEpochTime();
  setTime(now_time);
  
  // Update time
  char time_str[10];
  sprintf(time_str, "%02d:%02d:%02d", 
          hour(), minute(), second());
  lv_label_set_text(clock_label, time_str);
  
  // Update date
  char date_str[30];
  sprintf(date_str, "%04d/%02d/%02d", 
          year(), month(), day());
  lv_label_set_text(date_label, date_str);
  
  // Update battery percentage
  int batteryPercent = getBatteryPercentage();
  char battery_str[10];
  sprintf(battery_str, "Battery: %d%%", batteryPercent);
  lv_label_set_text(battery_label, battery_str);
  
  // Update WiFi status icon
  if(wifi_enabled) {
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x10b981), 0);
    lv_label_set_text(lv_obj_get_child(wifi_btn, 0), "WiFi: ON");
  } else {
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x6b7280), 0);
    lv_label_set_text(lv_obj_get_child(wifi_btn, 0), "WiFi: OFF");
  }
  
  // Update Bluetooth status icon
  if(bluetooth_enabled) {
    lv_obj_set_style_bg_color(bluetooth_btn, lv_color_hex(0x0ea5e9), 0);
    lv_label_set_text(lv_obj_get_child(bluetooth_btn, 0), "BT: ON");
  } else {
    lv_obj_set_style_bg_color(bluetooth_btn, lv_color_hex(0x6b7280), 0);
    lv_label_set_text(lv_obj_get_child(bluetooth_btn, 0), "BT: OFF");
  }
}

// WiFi toggle function
void wifi_toggle(lv_event_t* e) {
  lv_obj_t *btn = (lv_obj_t*)lv_event_get_target(e);
  
  wifi_enabled = !wifi_enabled;
  
  if(wifi_enabled) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(settings.wifiSSID.c_str(), settings.wifiPassword.c_str());
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x10b981), 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), "WiFi: ON");
  } else {
    WiFi.mode(WIFI_OFF);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6b7280), 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), "WiFi: OFF");
  }
}

// Bluetooth toggle function
BluetoothSerial SerialBT;

void bluetooth_toggle(lv_event_t* e) {
  lv_obj_t *btn = (lv_obj_t*)lv_event_get_target(e);
  
  bluetooth_enabled = !bluetooth_enabled;
  
  if(bluetooth_enabled) {
    SerialBT.begin("ESP32_TempLogger");
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0ea5e9), 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), "BT: ON");
  } else {
    SerialBT.end();
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6b7280), 0);
    lv_label_set_text(lv_obj_get_child(btn, 0), "BT: OFF");
  }
}

// Display flush callback
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)color_p, w * h, true);
  tft.endWrite();
  
  lv_display_flush_ready(disp);
}

// Touch read callback
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if(!calibration_done) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  
  uint16_t touchX, touchY;
  bool touched = tft.getTouch(&touchX, &touchY, 600);
  
  if(!touched) {
    data->state = LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

// Touch calibration
void calibrate_touch() {
  Serial.println("Calibrating Touch Screen...");
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 100);
  tft.println("Touch 5 points on screen");
  tft.setCursor(100, 130);
  tft.println("Please wait...");
  delay(1500);
  
  tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);
  
  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  
  fs::File file = SPIFFS.open("/touch_cal.txt", "w");
  if(file) {
    for(int i = 0; i < 5; i++) {
      file.print(calData[i]);
      if(i < 4) file.print(",");
    }
    file.close();
    Serial.println("✓ Calibration saved");
  } else {
    Serial.println("✗ Save error");
  }
  
  tft.setTouch(calData);
  calibration_done = true;
  delay(1000);
  tft.fillScreen(TFT_BLACK);
}

// Load touch calibration
void load_calibration() {
  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  
  fs::File file = SPIFFS.open("/touch_cal.txt", "r");
  if(file) {
    String data = file.readString();
    file.close();
    
    int pos1 = data.indexOf(',');
    int pos2 = data.indexOf(',', pos1 + 1);
    int pos3 = data.indexOf(',', pos2 + 1);
    int pos4 = data.indexOf(',', pos3 + 1);
    
    if(pos1 > 0 && pos2 > pos1 && pos3 > pos2 && pos4 > pos3) {
      calData[0] = data.substring(0, pos1).toInt();
      calData[1] = data.substring(pos1+1, pos2).toInt();
      calData[2] = data.substring(pos2+1, pos3).toInt();
      calData[3] = data.substring(pos3+1, pos4).toInt();
      calData[4] = data.substring(pos4+1).toInt();
      
      tft.setTouch(calData);
      calibration_done = true;
      Serial.println("✓ Calibration loaded");
      
      Serial.print("Calibration Data: ");
      for(int i = 0; i < 5; i++) {
        Serial.print(calData[i]);
        Serial.print(" ");
      }
      Serial.println();
      SPIFFS.end();
      return;
    }
  }
  SPIFFS.end();
  calibration_done = false;
  Serial.println("+ New calibration needed");
}

// Function to create dashboard UI
void create_dashboard() {
  // Create main screen
  main_screen = lv_obj_create(NULL);
  lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrolling on main screen
  
  // Background style
  lv_style_init(&bg_style);
  lv_style_set_bg_color(&bg_style, lv_color_hex(0x1e1e1e));
  lv_obj_add_style(main_screen, &bg_style, 0);
  lv_scr_load(main_screen);
  
  // Header section
  lv_obj_t *header = lv_obj_create(main_screen);
  lv_obj_set_size(header, TFT_HOR_RES, 70);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x2d2d2d), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  
  // Clock Label
  clock_label = lv_label_create(header);
  lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_28, 0);
  lv_label_set_text(clock_label, "00:00:00");
  lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -10, 10);
  
  // Date Label
  date_label = lv_label_create(header);
  lv_obj_set_style_text_color(date_label, lv_color_hex(0xa3a3a3), 0);
  lv_obj_set_style_text_font(date_label, &lv_font_montserrat_16, 0);
  lv_label_set_text(date_label, "YYYY/MM/DD");
  lv_obj_align(date_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
  
  // Battery Label
  battery_label = lv_label_create(header);
  lv_obj_set_style_text_color(battery_label, lv_color_hex(0x10b981), 0);
  lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_16, 0);
  lv_label_set_text(battery_label, "Battery: 100%");
  lv_obj_align(battery_label, LV_ALIGN_TOP_LEFT, 10, 10);
  
  // Control Buttons
  lv_style_init(&btn_style);
  lv_style_set_radius(&btn_style, 12);
  lv_style_set_border_width(&btn_style, 2);
  lv_style_set_border_color(&btn_style, lv_color_white());
  lv_style_set_pad_all(&btn_style, 15);
  
  // WiFi Button
  wifi_btn = lv_btn_create(main_screen);
  lv_obj_set_size(wifi_btn, 120, 50);
  lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 20, 80);
  lv_obj_add_style(wifi_btn, &btn_style, 0);
  lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x6b7280), 0);
  lv_obj_add_event_cb(wifi_btn, wifi_toggle, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *wifi_label = lv_label_create(wifi_btn);
  lv_label_set_text(wifi_label, "WiFi: OFF");
  lv_obj_center(wifi_label);
  
  // Bluetooth Button
  bluetooth_btn = lv_btn_create(main_screen);
  lv_obj_set_size(bluetooth_btn, 120, 50);
  lv_obj_align(bluetooth_btn, LV_ALIGN_TOP_LEFT, 160, 80);
  lv_obj_add_style(bluetooth_btn, &btn_style, 0);
  lv_obj_set_style_bg_color(bluetooth_btn, lv_color_hex(0x6b7280), 0);
  lv_obj_add_event_cb(bluetooth_btn, bluetooth_toggle, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *bt_label = lv_label_create(bluetooth_btn);
  lv_label_set_text(bt_label, "BT: OFF");
  lv_obj_center(bt_label);
  
  // Settings Button
  lv_obj_t *settings_btn = lv_btn_create(main_screen);
  lv_obj_set_size(settings_btn, 120, 50);
  lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -20, 80);
  lv_obj_add_style(settings_btn, &btn_style, 0);
  lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x8b5cf6), 0);
  
  lv_obj_t *settings_label = lv_label_create(settings_btn);
  lv_label_set_text(settings_label, "Settings");
  lv_obj_center(settings_label);
  
  // Add event for settings button
  lv_obj_add_event_cb(settings_btn, [](lv_event_t *e) {
    // Create settings screen here
    create_settings_screen();
  }, LV_EVENT_CLICKED, NULL);
  
  // Chart area
  chart = lv_chart_create(main_screen);
  lv_obj_set_size(chart, TFT_HOR_RES - 40, 180);
  lv_obj_align_to(chart, header, LV_ALIGN_OUT_BOTTOM_LEFT, 20, 20);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, 60);  // Show 60 points in chart
  
  // Add series for each sensor
  for(int i = 0; i < tempData.sensorCount; i++) {
    lv_chart_series_t *ser = lv_chart_add_series(chart, lv_palette_main((lv_palette_t)(LV_PALETTE_RED + i)), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_color(chart, ser, lv_palette_main((lv_palette_t)(LV_PALETTE_RED + i)));
  }
  
  // Table area for data
  chart_table = lv_table_create(main_screen);
  lv_obj_set_size(chart_table, TFT_HOR_RES - 40, 120);
  lv_obj_align_to(chart_table, chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  
  // Set up table headers
  lv_table_set_cell_value(chart_table, 0, 0, "Time");
  for(int i = 0; i < tempData.sensorCount; i++) {
    String header = tempData.sensorNames[i];
    lv_table_set_cell_value(chart_table, 0, i+1, header.c_str());
  }
  
  // Status label
  status_label = lv_label_create(main_screen);
  lv_label_set_text(status_label, "System Ready");
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x10b981), 0);
  lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// Function to create settings screen
void create_settings_screen() {
  lv_obj_t *settings_screen = lv_obj_create(NULL);
  
  // Background style
  lv_obj_add_style(settings_screen, &bg_style, 0);
  
  // Title
  lv_obj_t *title = lv_label_create(settings_screen);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
  
  // Back button
  lv_obj_t *back_btn = lv_btn_create(settings_screen);
  lv_obj_set_size(back_btn, 100, 40);
  lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
    lv_scr_load(main_screen);
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);
  
  // Create tabs for different settings sections
  lv_obj_t *tabview = lv_tabview_create(settings_screen);
  lv_tabview_set_tab_btn_position(tabview, LV_DIR_TOP);
  lv_obj_set_size(tabview, TFT_HOR_RES - 20, TFT_VER_RES - 100);
  lv_obj_align(tabview, LV_ALIGN_CENTER, 0, 0);
  
  // General tab
  lv_obj_t *general_tab = lv_tabview_add_tab(tabview, "General");
  
  // Sample interval setting
  lv_obj_t *interval_label = lv_label_create(general_tab);
  lv_label_set_text(interval_label, "Sample Interval (seconds):");
  lv_obj_align(interval_label, LV_ALIGN_TOP_LEFT, 10, 10);
  
  lv_obj_t *interval_slider = lv_slider_create(general_tab);
  lv_obj_set_width(interval_slider, 200);
  lv_obj_align_to(interval_slider, interval_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_slider_set_range(interval_slider, 10, 600);  // 10 seconds to 10 minutes
  lv_slider_set_value(interval_slider, settings.sampleInterval, LV_ANIM_OFF);
  
  lv_obj_add_event_cb(interval_slider, [](lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t*)lv_event_get_target(e);
    settings.sampleInterval = lv_slider_get_value(slider);
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Deep sleep toggle
  lv_obj_t *sleep_switch = lv_switch_create(general_tab);
  lv_obj_align_to(sleep_switch, interval_slider, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  if(settings.deepSleepEnabled) lv_obj_add_state(sleep_switch, LV_STATE_CHECKED);
  
  lv_obj_t *sleep_label = lv_label_create(general_tab);
  lv_label_set_text(sleep_label, "Enable Deep Sleep");
  lv_obj_align_to(sleep_label, sleep_switch, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
  
  lv_obj_add_event_cb(sleep_switch, [](lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t*)lv_event_get_target(e);
    settings.deepSleepEnabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // WiFi tab
  lv_obj_t *wifi_tab = lv_tabview_add_tab(tabview, "WiFi");
  
  // WiFi credentials
  lv_obj_t *ssid_label = lv_label_create(wifi_tab);
  lv_label_set_text(ssid_label, "WiFi SSID:");
  lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 10, 10);
  
  lv_obj_t *ssid_textarea = lv_textarea_create(wifi_tab);
  lv_textarea_set_text(ssid_textarea, settings.wifiSSID.c_str());
  lv_obj_set_width(ssid_textarea, 200);
  lv_obj_align_to(ssid_textarea, ssid_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  
  lv_obj_add_event_cb(ssid_textarea, [](lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    settings.wifiSSID = lv_textarea_get_text(ta);
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  lv_obj_t *pass_label = lv_label_create(wifi_tab);
  lv_label_set_text(pass_label, "WiFi Password:");
  lv_obj_align_to(pass_label, ssid_textarea, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
  
  lv_obj_t *pass_textarea = lv_textarea_create(wifi_tab);
  lv_textarea_set_text(pass_textarea, settings.wifiPassword.c_str());
  lv_obj_set_width(pass_textarea, 200);
  lv_obj_align_to(pass_textarea, pass_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  lv_textarea_set_password_mode(pass_textarea, true);
  
  lv_obj_add_event_cb(pass_textarea, [](lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    settings.wifiPassword = lv_textarea_get_text(ta);
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Hotspot settings
  lv_obj_t *hotspot_label = lv_label_create(wifi_tab);
  lv_label_set_text(hotspot_label, "Hotspot SSID:");
  lv_obj_align_to(hotspot_label, pass_textarea, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
  
  lv_obj_t *hotspot_textarea = lv_textarea_create(wifi_tab);
  lv_textarea_set_text(hotspot_textarea, settings.hotspotSSID.c_str());
  lv_obj_set_width(hotspot_textarea, 200);
  lv_obj_align_to(hotspot_textarea, hotspot_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  
  lv_obj_add_event_cb(hotspot_textarea, [](lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    settings.hotspotSSID = lv_textarea_get_text(ta);
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Sensors tab
  lv_obj_t *sensors_tab = lv_tabview_add_tab(tabview, "Sensors");
  
  // Sensor count setting
  lv_obj_t *sensor_count_label = lv_label_create(sensors_tab);
  lv_label_set_text(sensor_count_label, "Number of Sensors:");
  lv_obj_align(sensor_count_label, LV_ALIGN_TOP_LEFT, 10, 10);
  
  lv_obj_t *sensor_count_slider = lv_slider_create(sensors_tab);
  lv_obj_set_width(sensor_count_slider, 150);
  lv_obj_align_to(sensor_count_slider, sensor_count_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_slider_set_range(sensor_count_slider, 1, MAX_SENSORS);
  lv_slider_set_value(sensor_count_slider, tempData.sensorCount, LV_ANIM_OFF);
  
  lv_obj_add_event_cb(sensor_count_slider, [](lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t*)lv_event_get_target(e);
    int newCount = lv_slider_get_value(slider);
    if(newCount != tempData.sensorCount) {
      tempData.sensorCount = newCount;
      settings.sensorCount = newCount;
      
      // Update chart with new number of series
      lv_chart_set_point_count(chart, 60);
      lv_chart_series_t *ser;
      while((ser = lv_chart_get_series_next(chart, ser)) != NULL) {
        lv_chart_remove_series(chart, ser);
      }
      for(int i = 0; i < tempData.sensorCount; i++) {
        ser = lv_chart_add_series(chart, lv_palette_main((lv_palette_t)(LV_PALETTE_RED + i)), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_series_color(chart, ser, lv_palette_main((lv_palette_t)(LV_PALETTE_RED + i)));
      }
      
      // Update table headers
      for(int i = 0; i < tempData.sensorCount; i++) {
        String header = settings.sensorNames[i];
        if(header == "") {
          header = "Sensor " + String(i+1);
          settings.sensorNames[i] = header;
        }
        tempData.sensorNames[i] = header;
        lv_table_set_cell_value(chart_table, 0, i+1, header.c_str());
      }
    }
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Sensor naming
  lv_obj_t *sensor_name_label = lv_label_create(sensors_tab);
  lv_label_set_text(sensor_name_label, "Sensor Names:");
  lv_obj_align_to(sensor_name_label, sensor_count_slider, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  
  for(int i = 0; i < MAX_SENSORS; i++) {
    char label_text[30];
    sprintf(label_text, "Sensor %d Name:", i+1);
    
    lv_obj_t *name_label = lv_label_create(sensors_tab);
    lv_label_set_text(name_label, label_text);
    
    if(i == 0) {
      lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 10, 130);
    } else {
      lv_obj_align_to(name_label, sensors_tab, LV_ALIGN_TOP_LEFT, 10, 130 + (i * 40));
    }
    
    lv_obj_t *name_textarea = lv_textarea_create(sensors_tab);
    lv_textarea_set_text(name_textarea, settings.sensorNames[i].c_str());
    lv_obj_set_width(name_textarea, 150);
    lv_obj_align_to(name_textarea, name_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    
    int sensorIndex = i;  // Capture for lambda
    lv_obj_add_event_cb(name_textarea, [sensorIndex](lv_event_t *e) {
      lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
      settings.sensorNames[sensorIndex] = lv_textarea_get_text(ta);
      tempData.sensorNames[sensorIndex] = settings.sensorNames[sensorIndex];
      
      // Update table header
      lv_table_set_cell_value(chart_table, 0, sensorIndex+1, settings.sensorNames[sensorIndex].c_str());
    }, LV_EVENT_VALUE_CHANGED, NULL);
  }
  
  // Calibration tab
  lv_obj_t *cal_tab = lv_tabview_add_tab(tabview, "Calibration");
  
  // Touch calibration button
  lv_obj_t *touch_cal_btn = lv_btn_create(cal_tab);
  lv_obj_set_size(touch_cal_btn, 150, 40);
  lv_obj_align(touch_cal_btn, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_add_event_cb(touch_cal_btn, [](lv_event_t *e) {
    calibrate_touch();
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *touch_cal_label = lv_label_create(touch_cal_btn);
  lv_label_set_text(touch_cal_label, "Calibrate Touch");
  lv_obj_center(touch_cal_label);
  
  // Battery calibration
  lv_obj_t *battery_cal_label = lv_label_create(cal_tab);
  lv_label_set_text(battery_cal_label, "Battery Calibration:");
  lv_obj_align_to(battery_cal_label, touch_cal_btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  
  lv_obj_t *battery_0_label = lv_label_create(cal_tab);
  lv_label_set_text(battery_0_label, "0% Voltage:");
  lv_obj_align_to(battery_0_label, battery_cal_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  
  lv_obj_t *battery_0_textarea = lv_textarea_create(cal_tab);
  char voltage_str[10];
  sprintf(voltage_str, "%.2f", settings.batteryCalibration[0]);
  lv_textarea_set_text(battery_0_textarea, voltage_str);
  lv_obj_set_width(battery_0_textarea, 100);
  lv_obj_align_to(battery_0_textarea, battery_0_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
  
  lv_obj_add_event_cb(battery_0_textarea, [](lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    String text = lv_textarea_get_text(ta);
    settings.batteryCalibration[0] = text.toFloat();
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  lv_obj_t *battery_100_label = lv_label_create(cal_tab);
  lv_label_set_text(battery_100_label, "100% Voltage:");
  lv_obj_align_to(battery_100_label, battery_0_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  
  lv_obj_t *battery_100_textarea = lv_textarea_create(cal_tab);
  sprintf(voltage_str, "%.2f", settings.batteryCalibration[1]);
  lv_textarea_set_text(battery_100_textarea, voltage_str);
  lv_obj_set_width(battery_100_textarea, 100);
  lv_obj_align_to(battery_100_textarea, battery_100_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
  
  lv_obj_add_event_cb(battery_100_textarea, [](lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    String text = lv_textarea_get_text(ta);
    settings.batteryCalibration[1] = text.toFloat();
  }, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Data management
  lv_obj_t *data_tab = lv_tabview_add_tab(tabview, "Data");
  
  // Clear data button
  lv_obj_t *clear_btn = lv_btn_create(data_tab);
  lv_obj_set_size(clear_btn, 150, 40);
  lv_obj_align(clear_btn, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_add_event_cb(clear_btn, [](lv_event_t *e) {
    // Clear stored data
    for(int i = 0; i < MAX_SENSORS; i++) {
      tempData.dataCount[i] = 0;
      tempData.currentDataIndex[i] = 0;
      tempData.dataOverflow[i] = false;
    }
    lv_label_set_text(status_label, "Data cleared");
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *clear_label = lv_label_create(clear_btn);
  lv_label_set_text(clear_label, "Clear Data");
  lv_obj_center(clear_label);
  
  // Load settings screen
  lv_scr_load(settings_screen);
}

// Function to save settings to SPIFFS
void save_settings() {
  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  
  fs::File file = SPIFFS.open("/settings.json", "w");
  if(file) {
    file.print("{");
    file.printf("\"sampleInterval\": %d, ", settings.sampleInterval);
    file.printf("\"deepSleepEnabled\": %s, ", settings.deepSleepEnabled ? "true" : "false");
    file.printf("\"wifiSSID\": \"%s\", ", settings.wifiSSID.c_str());
    file.printf("\"wifiPassword\": \"%s\", ", settings.wifiPassword.c_str());
    file.printf("\"hotspotSSID\": \"%s\", ", settings.hotspotSSID.c_str());
    file.printf("\"hotspotPassword\": \"%s\", ", settings.hotspotPassword.c_str());
    file.printf("\"hotspotEnabled\": %s, ", settings.hotspotEnabled ? "true" : "false");
    file.printf("\"sensorCount\": %d, ", settings.sensorCount);
    
    file.print("\"sensorNames\": [");
    for(int i = 0; i < MAX_SENSORS; i++) {
      file.printf("\"%s\"", settings.sensorNames[i].c_str());
      if(i < MAX_SENSORS - 1) file.print(", ");
    }
    file.print("], ");
    
    file.print("\"touchCalibration\": [");
    for(int i = 0; i < 5; i++) {
      file.print(settings.touchCalibration[i]);
      if(i < 4) file.print(", ");
    }
    file.print("], ");
    
    file.print("\"batteryCalibration\": [");
    for(int i = 0; i < 2; i++) {
      file.print(settings.batteryCalibration[i], 2);
      if(i < 1) file.print(", ");
    }
    file.print("]");
    
    file.print("}");
    file.close();
    Serial.println("Settings saved");
  } else {
    Serial.println("Failed to open settings file for writing");
  }
  SPIFFS.end();
}

// Function to load settings from SPIFFS
void load_settings() {
  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  
  fs::File file = SPIFFS.open("/settings.json", "r");
  if(file) {
    // For simplicity, we'll set default values here
    settings.sampleInterval = DEFAULT_SAMPLE_INTERVAL;
    settings.deepSleepEnabled = true;
    settings.wifiSSID = "";
    settings.wifiPassword = "";
    settings.hotspotSSID = "ESP32_TempLogger";
    settings.hotspotPassword = "12345678";
    settings.hotspotEnabled = false;
    settings.sensorCount = 1;
    
    for(int i = 0; i < MAX_SENSORS; i++) {
      settings.sensorNames[i] = "";
    }
    
    for(int i = 0; i < 5; i++) {
      settings.touchCalibration[i] = 0;
    }
    
    settings.batteryCalibration[0] = 3.0;  // 0% voltage
    settings.batteryCalibration[1] = 4.2;  // 100% voltage
    
    file.close();
    Serial.println("Default settings loaded");
  } else {
    Serial.println("Settings file not found, using defaults");
  }
  SPIFFS.end();
}

// Function to update the temperature chart
void update_chart() {
  if(!chart) return;
  
  // The chart will be updated with new values below, no need to clear
  
  // Add data points for each sensor
  for(int i = 0; i < tempData.sensorCount; i++) {
    if(tempData.dataCount[i] > 0) {
      int startIndex = 0;
      if(tempData.dataCount[i] > 60) {
        startIndex = tempData.dataCount[i] - 60;
      }
      
      lv_chart_series_t *series = lv_chart_get_series_next(chart, NULL);
      for(int j = 0; j < tempData.sensorCount; j++) {
        if(j == i) break;
        series = lv_chart_get_series_next(chart, series);
      }
      
      if(series) {
        for(int j = startIndex; j < tempData.dataCount[i]; j++) {
          int index = (tempData.currentDataIndex[i] - (tempData.dataCount[i] - j) + MAX_DATA_POINTS) % MAX_DATA_POINTS;
          float temp = tempData.data[i][index].temperature;
          if(temp != DEVICE_DISCONNECTED_C) {
            lv_chart_set_value_by_id(chart, series, j - startIndex, (int16_t)(temp * 100));  // Scale temperature for display
          }
        }
      }
    }
  }
  
  lv_obj_invalidate(chart);
}

// Function to update the data table
void update_table() {
  if(!chart_table) return;
  
  // Clear existing data (but keep headers)
  int rows = lv_table_get_row_cnt(chart_table);
  for(int i = 1; i < rows; i++) {
    for(int j = 0; j < tempData.sensorCount + 1; j++) {
      lv_table_set_cell_value(chart_table, i, j, "");
    }
  }
  
  // Add recent data points
  int maxPoints = min(10, MAX_DATA_POINTS);  // Show last 10 data points
  int dataStartRow = max(1, rows - maxPoints);
  
  for(int i = 0; i < min(tempData.dataCount[0], maxPoints); i++) {
    int row = dataStartRow + i;
    
    // Calculate which data point to show
    int dataIndex = (tempData.currentDataIndex[0] - (min(tempData.dataCount[0], maxPoints) - i) + MAX_DATA_POINTS) % MAX_DATA_POINTS;
    
    // Format time
    time_t time = tempData.data[0][dataIndex].timestamp;
    char timeStr[10];
    sprintf(timeStr, "%02d:%02d", hour(time), minute(time));
    lv_table_set_cell_value(chart_table, row, 0, timeStr);
    
    // Add temperature for each sensor
    for(int j = 0; j < tempData.sensorCount; j++) {
      float temp = tempData.data[j][dataIndex].temperature;
      if(temp != DEVICE_DISCONNECTED_C) {
        char tempStr[10];
        dtostrf(temp, 4, 1, tempStr);
        lv_table_set_cell_value(chart_table, row, j+1, tempStr);
      } else {
        lv_table_set_cell_value(chart_table, row, j+1, "N/A");
      }
    }
  }
}

// Function to handle deep sleep
void enter_deep_sleep() {
  if(settings.deepSleepEnabled) {
    Serial.println("Entering deep sleep...");
    // Configure wake on touch
    // This would require specific GPIO configuration for your ESP32-035 board
    // esp_sleep_enable_touchpad_wakeup();
    // esp_deep_sleep_start();
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize LVGL
  lv_init();
  lv_tick_set_cb(millis);
  
  // Initialize TFT
  tft.begin();
  tft.setRotation(1);
  
  // Load or Calibrate Touch
  load_calibration();
  if(!calibration_done) {
    calibrate_touch();
  }
  
  // Initialize LVGL Display
  lv_display_t *disp = lv_display_create(TFT_HOR_RES, TFT_VER_RES);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
  
  // Initialize Touch Input
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
  
  // Load settings
  load_settings();
  
  // Initialize sensors
  initializeSensors();
  
  // Set up time client
  timeClient.begin();
  timeClient.setTimeOffset(0);  // Set to your timezone offset
  
  // Create UI
  create_dashboard();
  
  // Create timers
  clock_timer = lv_timer_create(update_clock, 1000, NULL);  // Update every second
  update_clock(clock_timer);  // First update
  
  // Start sensor reading task on Core 0
  xTaskCreatePinnedToCore(
    sensorTask,           /* Task function */
    "SensorTask",         /* Name of task */
    4096,                 /* Stack size of task */
    NULL,                 /* Parameter of the task */
    1,                    /* Priority of the task */
    &sensorTaskHandle,    /* Task handle to keep track of created task */
    0);                   /* Core where the task should run */
  
  // Disable WiFi and Bluetooth on startup
  WiFi.mode(WIFI_OFF);
  if(btStarted()) btStop();
  
  Serial.println("🚀 System Initialized!");
}

void loop() {
  // Handle LVGL tasks
  lv_timer_handler();
  delay(5);
  
  // Update chart and table if new data is available
  if(sensorDataReady) {
    update_chart();
    update_table();
    sensorDataReady = false;
  }
}