# Understanding meshtastic/i2c-sensor: In-Depth Explanation

## The Problem It Solves

Meshtastic firmware runs on resource-constrained hardware (ESP32, nRF52, RP2040) with complex timing requirements:
- Mesh radio communication (LoRa)
- GPS processing with serial parsing
- Display updates (e-ink, OLED)
- BLE/WiFi networking
- Power management with sleep states
- Watchdog timers that must be serviced

**When you add complex sensors directly to Meshtastic firmware, you risk:**

1. **Blocking I/O killing the watchdog** - Sensors like SEN66 can take 100ms+ to read all values
2. **I2C bus conflicts** - GPS, displays, and sensors competing for the same bus
3. **Memory pressure** - Sensor libraries (Sensirion, Bosch, etc.) can use significant RAM
4. **Timing sensitivity** - nRF52 BLE stack (SoftDevice) is extremely timing-sensitive
5. **Difficult debugging** - Sensor hangs crash the entire node
6. **Limited flexibility** - Hard to support exotic/custom sensors without firmware changes

## The Solution: Offload to a Dedicated MCU

**meshtastic/i2c-sensor** provides a pattern to run sensors on a **separate, cheap microcontroller** that acts as an **I2C slave** to Meshtastic.

```
┌──────────────────────────────────┐
│  Cheap Arduino/KB2040/Nano/etc   │  $5-10 hardware
│  (The "sensor board")            │
│                                  │
│  Jobs:                           │
│  • Read complex sensor(s)        │  ← Blocking is OK here!
│  • Handle timing-sensitive ops   │
│  • Encode to protobuf            │
│  • Store in buffer               │
│  • Act as I2C SLAVE (0x11)       │  ← Responds when asked
│                                  │
│  Benefits:                       │
│  • Isolated timing               │
│  • Own memory space              │
│  • Can hang without crashing     │
│  • Easy to debug separately      │
└───────────┬──────────────────────┘
            │
            │ I2C Bus (SDA/SCL)
            │ Simple request/response
            │
┌───────────┴──────────────────────┐
│  Meshtastic Device (RAK4631)     │  Main device
│  (The "main board")              │
│                                  │
│  Jobs:                           │
│  • Mesh networking (LoRa)        │
│  • GPS, Display, BLE, etc.       │
│  • Act as I2C MASTER             │  ← Requests data
│  • Periodically poll 0x11        │
│  • Decode protobuf               │
│  • Publish to mesh               │
│                                  │
│  Benefits:                       │
│  • Non-blocking sensor reads     │
│  • Protected from sensor hangs   │
│  • Clean separation of concerns  │
└──────────────────────────────────┘
```

## The Confusing README Terminology

The meshtastic/i2c-sensor README uses confusing terms:

| README Term | What It Really Means | Actual I2C Role |
|-------------|---------------------|-----------------|
| "Host Mode" | **Sensor Board** | I2C SLAVE (0x11) |
| "Client Mode" | **Main Board** (Meshtastic) | I2C MASTER |

**Why is this confusing?**
- In networking, "host" usually means server/provider
- In I2C, "host" typically means master
- But here, "host" means the slave device that *hosts the sensor*
- And "client" means the master that *consumes the data*

**Better mental model:**
- **Sensor Board** = I2C Slave = Waits for requests = Responds with data
- **Main Board** = I2C Master = Controls bus = Requests data

## How It Actually Works

### Phase 1: Sensor Board Setup (I2C Slave Mode)

```cpp
// Running on Arduino/KB2040/etc.

#include <Wire.h>
#include <SomeSensorLibrary.h>  // BME280, SEN66, whatever
#include "telemetry.pb.h"        // Meshtastic protobuf definitions

// Become I2C slave at address 0x11
void setup() {
    Wire.begin(0x11);           // Slave mode with address
    Wire.onRequest(sendData);   // Callback when master requests
    
    initializeSensor();         // Start your actual sensor
}

// Pre-encoded buffer ready to send
uint8_t encodedData[100];
size_t encodedSize = 0;

void loop() {
    // Read sensor in MAIN LOOP (blocking is OK here!)
    float temperature = sensor.readTemperature();
    float humidity = sensor.readHumidity();
    
    // Populate protobuf struct
    meshtastic_EnvironmentMetrics metrics = {
        .has_temperature = true,
        .temperature = temperature,
        .has_relative_humidity = true,
        .relative_humidity = humidity
    };
    
    // Encode to protobuf bytes
    encodedSize = encodeProtobuf(encodedData, &metrics);
    
    delay(10000);  // Read every 10 seconds
}

// Called by I2C hardware when master requests data
// MUST BE FAST - runs in interrupt context!
void sendData() {
    // Just send the pre-encoded buffer
    Wire.write(encodedData, encodedSize);
}
```

