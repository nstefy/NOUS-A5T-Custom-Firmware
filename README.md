# NOUS A5T Smart Power Strip - Custom Firmware (v3.0.1)

Professional-grade custom firmware for the **NOUS A5T Smart Power Strip** (based on the ESP8285 chip). This firmware replaces the stock software to provide enhanced privacy, local control via Web UI/MQTT, and highly accurate energy monitoring.

<p align="center">
  <img src="images/1.Status.png" width="24%" />
  <img src="images/2.Config.png" width="24%" />
  <img src="images/3.Security.png" width="24%" />
  <img src="images/4.Update.png" width="24%" />
</p>

## Key Features

- **Multi-Socket Control**: Independent control for 3 standard AC outlets and 1 USB port.
- **Advanced Energy Monitoring**: High-precision monitoring of Voltage (V), Current (A), Active Power (W), and Power Factor (PF) via the CSE7766 chipset.
- **Industrial-Grade Reliability**: 
    - **WiFi Resilience**: Automatic background reconnection and emergency AP fallback (after 5 minutes of disconnect).
    - **Safety First**: Automatic cut-off at **3680W (16A)**.
- **Smart Home Ready**: Full MQTT support with Home Assistant Auto-Discovery.
- **Physical Security**: "Child Lock" feature to disable physical button interactions.
- **Configurable Behavior**: Set power-on state (ON/OFF/PREVIOUS) and access via mDNS (`nous-a5t-XXXXXX.local`).
- **Unique Identification**: Setup AP name is unique per device (`NOUS-Setup-XXXXXX`) to avoid conflicts during mass deployment.

## Project Structure

 - `/NOUS_A5T_firmware`: Unified source code (Multilingual: EN/RO).
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
| **All Relays** | - | `nous/relay/all/set` |
| **Child Lock** | `nous/child_lock` | `nous/child_lock/set` |
| **Power-on Behavior** | `nous/power_on_behavior` | `nous/power_on_behavior/set` |
| **Energy (V, A, W, PF)** | `nous/[voltage|current|power|pf]` | - |
| **System Stats** | `nous/stats/[wifi|mqtt]` | `nous/stats/reset` |

## Physical Buttons

### Main Button (Side)
- **Short Press**: Toggles all 4 sockets.
- **Long Press (>10s)**: Initiates a **Factory Reset**.

### Individual Buttons (Top)
- **Button 1**: Toggles Relay 0 (Socket 1).
- **Button 2**: Toggles Relay 1 (Socket 2).
- **Button 3**: Toggles Relay 2 (Socket 3).
*Note: Individual control is handled via the internal ADC.*

## Developer Information

- **Module**: ESP8285 (1MB Flash).
- **Energy Chip**: CSE7766 (UART @ 4800 baud, 8E1).
- **Storage**: LittleFS used for configuration persistence.

*This firmware is provided as-is. Always exercise caution when working with mains voltage.*