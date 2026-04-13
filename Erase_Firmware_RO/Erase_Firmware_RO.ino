#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Updater.h>
#include <EEPROM.h>

// ================= CONFIGURARE =================
const char* AP_SSID = "Firmware_Rescue_AP";
const char* AP_PASS = NULL;       // Retea deschisa, fara parola
// ===============================================

ESP8266WebServer server(80);

// Pagina web pentru upload firmware. Folosim un raw string literal (R"html(...)html")
// pentru a scrie HTML multi-linie direct in cod.
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
  <h3>Actualizare Firmware</h3>
  <form id='u' method='POST' action='/update' enctype='multipart/form-data'>
    <input type='file' name='update' accept='.bin'>
    <button type='button' id='b' onclick='up()'>Upload</button>
    <div style='margin-top:10px; font-size:13px;'>
      <input type='checkbox' name='nuke' id='nuke'> <label for='nuke'><b>Curatare Absoluta</b> (Formatare Flash + Reset WiFi)</label>
    </div>
  </form>
  <script>
  function up(){
    var n=document.getElementById('nuke').checked;
    var f=document.getElementById('u');
    document.getElementById('b').disabled = true;
    document.getElementById('b').innerHTML = 'Se incarca...';
    f.action='/update'+(n?'?nuke=1':'');
    f.submit();
  }
  </script>
</div>
</body></html>
)html";

void setup() {
  // 1. Pornim Access Point (AP)
  WiFi.softAP(AP_SSID, AP_PASS);

  // 2. Initializam Web Server (pentru update prin fisier)
  
  // Handler pentru pagina principala, care afiseaza formularul de upload
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", updatePage);
  });

  server.on("/info", HTTP_GET, []() {
    uint32_t max_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    server.send(200, "text/plain", String(max_space / 1024));
  });

  // Handler pentru procesul de upload. Are doua parti:
  // - o functie care se executa la finalul upload-ului (pentru a trimite raspunsul si a restarta)
  // - o functie care se executa in timpul upload-ului (pentru a scrie datele in flash)
  server.on("/update", HTTP_POST, []() {
    bool success = !Update.hasError();
    if (success) {
      server.send(200, "text/plain", "OK - Firmware instalat. Se restarteaza...");
      
      if (server.hasArg("nuke")) {
        // 1. Stergere Sistem Fisiere
        LittleFS.begin();
        LittleFS.format();
        // 2. Stergere EEPROM (pentru a elimina config-ul din firmware-uri tip Tasmota/Custom)
        EEPROM.begin(512);
        for (int i = 0; i < 512; i++) EEPROM.write(i, 0xFF);
        EEPROM.commit();
        // 3. Stergere setari WiFi si Config SDK
        WiFi.disconnect(true);
        ESP.eraseConfig();
      }
      
      delay(1000);
      ESP.restart();
    } else {
      server.send(200, "text/plain", "EROARE: " + Update.getErrorString());
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      // Scrie bucatile de fisier (buffer) in memorie
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      // Finalizeaza scrierea si verifica integritatea
      Update.end(true);
    }
  });

  server.begin();
}

void loop() {
  // Procesam constant cererile venite pe web server
  server.handleClient();
}