**Key insight**: The sensor is read in `loop()` where blocking is acceptable. The I2C interrupt (`onRequest`) just dumps the pre-encoded buffer - no blocking operations.

### Phase 2: Main Board Requests (I2C Master Mode)

```cpp
// Running on Meshtastic RAK4631/ESP32/etc.

#include <Wire.h>
#include "telemetry.pb.h"

#define SENSOR_SLAVE_ADDRESS 0x11

void readSensorTelemetry() {
    // Request data from slave
    size_t bytesReceived = Wire.requestFrom(SENSOR_SLAVE_ADDRESS, 100);
    
    if (bytesReceived == 0) {
        // Sensor board not responding
        return;
    }
    
    // Read the protobuf bytes
    uint8_t buffer[100];
    for (size_t i = 0; i < bytesReceived; i++) {
        buffer[i] = Wire.read();
    }
    
    // Decode protobuf
    meshtastic_EnvironmentMetrics metrics;
    decodeProtobuf(buffer, bytesReceived, &metrics);
    
    // Now publish to mesh
    publishTelemetry(&metrics);
}

void loop() {
    // Poll sensor board every 60 seconds
    if (shouldReadSensor()) {
        readSensorTelemetry();  // Non-blocking - quick I2C transaction
    }
    
    // Continue with mesh networking, GPS, etc.
}
```

**Key insight**: The main board just does a quick I2C transaction (`requestFrom`) which is non-blocking. No complex sensor libraries, no timing issues.

## The Protobuf Magic

### Why Protobuf?

Meshtastic already uses Protocol Buffers (protobuf) for all mesh messages. It's a compact, structured binary format that:
- Is space-efficient (critical for LoRa bandwidth)
- Has type safety
- Is backward/forward compatible
- Has code generators for many languages

### The Telemetry Protobuf Schema

From `telemetry.proto` (simplified):

```protobuf
message Telemetry {
    uint32 time = 1;
    oneof variant {
        DeviceMetrics device_metrics = 2;
        EnvironmentMetrics environment_metrics = 3;
        AirQualityMetrics air_quality_metrics = 4;
        PowerMetrics power_metrics = 5;
    }
}

message EnvironmentMetrics {
    optional float temperature = 1;           // Celsius
    optional float relative_humidity = 2;     // Percent
    optional float barometric_pressure = 3;   // hPa
    optional float gas_resistance = 4;        // MOhm
    optional uint32 iaq = 7;                  // Air quality index
    // ... many more fields
}

message AirQualityMetrics {
    optional uint32 pm10_environmental = 4;   // PM1.0 µg/m³
    optional uint32 pm25_environmental = 5;   // PM2.5 µg/m³
    optional uint32 pm100_environmental = 6;  // PM10 µg/m³
    optional float pm_voc_idx = 13;           // VOC index
    optional float pm_nox_idx = 14;           // NOx index
    // ... more fields
}
```

### Encoding on Sensor Board

Using nanopb (embedded protobuf library):

```cpp
#include <pb_encode.h>

meshtastic_EnvironmentMetrics metrics = {
    .has_temperature = true,
    .temperature = 23.5,
    .has_relative_humidity = true,
    .relative_humidity = 45.0
};

uint8_t buffer[meshtastic_EnvironmentMetrics_size];  // Max size: 85 bytes
pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

if (pb_encode(&stream, meshtastic_EnvironmentMetrics_fields, &metrics)) {
    size_t bytesWritten = stream.bytes_written;  // Actual size (varies)
    // buffer now contains: [binary protobuf data]
    // Example: 0d 00 00 bc 41 15 00 00 34 42
    //          ^^^^^^^^^^^ ^^^^^^^^^^^ 
    //          temperature  humidity (floats encoded as little-endian)
}
```

Only fields marked `has_xxx = true` are encoded, saving space.

### Decoding on Main Board

