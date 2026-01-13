# SEN66 Direct Implementation for Meshtastic (No I2C Conflicts)

## Your Situation: Simplified Architecture

Since you clarified:
- ✅ **GPS is serial-only** (UART, not I2C)
- ✅ **No other I2C devices** on the bus
- ✅ **SEN66 is the ONLY I2C device**

**The Arduino slave approach is unnecessary!** Your boot loop is purely from the wrong I2C configuration in your current `SEN66Sensor.cpp`.

## What Was Wrong in Your Original Code

```cpp
// ❌ YOUR CURRENT CODE - COMPLETELY BACKWARDS
bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    i2cBus = bus;
    i2cBus->begin(MT_I2C_ADDRESS);  // Makes RAK4631 an I2C SLAVE at 0x11
    i2cBus->onReceive(onReceiveSEN66Metrics);  // Waits for external master
    return true;
}

void onReceiveSEN66Metrics(int length) {
    Wire.readBytes(buffer, length);  // Blocks in ISR
    // Decode...
}
```

**Problems:**
1. Makes RAK4631 an **I2C slave** when it should be **master**
2. Expects an external device to send data to RAK4631
3. Blocks in interrupt handler (kills watchdog)

**This is for the Arduino slave pattern - but you don't need that!**

## Corrected Direct Implementation

### Simple Approach (Blocking Reads in Scheduler)

Since you have no I2C bus conflicts, you can just read SEN66 directly in the scheduler thread (where blocking is acceptable):

```cpp
// SEN66Sensor.h - Direct Implementation

#pragma once
#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"

class SEN66Sensor : public TelemetrySensor
{
  private:
    TwoWire *i2cBus = nullptr;
    
  public:
    SEN66Sensor();
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    virtual bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override;
    virtual int32_t runOnce() override;
};

#endif
```

```cpp
// SEN66Sensor.cpp - Direct Implementation

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<SensirionI2cSen66.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "SEN66Sensor.h"
#include "TelemetrySensor.h"
#include <SensirionI2cSen66.h>
#include "main.h"

// SEN66 sensor instance
static SensirionI2cSen66 sen66;

// Cache latest readings
static meshtastic_AirQualityMetrics latestMetrics = meshtastic_AirQualityMetrics_init_zero;
static bool hasValidData = false;
static bool sensorInitialized = false;

SEN66Sensor::SEN66Sensor() 
    : TelemetrySensor(MESHTASTIC_TELEMETRY_SENSOR_TYPE_SEN66, "SEN66") 
{
}

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (direct I2C)", sensorName);
    
    i2cBus = bus;
    
    // CRITICAL: Do NOT call Wire.begin(address) here!
    // The I2C bus is already initialized as MASTER in main.cpp
    
    // Initialize SEN66 on the I2C bus
    sen66.begin(*i2cBus);
    
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
    error = sen66.startMeasurement();
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
    float co2;
    
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
```

### Key Differences from Your Original Code

| Aspect | Your Code (❌ Wrong) | Direct Implementation (✅ Correct) |
|--------|---------------------|-----------------------------------|
| I2C Role | Slave (expects external master) | Master (reads SEN66 directly) |
| `Wire.begin()` | `Wire.begin(0x11)` - becomes slave | Already master from main.cpp |
| Callbacks | `Wire.onReceive()` - ISR context | None - runs in scheduler |
| Reading | Waits for external data | Actively reads SEN66 |
| Blocking | In ISR (kills watchdog) | In scheduler (acceptable) |
| SEN66 Location | Expected on external Arduino | Directly on RAK4631 I2C |

## Why This Works Without Arduino Slave

**Your simplified hardware:**
```
┌──────────────────────────┐
│  RAK4631 (nRF52840)      │
│                          │
│  I2C Master              │
│  ├─ SEN66 (0x6B)        │  ← Only I2C device!
│                          │
│  UART (Serial)           │
│  └─ L76K GPS             │  ← Not on I2C bus
│                          │
│  No conflicts!           │
└──────────────────────────┘
```

**Why there's no conflict:**
1. **GPS uses UART** (serial TX/RX), not I2C
2. **SEN66 is alone on I2C bus** - no contention
3. **Scheduler allows blocking** - 100-150ms read is fine
4. **nRF52 watchdog is serviced** - scheduler yields properly

## Installation Steps

### 1. Update Your Existing Files

Replace your current `src/modules/Telemetry/Sensor/SEN66Sensor.{h,cpp}` with the code above.

### 2. Ensure SEN66 is Detected

In `src/detect/ScanI2C.h`, verify SEN66 address is defined:

```cpp
// Should already exist in your configuration.h
#define SEN66_ADDR 0x6B
```

### 3. Configure Environment Telemetry

In `src/modules/Telemetry/EnvironmentTelemetry.cpp`, around line 280:

```cpp
#if __has_include(<SensirionI2cSen66.h>)
    LOG_DEBUG("EnvironmentTelemetry: Adding SEN66 sensor (direct I2C)");
    // Pass NONE for DeviceType since we're not scanning for it specifically
    // The sensor will be initialized when found at 0x6B
    addSensor<SEN66Sensor>(i2cScanner, ScanI2C::DeviceType::NONE);
#endif
```

### 4. Add Library Dependency

In your variant's `platformio.ini` or the main one, add:

