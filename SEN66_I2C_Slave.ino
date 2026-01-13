/*
 * SEN66 I2C Slave for Meshtastic Telemetry
 * 
 * Based on: https://github.com/meshtastic/i2c-sensor
 * 
 * This sketch implements an I2C SLAVE that:
 * 1. Reads SEN66 air quality sensor data
 * 2. Encodes to Meshtastic AirQualityMetrics protobuf
 * 3. Responds to I2C requests from RAK4631 master
 * 
 * Hardware:
 * - Arduino/KB2040/compatible MCU as I2C slave
 * - SEN66 connected to this board via I2C
 * - This board connected to RAK4631 via I2C (SDA/SCL)
 * - IMPORTANT: Use 3.3V logic levels (or level shifters if 5V board)
 * 
 * I2C Address: 0x11 (MT_I2C_ADDRESS)
 */

#include <Wire.h>
#include <SensirionI2cSen66.h>
#include <pb_encode.h>
#include "telemetry.pb.h"  // Copy from https://github.com/meshtastic/i2c-sensor/tree/main/src/generated

// I2C slave address - must match Meshtastic firmware
#define MT_I2C_ADDRESS 0x11

// SEN66 sensor instance
SensirionI2cSen66 sen66;

// Latest encoded protobuf data (ready to send)
uint8_t encodedBuffer[meshtastic_AirQualityMetrics_size];
size_t encodedSize = 0;
bool dataReady = false;

// Latest metrics
meshtastic_AirQualityMetrics latestMetrics = meshtastic_AirQualityMetrics_init_zero;

// Protobuf encoding helper
size_t proto_encode(uint8_t *destbuf, size_t destbufsize, const pb_msgdesc_t *fields, const void *src_struct)
{
    pb_ostream_t stream = pb_ostream_from_buffer(destbuf, destbufsize);
    if (!pb_encode(&stream, fields, src_struct)) {
        return 0;
    } else {
        return stream.bytes_written;
    }
}

// I2C request handler - called when master (RAK4631) requests data
// CRITICAL: Must be FAST and NON-BLOCKING (runs in ISR context)
void onRequest()
{
    if (dataReady && encodedSize > 0) {
        // Send pre-encoded protobuf data
        Wire.write(encodedBuffer, encodedSize);
        Serial.print("→ Sent ");
        Serial.print(encodedSize);
        Serial.println(" bytes");
    } else {
        // No data available yet
        Serial.println("⚠ Master requested data but none ready");
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(100);
    }

    Serial.println("\n================================");
    Serial.println("SEN66 I2C Slave for Meshtastic");
    Serial.println("================================\n");

    // Initialize as I2C SLAVE at address 0x11
    Wire.begin(MT_I2C_ADDRESS);
    Wire.onRequest(onRequest);
    
    Serial.print("✓ I2C slave initialized at 0x");
    Serial.println(MT_I2C_ADDRESS, HEX);

    // Initialize SEN66 on I2C bus
    sen66.begin(Wire);

    // Start measurement
    uint16_t error;
    char errorMessage[256];
    
    error = sen66.startMeasurement();
    if (error) {
        Serial.print("✗ SEN66 start failed: ");
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(errorMessage);
        Serial.println("  Check SEN66 connections and address (0x6B)");
    } else {
        Serial.println("✓ SEN66 measurement started");
    }

    Serial.println("\n→ Ready to respond to I2C master requests\n");
}

