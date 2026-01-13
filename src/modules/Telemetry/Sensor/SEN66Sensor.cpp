#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include <SensirionI2cSen66.h>
#include "main.h"

// SEN66 sensor instance for direct I2C communication
static SensirionI2cSen66 sen66;

// Cache latest readings
static meshtastic_AirQualityMetrics latestMetrics = meshtastic_AirQualityMetrics_init_zero;
static bool hasValidData = false;
static bool sensorInitialized = false;

SEN66Sensor::SEN66Sensor() : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66")
{
}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (direct I2C)", sensorName);
    
    i2cBus = bus;
    
    // CRITICAL: Do NOT call Wire.begin(address) here!
    // The I2C bus is already initialized as MASTER in main.cpp
    // Calling Wire.begin(address) would make us an I2C SLAVE
    
    // Initialize SEN66 on the I2C bus at its default address
    sen66.begin(*i2cBus, SEN66_ADDR);
    
    // Optional: Reset sensor to known state
    uint16_t error = sen66.deviceReset();
    if (error) {
        char errorMsg[256];
        errorToString(error, errorMsg, sizeof(errorMsg));
        LOG_WARN("SEN66 reset failed: %s", errorMsg);
    } else {
        delay(100);  // Allow reset to complete
    }
    
    // Start continuous measurement
    error = sen66.startContinuousMeasurement();
    if (error) {
        char errorMsg[256];
        errorToString(error, errorMsg, sizeof(errorMsg));
        LOG_ERROR("SEN66 start measurement failed: %s", errorMsg);
        return false;
    }
    
    LOG_INFO("SEN66 measurement started successfully");
    sensorInitialized = true;
    
    return true;
}

int32_t SEN66Sensor::runOnce()
{
    // This runs in the Meshtastic scheduler thread, NOT in an ISR
    // Blocking calls are acceptable here (within reason)
    
    if (!sensorInitialized) {
        LOG_DEBUG("SEN66: Sensor not initialized, skipping read");
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    LOG_DEBUG("SEN66: Reading sensor data...");
    
    // Read all values from SEN66
    float massConcentrationPm1p0;
    float massConcentrationPm2p5;
    float massConcentrationPm4p0;
    float massConcentrationPm10p0;
    float ambientHumidity;
    float ambientTemperature;
    float vocIndex;
    float noxIndex;
    uint16_t co2;
    
    // This call takes ~100-150ms - that's OK in scheduler context
    // (Not OK in ISR, but we're NOT in an ISR!)
    uint16_t error = sen66.readMeasuredValues(
        massConcentrationPm1p0,
        massConcentrationPm2p5,
        massConcentrationPm4p0,
        massConcentrationPm10p0,
        ambientHumidity,
        ambientTemperature,
        vocIndex,
        noxIndex,
        co2
    );
    
    if (error) {
        char errorMsg[256];
        errorToString(error, errorMsg, sizeof(errorMsg));
        LOG_WARN("SEN66 read error: %s", errorMsg);
        hasValidData = false;
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    // Populate AirQualityMetrics
    latestMetrics = meshtastic_AirQualityMetrics_init_zero;
    
    // PM values (convert float µg/m³ to uint32_t)
    latestMetrics.has_pm10_environmental = true;
    latestMetrics.pm10_environmental = (uint32_t)(massConcentrationPm1p0 + 0.5f);
    
    latestMetrics.has_pm25_environmental = true;
    latestMetrics.pm25_environmental = (uint32_t)(massConcentrationPm2p5 + 0.5f);
    
    latestMetrics.has_pm40_standard = true;
    latestMetrics.pm40_standard = (uint32_t)(massConcentrationPm4p0 + 0.5f);
    
    latestMetrics.has_pm100_environmental = true;
    latestMetrics.pm100_environmental = (uint32_t)(massConcentrationPm10p0 + 0.5f);
    
    // VOC and NOx indices
    latestMetrics.has_pm_voc_idx = true;
    latestMetrics.pm_voc_idx = vocIndex;
    
    latestMetrics.has_pm_nox_idx = true;
    latestMetrics.pm_nox_idx = noxIndex;
    
    // CO2 (if available)
    if (co2 > 0) {
        latestMetrics.has_co2 = true;
        latestMetrics.co2 = (uint32_t)(co2 + 0.5f);
    }
    
    // Temperature and humidity
    latestMetrics.has_pm_temperature = true;
    latestMetrics.pm_temperature = ambientTemperature;
    
    latestMetrics.has_pm_humidity = true;
    latestMetrics.pm_humidity = ambientHumidity;
    
    hasValidData = true;
    
    LOG_INFO("SEN66: PM1.0=%u, PM2.5=%u, PM4.0=%u, PM10=%u, VOC=%.1f, NOx=%.1f, T=%.1f°C, RH=%.1f%%",
             latestMetrics.pm10_environmental,
             latestMetrics.pm25_environmental,
             latestMetrics.pm40_standard,
             latestMetrics.pm100_environmental,
             latestMetrics.pm_voc_idx,
             latestMetrics.pm_nox_idx,
             latestMetrics.pm_temperature,
             latestMetrics.pm_humidity);
    
    // Return interval before next read (10 seconds recommended for SEN66)
    return 10000; // 10 seconds
}

bool SEN66Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    if (!hasValidData) {
        LOG_DEBUG("SEN66: No valid data available");
        return false;
    }
    
    // Populate telemetry message
    measurement->which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    measurement->variant.air_quality_metrics = latestMetrics;
    
    return true;
}

#endif