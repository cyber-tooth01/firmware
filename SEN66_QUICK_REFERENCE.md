# SEN66 Meshtastic Integration - Quick Reference

## Problem Root Cause

**Your current `SEN66Sensor.cpp` has the architecture BACKWARDS:**

```cpp
// ❌ WRONG - Makes RAK4631 an I2C SLAVE (conflicts with being master for GPS/sensors)
i2cBus->begin(MT_I2C_ADDRESS);       // RAK4631 becomes slave at 0x11
i2cBus->onReceive(onReceiveSEN66Metrics);  // Waits for external master to send data
```

**Additionally, it blocks in an interrupt:**
```cpp
void onReceiveSEN66Metrics(int length) {
    Wire.readBytes(buffer, length);  // ❌ BLOCKING in ISR causes watchdog reset!
}
```

This causes boot loops because:
1. RAK4631 MUST be I2C master (for GPS, displays, other sensors)
2. `Wire.readBytes()` blocking in ISR starves nRF52 watchdog
3. Conflicts with GPS L76K initialization delays

## Correct Architecture

```
┌──────────────────────┐           ┌──────────────────────┐
│  Arduino/KB2040      │           │  RAK4631 (nRF52)     │
│  I2C SLAVE (0x11)    │  <─I2C─>  │  I2C MASTER          │
│                      │           │                      │
│  • Reads SEN66       │           │  • Reads GPS (0x42)  │
│  • Encodes protobuf  │           │  • Requests 0x11     │
│  • Responds to       │           │  • Decodes protobuf  │
│    master requests   │           │  • Publishes mesh    │
└──────────────────────┘           └──────────────────────┘
         │
    SEN66 (0x6B)
```

## Files Created

### 1. `SEN66_BOOT_LOOP_ANALYSIS.md`
Complete technical analysis including:
- Root cause explanation
- Hardware architecture diagrams
- Protobuf data format details
- Step-by-step debugging guide
- nRF52 watchdog analysis
- Hardware validation procedures

### 2. `SEN66_I2C_Slave.ino` (THIS FILE)
**Corrected Arduino I2C slave sketch**

