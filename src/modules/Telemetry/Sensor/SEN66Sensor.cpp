#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include <SensirionI2cSen66.h>
#include "RTC.h"
#include "main.h"

SEN66Sensor::SEN66Sensor() : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66") {}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s", sensorName);
    sensor.begin(*bus, SEN66_I2C_ADDR_6B);
    
    // Device reset
    int16_t error = sensor.deviceReset();
    if (error != 0) {
        LOG_ERROR("SEN66 device reset failed: %d", error);
        return false;
    }
    delay(1200); // Wait for reset
    
    // Setup advanced features (temperature compensation, CO2 calibration)
    setupTemperatureCompensation();
    
    // Start continuous measurement
    error = sensor.startContinuousMeasurement();
    if (error != 0) {
        LOG_ERROR("SEN66 start measurement failed: %d", error);
        return false;
    }
    
    lastFanCleanTime = getTime() / 1000; // Initialize fan cleaning timer
    initI2CSensor();
    return true;
}

void SEN66Sensor::setupTemperatureCompensation()
{
    // Set temperature offset parameters for PCB heat compensation
    // The SEN66 automatically applies this compensation to all measurements
    // Parameters: offset (scaled by 200), slope (scaled by 10000), time_constant (seconds), slot (0-4)
    // Example: 0.5°C offset, 0.01 slope, 10s time constant, slot 0
    // Formula applied internally: T_compensated = T_raw + (slope * T_raw) + offset
    int16_t error = sensor.setTemperatureOffsetParameters(100, 100, 10, 0);
    if (error == 0) {
        LOG_INFO("SEN66 temperature compensation configured (offset=0.5°C, slope=0.01)");
    } else {
        LOG_WARN("SEN66 temperature compensation setup failed: %d", error);
    }
    
    // Optional: Set ambient pressure for CO2 compensation (default: 1013 hPa)
    // This affects CO2 sensor accuracy - set to your local pressure for best results
    // error = sensor.setAmbientPressure(1013);
    
    // Optional: CO2 automatic self-calibration is enabled by default
    // Disable if you need manual calibration control
}

void SEN66Sensor::checkDeviceStatus()
{
    SEN66DeviceStatus deviceStatus;
    int16_t error = sensor.readDeviceStatus(deviceStatus);
    if (error == 0) {
        if (deviceStatus.fanError) {
            LOG_WARN("SEN66: Fan error detected");
        }
        if (deviceStatus.rhtError) {
            LOG_WARN("SEN66: RH/T sensor error");
        }
        if (deviceStatus.gasError) {
            LOG_WARN("SEN66: Gas sensor (VOC/NOx) error");
        }
        if (deviceStatus.pmError) {
            LOG_WARN("SEN66: PM sensor error");
        }
        if (deviceStatus.co22Error) {
            LOG_WARN("SEN66: CO2 sensor error");
        }
        if (deviceStatus.fanSpeedWarning) {
            LOG_WARN("SEN66: Fan speed warning - consider running fan cleaning");
        }
    }
}

void SEN66Sensor::performFanCleaning()
{
    LOG_INFO("SEN66: Starting fan cleaning routine");
    int16_t error = sensor.startFanCleaning();
    if (error == 0) {
        LOG_INFO("SEN66: Fan cleaning initiated (10 seconds at max speed)");
        lastFanCleanTime = getTime() / 1000;
        // The fan will automatically stop after 10 seconds
    } else {
        LOG_ERROR("SEN66: Fan cleaning failed with error: %d", error);
    }
}

int32_t SEN66Sensor::runOnce()
{
    // Check device status periodically (every 100 reads ≈ 100 seconds)
    if ((readCount++ % 100) == 0) {
        checkDeviceStatus();
    }
    
    // Perform fan cleaning periodically (every 7 days to remove dust)
    // The fan keeps PM sensor accurate and extends device lifetime
    uint32_t currentTime = getTime() / 1000;
    if ((currentTime - lastFanCleanTime) > (FAN_CLEANING_INTERVAL_MS / 1000)) {
        performFanCleaning();
        // Return longer interval while fan cleaning is in progress
        return 10000; // 10 seconds
    }
    
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}

bool SEN66Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    float massConcentrationPm1p0 = 0.0;
    float massConcentrationPm2p5 = 0.0;
    float massConcentrationPm4p0 = 0.0;
    float massConcentrationPm10p0 = 0.0;
    float humidity = 0.0;
    float temperature = 0.0;
    float vocIndex = 0.0;
    float noxIndex = 0.0;
    uint16_t co2 = 0;
    
    int16_t error = sensor.readMeasuredValues(
        massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0,
        massConcentrationPm10p0, humidity, temperature, vocIndex, noxIndex, co2);
    
    if (error != 0) {
        LOG_ERROR("SEN66 read failed: %d", error);
        return false;
    }
    
    // Populate AirQualityMetrics
    measurement->which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    
    // PM measurements
    measurement->variant.air_quality_metrics.has_pm10_environmental = true;
    measurement->variant.air_quality_metrics.pm10_environmental = (uint32_t)massConcentrationPm1p0;
    
    measurement->variant.air_quality_metrics.has_pm25_environmental = true;
    measurement->variant.air_quality_metrics.pm25_environmental = (uint32_t)massConcentrationPm2p5;
    
    measurement->variant.air_quality_metrics.has_pm40_standard = true;
    measurement->variant.air_quality_metrics.pm40_standard = (uint32_t)massConcentrationPm4p0;
    
    measurement->variant.air_quality_metrics.has_pm100_environmental = true;
    measurement->variant.air_quality_metrics.pm100_environmental = (uint32_t)massConcentrationPm10p0;
    
    // VOC and NOx
    measurement->variant.air_quality_metrics.has_pm_voc_idx = true;
    measurement->variant.air_quality_metrics.pm_voc_idx = vocIndex;
    
    measurement->variant.air_quality_metrics.has_pm_nox_idx = true;
    measurement->variant.air_quality_metrics.pm_nox_idx = noxIndex;
    
    // CO2
    measurement->variant.air_quality_metrics.has_co2 = true;
    measurement->variant.air_quality_metrics.co2 = co2;
    
    // Temperature and humidity
    measurement->variant.air_quality_metrics.has_pm_temperature = true;
    measurement->variant.air_quality_metrics.pm_temperature = temperature;
    
    measurement->variant.air_quality_metrics.has_pm_humidity = true;
    measurement->variant.air_quality_metrics.pm_humidity = humidity;
    
    LOG_DEBUG("SEN66 metrics: PM1.0=%.1f, PM2.5=%.1f, PM4.0=%.1f, PM10.0=%.1f, VOC=%.1f, NOx=%.1f, CO2=%u, T=%.1f, RH=%.1f",
              massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0, massConcentrationPm10p0,
              vocIndex, noxIndex, co2, temperature, humidity);
    
    return true;
}

#endif