void loop() {
    static unsigned long lastRead = 0;
    const unsigned long READ_INTERVAL_MS = 10000; // 10 seconds (SEN66 recommended)
    
    unsigned long now = millis();

    // Read SEN66 periodically in main loop (NOT in ISR!)
    if (now - lastRead >= READ_INTERVAL_MS) {
        lastRead = now;

        float massConcentrationPm1p0;
        float massConcentrationPm2p5;
        float massConcentrationPm4p0;
        float massConcentrationPm10p0;
        float ambientHumidity;
        float ambientTemperature;
        float vocIndex;
        float noxIndex;
        float co2;

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
            char errorMessage[256];
            errorToString(error, errorMessage, sizeof(errorMessage));
            Serial.print("✗ SEN66 read error: ");
            Serial.println(errorMessage);
            dataReady = false;
            return;
        }

        // Populate AirQualityMetrics protobuf structure
        latestMetrics = meshtastic_AirQualityMetrics_init_zero;

        // PM concentrations (convert float µg/m³ to uint32_t)
        latestMetrics.has_pm10_environmental = true;
        latestMetrics.pm10_environmental = (uint32_t)(massConcentrationPm1p0 + 0.5f);

        latestMetrics.has_pm25_environmental = true;
        latestMetrics.pm25_environmental = (uint32_t)(massConcentrationPm2p5 + 0.5f);

        latestMetrics.has_pm40_standard = true;
        latestMetrics.pm40_standard = (uint32_t)(massConcentrationPm4p0 + 0.5f);

        latestMetrics.has_pm100_environmental = true;
        latestMetrics.pm100_environmental = (uint32_t)(massConcentrationPm10p0 + 0.5f);

        // VOC and NOx indices (0-500 scale)
        latestMetrics.has_pm_voc_idx = true;
        latestMetrics.pm_voc_idx = vocIndex;

        latestMetrics.has_pm_nox_idx = true;
        latestMetrics.pm_nox_idx = noxIndex;

        // CO2 (if available - may be 0 if not enabled on SEN66)
        if (co2 > 0) {
            latestMetrics.has_co2 = true;
            latestMetrics.co2 = (uint32_t)(co2 + 0.5f);
        }

        // Temperature and humidity from SEN66
        latestMetrics.has_pm_temperature = true;
        latestMetrics.pm_temperature = ambientTemperature;

        latestMetrics.has_pm_humidity = true;
        latestMetrics.pm_humidity = ambientHumidity;

        // Encode to protobuf (do this in loop, not in ISR)
        encodedSize = proto_encode(
            encodedBuffer,
            sizeof(encodedBuffer),
            meshtastic_AirQualityMetrics_fields,
            &latestMetrics
        );

        if (encodedSize > 0) {
            dataReady = true;

            // Log sensor data
            Serial.println("─────────────────────────");
            Serial.println("📊 SEN66 Reading:");
            Serial.print("  PM1.0:  "); Serial.print(massConcentrationPm1p0, 1); Serial.println(" µg/m³");
            Serial.print("  PM2.5:  "); Serial.print(massConcentrationPm2p5, 1); Serial.println(" µg/m³");
            Serial.print("  PM4.0:  "); Serial.print(massConcentrationPm4p0, 1); Serial.println(" µg/m³");
            Serial.print("  PM10:   "); Serial.print(massConcentrationPm10p0, 1); Serial.println(" µg/m³");
            Serial.print("  VOC:    "); Serial.println(vocIndex, 1);
            Serial.print("  NOx:    "); Serial.println(noxIndex, 1);
            if (co2 > 0) {
                Serial.print("  CO2:    "); Serial.print(co2, 0); Serial.println(" ppm");
            }
            Serial.print("  Temp:   "); Serial.print(ambientTemperature, 1); Serial.println(" °C");
            Serial.print("  RH:     "); Serial.print(ambientHumidity, 1); Serial.println(" %");
            Serial.print("  Encoded: "); Serial.print(encodedSize); Serial.println(" bytes ready");
            Serial.println("─────────────────────────\n");
        } else {
            Serial.println("✗ Protobuf encode failed!");
            dataReady = false;
        }
    }

    // Small delay to prevent CPU spinning
    delay(100);
}

            metrics.has_pm100_environmental = true;
            metrics.pm100_environmental = (uint32_t)pm10p0;

            metrics.has_pm_voc_idx = true;
            metrics.pm_voc_idx = vocIndex;

            metrics.has_pm_nox_idx = true;
            metrics.pm_nox_idx = noxIndex;

            metrics.has_co2 = true;
            metrics.co2 = co2;

            metrics.has_pm_temperature = true;
            metrics.pm_temperature = temperature;

            metrics.has_pm_humidity = true;
            metrics.pm_humidity = humidity;

            // Send metrics to Meshtastic device
            sendMetrics(metrics);

            Serial.println("SEN66 data sent to Meshtastic device");
            Serial.printf("PM1.0=%.1f, PM2.5=%.1f, PM4.0=%.1f, PM10.0=%.1f, VOC=%.1f, NOx=%.1f, CO2=%u, T=%.1f, RH=%.1f\n",
                         pm1p0, pm2p5, pm4p0, pm10p0, vocIndex, noxIndex, co2, temperature, humidity);
        } else {
            Serial.print("SEN66 read failed: ");
            Serial.println(error);
        }
    }

    delay(100);
}