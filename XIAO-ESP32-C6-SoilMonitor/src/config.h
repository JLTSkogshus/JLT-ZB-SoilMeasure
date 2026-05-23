// Configuration file for XIAO ESP32-C6 Soil Monitor

#ifndef CONFIG_H
#define CONFIG_H

// Zigbee Configuration
#define ZIGBEE_NETWORK_ID 0x1234 // Replace with your Zigbee network ID
#define ZIGBEE_DEVICE_ID 0x5678 // Replace with your Zigbee device ID

// Sensor Configuration
#define MAX_SENSORS 9

// Battery Configuration
#define BATTERY_PIN A0
#define BATTERY_FULL_VOLTAGE 4.2 // Full battery voltage
#define BATTERY_EMPTY_VOLTAGE 3.0 // Empty battery voltage

// Sleep Configuration
#define SLEEP_TIME_MS 60000 // Sleep time in milliseconds

#endif