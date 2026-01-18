#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "PulseAQIModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include "Default.h"
#include "main.h"
#include <SensirionI2cSen66.h>

#define PULSE_AQI_UPDATE_INTERVAL_MS 10000  // 10 seconds

// SEN66 sensor instance
static SensirionI2cSen66 sen66_pulseaqi;
static bool sen66_initialized = false;
static TwoWire *sen66_bus = nullptr;

PulseAQIModule::PulseAQIModule() : concurrency::OSThread("PulseAQI")
{
    // Check if SEN66 sensor is available on I2C bus
    if (nodeTelemetrySensorsMap[MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66].first > 0) {
        sensorAvailable = true;
        LOG_INFO("PulseAQI: SEN66 sensor detected");
    }
}

int32_t PulseAQIModule::runOnce()
{
    if (!sensorAvailable) {
        return disable();
    }

    // Initialize sensor on first run
    if (!sen66_initialized && nodeTelemetrySensorsMap[MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66].first > 0) {
        auto sensorInfo = nodeTelemetrySensorsMap[MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66];
        sen66_bus = sensorInfo.second;
        
        if (sen66_bus) {
            sen66_pulseaqi.begin(*sen66_bus, SEN66_ADDR);
            
            // Start continuous measurement
            uint16_t error = sen66_pulseaqi.startContinuousMeasurement();
            if (error) {
                char errorMsg[256];
                errorToString(error, errorMsg, sizeof(errorMsg));
                LOG_ERROR("PulseAQI: Failed to start SEN66: %s", errorMsg);
                return disable();
            }
            
            sen66_initialized = true;
            LOG_INFO("PulseAQI: SEN66 initialized successfully");
        }
    }

    // Send data to mesh
    uint32_t now = millis();
    if (sen66_initialized && (now - lastSentToMesh >= PULSE_AQI_UPDATE_INTERVAL_MS || lastSentToMesh == 0)) {
        if (sendAQIData()) {
            lastSentToMesh = now;
        }
    }

    return PULSE_AQI_UPDATE_INTERVAL_MS;
}

bool PulseAQIModule::sendAQIData()
{
    if (!sen66_initialized || !sen66_bus) {
        return false;
    }

    // Read sensor data
    float massConcentrationPm1p0;
    float massConcentrationPm2p5;
    float massConcentrationPm4p0;
    float massConcentrationPm10p0;
    float ambientHumidity;
    float ambientTemperature;
    float vocIndex;
    float noxIndex;
    uint16_t co2;

    uint16_t error = sen66_pulseaqi.readMeasuredValues(
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
        LOG_WARN("PulseAQI: Read error: %s", errorMsg);
        return false;
    }

    // Format as JSON text message for easy MQTT propagation
    char jsonMsg[200];
    snprintf(jsonMsg, sizeof(jsonMsg),
             "{\"pm1\":%u,\"pm25\":%u,\"pm4\":%u,\"pm10\":%u,\"voc\":%.1f,\"nox\":%.1f,\"t\":%.1f,\"rh\":%.1f,\"co2\":%u}",
             (uint16_t)(massConcentrationPm1p0 + 0.5f),
             (uint16_t)(massConcentrationPm2p5 + 0.5f),
             (uint16_t)(massConcentrationPm4p0 + 0.5f),
             (uint16_t)(massConcentrationPm10p0 + 0.5f),
             vocIndex,
             noxIndex,
             ambientTemperature,
             ambientHumidity,
             co2);

    // Create text message packet
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    p->channel = 0;  // Primary channel
    
    // Copy JSON string to payload
    strcpy((char *)p->decoded.payload.bytes, jsonMsg);
    p->decoded.payload.size = strlen(jsonMsg) + 1;

    LOG_INFO("PulseAQI: %s", jsonMsg);
    service->sendToMesh(p, RX_SRC_LOCAL, true);

    return true;
}

#endif
