#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include <SensirionI2cSen66.h>
#include "RTC.h"
#include "main.h"
#include <pb_decode.h>

// I2C slave address for Meshtastic device
#define MT_I2C_ADDRESS 0x11

// Last received metrics from I2C master (sensor board)
static meshtastic_AirQualityMetrics lastMetrics = meshtastic_AirQualityMetrics_init_zero;
static bool hasNewData = false;

// Protobuf decode helper
bool proto_decode(const uint8_t *srcbuf, size_t srcbufsize, const pb_msgdesc_t *fields, void *dest_struct)
{
    pb_istream_t stream = pb_istream_from_buffer(srcbuf, srcbufsize);
    if (!pb_decode(&stream, fields, dest_struct)) {
        return false;
    } else {
        return true;
    }
}

// I2C receive callback - called when sensor board sends data
void onReceiveSEN66Metrics(int length)
{
    uint8_t buffer[meshtastic_AirQualityMetrics_size];
    Wire.readBytes(buffer, length);

    meshtastic_AirQualityMetrics received = meshtastic_AirQualityMetrics_init_zero;
    if (proto_decode(buffer, length, meshtastic_AirQualityMetrics_fields, &received)) {
        lastMetrics = received;
        hasNewData = true;
    }
}

SEN66Sensor::SEN66Sensor() : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66") {}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (I2C slave mode)", sensorName);

    // Store the bus for later use
    i2cBus = bus;

    // Initialize as I2C slave to receive data from sensor board
    i2cBus->begin(MT_I2C_ADDRESS);
    i2cBus->onReceive(onReceiveSEN66Metrics);

    LOG_INFO("SEN66 I2C slave initialized at address 0x%x", MT_I2C_ADDRESS);
    return true;
}

int32_t SEN66Sensor::runOnce()
{
    // No periodic tasks needed for I2C slave mode
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}

bool SEN66Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    if (!hasNewData) {
        LOG_DEBUG("SEN66: No new data received from sensor board");
        return false;
    }

    // Copy the received metrics
    measurement->which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    measurement->variant.air_quality_metrics = lastMetrics;

    // Reset the new data flag
    hasNewData = false;

    LOG_DEBUG("SEN66 metrics received: PM1.0=%u, PM2.5=%u, PM4.0=%u, PM10.0=%u, VOC=%.1f, NOx=%.1f, CO2=%u, T=%.1f, RH=%.1f",
              lastMetrics.pm10_environmental, lastMetrics.pm25_environmental, lastMetrics.pm40_standard,
              lastMetrics.pm100_environmental, lastMetrics.pm_voc_idx, lastMetrics.pm_nox_idx,
              lastMetrics.co2, lastMetrics.pm_temperature, lastMetrics.pm_humidity);

    return true;
}

#endif