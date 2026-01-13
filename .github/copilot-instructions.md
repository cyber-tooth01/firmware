# Meshtastic Firmware AI Assistant Instructions

## Project Overview
Meshtastic firmware enables decentralized LoRa mesh networking for text messaging, location sharing, and telemetry across ESP32, nRF52, RP2040/RP2350, STM32WL, and Linux platforms. The architecture centers on a modular mesh networking stack with pluggable radio interfaces and feature modules.

## Core Architecture
- **Mesh Layer** (`src/mesh/`): Handles packet routing, cryptography, and radio abstraction
  - Routers: FloodingRouter, ReliableRouter, NextHopRouter for different routing strategies
  - Radio Interfaces: Platform-specific implementations (SX126x, SX128x, RF95, LR11x0, LLCC68, etc.) inheriting from RadioInterface
  - Core Services: MeshService coordinates packet flow, NodeDB manages device state and persistence
  - Packet Management: PacketCache for deduplication, PacketHistory for tracking, MeshPacketQueue for scheduling
- **Modules** (`src/modules/`): Feature extensions inheriting from SinglePortModule or ProtobufModule
  - Core: TextMessageModule, PositionModule, RoutingModule, NodeInfoModule
  - Optional: AdminModule, CannedMessageModule, DetectionSensorModule, NeighborInfoModule, TraceRouteModule, WaypointModule, AtakPluginModule
  - Telemetry: DeviceTelemetry, EnvironmentTelemetry, AirQualityTelemetry, PowerTelemetry
  - Communication via protobuf messages over mesh, modules conditionally compiled with `MESHTASTIC_EXCLUDE_*` flags
- **Platform Layer** (`src/platform/`): Hardware abstraction per architecture (ESP32, nRF52, RP2040/RP2350, STM32WL, Portduino for Linux)
- **Variants** (`variants/`): Board-specific configurations organized by chip family (esp32s3/, nrf52840/, rp2040/, stm32/, etc.)
  - Each variant has `platformio.ini` (extends base config), `variant.h` (pin definitions), optional custom metadata

## Key Patterns
- **Module Registration**: Modules instantiated and configured in `src/modules/Modules.cpp` conditionally based on `#ifdef` flags
- **Protobuf Serialization**: All mesh messages use nanopb-generated code from `src/mesh/generated/meshtastic/`
- **Observer Pattern**: Event handling via Observer/Observable classes for decoupled communication
- **Threading**: Uses `concurrency/OSThread` for background tasks, `MAX_THREADS=40` defined in platformio.ini
- **Conditional Compilation**: Extensive use of `#ifdef` flags:
  - `MESHTASTIC_EXCLUDE_*` to disable modules/features
  - `RADIOLIB_EXCLUDE_*` to exclude unused RadioLib components
  - `HAS_*` for hardware capabilities (HAS_WIFI, HAS_BLUETOOTH, HAS_TELEMETRY, HAS_SENSOR, etc.)
  - Platform-specific: `ARCH_ESP32`, `ARCH_NRF52`, `ARCH_RP2040`, `ARCH_STM32WL`, `ARCH_PORTDUINO`

## Build System
- **PlatformIO**: Primary build tool via `platformio.ini`
  - `default_envs` defines default target (commonly `tbeam`)
  - `extra_configs` dynamically includes variant-specific `.ini` files from `variants/`
  - Base configs: `arduino_base`, `networking_base`, platform-specific bases (`esp32_base`, `nrf52840_base`, etc.)
- **Build Scripts**: 
  - `bin/build-firmware.sh`: Main dispatcher, delegates to platform-specific scripts
  - `bin/build-esp32.sh`, `bin/build-nrf52.sh`, `bin/build-rp2xx0.sh`, `bin/build-stm32wl.sh`: Platform builds
  - `bin/buildinfo.py`: Generates version metadata from git
  - Pre/post scripts: `bin/platformio-pre.py`, `bin/platformio-custom.py` for build customization
- **Common Flags**: 
  - `-Os` size optimization for embedded targets
  - `-DTINYGPS_OPTION_NO_CUSTOM_FIELDS` (CRITICAL: prevents heap corruption in TinyGPS++)
  - Include paths: `-Isrc -Isrc/mesh -Isrc/mesh/generated -Isrc/gps`
  - RadioLib exclusions to reduce binary size
  - `-Wl,-Map,"${platformio.build_dir}"/output.map` for memory analysis
- **Variant Inheritance**: Variants use `extends` in platformio.ini to inherit from platform base configs, then override with board-specific settings

## Development Workflows
- **Formatting**: REQUIRED - Use `trunk fmt` (Trunk.io extension) before committing
- **Building**: 
  - `pio run -e <target>` for specific target
  - `bin/build-firmware.sh` for scripted builds
  - Default serial monitor: 115200 baud