```cpp
#include <pb_decode.h>

uint8_t buffer[85];  // Received from I2C slave
size_t length = 10;  // Actual bytes received

meshtastic_EnvironmentMetrics metrics = meshtastic_EnvironmentMetrics_init_zero;
pb_istream_t stream = pb_istream_from_buffer(buffer, length);

if (pb_decode(&stream, meshtastic_EnvironmentMetrics_fields, &metrics)) {
    if (metrics.has_temperature) {
        printf("Temperature: %.1f°C\n", metrics.temperature);
    }
    if (metrics.has_relative_humidity) {
        printf("Humidity: %.1f%%\n", metrics.relative_humidity);
    }
}
```

## Real-World Use Cases

### Use Case 1: Complex Air Quality Monitoring

**Problem:** SEN66 sensor provides:
- Particulate matter (PM1.0, PM2.5, PM4.0, PM10)
- VOC index (requires warm-up time)
- NOx index (requires warm-up time)  
- Temperature and humidity
- CO2 (optional)

Reading all values takes 100-200ms with multiple I2C transactions. This blocks the nRF52's BLE stack.

**Solution:** Run SEN66 on Arduino slave:
```cpp
// Arduino reads SEN66 every 10s (blocking OK)
void loop() {
    sen66.readMeasuredValues(
        pm1, pm2p5, pm4, pm10, 
        humidity, temp, voc, nox, co2
    );  // Takes 150ms - no problem!
    
    encodeToProtobuf();
    delay(10000);
}
```

### Use Case 2: Custom Sensor Not in Meshtastic

**Problem:** You built a custom sensor (maybe using analog inputs, OneWire, exotic I2C device) that's not in Meshtastic firmware.

**Solution:** Write Arduino sketch that:
1. Reads your custom sensor
2. Maps values to closest Meshtastic protobuf type
3. Becomes an I2C slave

No need to modify Meshtastic firmware or submit PRs!

### Use Case 3: Multiple Sensors on One Board

**Problem:** You want BME280 + TSL2561 + INA219 but Meshtastic has limited RAM.

**Solution:** Arduino slave reads all three:
```cpp
void loop() {
    float temp = bme280.readTemperature();
    float pressure = bme280.readPressure();
    float lux = tsl2561.readLux();
    float voltage = ina219.readVoltage();
    
    // Combine into one EnvironmentMetrics message
    metrics.temperature = temp;
    metrics.barometric_pressure = pressure;
    metrics.lux = lux;
    metrics.voltage = voltage;
    
    encodeToProtobuf();
}
```

Single I2C transaction to Meshtastic gets all data.

### Use Case 4: Sensor Requires Long Warm-up

**Problem:** Some gas sensors need 30+ seconds to stabilize after power-on.

**Solution:** Arduino slave handles warm-up:
```cpp
void setup() {
    sensor.powerOn();
    // Keep Arduino running during warm-up
    // Meshtastic can sleep/do other things
}

void loop() {
    if (sensor.isReady()) {
        float reading = sensor.read();
        encodeToProtobuf();
    }
}
```

## How meshtastic/i2c-sensor Repo is Structured

```
meshtastic/i2c-sensor/
├── src/
│   ├── main.cpp                    # Example sketch with #ifdef for modes
│   ├── I2CHost.h                   # Sensor board (slave) helpers
│   ├── I2CClient.h                 # Main board (master) helpers  
│   ├── I2CDefinitions.h            # Shared constants (address 0x11)
│   └── generated/
│       ├── telemetry.pb.h          # Protobuf definitions (nanopb)
│       └── telemetry.pb.c          # Protobuf field descriptors
├── platformio.ini                  # Build configs for kb2040_host, kb2040_client
└── README.md                       # Brief (too brief!) usage guide
```

### The main.cpp Pattern

```cpp
#include <I2CDefinitions.h>
#include <Wire.h>

#if MESHTASTIC_I2C_SENSOR_HOST  // Sensor board mode
#include <I2CHost.h>

meshtastic_EnvironmentMetrics metrics = {...};

void onReceiveRequest(int numBytes) {
    sendMetrics(metrics);  // Helper from I2CHost.h
}

void setup() {
    Wire.begin(MT_I2C_ADDRESS);     // Slave at 0x11
    Wire.onReceive(onReceiveRequest);
}

void loop() {
    // Read your sensor
    metrics.temperature = readSensor();
    delay(10000);
}

#else  // MESHTASTIC_I2C_SENSOR_HOST == 0 → Main board mode
#include <I2CClient.h>

meshtastic_EnvironmentMetrics lastMetrics;

void onReceiveMetrics(int length) {
    // Called when data received from slave
    uint8_t buffer[100];
    Wire.readBytes(buffer, length);
    decodeProtobuf(buffer, length, &lastMetrics);
}

void setup() {
    Wire.begin(MT_I2C_ADDRESS);  // Master mode (address ignored)
    Wire.onReceive(onReceiveMetrics);
}

void loop() {
    // Request data from slave
    Wire.requestFrom(MT_I2C_ADDRESS, 100);
    // Data arrives → onReceiveMetrics called
    
    // Use lastMetrics...
    delay(60000);
}
#endif
```

