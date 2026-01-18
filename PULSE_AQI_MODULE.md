# Pulse AQI Module - SEN66 Binary Telemetry

## Overview
Custom Meshtastic module that reads SEN66 sensor data and transmits it in a highly compact binary format over the mesh network using the PRIVATE_APP port (256).

## Features
- **Compact Binary Format**: 22 bytes total per transmission
- **10-second Update Interval**: Regular air quality updates
- **Automatic Initialization**: Detects SEN66 sensor and starts automatically
- **Mesh Broadcasting**: Sends to all mesh nodes
- **Decode Support**: Receives and logs packets from other nodes

## Data Format

### Binary Packet Structure (22 bytes)
```c
struct PulseAQIData {
    uint32_t timestamp;       // Unix timestamp (4 bytes)
    uint16_t pm1_0;           // PM1.0 µg/m³ (2 bytes)
    uint16_t pm2_5;           // PM2.5 µg/m³ (2 bytes)
    uint16_t pm4_0;           // PM4.0 µg/m³ (2 bytes)
    uint16_t pm10;            // PM10 µg/m³ (2 bytes)
    uint16_t voc_idx;         // VOC index * 10 (2 bytes, divide by 10)
    uint16_t nox_idx;         // NOx index * 10 (2 bytes, divide by 10)
    int16_t  temperature;     // Temp °C * 100 (2 bytes, divide by 100)
    uint16_t humidity;        // RH% * 100 (2 bytes, divide by 100)
    uint16_t co2;             // CO2 ppm (2 bytes)
};
```

### Comparison to Standard Telemetry
- **Standard Telemetry**: ~65 bytes encrypted protobuf
- **Pulse AQI**: 22 bytes raw binary
- **Savings**: ~66% reduction in payload size

## Serial Output Example
```
INFO  | [PulseAQI] SEN66: PM1.0=2, PM2.5=3, PM4.0=3, PM10=4, VOC=78.0, NOx=1.0, T=26.3°C, RH=66.9%, CO2=400ppm
INFO  | [PulseAQI] Sending 22 bytes on PRIVATE_APP port
```

## Module Configuration

### Port Number
- **PortNum**: `meshtastic_PortNum_PRIVATE_APP` (256)
- **Channel**: Primary (0) - uses default mesh encryption
- **Priority**: BACKGROUND

### Files Created
1. `src/modules/PulseAQIModule.h` - Module header with data structure
2. `src/modules/PulseAQIModule.cpp` - Implementation with SEN66 interface
3. Modified: `src/modules/Modules.cpp` - Registered module

### Registration Logic
```cpp
#if __has_include(<SensirionI2cSen66.h>)
    if (nodeTelemetrySensorsMap[meshtastic_TelemetrySensorType_SEN66].first > 0) {
        new PulseAQIModule();
    }
#endif
```

## Decoding Messages

### Python Decoder Example
```python
import struct

def decode_pulse_aqi(payload_bytes):
    """Decode 22-byte PulseAQI packet"""
    if len(payload_bytes) != 22:
        return None
    
    data = struct.unpack('<IHHHHHHhHH', payload_bytes)
    
    return {
        'timestamp': data[0],
        'pm1_0': data[1],
        'pm2_5': data[2],
        'pm4_0': data[3],
        'pm10': data[4],
        'voc_idx': data[5] / 10.0,
        'nox_idx': data[6] / 10.0,
        'temperature': data[7] / 100.0,
        'humidity': data[8] / 100.0,
        'co2': data[9]
    }

# Usage with Meshtastic Python API
def on_receive(packet, interface):
    if packet['decoded']['portnum'] == 'PRIVATE_APP':
        data = decode_pulse_aqi(packet['decoded']['payload'])
        if data:
            print(f"PM2.5: {data['pm2_5']} µg/m³")
            print(f"Temp: {data['temperature']}°C")
            print(f"VOC: {data['voc_idx']}")
```

### JavaScript Decoder Example
```javascript
function decodePulseAQI(payloadBuffer) {
    if (payloadBuffer.length !== 22) return null;
    
    const view = new DataView(payloadBuffer.buffer);
    
    return {
        timestamp: view.getUint32(0, true),
        pm1_0: view.getUint16(4, true),
        pm2_5: view.getUint16(6, true),
        pm4_0: view.getUint16(8, true),
        pm10: view.getUint16(10, true),
        voc_idx: view.getUint16(12, true) / 10.0,
        nox_idx: view.getUint16(14, true) / 10.0,
        temperature: view.getInt16(16, true) / 100.0,
        humidity: view.getUint16(18, true) / 100.0,
        co2: view.getUint16(20, true)
    };
}
```

## Integration with Phone App

To receive these messages in the Meshtastic phone app:

1. **iOS/Android**: Messages appear as PRIVATE_APP port
2. **Custom App**: Use Meshtastic SDK to subscribe to PRIVATE_APP packets
3. **MQTT**: Packets will be published to MQTT if enabled

### Example: Custom App Integration
```kotlin
// Android Kotlin example
meshtasticClient.registerListener { packet ->
    if (packet.decoded.portnum == Portnums.PortNum.PRIVATE_APP) {
        val data = decodePulseAQI(packet.decoded.payload.toByteArray())
        updateAirQualityDisplay(data)
    }
}
```

## Advantages Over Standard Telemetry

1. **Size**: 22 bytes vs ~65 bytes (66% smaller)
2. **Simplicity**: No protobuf overhead
3. **Speed**: Faster transmission over LoRa
4. **Dedicated**: PRIVATE_APP port avoids telemetry congestion
5. **Custom**: Can be extended with additional fields

## Disadvantages

1. **Fixed Format**: Schema changes require firmware update
2. **No Native App Support**: Requires custom decoder
3. **Binary**: Not human-readable without decoder
4. **Limited Precision**: Some values scaled/truncated

## Building

The module is automatically compiled if:
- `HAS_SENSOR` is defined (board has sensor support)
- `!MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR` (environment telemetry enabled)
- `<SensirionI2cSen66.h>` is available (library installed)
- SEN66 sensor detected during I2C scan

## Testing

1. Flash firmware to RAK4631 with SEN66 connected
2. Monitor serial output for "PulseAQI: Sending..." messages
3. Use Python/JavaScript decoder to verify packet format
4. Check mesh traffic shows PRIVATE_APP packets

## Future Enhancements

- [ ] Add configuration for update interval
- [ ] Support multiple sensor types in same packet format
- [ ] Add JSON fallback mode for debugging
- [ ] Implement request/response for on-demand readings
- [ ] Add packet versioning for backward compatibility

## Notes

- Module runs independently of EnvironmentTelemetry
- Both can coexist (standard telemetry + binary telemetry)
- Uses same SEN66 sensor but reads independently
- PRIVATE_APP port allows custom protocols without conflicts