Pattern follows [meshtastic/i2c-sensor](https://github.com/meshtastic/i2c-sensor):
- ✅ Acts as I2C **SLAVE** at address 0x11
- ✅ Reads SEN66 in **main loop** (blocking OK here)
- ✅ Encodes protobuf in main loop
- ✅ ISR handler (`onRequest`) is **fast/non-blocking** - just sends buffer

## Next Steps

### Step 1: Flash Arduino Slave
1. Copy protobuf files from [meshtastic/i2c-sensor](https://github.com/meshtastic/i2c-sensor/tree/main/src/generated) to your Arduino project:
   - `telemetry.pb.h`
   - `telemetry.pb.c`
   - `pb.h`
   - `pb_common.h`
   - `pb_encode.h`

2. Install libraries:
   - `Sensirion I2C SEN66` by Sensirion
   - `Nanopb` by Petteri Aimonen

3. Flash `SEN66_I2C_Slave.ino` to your Arduino/KB2040

4. Verify serial output shows:
   ```
   ✓ I2C slave initialized at 0x11
   ✓ SEN66 measurement started
   📊 SEN66 Reading: [data]
   ```

### Step 2: Rewrite Meshtastic SEN66Sensor.cpp

Replace your current implementation with I2C **MASTER** polling:

```cpp
// SEN66Sensor.cpp - CORRECT approach

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include <pb_decode.h>

#define MT_I2C_ADDRESS 0x11  // Arduino slave address

static meshtastic_AirQualityMetrics lastMetrics = meshtastic_AirQualityMetrics_init_zero;

// Protobuf decode helper
bool proto_decode(const uint8_t *srcbuf, size_t srcbufsize, 
                  const pb_msgdesc_t *fields, void *dest_struct)
{
    pb_istream_t stream = pb_istream_from_buffer(srcbuf, srcbufsize);
    return pb_decode(&stream, fields, dest_struct);
}

SEN66Sensor::SEN66Sensor() 
    : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66") {}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (I2C master polling mode)", sensorName);
    i2cBus = bus;
    // Note: Wire already initialized as master in main.cpp
    // Do NOT call Wire.begin(address) - that makes us a slave!
    return true;
}

int32_t SEN66Sensor::runOnce()
{
    // Called periodically by Meshtastic scheduler
    // Request data from Arduino slave
    
    size_t bytesToRequest = meshtastic_AirQualityMetrics_size;  // 72 bytes max
    
    // Non-blocking request from slave
    size_t bytesReceived = i2cBus->requestFrom(MT_I2C_ADDRESS, bytesToRequest);
    
    if (bytesReceived == 0) {
        LOG_DEBUG("SEN66: No response from slave at 0x%02X", MT_I2C_ADDRESS);
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    // Read with timeout
    uint8_t buffer[meshtastic_AirQualityMetrics_size];
    size_t i = 0;
    unsigned long timeout = millis() + 100;  // 100ms timeout
    
    while (i2cBus->available() && i < bytesReceived && millis() < timeout) {
        buffer[i++] = i2cBus->read();
    }
    
    if (i != bytesReceived) {
        LOG_WARN("SEN66: Incomplete read: got %d/%d bytes", i, bytesReceived);
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }
    
    // Decode protobuf
    if (proto_decode(buffer, bytesReceived, 
                     meshtastic_AirQualityMetrics_fields, 
                     &lastMetrics)) {
        LOG_DEBUG("SEN66: Decoded %d bytes successfully", bytesReceived);
    } else {
        LOG_WARN("SEN66: Protobuf decode failed");
    }
    
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}

bool SEN66Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    // Check if we have valid data
    if (!lastMetrics.has_pm25_environmental) {
        LOG_DEBUG("SEN66: No valid data from slave");
        return false;
    }
    
    // Populate telemetry message
    measurement->which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    measurement->variant.air_quality_metrics = lastMetrics;
    
    LOG_INFO("SEN66: PM1.0=%u, PM2.5=%u, PM4.0=%u, PM10=%u, VOC=%.1f, NOx=%.1f",
             lastMetrics.pm10_environmental,
             lastMetrics.pm25_environmental,
             lastMetrics.pm40_standard,
             lastMetrics.pm100_environmental,
             lastMetrics.pm_voc_idx,
             lastMetrics.pm_nox_idx);
    
    return true;
}

#endif
```

### Step 3: Update EnvironmentTelemetry.cpp

Keep your existing code around line 280, but verify device detection:

```cpp
#if __has_include(<SensirionI2cSen66.h>)
    LOG_DEBUG("EnvironmentTelemetry: Adding SEN66 sensor (I2C master polling mode)");
    // Note: NONE means we don't scan for SEN66 directly - we scan for Arduino slave at 0x11
    // You might need to add a scan entry for 0x11 if not already present
    addSensor<SEN66Sensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#endif
```

### Step 4: Test Without GPS First

In your RAK4631 variant `platformio.ini`, temporarily add:

```ini
build_flags = 
    ${nrf52840_base.build_flags}
    -DMESHTASTIC_EXCLUDE_GPS=1  ; Temporary - disable GPS to isolate issue
```

This eliminates GPS timing conflicts during initial testing.

### Step 5: Monitor Both Devices

**Arduino Serial (115200 baud):**
```
📊 SEN66 Reading:
  PM2.5:  18.5 µg/m³
  Encoded: 68 bytes ready
→ Sent 68 bytes
```

**RAK4631 Serial (115200 baud):**
```
[INFO] Init sensor: SEN66 (I2C master polling mode)
[DEBUG] SEN66: Decoded 68 bytes successfully
[INFO] SEN66: PM2.5=18 ...
```

### Step 6: Re-enable GPS

Once stable without GPS, remove the `-DMESHTASTIC_EXCLUDE_GPS=1` flag and test again.

If boot loops return, add GPS init safety:

```cpp
// In SEN66Sensor::runOnce()
extern bool GPSInitFinished;

if (!GPSInitFinished) {
    LOG_DEBUG("SEN66: Skipping I2C during GPS init");
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}
```

## Key Differences from Your Original Code

| Aspect | Your Code (❌ Wrong) | Correct Approach (✅) |
|--------|---------------------|---------------------|
| RAK4631 Role | I2C Slave | I2C Master |
| `Wire.begin()` | `Wire.begin(0x11)` - becomes slave | Already master from `main.cpp` |
| Callback | `Wire.onReceive()` | Uses `requestFrom()` |
| Data Flow | Waits for external master to send | Actively requests from slave |
| Blocking | `Wire.readBytes()` in ISR | Blocking OK in scheduler thread |
| SEN66 Location | Directly on RAK4631 I2C bus | On separate Arduino board |

## Expected Telemetry Output

In Meshtastic app, you should see:
- **Air Quality** node metrics
- PM1.0, PM2.5, PM4.0, PM10 values
- VOC index (0-500)
- NOx index (0-500)
- Temperature and humidity from SEN66
- Updates every 10-60 seconds

## Troubleshooting Quick Checks

**If Arduino slave not responding:**
```bash
# Scan I2C bus from RAK4631
# Add to setup() temporarily:
for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
        LOG_INFO("Found I2C device at 0x%02X", addr);
    }
}
# Should see 0x11 (Arduino) and 0x42 (GPS)
```

**If still boot loops:**
1. Check [SEN66_BOOT_LOOP_ANALYSIS.md](SEN66_BOOT_LOOP_ANALYSIS.md) for watchdog debugging
2. Reduce I2C clock speed: `-DI2C_CLOCK_SPEED=100000`
3. Check for 3.3V vs 5V logic level conflicts
4. Verify SDA/SCL pull-up resistors (2.2kΩ - 4.7kΩ to 3.3V)

## Reference Links

- [meshtastic/i2c-sensor](https://github.com/meshtastic/i2c-sensor) - Reference implementation
- [Meshtastic Telemetry Docs](https://meshtastic.org/docs/configuration/module/telemetry/)
- [SEN66 Arduino Library](https://github.com/Sensirion/arduino-i2c-sen66)
- [nRF52 I2C Errata](https://infocenter.nordicsemi.com/topic/errata_nRF52840_Rev3/ERR/nRF52840/Rev3/latest/anomaly_840_89.html)

## When Direct Integration Makes Sense

**Only consider removing the Arduino slave if:**
- ✅ You've implemented fully **async/non-blocking** SEN66 reads (state machine)
- ✅ FreeRTOS mutex protects I2C bus from concurrent access
- ✅ SEN66 reads only happen when GPS is idle
- ✅ You've accounted for nRF52 I2C timing errata
- ✅ Extensive testing shows zero boot loops over 48+ hours

**Reality check:** The Arduino slave approach is **production-ready** and **recommended** because it:
- Isolates sensor timing from main firmware
- Eliminates I2C bus contention
- Protects against sensor hangs
- Easier to debug and maintain
- Costs ~$5 extra hardware but saves weeks of debugging