## Integration with Meshtastic Firmware

When you use this pattern with real Meshtastic, you only need the **sensor board** (host mode) sketch. The Meshtastic firmware already has the master/client side built in.

### In Meshtastic Firmware

The `TelemetrySensor` base class in `src/modules/Telemetry/Sensor/TelemetrySensor.h` provides the framework. You create a sensor class like:

```cpp
class CustomI2CSensor : public TelemetrySensor {
public:
    bool initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev) override {
        // Initialize I2C master connection
        this->i2cBus = bus;
        return true;
    }
    
    int32_t runOnce() override {
        // Poll the Arduino slave
        size_t bytes = i2cBus->requestFrom(0x11, 100);
        // Decode protobuf...
        return 60000;  // Poll every 60 seconds
    }
    
    bool getMetrics(meshtastic_Telemetry *measurement) override {
        // Provide decoded data for mesh publication
        measurement->variant.environment_metrics = lastMetrics;
        return true;
    }
};
```

## Common Mistakes & Gotchas

### Mistake 1: Backwards I2C Roles

```cpp
// ❌ WRONG - Makes main board a slave
Wire.begin(0x11);           // On Meshtastic device
Wire.onReceive(callback);   // Waiting for data

// ✅ CORRECT - Main board is master
Wire.begin();               // No address = master mode
Wire.requestFrom(0x11, 100); // Request from slave
```

### Mistake 2: Blocking in onRequest/onReceive

```cpp
// ❌ WRONG - Blocking in ISR
void onRequest() {
    float temp = sensor.readTemperature();  // Takes 50ms!
    encodeAndSend(temp);
}

// ✅ CORRECT - Send pre-encoded data
uint8_t buffer[100];
size_t bufferSize = 0;

void loop() {
    float temp = sensor.readTemperature();
    bufferSize = encode(buffer, temp);
}

void onRequest() {
    Wire.write(buffer, bufferSize);  // Fast!
}
```

### Mistake 3: Wrong Protobuf Type

```cpp
// ❌ WRONG - Using EnvironmentMetrics for air quality
meshtastic_EnvironmentMetrics metrics;
metrics.pm25_environmental = ...;  // Field doesn't exist!

// ✅ CORRECT - Use AirQualityMetrics
meshtastic_AirQualityMetrics metrics;
metrics.has_pm25_environmental = true;
metrics.pm25_environmental = 25;
```

### Mistake 4: Forgetting Voltage Level Shifters

- Arduino Uno/Nano: **5V logic**
- RAK4631/ESP32: **3.3V logic**
- **Direct connection = magic smoke!**

Use bi-directional level shifter (e.g., TXS0108E) or use 3.3V Arduino (KB2040, ESP32-based).

## When NOT to Use This Pattern

**Don't use meshtastic/i2c-sensor if:**

1. **Simple, fast sensors** - If your sensor is already supported in Meshtastic (BME280, SHT31, etc.) and works fine, no need to add complexity

2. **Space-constrained** - If you're building a tiny device and can't fit two MCUs

3. **Power-critical** - Running two MCUs uses more power than one (though you can power-down the sensor board)

4. **You're comfortable with firmware** - If you can write proper non-blocking sensor code and handle I2C conflicts, direct integration is simpler

## Conclusion: Why This Exists

**meshtastic/i2c-sensor** is an elegant solution to the fundamental problem of **mixing hard real-time requirements** (mesh networking, BLE, GPS) **with unpredictable I/O** (sensors that block, hang, or take long reads).

By offloading sensors to a dedicated slave MCU:
- Main firmware stays responsive
- Sensor hangs don't crash the node  
- Easy to support exotic sensors
- Clean separation of concerns
- $5-10 extra hardware, but saves weeks of debugging

It's the embedded equivalent of microservices - isolate components that have different timing/reliability requirements.

The confusing "host/client" terminology obscures this simple pattern: **delegate sensor complexity to a cheap Arduino that waits for requests**.
