# Changelog - NOUS A5T Custom Firmware

## [3.0.1] - 2024-04-20

### User-Facing Improvements
- **MQTT Connection Verification**: You can now verify if the MQTT broker details are correct before saving them. The test button uses the data currently entered in the form fields in real-time.
- **Improved Visibility**: Fixed the button control text. When a socket is turned ON, the text is now bright white on a green background, making it much easier to read than the previous grey text.
- **Activity Indicator**: The MQTT test button now displays a "Testing..." message while verifying the connection, letting you know the process is in progress.
- **Increased Stability**: Optimized web interface communication to prevent accidental reboots when testing invalid or unreachable network settings.

## [3.0.0] - 2024-04-18

### Refactored
- **Unified Source Code**: Merged `NOUS_A5T_firmware_EN` and `NOUS_A5T_firmware_RO` into a single, multilingual `NOUS_A5T_firmware` project. 
- **Hardware Abstraction Layer**: Decoupled hardware-specific logic from `NOUS_A5T_firmware.ino`. Moved GPIO definitions, button polling, and sensor handling into `DeviceHardware_NousA5T.h`.
- **Centralized Configuration**: Moved default credentials and SSID prefixes to the `DeviceHardware` class to ensure consistency across the application.
- **Dynamic UI Rendering**: Refactored the Web Server routes to call hardware-specific UI builders, allowing for cleaner main application code.
- **String Optimization**: Standardized the use of `F()` macros and `reserve()` for string buffers across both languages to reduce heap fragmentation.

### Added
- **Dynamic Setup SSID**: The Access Point name in setup mode now automatically appends the unique ESP Chip ID (`NOUS-Setup-XXXXXX`), preventing conflicts during multi-device deployments.
- **On-the-fly Language Switching**: Added UI toggle to switch between Romanian and English without requiring a firmware reflash.
- **Unified mDNS**: Synchronized mDNS hostname logic with the unique Chip ID format (`nous-a5t-XXXXXX.local`).
- **Comprehensive MQTT Discovery**: Added Home Assistant Auto-Discovery for all 4 relays (3 AC + 1 USB) and 5 sensors (V, A, W, PF, Energy).

### Fixed
- **Version Parity**: Corrected mismatched version strings across Romanian and English documentation.
- **Atomic Saves**: Improved LittleFS reliability by ensuring atomic renames for `hwcfg.bin` and `app.bin`.

## [2.7.3] - 2024-04-14
### Added
- **Unique mDNS Hostname**: Introduced Chip ID suffix for mDNS.
- **Automatic Migration**: Logic to migrate legacy static hostnames to the new unique format.

## [2.7.2] - 2024-04-13
### Improved
- **Memory Sanitization**: Used `memset` on config structures at boot to prevent residual data corruption.
- **Watchdog Stability**: Added `yield()` calls during filesystem cleanup to prevent WDT resets.

## [2.7.1] - 2024-04-10
### Added
- MQTT Connection Test button in Web UI.
- Configuration exporters for OpenHAB and ESPHome.
- Password masking in the web interface.

### Improved
- **Energy Filtering**: Hybrid algorithm (Median + Trimmed Mean) for low-power stabilization (<15W).

## [2.7.0] - 2024-03-15
### Added
- Initial support for NOUS A5T (3 AC + 1 USB).
- Real-time energy monitoring via CSE7766.
- Child Lock and Power-on Behavior logic.

## [1.0.0] - 2024-01-20
### Added
- Initial Erase/Rescue Bridge for Tasmota-to-Custom migration.