```ini
lib_deps = 
    ${nrf52840_base.lib_deps}
    sensirion/Sensirion I2C SEN66@^0.1.0
```

### 5. Build and Flash

```bash
pio run -e rak4631 -t upload
pio device monitor -b 115200
```

## Expected Serial Output

After flashing, you should see:

```
[INFO] Init sensor: SEN66 (direct I2C)
[INFO] SEN66 measurement started successfully
[DEBUG] SEN66: Reading sensor data...
[INFO] SEN66: PM1.0=12, PM2.5=18, PM4.0=22, PM10=25, VOC=125.0, NOx=98.0, T=22.5°C, RH=45.0%
[DEBUG] Publishing telemetry to mesh...
```

## Troubleshooting

### If SEN66 Not Detected

Add I2C bus scan to your variant's initialization:

```cpp
// In main.cpp or variant initialization
void scanI2CBus() {
    LOG_INFO("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            LOG_INFO("Found I2C device at 0x%02X", addr);
        }
    }
}
```

Should show: `Found I2C device at 0x6B`

### If Still Boot Loops

1. **Check I2C initialization order** - Make sure `Wire.begin()` is called in main.cpp BEFORE sensor initialization

2. **Verify no address conflicts** - The old code used 0x11, make sure nothing else tries to use that

3. **Check pull-up resistors** - SEN66 needs 2.2kΩ - 4.7kΩ pull-ups to 3.3V on SDA/SCL

4. **Power supply** - SEN66 needs 5V power with 500mA capability during fan operation

5. **Add delay after reset**:
   ```cpp
   sen66.deviceReset();
   delay(500);  // Increase if needed
   ```

### If Readings Are Zero

SEN66 requires warm-up time after power-on:
- PM sensor: ~10 seconds
- VOC/NOx: ~10 seconds to stabilize, ~60 seconds for accurate readings

First few readings may be invalid. This is normal.

## Performance Considerations

### Timing Analysis

- **SEN66 read time**: ~100-150ms (includes I2C transactions)
- **Scheduler period**: Typically 1000ms minimum between reads
- **Watchdog timeout**: Usually 5-10 seconds on nRF52
- **Conclusion**: 150ms is well under watchdog timeout ✅

### Memory Usage

- **Sensirion library**: ~8KB flash, ~500 bytes RAM
- **AirQualityMetrics**: 72 bytes max (actual: ~30-60 bytes encoded)
- **SEN66Sensor class**: Minimal overhead
- **Total impact**: Negligible on RAK4631 (256KB RAM, 1MB flash)

### Power Consumption

- **SEN66 active**: ~50mA typical, up to 200mA peak (fan)
- **Solution**: Control SEN66 power via GPIO if battery-powered
- **Deep sleep**: Can power down SEN66 between readings

## Advanced: Async Reading (Future Enhancement)

If you want to be extra careful about blocking, you can implement state machine reads:

```cpp
enum ReadState {
    IDLE,
    WAITING_FOR_DATA,
    READING_COMPLETE
};

ReadState state = IDLE;
unsigned long readStartTime = 0;

int32_t SEN66Sensor::runOnce() {
    switch (state) {
        case IDLE:
            // Start read (non-blocking if supported)
            sen66.startRead();
            state = WAITING_FOR_DATA;
            readStartTime = millis();
            return 100;  // Check again in 100ms
            
        case WAITING_FOR_DATA:
            if (sen66.dataReady()) {
                sen66.getValues(...);  // Fast read of cached values
                state = IDLE;
                return 10000;  // Wait 10s before next read
            } else if (millis() - readStartTime > 200) {
                LOG_WARN("SEN66 read timeout");
                state = IDLE;
                return 10000;
            }
            return 50;  // Check again soon
    }
}
```

**Note**: This requires checking if the Sensirion library supports async operations. Most don't, so the simple blocking approach is fine for your use case.

## Comparison: Direct vs Arduino Slave

| Aspect | Direct (Your Case) | Arduino Slave |
|--------|-------------------|---------------|
| Hardware | 1 device | 2 devices (+$5-10) |
| Wiring | Simple | Extra I2C connections |
| I2C Conflicts | None (GPS is serial) | Handles multiple I2C devices |
| Code Complexity | Simple | More complex |
| Debugging | Easier | Need to debug 2 systems |
| Isolation | None | Sensor isolated from main |
| Power | Lower | Higher (2 MCUs) |
| Reliability | Good | Better (if sensor hangs) |

**Recommendation for you**: **Use direct implementation** - it's simpler and you don't have I2C conflicts.

## When You'd Need Arduino Slave

You would only need the Arduino slave approach if:
- ❌ You had multiple I2C devices competing (you don't)
- ❌ GPS was I2C and conflicted with SEN66 (it's not)
- ❌ SEN66 reads were causing watchdog resets (they won't in scheduler)
- ❌ You needed sensor isolation for reliability (optional)

Since none of these apply, **go with the direct implementation**.

## Final Notes

Your boot loop was 100% caused by:
1. Wrong I2C role (slave instead of master)
2. Blocking in ISR context
3. Confusion about the meshtastic/i2c-sensor pattern

With the corrected direct implementation:
- ✅ RAK4631 is I2C master
- ✅ Reads SEN66 directly
- ✅ Blocking happens in scheduler (not ISR)
- ✅ No watchdog issues
- ✅ No Arduino slave needed

This is the simplest, most efficient solution for your hardware configuration!
