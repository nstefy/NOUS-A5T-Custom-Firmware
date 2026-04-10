# NOUS A5T Smart Power Strip - Custom Firmware (v2.7.1)

Professional-grade custom firmware for the **NOUS A5T Smart Power Strip** (based on the ESP8285 chip). This firmware replaces the stock software to provide enhanced privacy, local control via Web UI/MQTT, and highly accurate energy monitoring.

## Key Features

- **Multi-Socket Control**: Independent control for 3 standard AC outlets and 1 USB port.
- **Advanced Energy Monitoring**: High-precision monitoring of Voltage (V), Current (A), Active Power (W), and Power Factor (PF) via the CSE7766 chipset.
- **Industrial-Grade Reliability**: 
    - **WiFi Resilience**: Automatic background reconnection and emergency AP fallback (after 5 minutes of disconnect).
    - **Safety First**: Automatic cut-off at **3680W (16A)**.
- **Smart Home Ready**: Full MQTT support with Home Assistant Auto-Discovery.
- **Physical Security**: "Child Lock" feature to disable physical button interactions.

## Project Structure

 - `/NOUS_A5T_firmware_EN`: Main source code (English).
 - `/NOUS_A5T_firmware_RO`: Main source code (Romanian).
 - `/Erase_Firmware_EN`: Rescue/OTA bridge (English).
 - `/Erase_Firmware_RO`: Rescue/OTA bridge (Romanian).
- `/docs`: Detailed installation guides and READMEs in English and Romanian.
- `/bin`: Pre-compiled binaries for quick flashing.

## Installation Quick Start

If you are migrating from **Tasmota**, please follow the Step-by-Step Installation Guide (EN). 

Pentru utilizatorii din România, consultați Ghidul de Instalare (RO).

## MQTT Integration

The base topic defaults to `nous`.
| Feature | State Topic | Command Topic |
| :--- | :--- | :--- |
| **Relays 0-3** | `nous/relay/[0-3]` | `nous/relay/[0-3]/set` |
| **Child Lock** | `nous/child_lock` | `nous/child_lock/set` |

## Physical Buttons

### Main Button (Side)
- **Short Press**: Toggles all 4 sockets.
- **Long Press (>10s)**: Initiates a **Factory Reset**.

## Developer Information

- **Module**: ESP8285 (1MB Flash).
- **Energy Chip**: CSE7766 (UART @ 4800 baud, 8E1).
- **Storage**: LittleFS used for configuration persistence.

*This firmware is provided as-is. Always exercise caution when working with mains voltage.*