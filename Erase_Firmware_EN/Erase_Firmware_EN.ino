#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Updater.h>
#include <EEPROM.h>

// ================= CONFIGURATION =================
const char* AP_SSID = "Firmware_Rescue_AP";
const char* AP_PASS = NULL;       // Open network, no password
// ===============================================

ESP8266WebServer server(80);

// Web page for firmware upload. Using a raw string literal (R"html(...)html")
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
<h2>ESP Rescue & Erase Tool</h2>
<div class='card'>
  <h3>Firmware Update</h3>
  <form id='u' method='POST' action='/update' enctype='multipart/form-data'>
    <input type='file' name='update' accept='.bin'>
    <button type='button' id='b' onclick='up()'>Upload</button>
    <div style='margin-top:10px; font-size:13px;'>
      <input type='checkbox' name='nuke' id='nuke'> <label for='nuke'><b>Absolute Wipe</b> (Format Flash + Reset WiFi)</label>
    </div>
  </form>
  <script>
  function up(){
    var n=document.getElementById('nuke').checked;
    var f=document.getElementById('u');
    document.getElementById('b').disabled = true;
    document.getElementById('b').innerHTML = 'Uploading...';
    f.action='/update'+(n?'?nuke=1':'');
    f.submit();
  }
  </script>
</div>
</body></html>
)html";

void setup() {
  // 1. Start Access Point (AP)
  WiFi.softAP(AP_SSID, AP_PASS);

  // 2. Initialize Web Server (for file-based update)
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", updatePage);
  });

  server.on("/info", HTTP_GET, []() {
    uint32_t max_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    server.send(200, "text/plain", String(max_space / 1024));
  });

  server.on("/update", HTTP_POST, []() {
    bool success = !Update.hasError();
    if (success) {
      server.send(200, "text/plain", "OK - Firmware installed. Restarting...");
      
      if (server.hasArg("nuke")) {
        LittleFS.begin();
        LittleFS.format();
        EEPROM.begin(512);
        for (int i = 0; i < 512; i++) EEPROM.write(i, 0xFF);
        EEPROM.commit();
        WiFi.disconnect(true);
        ESP.eraseConfig();
      }
      
      delay(1000);
      ESP.restart();
    } else {
      server.send(200, "text/plain", "ERROR: " + Update.getErrorString());
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
    }
  });

  server.begin();
}

void loop() {
  server.handleClient();
}