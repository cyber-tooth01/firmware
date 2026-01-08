# Meshtastic Firmware AI Assistant Instructions

## Project Overview
Meshtastic firmware enables decentralized LoRa mesh networking for text messaging, location sharing, and telemetry across ESP32, nRF52, RP2040, STM32WL, and Linux platforms. The architecture centers on a modular mesh networking stack with pluggable radio interfaces and feature modules.

## Core Architecture
- **Mesh Layer** (`src/mesh/`): Handles packet routing, cryptography, and radio abstraction
  - Routers: FloodingRouter, ReliableRouter, NextHopRouter for different routing strategies
  - Radio Interfaces: Platform-specific implementations (SX126x, RF95, etc.) inheriting from RadioInterface
  - Core Services: MeshService coordinates packet flow, NodeDB manages device state
- **Modules** (`src/modules/`): Feature extensions inheriting from SinglePortModule
  - Examples: TextMessageModule, PositionModule, Telemetry modules
  - Communication via protobuf messages over mesh
- **Platform Layer** (`src/platform/`): Hardware abstraction per architecture (ESP32, nRF52, etc.)
- **Variants** (`variants/`): Board-specific configurations combining platform + hardware pins/features

## Key Patterns
- **Module Registration**: Modules register with `Modules::registerModule()` in setup
- **Protobuf Serialization**: All mesh messages use generated protobuf code from `src/mesh/generated/`
- **Observer Pattern**: Event handling via Observer/Observable classes
- **Threading**: Uses concurrency/OSThread for background tasks, max 40 threads
- **Conditional Compilation**: Extensive use of `#ifdef` flags like `MESHTASTIC_EXCLUDE_*` to customize builds

## Build System
- **PlatformIO**: Primary build tool via `platformio.ini`
- **Build Scripts**: `bin/build-*.sh` dispatch to platform-specific builds
- **Common Flags**: `-Os` optimization, excludes unused RadioLib features, custom include paths
- **Output**: Binaries in `release/`, includes factory/update images and LittleFS filesystem

## Development Workflows
- **Formatting**: Use `trunk fmt` (Trunk extension) for consistent code style
- **Testing**: Unit tests in `test/` subdirs, run via PlatformIO `pio test`
- **Debugging**: 
  - ESP32: Use ESP-IDF debugger or serial monitor at 115200 baud
  - nRF52: pyocd with `pyocd.yaml` config, semihosting on telnet port 4444
- **Flashing**: `bin/device-install.sh` or `device-update.sh` for over-the-air updates
- **Versioning**: `bin/buildinfo.py` generates version strings from git/tags

## Common Conventions
- **Error Handling**: Use `LOG_ERROR` macros, avoid exceptions in embedded code
- **Memory Management**: Prefer static allocation, use `MemoryPool` for dynamic needs
- **Configuration**: Stored in protobuf format, accessed via `config.` prefixed globals
- **GPS Integration**: TinyGPS++ library, exclude custom fields to avoid heap corruption
- **Power Management**: PowerFSM manages device states (sleep, active, etc.)

## Integration Points
- **MQTT**: `src/mqtt/` for cloud bridging
- **HTTP/WebServer**: ESP32 web interface for configuration
- **Bluetooth**: NimBLE stack for phone connectivity
- **External APIs**: ATAK plugin, Serial API for external integrations

## File Organization Examples
- New radio: Add to `src/mesh/` with RadioInterface inheritance
- New module: Create in `src/modules/`, register in `Modules.cpp`
- New board: Add variant in `variants/`, define pins in `variant.h`, config in `platformio.ini`