- **Testing**: Unit tests in `test/` subdirectories, run with `pio test`
- **Debugging**: 
  - ESP32: ESP-IDF debugger, serial monitor at 115200 baud, optional `#define DEBUG_HEAP` and `DEBUG_LOOP_TIMING`
  - nRF52: pyocd with `pyocd.yaml` config, JLink support, semihosting on telnet port 4444
  - Map file analysis: Use `bin/analyze_map.py` to analyze memory usage
- **Flashing**: 
  - `bin/device-install.sh` (first install) or `device-update.sh` (OTA updates)
  - Windows: `.bat` equivalents available
  - nRF52: Uses UF2 bootloader, factory erase files in `bin/`
- **Protobuf Changes**: Run `bin/regen-protos.sh` (or `.bat` on Windows) after modifying `.proto` files in `protobufs/`
- **Versioning**: Managed by `bin/buildinfo.py`, reads from git tags and `version.properties`

## Common Conventions
- **Error Handling**: Use `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG` macros (no exceptions in embedded code)
- **Memory Management**: 
  - Prefer static allocation for embedded targets
  - Use `MemoryPool` class for managed dynamic allocation
  - Watch for heap fragmentation on constrained devices
- **Configuration**: 
  - Stored in protobuf format (nanopb), persisted to filesystem
  - Accessed via `config.*` globals (e.g., `config.device.role`, `config.power.ls_secs`)
  - Defaults managed via `Default::getConfiguredOrDefaultMs()` and similar helpers
- **GPS Integration**: 
  - TinyGPS++ library with `TINYGPS_OPTION_NO_CUSTOM_FIELDS` MANDATORY to prevent heap corruption
  - GPS status tracked globally via `gpsStatus` singleton
- **Power Management**: 
  - PowerFSM (finite state machine) manages device states: BOOT, POWER, ON, LS (light sleep), SDS (deep sleep), SHUTDOWN
  - State transitions trigger callbacks: `*Enter()`, `*Idle()`, `*Exit()` functions
  - `isPowered()` logic determines external power vs battery, affects state transitions
- **Display Management**: 
  - Global `screen` object, optional initialization based on hardware detection
  - E-ink displays: Special handling with refresh limits, fast/slow refresh modes
  - Status indicators: Global singletons `powerStatus`, `nodeStatus`, `bluetoothStatus`

## Integration Points
- **MQTT**: `src/mqtt/` for cloud bridging, enabled with `HAS_MQTT` flag
- **HTTP/WebServer**: 
  - ESP32: `src/mesh/http/WebServer.h`, excluded with `MESHTASTIC_EXCLUDE_WEBSERVER`
  - Linux/Portduino: `src/mesh/raspihttp/PiWebServer.h`
- **Bluetooth**: 
  - ESP32: NimBLE stack (`src/nimble/NimbleBluetooth.h`), excluded with `MESHTASTIC_EXCLUDE_BLUETOOTH`
  - nRF52: Nordic SoftDevice (`src/platform/nrf52/NRF52Bluetooth.h`)
- **Networking**:
  - WiFi: `src/mesh/wifi/WiFiAPClient.h` (ESP32)
  - Ethernet: `src/mesh/eth/ethClient.h`, WS5500 support
  - UDP Multicast: `src/mesh/udp/UdpMulticastHandler.h` for local mesh bridging
- **External APIs**: 
  - Phone API: `src/mesh/PhoneAPI.h` and `StreamAPI.h` for app communication
  - ATAK plugin: `src/modules/AtakPluginModule.h` for tactical awareness
  - Serial API: `src/SerialConsole.h` for CLI and external integrations

## File Organization Examples
- **New Radio Interface**: 
  1. Create `src/mesh/NewRadioInterface.h` and `.cpp` inheriting from `RadioInterface` or `RadioLibInterface`
  2. Add conditional instantiation in `src/main.cpp` based on detected radio type
  3. Update `src/detect/LoRaRadioType.h` if new radio detection needed
- **New Module**: 
  1. Create files in `src/modules/` inheriting from `SinglePortModule` or `ProtobufModule`
  2. Add conditional include and instantiation in `src/modules/Modules.cpp`
  3. Define new `MESHTASTIC_EXCLUDE_*` flag in relevant variant configs
  4. Update protobuf definitions in `protobufs/` if needed, run `bin/regen-protos.sh`
- **New Board Variant**: 
  1. Create directory under `variants/<chip_family>/<board_name>/`
  2. Add `platformio.ini` with `extends = <chip_family>_base` and board-specific flags
  3. Create `variant.h` defining all GPIO pins (LEDs, buttons, SPI, I2C, radio pins, etc.)
  4. Optional: Add `variant.cpp` for board-specific initialization
  5. Add custom metadata: `custom_meshtastic_hw_model`, `custom_meshtastic_hw_model_slug`, etc.
- **Platform-Specific Code**: Use `#ifdef ARCH_*` to conditionally include platform code, see `src/main.cpp` for examples

## Contributing
- Sign CLA at https://cla-assistant.io/meshtastic/firmware
- Use GitHub Discussions for ideas, Discord for real-time help
- Format code with `trunk fmt` before PR submission
- Follow existing patterns for modules, variants, and conditional compilation