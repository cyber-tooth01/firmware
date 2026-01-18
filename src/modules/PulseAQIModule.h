#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

/**
 * Pulse AQI Module - Sends compact serialized SEN66 air quality data
 * 
 * Binary packet format (22 bytes total):
 *   uint32_t timestamp;       // Unix timestamp (4 bytes)
 *   uint16_t pm1_0;           // PM1.0 µg/m³ (2 bytes)
 *   uint16_t pm2_5;           // PM2.5 µg/m³ (2 bytes)
 *   uint16_t pm4_0;           // PM4.0 µg/m³ (2 bytes)
 *   uint16_t pm10;            // PM10 µg/m³ (2 bytes)
 *   uint16_t voc_idx;         // VOC index * 10 (2 bytes, divide by 10 for float)
 *   uint16_t nox_idx;         // NOx index * 10 (2 bytes, divide by 10 for float)
 *   int16_t  temperature;     // Temp °C * 100 (2 bytes, divide by 100 for float)
 *   uint16_t humidity;        // RH% * 100 (2 bytes, divide by 100 for float)
 *   uint16_t co2;             // CO2 ppm (2 bytes)
 */

#pragma pack(push, 1)
struct PulseAQIData {
    uint32_t timestamp;
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm4_0;
    uint16_t pm10;
    uint16_t voc_idx;
    uint16_t nox_idx;
    int16_t temperature;
    uint16_t humidity;
    uint16_t co2;
};
#pragma pack(pop)

class PulseAQIModule : private concurrency::OSThread
{
  private:
    uint32_t lastSentToMesh = 0;
    bool sensorAvailable = false;

    int32_t runOnce() override;
    bool sendAQIData();

  public:
    PulseAQIModule();
};

#endif
