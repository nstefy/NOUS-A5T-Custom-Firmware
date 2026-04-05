# Installation Guide: From Tasmota to Custom Firmware (NOUS A5T)

This document describes the procedure for installing the custom firmware v2.7.0 on **NOUS A5T** devices that come with Tasmota pre-installed.

## The Space Problem (Flash Limit)
Because the NOUS A5T uses the **ESP8285 chip with 1MB Flash**, there is insufficient space to directly transition from Tasmota to the complex final firmware. It is necessary to use an intermediate "minimal" (Erase/Rescue) firmware that serves as a bridge.

---

## Workflow
1. **Tasmota UI** ➔ Upload `Erase_Firmware.bin`
2. **Erase/Rescue AP** ➔ Upload `NOUS_A5T_firmware.bin`
3. **Final Setup** ➔ WiFi and MQTT Configuration

---

## Step 1: Prepare the Files
To begin, ensure you have the two necessary binary files available:
- `Erase_Firmware.bin` (The intermediate "rescue" firmware)
- `NOUS_A5T_firmware.bin` (The final v2.7.0 firmware for NOUS A5T)

*Note: These files are already compiled and ready for use. Manual compilation is not required if you already have them. Make sure the `NOUS_A5T_firmware.bin` file is accessible from the device (phone or PC) you will use to connect to the `Firmware_Rescue_AP` network in Step 3.*

---

## Step 2: Upload the Intermediate (Rescue) Firmware
1. Access the device's Tasmota web interface.
2. Go to **Firmware Upgrade**.
3. In the **Upgrade by File Upload** section, select the `Erase_Firmware.bin` file.
4. Click **Start Upgrade**.
5. After completion, the device will restart and create a new WiFi access point named `Firmware_Rescue_AP`.

---

## Step 3: Upload the Final Firmware
1. Connect your phone or PC to the `Firmware_Rescue_AP` WiFi network (no password).
2. Open your browser and navigate to: `http://192.168.4.1`.
3. In the rescue interface, click **Choose File** and select the final `NOUS_A5T_firmware.bin` file.
4. Click **Upload**.
5. Wait for completion. The device will automatically restart with the new custom firmware.

---

## Step 4: Initial Configuration (Custom Firmware)
After restarting, the device will enter the configuration mode specific to the NOUS firmware:
1. Connect to the `NOUS-Setup` WiFi network.
2. Access `http://192.168.4.1`.
3. Configure your home WiFi credentials.
4. After saving, the device will be accessible at `http://nous-a5t.local` (mDNS).

---

## Final Firmware Features (v2.7.0)
- **Multi-Socket Control**: 3 AC outlets + 1 USB port (independent).
- **Energy Monitoring**: Voltage (V), Current (A), Power (W), and Power Factor (PF) via CSE7766.
- **Protection**: Automatic shutdown if power exceeds **3680W (16A)**.
- **HA Integration**: MQTT auto-discovery for Home Assistant.
- **Security**: Child Lock (physical button blocking) and Web authentication.

---

## Troubleshooting
| Problem | Possible Cause | Solution |
| :--- | :--- | :--- |
| Tasmota refuses the .bin file | File too large | Ensure `Erase_Firmware` is compiled with minimal settings. |
| `Firmware_Rescue_AP` does not appear | Flash failed | Check power supply and try a hardware reset. |
| Energy values are 0 | Missing calibration | Access `/calibration` in the web interface after installation. |

---
*Note: This process is irreversible by simple software methods. Keep a copy of your Tasmota settings if you wish to revert.*

---

## Advanced Instructions / For Developers

This section is intended for advanced users or those who wish to modify, compile, or understand the firmware's operation in more depth.

### 1. Compiling Firmware from Source
If you wish to compile the firmware yourself (either the rescue or the final one), you will need:
*   **Arduino IDE** (or PlatformIO).
*   **ESP8266 board support** installed in the IDE.
*   **Required libraries**: `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266mDNS`, `Updater`, `ArduinoOTA`, `PubSubClient`, `LittleFS`, `SoftwareSerial`.
*   **Specific settings for `Erase_Firmware`**: To ensure a minimal size, select `Tools -> Flash Size: 1MB (FS:64KB)` in the Arduino IDE.

### 2. Initial Installation via Serial Cable
If the device does not respond or for the first installation on a blank chip, you can use a serial connection:
*   Connect the device to the computer via a USB-Serial converter (FTDI).
*   Identify the `RX`, `TX`, `VCC`, `GND`, and `GPIO0` pins (for flash mode).
*   Select the correct COM port in the Arduino IDE and upload the firmware.

### 3. Customizing the Intermediate Firmware (Erase/Rescue)
If you want to change the WiFi network name or add a password for `Firmware_Rescue_AP`, you can edit the `.ino` file of `Erase_Firmware` before compiling:
```cpp
const char* AP_SSID = "Custom_Rescue_Network_Name"; // WiFi network name
const char* AP_PASS = "My_Secret_Password";           // Password (NULL for open network)
const char* OTA_HOSTNAME = "esp-rescue-custom";       // Device name for OTA in Arduino IDE
```
*Caution: Adding a password to `AP_PASS` is recommended for security, especially in public environments.*

### 4. Integration with Other Smart Home Platforms
The final firmware provides tools to facilitate integration with other systems:
*   **OpenHAB**: On the configuration page (`/config`) of the web interface, you can generate code snippets for `.things` and `.items` files.
*   **ESPHome**: Also on the `/config` page, you can generate a `.yaml` file compatible with ESPHome, useful for migration or understanding the structure.

### 5. Additional Technical Details
*   **ESP8285**: The firmware disables sleep mode (`WiFi.setSleepMode(WIFI_NONE_SLEEP)`) to ensure stability on the ESP8285.
*   **LittleFS**: The LittleFS file system is used for configuration storage, with an atomic save method (`.tmp` -> `.bin`) to prevent data corruption.
*   **CSE7766**: Communication with the energy measurement chip is done via SoftwareSerial at 4800 baud, 8E1.
*   **Overload Protection**: The firmware includes automatic protection that turns off all relays if the power exceeds 3680W (16A), displaying a warning message.