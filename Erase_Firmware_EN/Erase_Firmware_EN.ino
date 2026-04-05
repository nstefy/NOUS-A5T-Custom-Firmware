#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <Updater.h>

// ================= CONFIGURATION =================
const char* AP_SSID = "Firmware_Rescue_AP";
const char* AP_PASS = NULL;       // Open network, no password
const char* OTA_HOSTNAME = "esp-rescue";
// ===============================================

ESP8266WebServer server(80);

// Web page for firmware upload. We use a raw string literal (R"html(...)html")
// to write multi-line HTML directly in the code.
const char* updatePage = R"html(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP Rescue OTA</title>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{font-family:Arial,sans-serif;margin:0 auto;padding:20px;max-width:500px;background:#f4f4f9;text-align:center;}
h2{color:#333;}
.card{background:#fff;padding:20px;border-radius:5px;border:1px solid #ddd;margin-bottom:15px;}
input[type=file]{margin-bottom:10px;}
button{width:100%;background-color:#2196F3;color:white;padding:12px;border:none;border-radius:4px;cursor:pointer;font-size:16px;}
button:hover{background-color:#0b7dda;}
.info{font-size:14px;color:#555;}
</style></head><body>
<div class='card'>
  <h2>Firmware Update (File)</h2>
  <p class='info'>Select the .bin file and press 'Upload'. Device will restart automatically.</p>
  <form method='POST' action='/update' enctype='multipart/form-data'>
    <input type='file' name='update'>
    <button type='submit'>Upload</button>
  </form>
</div>
<div class='card'>
  <h2>OTA Update (WiFi)</h2>
  <p class='info'>Device is visible in Arduino IDE at network port 'esp-rescue'.</p>
</div>
</body></html>
)html";

void setup() {
  // 1. Start Access Point (AP)
  WiFi.softAP(AP_SSID, AP_PASS);

  // 2. Initialize ArduinoOTA (for update from IDE)
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([]() {
    // No special action needed at start
  });
  ArduinoOTA.onEnd([]() {
    // After update is complete, restart the device
    ESP.restart();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    // Here OTA errors can be handled if needed
  });
  ArduinoOTA.begin();

  // 3. Initialize Web Server (for update via file)
  
  // Handler for main page, which displays the upload form
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", updatePage);
  });

  // Handler for upload process. Has two parts:
  // - a function that executes at the end of upload (to send response and restart)
  // - a function that executes during upload (to write data to flash)
  server.on("/update", HTTP_POST, []() {
    // Send simple response to browser and restart
    server.send(200, "text/plain", (Update.hasError()) ? "FAILED" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Start flash write process
      Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      // Write file chunks (buffer) to memory
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      // Finalize writing and verify integrity
      Update.end(true);
    }
  });

  server.begin();
}

void loop() {
  // Constantly handle requests from web server and ArduinoOTA
  server.handleClient();
  ArduinoOTA.handle();
}
