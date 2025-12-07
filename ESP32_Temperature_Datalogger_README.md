# ESP32-035 Temperature Datalogger

A comprehensive temperature monitoring system using ESP32-035 board with 3.5" touchscreen display, multiple DS18B20 sensors, and data logging capabilities.

## Features

- **Multiple DS18B20 temperature sensors** on OneWire bus (up to 10 sensors)
- **Graphical temperature charts** with time ranges (1H, 6H, 24H)
- **Data table with scroll** functionality
- **Data storage** on SPIFFS with up to 30 days of data
- **Real-time clock** with NTP synchronization
- **Battery voltage measurement** and percentage display
- **WiFi/Bluetooth status and control** with toggle icons
- **Comprehensive settings screen** with multiple pages
- **Touch calibration** capability
- **Dual-core operation** for smooth performance
- **Power management** with deep sleep mode

## Hardware Requirements

- ESP32-035 board (3.5" ST7796 display with touch)
- DS18B20 temperature sensors (1 to 10 sensors)
- 4.7kΩ pull-up resistor for OneWire bus
- Li-ion battery (1100mAh recommended)
- Voltage divider circuit for battery monitoring

## Hardware Connections

### DS18B20 Sensors
- Connect all DS18B20 sensors in parallel:
  - VDD → 3.3V
  - GND → GND
  - Data → GPIO4 (with 4.7kΩ pull-up resistor to 3.3V)

### Battery Monitoring
- Connect battery voltage through voltage divider to GPIO36:
  - Battery + → Voltage divider input
  - Voltage divider output → GPIO36
  - Battery - → GND
- Voltage divider ratio: Adjust resistors to ensure voltage is within 0-3.3V range at max battery voltage

### Display
- The ESP32-035 board has the ST7796 display and touch controller built-in
- No additional connections needed for display

## Pin Configuration

- `ONE_WIRE_BUS` (DS18B20): GPIO4
- `BATTERY_ADC_PIN`: GPIO36
- `TFT_BL` (Backlight): GPIO5
- Display SPI pins are configured in User_Setup.h

## Software Setup

1. Install required libraries in Arduino IDE:
   - WiFi
   - OneWire
   - DallasTemperature
   - TFT_eSPI
   - SPIFFS
   - Preferences

2. Configure TFT_eSPI library:
   - Copy `User_Setup.h` to your TFT_eSPI library folder
   - This configures the ST7796 display for the ESP32-035 board

3. Upload the sketch to your ESP32-035 board

## User Interface

### Main Screen
- Shows current time and date
- Displays battery level with percentage
- Shows WiFi and Bluetooth status icons
- Presents current temperature readings from all sensors
- Navigation buttons to Chart, Table, and Settings screens

### Chart Screen
- Graphical representation of temperature data over time
- Three time range options: 1 hour, 6 hours, 24 hours
- Different colors for each sensor
- Automatic scaling of temperature range
- Time range selection buttons

### Table Screen
- Tabular view of temperature data
- Shows timestamp and temperature values
- Scrollable through historical data
- Up/down arrow indicators for scrolling

### Settings Screen
- **General**: WiFi, Bluetooth, sample interval, deep sleep
- **WiFi**: SSID, password, hotspot settings
- **Sensors**: Number of sensors, rename sensors
- **Calibration**: Touch screen and battery calibration

## Data Storage

- Temperature data is saved to SPIFFS
- Format: CSV with timestamp and temperature values
- Data retention: Up to 30 days with 1-minute sampling
- Circular buffer implementation to manage storage space

## Power Management

- Deep sleep mode after 1 minute of inactivity
- Configurable through settings (can be disabled)
- Wake on touch interrupt
- Low power modes for extended battery life

## Calibration

### Touch Calibration
- Access through Settings → Calibration → Calibrate Touch
- Follow on-screen instructions to calibrate touch points

### Battery Calibration
- Access through Settings → Calibration → Calibrate Battery
- Enter actual battery voltage when prompted to adjust readings

## Troubleshooting

- If touch is not responsive, perform touch calibration
- If battery readings are incorrect, perform battery calibration
- If sensors are not detected, check OneWire connections and pull-up resistor
- If WiFi fails to connect, verify credentials in settings

## Notes

- The system uses dual-core processing for optimal performance
- Core 0 handles sensor readings and data logging
- Core 1 handles display updates and user interface
- Sample interval can be adjusted from 10 seconds to 10 minutes
- Up to 10 DS18B20 sensors are supported with unique addresses