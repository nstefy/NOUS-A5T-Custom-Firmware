# ESP8266 Firmware Rescue - OTA Update Tool

## Overview

This firmware enables **Over-The-Air (OTA)** firmware updates for ESP8266 devices. It creates an access point that allows you to upload new firmware via:
- **Web browser interface** - Upload `.bin` files directly

This is particularly useful for rescuing devices with corrupted firmware or for transitioning from other firmwares like Tasmota.

> **Note:** This tool can be uploaded directly via the Tasmota **"Upgrade by File Upload"** option. This is the recommended method for devices with 1MB Flash where space is limited.

---

## Features

✅ **Access Point Mode** - Device becomes a WiFi hotspot  
✅ **Web Upload Interface** - Drag-and-drop firmware upload via browser  
✅ **Auto Restart** - Device restarts after successful firmware update  
✅ **Error Handling** - Feedback on upload success/failure  

---

## Hardware Requirements

- **ESP8266** microcontroller (including ESP8285)
- USB cable for initial programming
- WiFi-enabled device (computer, smartphone) for updates
*Note: The pre-compiled binary provided in this repository is compiled specifically for ESP8285 (1MB Flash).*

---

## Installation

### 1. Arduino IDE Setup

1. Install **Arduino IDE** (if not already installed)
2. Add ESP8266 board support:
   - Go to: `File → Preferences`
   - Add to "Additional Boards Manager URLs":
     ```
     http://arduino.esp8266.com/stable/package_esp8266com_index.json
     ```
   - Go to: `Tools → Board → Boards Manager`
   - Search and install: **esp8266**

3. Select the board: `Tools → Board → Generic ESP8266 Module`

### 2. Libraries

Install required libraries via `Sketch → Include Library → Manage Libraries`:
- **ESP8266WiFi** (built-in)
- **ESP8266WebServer** (built-in)

### 3. Upload Initial Firmware

1. Connect ESP8266 to computer via USB
2. Open `erase_firmware.ino` in Arduino IDE
4. Click **Upload** (→ button)

---

## Usage

### Method 1: Web Browser Upload

1. **Connect to WiFi Network:**
   - Look for SSID: `Firmware_Rescue_AP`
   - No password required (open network)

2. **Open in Browser:**
   - Navigate to: `192.168.4.1`

3. **Upload Firmware:**
   - Click **Choose File** and select your `.bin` firmware file
   - Click **Upload**
   - Wait for completion (device will auto-restart)

---

## Configuration

Edit these lines in the code to customize:

```cpp
const char* AP_SSID = "Firmware_Rescue_AP";    // WiFi network name
const char* AP_PASS = NULL;                    // Password (NULL = open)
```

---

## Web Server Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Displays firmware upload page |
| `/update` | POST | Handles firmware file upload and flashing |

---

## How It Works

### Startup (setup):
1. Creates an **Access Point** with SSID `Firmware_Rescue_AP`
2. Starts **Web Server** on port 80 for file-based upload

### Upload Process:
1. User uploads `.bin` file via the Web Interface
2. Firmware is written to flash memory in chunks
3. Integrity is verified using CRC
4. Device automatically restarts with new firmware

### Continuous Operation (loop):
- Handles incoming web requests

---

## Firmware File Format

- **File Type:** `.bin` (compiled binary)
- **Source:** Exported from Arduino IDE with `Sketch → Export compiled Binary`
- **Maximum Size:** Available flash space minus 0x1000 bytes (reserved)

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| **Can't find WiFi network** | Ensure ESP8266 is powered and running firmware |
| **Can't connect to 192.168.4.1** | Make sure you're connected to `Firmware_Rescue_AP` network |
| **Upload fails** | Try smaller firmware file or check USB power supply |
| **Device doesn't restart** | Manually press reset button or power cycle |

---

## API Response Codes

- **"OK"** - Firmware update successful
- **"FAILED"** - Firmware update failed (check file integrity)

---

## Security Notes

⚠️ **Open Network:** By default, the access point has no password.  
⚠️ **For Production:** Add password protection by modifying `AP_PASS`:
```cpp
const char* AP_PASS = "YourPassword";
```

---

## Example Workflow

```
1. ESP8266 boots → Creates "Firmware_Rescue_AP" network
2. You connect WiFi → Browser to 192.168.4.1
3. Select new firmware → Click Upload
4. Device flashes firmware → Auto-restarts
5. New firmware runs ✓
```

---

## Technical Details

- **Processor:** ESP8266 (32-bit RISC)
- **Flash Memory:** 4MB typical
- **Update Method:** HTTP POST with multipart/form-data
- **Restart:** ESP.restart() function

---

## Version Info

- **Firmware:** Erase/Rescue Loader v1.0
- **Target:** ESP8266
- **Language:** C++ (Arduino)
- **Last Updated:** April 2026

---

## Support & Resources

- [ESP8266 Arduino Core](https://github.com/esp8266/Arduino)
- [ArduinoOTA Documentation](https://arduino.github.io/arduino-cli/0.23/getting-started/#upload)
- [ESP8266 Datasheet](https://www.espressif.com/en/products/socs/esp8266)

---

**Created for easy and safe ESP8266 firmware recovery and updates.**
