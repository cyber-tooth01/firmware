# SEN66 Boot Loop Analysis & I2C Slave Validation Strategy

## Problem Summary
RAK4631 (nRF52) running Meshtastic enters a boot loop after GPS (L76K) initialization when SEN66 sensor integration is enabled. Symptoms:
- USB disconnects/reconnects repeatedly
- Node unresponsive to Meshtastic clients
- SEN66 detected only after hardcoding address

## Root Cause Analysis

### Critical Issue #1: **BLOCKING I2C READ IN INTERRUPT CONTEXT**

Location: [SEN66Sensor.cpp](src/modules/Telemetry/Sensor/SEN66Sensor.cpp#L35)

```cpp
void onReceiveSEN66Metrics(int length)
{
    uint8_t buffer[meshtastic_AirQualityMetrics_size];
    Wire.readBytes(buffer, length);  // <-- BLOCKING CALL IN ISR!
    // ...
}
```

**Why This Causes Boot Loops:**

1. **`Wire.readBytes()` is BLOCKING** - it waits for I2C transaction to complete
2. **Called from I2C ISR** (`Wire.onReceive()` callback)
3. **On nRF52**, blocking in ISR can:
   - Starve the SoftDevice (BLE stack) watchdog
   - Block critical system interrupts
   - Prevent the watchdog timer from being serviced
4. **Especially problematic during GPS initialization** when I2C bus may already be busy

**Comparison with Arduino Example:**
The [meshtastic/i2c-sensor](https://github.com/meshtastic/i2c-sensor) example works because:
- It runs on a **dedicated MCU** as I2C **slave** (not master)
- No competing I2C devices (GPS, sensors, etc.)
- Simpler interrupt environment (no BLE SoftDevice)

### Critical Issue #2: **ARCHITECTURE MISMATCH**

Your current `SEN66Sensor.cpp` implements the **WRONG I2C ROLE**:

```cpp
// Current (WRONG):
i2cBus->begin(MT_I2C_ADDRESS);       // RAK4631 becomes I2C SLAVE
i2cBus->onReceive(onReceiveSEN66Metrics);  // Waits for external master

// This makes RAK4631 an I2C slave, but:
// - RAK4631 MUST be I2C master for GPS, displays, other sensors
// - Creating DUAL-ROLE I2C is extremely complex on nRF52
```

The meshtastic/i2c-sensor architecture is:
```
Arduino/KB2040 (I2C Slave 0x11)  <----I2C--->  RAK4631 (I2C Master)
   ├─ Reads SEN66                                 ├─ Reads GPS
   ├─ Encodes protobuf                            ├─ Requests data from 0x11
   └─ Responds to requests                        └─ Publishes telemetry
```

### Critical Issue #3: **GPS TIMING INTERFERENCE**

From [GPS.cpp](src/gps/GPS.cpp#L534-L540), L76K initialization sends multiple commands with `delay()`:

```cpp
_serial_gps->write("$PCAS04,7*1E\r\n");
delay(250);  // Blocks for 250ms
_serial_gps->write("$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n");
delay(250);  // Another 250ms block
```

If SEN66 I2C code tries to use the I2C bus **during GPS init**, conflicts occur because:
- GPS init happens in main thread with delays
- I2C slave interrupt can fire during these delays
- Blocking `Wire.readBytes()` in ISR deadlocks the system

### Critical Issue #4: **nRF52 I2C ERRATA**

RAK4631 uses nRF52840 which has known I2C hardware issues:
- **Errata 89**: TWIM: Data might be corrupted during continuous transmission
- **Errata 199**: TWIM: Data might be sent twice in some cases
- Requires careful clock stretching and timing management

## Solution: Proper I2C Slave Architecture

### Hardware Architecture

```
┌─────────────────────────────────────┐
│  Arduino MCU (e.g., KB2040, Nano)   │
│  I2C Slave Address: 0x11            │
│                                      │
│  ┌──────────────────────┐           │
│  │  SEN66 Sensor        │           │
│  │  Address: 0x6B       │           │
│  │  Connected via I2C   │           │
│  └──────────────────────┘           │
│                                      │
│  Reads SEN66 every 10s              │
│  Encodes to AirQualityMetrics       │
│  Stores in buffer                   │
│  Responds to I2C requests           │
└──────────┬──────────────────────────┘
           │ I2C (SDA/SCL)
           │ @ 3.3V logic
           │
┌──────────┴──────────────────────────┐
│  RAK4631 (nRF52840)                 │
│  I2C Master                          │
│                                      │
│  ┌──────────────────────┐           │
│  │  L76K GPS            │           │
│  │  Address: 0x42       │           │
│  └──────────────────────┘           │
│                                      │
│  ┌──────────────────────┐           │
│  │  Other I2C sensors   │           │
│  │  (BME280, etc.)      │           │
│  └──────────────────────┘           │
│                                      │
│  Meshtastic Firmware:               │
│  - Polls 0x11 periodically          │
│  - Non-blocking requests             │
│  - Publishes telemetry               │
└─────────────────────────────────────┘
```

### Meshtastic I2C Data Format

Based on [meshtastic/i2c-sensor](https://github.com/meshtastic/i2c-sensor) and protobuf definitions:

**Protocol:**
1. Slave (Arduino) maintains a buffer of encoded protobuf data
2. Master (RAK4631) sends I2C read request to 0x11
3. Slave responds with raw protobuf bytes
4. Master decodes using `pb_decode()`

**Data Structure (AirQualityMetrics):**
```cpp
typedef struct _meshtastic_AirQualityMetrics {
    bool has_pm10_environmental;      // PM1.0 (µg/m³)
    uint32_t pm10_environmental;
    
    bool has_pm25_environmental;      // PM2.5 (µg/m³)
    uint32_t pm25_environmental;
    
    bool has_pm40_standard;           // PM4.0 (µg/m³) - SEN66 specific
    uint32_t pm40_standard;
    
    bool has_pm100_environmental;     // PM10 (µg/m³)
    uint32_t pm100_environmental;
    
    bool has_pm_voc_idx;              // VOC index (0-500)
    float pm_voc_idx;
    
    bool has_pm_nox_idx;              // NOx index (0-500)
    float pm_nox_idx;
    
    bool has_co2;                     // CO2 (ppm, if available)
    uint32_t co2;
    
    bool has_pm_temperature;          // Temperature (°C) from SEN66
    float pm_temperature;
    
    bool has_pm_humidity;             // Humidity (%) from SEN66
    float pm_humidity;
} meshtastic_AirQualityMetrics;
```

**Maximum encoded size:** 72 bytes (see `meshtastic_AirQualityMetrics_size`)

### Arduino I2C Slave Implementation

See the corrected sketch in your existing `SEN66_I2C_Slave.ino` - key differences needed:

```cpp
// CORRECT - Slave responds to master requests
void setup() {
    Wire.begin(MT_I2C_ADDRESS);  // Slave at 0x11
    Wire.onRequest(onRequest);   // Triggered by master read
}

void onRequest() {
    // NON-BLOCKING - just send pre-encoded buffer
    Wire.write(encodedBuffer, encodedSize);
}

void loop() {
    // Read SEN66 in main loop (NOT ISR)
    if (timeToRead) {
        readSEN66();
        encodeProtobuf();
    }
}
```

**Key Pattern:**
- **Read sensor in `loop()`** - blocking is OK in main thread
- **Encode protobuf in `loop()`** - store in static buffer
- **`onRequest()` just sends buffer** - fast, non-blocking ISR

### Meshtastic Master Changes Needed

Your current `SEN66Sensor.cpp` needs complete rewrite:

```cpp
// CORRECT approach for Meshtastic master:

bool SEN66Sensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("Init sensor: %s (I2C master polling mode)", sensorName);
    i2cBus = bus;
    // Do NOT call Wire.begin(address) - already initialized as master
    // Do NOT set onReceive callback - we're the master
    return true;
}

int32_t SEN66Sensor::runOnce()
{
    // Called periodically by Meshtastic scheduler
    // Request data from slave (non-blocking or with timeout)
    requestDataFromSlave();
    return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
}

bool SEN66Sensor::requestDataFromSlave()
{
    // Non-blocking I2C request
    size_t bytesRequested = meshtastic_AirQualityMetrics_size;
    size_t bytesReceived = i2cBus->requestFrom(MT_I2C_ADDRESS, bytesRequested);
    
    if (bytesReceived == 0) {
        LOG_DEBUG("SEN66: No response from slave at 0x%x", MT_I2C_ADDRESS);
        return false;
    }
    
    // Read response with timeout
    uint8_t buffer[meshtastic_AirQualityMetrics_size];
    size_t i = 0;
    unsigned long timeout = millis() + 100; // 100ms timeout
    
    while (i2cBus->available() && i < bytesReceived && millis() < timeout) {
        buffer[i++] = i2cBus->read();
    }
    
    if (i != bytesReceived) {
        LOG_WARN("SEN66: Incomplete read: got %d/%d bytes", i, bytesReceived);
        return false;
    }
    
    // Decode protobuf
    return proto_decode(buffer, bytesReceived, 
                       meshtastic_AirQualityMetrics_fields, 
                       &lastMetrics);
}
```

## Debugging Strategy

### Phase 1: Validate Slave-Only Architecture (Current Focus)

**Step 1: Test Arduino I2C Slave Without Meshtastic**

Equipment needed:
- Arduino/KB2040 with SEN66 connected
- USB logic analyzer or scope (optional but helpful)
- Another Arduino as simple I2C master for testing

Test sketch for validation master:
```cpp
#include <Wire.h>

void setup() {
    Serial.begin(115200);
    Wire.begin(); // Master mode
}

void loop() {
    Serial.println("Requesting data from 0x11...");
    
    int bytesReceived = Wire.requestFrom(0x11, 72); // Request max size
    
    Serial.print("Received ");
    Serial.print(bytesReceived);
    Serial.println(" bytes:");
    
    while (Wire.available()) {
        uint8_t b = Wire.read();
        Serial.print(b, HEX);
        Serial.print(" ");
    }
    Serial.println();
    
    delay(5000);
}
```

**Expected output:**
- Slave prints "Sent X bytes to master"
- Master receives consistent byte count
- Data changes when SEN66 environment changes

**Step 2: Verify Protobuf Encoding**

Add to slave:
```cpp
// After encoding, verify decode works
meshtastic_AirQualityMetrics test = meshtastic_AirQualityMetrics_init_zero;
if (proto_decode(encodedBuffer, encodedSize, 
                 meshtastic_AirQualityMetrics_fields, &test)) {
    Serial.println("Encode/decode verification: OK");
    Serial.print("PM2.5 roundtrip: ");
    Serial.println(test.pm25_environmental);
} else {
    Serial.println("ERROR: Protobuf verification FAILED!");
}
```

**Step 3: Test with RAK4631 (Without GPS Conflicts)**

Temporarily disable GPS in RAK4631:
```cpp
// In platformio.ini for your RAK4631 variant:
build_flags = 
    ${nrf52840_base.build_flags}
    -DMESHTASTIC_EXCLUDE_GPS=1  // Disable GPS temporarily
```

Modify `SEN66Sensor.cpp` to use correct master polling (code above).

Monitor with:
```bash
pio device monitor -b 115200
```

**Expected behavior:**
- No boot loop
- Log shows "SEN66: Received X bytes from slave"
- Telemetry appears in Meshtastic app

### Phase 2: Identify GPS Timing Conflicts

**Step 4: Add GPS Init Logging**

In [GPS.cpp](src/gps/GPS.cpp) around line 534:
```cpp
LOG_INFO("GPS: Starting L76K init sequence");
_serial_gps->write("$PCAS04,7*1E\r\n");
LOG_DEBUG("GPS: Sent PCAS04, delaying 250ms");
delay(250);
_serial_gps->write("$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n");
LOG_DEBUG("GPS: Sent PCAS03, delaying 250ms");
delay(250);
// ... continue
LOG_INFO("GPS: L76K init complete");
```

**Step 5: Monitor I2C During GPS Init**

Add to `SEN66Sensor::requestDataFromSlave()`:
```cpp
LOG_DEBUG("SEN66: Attempting I2C request (GPS init: %d)", GPSInitFinished);
```

Look for correlation:
- Does boot loop occur only when `GPSInitFinished == false`?
- Do I2C errors appear during GPS delays?

**Step 6: Implement I2C Mutual Exclusion**

If conflicts occur, add to `SEN66Sensor.cpp`:
```cpp
bool SEN66Sensor::requestDataFromSlave()
{
    // Don't access I2C during GPS initialization
    if (!GPSInitFinished) {
        LOG_DEBUG("SEN66: Skipping I2C read during GPS init");
        return false;
    }
    
    // Optionally: use I2C semaphore if Meshtastic provides one
    // ...
}
```

### Phase 3: Watchdog Analysis (If Boot Loop Persists)

**Step 7: Check nRF52 Watchdog Status**

Add to RAK4631 startup (before SEN66 init):
```cpp
// In main.cpp or SEN66Sensor.cpp
#ifdef ARCH_NRF52
void logWatchdogStatus() {
    LOG_INFO("Watchdog REQSTATUS: 0x%08X", NRF_WDT->REQSTATUS);
    LOG_INFO("Watchdog RUNSTATUS: 0x%08X", NRF_WDT->RUNSTATUS);
}
#endif
```

**Step 8: Reduce I2C Clock Speed**

nRF52 I2C errata can cause issues at high speeds. In your variant's `platformio.ini`:
```ini
build_flags =
    ${nrf52840_base.build_flags}
    -DI2C_CLOCK_SPEED=100000  # Reduce from 400kHz to 100kHz
```

**Step 9: Enable Detailed I2C Logging**

```cpp
// In platformio.ini
build_flags =
    -DDEBUG_I2C=1
    -DCORE_DEBUG_LEVEL=5  # Verbose logging
```

### Phase 4: Hardware Validation

**Step 10: Verify Physical I2C Connection**

With multimeter/scope, check:
- **SDA/SCL pull-ups:** Should have 2.2kΩ - 4.7kΩ to 3.3V
- **Logic levels:** Must be 3.3V (not 5V!)
- **Ground connection:** Common ground between all devices
- **Clock frequency:** Measure SCL - should be ~100kHz or 400kHz

**Step 11: Test I2C Bus Scanner**

```cpp
// Temporary diagnostic in RAK4631 setup()
void scanI2CBus() {
    LOG_INFO("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            LOG_INFO("Found device at 0x%02X", addr);
        }
    }
}
```

Should see:
- `0x11` - Your Arduino slave
- `0x42` - L76K GPS (if present)
- Other sensors

**If 0x11 not detected:**
- Check Arduino is powered and running
- Verify `Wire.begin(0x11)` executed on Arduino
- Check physical SDA/SCL connections
- Check voltage levels (3.3V required)

## Expected Behavior After Fix

### Arduino Slave Serial Output:
```
SEN66 I2C Slave for Meshtastic
================================
I2C slave initialized at address 0x11
SEN66 measurement started successfully

--- SEN66 Reading ---
PM1.0:  12.3 µg/m³
PM2.5:  18.5 µg/m³
PM4.0:  22.1 µg/m³
PM10:   25.3 µg/m³
VOC:    125.0
NOx:    98.0
CO2:    450 ppm
Temp:   22.5 °C
RH:     45.2 %
Encoded: 68 bytes
---------------------

Sent 68 bytes to master
Sent 68 bytes to master
```

### RAK4631 Serial Output:
```
[INFO] Init sensor: SEN66 (I2C master polling mode)
[DEBUG] SEN66: Requesting data from slave at 0x11
[DEBUG] SEN66: Received 68 bytes from slave
[INFO] SEN66 metrics: PM1.0=12, PM2.5=18, PM4.0=22, PM10=25, VOC=125.0, NOx=98.0
[INFO] Publishing telemetry to mesh...
```

### Meshtastic App Display:
- Air Quality telemetry appears on node
- Updates every 10-60 seconds (configurable)
- All PM, VOC, NOx, CO2 values visible

## Common Pitfalls to Avoid

1. **Don't use `delay()` in Meshtastic firmware** - use scheduler/timers
2. **Don't block in I2C ISR** - keep `onReceive`/`onRequest` fast
3. **Don't access I2C from multiple threads without mutex** - nRF52 I2C isn't thread-safe
4. **Don't ignore WDT_TIMEOUT errors** - they indicate blocking code
5. **Don't mix 3.3V and 5V I2C logic** - use level shifters
6. **Don't rely on clock stretching** - nRF52 I2C errata makes it unreliable
7. **Don't hardcode I2C addresses in scanning code** - always check scan results

## When to Reintegrate Direct SEN66

Only proceed with direct SEN66 integration (no Arduino slave) after:

1. ✅ Slave architecture works reliably for 24+ hours
2. ✅ No boot loops with GPS enabled
3. ✅ All I2C conflicts resolved
4. ✅ Watchdog issues understood
5. ✅ You implement **async/non-blocking** SEN66 reads in Meshtastic

Direct integration would require:
- Rewriting SEN66 read as non-blocking state machine
- Careful timing around GPS initialization
- Extensive testing with all I2C devices enabled
- Possible FreeRTOS mutex for I2C bus access

The slave approach is **recommended for production** because:
- Isolates timing-sensitive sensor reads
- Eliminates I2C bus contention
- Allows different I2C speeds per bus
- Protects main firmware from sensor hangs
- Easier to debug and maintain

## Next Steps

1. **Review your existing `SEN66_I2C_Slave.ino`** - compare with meshtastic/i2c-sensor pattern
2. **Flash slave sketch to Arduino** and verify it responds to I2C requests
3. **Rewrite `SEN66Sensor.cpp`** to be I2C master (polling mode, not slave mode)
4. **Test without GPS first**, then progressively enable features
5. **Monitor serial output** from both devices during testing
6. **Report back with serial logs** if issues persist

## References

- https://github.com/meshtastic/i2c-sensor (reference implementation)
- https://infocenter.nordicsemi.com/index.jsp?topic=%2Ferrata_nRF52840_Rev3%2FERR%2FnRF52840%2FRev3%2Flatest%2Fanomaly_840_89.html (nRF52 I2C Errata 89)
- https://meshtastic.org/docs/configuration/module/telemetry/
- https://github.com/Sensirion/arduino-i2c-sen66 (SEN66 library)
