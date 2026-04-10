# Jurnal de Modificări (Changelog) - NOUS A5T Custom Firmware

All notable changes to this project will be documented in this file.

## [2.7.1] - 2026-04-10
### Added
- Dynamic MQTT connection test button in the configuration page (using AJAX).
- Automatic configuration generators for **OpenHAB** (.things and .items) and **ESPHome** (.yaml) to facilitate manual migration or integration.
- Support for Raw Data reporting via MQTT for advanced debugging.
- Password masking option in the web interface for WiFi, MQTT, and administration.
- Visual indicator for Child Lock status and Power-on Behavior directly on the main dashboard.

### Improved
- **Energy filtering:** Implementation of a hybrid algorithm (Median + Trimmed Mean) to stabilize readings below 15W, eliminating fluctuations caused by electronic noise.
- **Memory Management:** Optimized RAM usage by reserving string buffers (`reserve()`) and using the `F()` macro for all static strings.
- **Home Assistant Discovery:** Expansion of automatically detected entities (now includes Power Factor and the raw data sensor).

### Fixed
- Correction to the Checksum calculation for the CSE7766 chip (the sum now correctly starts from the third byte).
- Fix for WiFi stability on ESP8285 modules by forcing `WIFI_NONE_SLEEP` mode.
- Correct resetting of statistics counters (WiFi/MQTT) from the web interface.

## [2.7.0] - 2026-03-15
### Added
- Full support for NOUS A5T (3 AC outlets + 1 USB port).
- Real-time energy monitoring (V, A, W, PF) via CSE7766.
- MQTT integration with Home Assistant Auto-Discovery support.
- **Child Lock** function for disabling physical buttons.
- **Power-on Behavior** configuration (OFF, ON, PREVIOUS).
- Hardware overload protection (automatic disconnection at 3680W / 16A).
- Responsive web interface with 2-second automatic updates.
- Multi-level authentication system (Root vs. Configuration).

### Technical
- Use of **LittleFS** file system for persistent and atomic settings storage.
- Support for **OTA** (Over-The-Air) firmware updates with password protection.
- mDNS implementation for easy access via `http://nous-a5t.local`.

## [1.0.0] - Erase/Rescue Bridge
### Added
- Minimal intermediate firmware to allow transition from Tasmota (1MB Flash limit).
- Emergency WiFi access point (`Firmware_Rescue_AP`).
- Simple HTTP upload interface for `.bin` files.

---
*Note: Versions are specifically developed for the ESP8285 chip with 1MB Flash.*

---
**Legend:**
- **Added**: For new features.
- **Improved**: For modifications to existing features.
- **Fixed**: For any repaired bugs.