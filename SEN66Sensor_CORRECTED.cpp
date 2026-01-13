// SEN66Sensor.cpp - Corrected I2C Master Implementation
// Replace your existing src/modules/Telemetry/Sensor/SEN66Sensor.cpp with this

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include "main.h"
#include <pb_decode.h>

// I2C address of the Arduino slave device
#define MT_I2C_ADDRESS 0x11

// Cache of last received metrics
static meshtastic_AirQualityMetrics lastMetrics = meshtastic_AirQualityMetrics_init_zero;
static bool hasValidData = false;

// Protobuf decode helper function
bool proto_decode(const uint8_t *srcbuf, size_t srcbufsize, const pb_msgdesc_t *fields, void *dest_struct)
{
    pb_istream_t stream = pb_istream_from_buffer(srcbuf, srcbufsize);
    if (!pb_decode(&stream, fields, dest_struct)) {
        return false;
    }
    return true;
}

SEN66Sensor::SEN66Sensor() : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66")
{
    LOG_DEBUG("SEN66Sensor constructor called");
}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (I2C master polling mode)", sensorName);
    
    // Store the I2C bus reference
    i2cBus = bus;
    
    // IMPORTANT: Do NOT call Wire.begin(address) here!
    // The I2C bus is already initialized as MASTER in main.cpp
    // Calling Wire.begin(address) would reconfigure us as SLAVE
    
    // Verify the Arduino slave is present on the bus
    i2cBus->beginTransmission(MT_I2C_ADDRESS);
    uint8_t error = i2cBus->endTransmission();
    
    if (error != 0) {
        LOG_WARN("SEN66: Arduino slave not detected at 0x%02X (error %d)", MT_I2C_ADDRESS, error);
        LOG_WARN("SEN66: Make sure Arduino slave is powered and running");
        // Don't fail completely - slave might come online later
    } else {
        LOG_INFO("SEN66: Arduino slave detected at 0x%02X", MT_I2C_ADDRESS);
    }
    
    return true;
}

int32_t SEN66Sensor::runOnce()
{
    // This method is called periodically by the Meshtastic scheduler
    // It runs in the main thread context (NOT in an ISR), so blocking calls are acceptable
    
    LOG_DEBUG("SEN66: runOnce() - requesting data from Arduino slave");
    
    // Optional: Skip I2C requests during GPS initialization to avoid conflicts
    // Uncomment if you experience boot loops related to GPS timing
    /*
    extern bool GPSInitFinished;
    if (!GPSInitFinished) {
        LOG_DEBUG("SEN66: Skipping I2C request during GPS initialization");
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    */
    
    // Request data from Arduino slave
    // Maximum protobuf size for AirQualityMetrics is 72 bytes
    const size_t maxBytes = meshtastic_AirQualityMetrics_size;
    
    // Send I2C read request to slave
    size_t bytesReceived = i2cBus->requestFrom((uint8_t)MT_I2C_ADDRESS, (uint8_t)maxBytes);
    
    if (bytesReceived == 0) {
        LOG_DEBUG("SEN66: No response from Arduino slave at 0x%02X", MT_I2C_ADDRESS);
        LOG_DEBUG("SEN66: Check that Arduino is powered and slave sketch is running");
        hasValidData = false;
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    LOG_DEBUG("SEN66: Received %d bytes from slave", bytesReceived);
    
    // Read the response into a buffer with timeout
    uint8_t buffer[meshtastic_AirQualityMetrics_size];
    size_t bytesRead = 0;
    unsigned long startTime = millis();
    const unsigned long timeout = 100; // 100ms timeout
    
    while (bytesRead < bytesReceived && i2cBus->available() && (millis() - startTime) < timeout) {
        buffer[bytesRead++] = i2cBus->read();
    }
    
    if (bytesRead != bytesReceived) {
        LOG_WARN("SEN66: Incomplete I2C read - expected %d bytes, got %d", bytesReceived, bytesRead);
        hasValidData = false;
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    // Decode the protobuf message
    meshtastic_AirQualityMetrics receivedMetrics = meshtastic_AirQualityMetrics_init_zero;
    
    if (!proto_decode(buffer, bytesRead, meshtastic_AirQualityMetrics_fields, &receivedMetrics)) {
        LOG_WARN("SEN66: Failed to decode protobuf message");
        hasValidData = false;
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    // Successfully decoded - update cached metrics
    lastMetrics = receivedMetrics;
    hasValidData = true;
    
    LOG_INFO("SEN66: PM1.0=%u, PM2.5=%u, PM4.0=%u, PM10=%u, VOC=%.1f, NOx=%.1f, Temp=%.1f°C, RH=%.1f%%",
             lastMetrics.pm10_environmental,
             lastMetrics.pm25_environmental,
             lastMetrics.pm40_standard,
             lastMetrics.pm100_environmental,
             lastMetrics.pm_voc_idx,
             lastMetrics.pm_nox_idx,
             lastMetrics.pm_temperature,
             lastMetrics.pm_humidity);
    
    // Return the polling interval (1 second minimum between reads)
    // Adjust this based on your telemetry update frequency preference
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}

bool SEN66Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    // This method is called when Meshtastic wants to publish telemetry
    
    if (!hasValidData) {
        LOG_DEBUG("SEN66: No valid data available for telemetry");
        return false;
    }
    
    // Check that we have at least one valid metric
    if (!lastMetrics.has_pm25_environmental && !lastMetrics.has_pm10_environmental) {
        LOG_DEBUG("SEN66: Metrics exist but no PM values present");
        return false;
    }
    
    // Populate the telemetry message with air quality metrics
    measurement->which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    measurement->variant.air_quality_metrics = lastMetrics;
    
    LOG_DEBUG("SEN66: Provided metrics for telemetry publication");
    
    return true;
}

#endif
