#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <Updater.h>

// ================= CONFIGURARE =================
const char* AP_SSID = "Firmware_Rescue_AP";
const char* AP_PASS = NULL;       // Retea deschisa, fara parola
const char* OTA_HOSTNAME = "esp-rescue";
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
<div class='card'>
  <h2>Actualizare Firmware (Fisier)</h2>
  <p class='info'>Selectati fisierul .bin si apasati 'Upload'. Dispozitivul se va restarta automat.</p>
  <form method='POST' action='/update' enctype='multipart/form-data'>
    <input type='file' name='update'>
    <button type='submit'>Upload</button>
  </form>
</div>
<div class='card'>
  <h2>Actualizare OTA (WiFi)</h2>
  <p class='info'>Dispozitivul este vizibil in Arduino IDE la portul de retea 'esp-rescue'.</p>
</div>
</body></html>
)html";

void setup() {
  // 1. Pornim Access Point (AP)
  WiFi.softAP(AP_SSID, AP_PASS);

  // 2. Initializam ArduinoOTA (pentru update din IDE)
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([]() {
    // Nu este necesara o actiune speciala la start
  });
  ArduinoOTA.onEnd([]() {
    // Dupa ce update-ul s-a terminat, restartam dispozitivul
    ESP.restart();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    // Aici se pot gestiona erorile OTA, daca este necesar
  });
  ArduinoOTA.begin();

  // 3. Initializam Web Server (pentru update prin fisier)
  
  // Handler pentru pagina principala, care afiseaza formularul de upload
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", updatePage);
  });

  // Handler pentru procesul de upload. Are doua parti:
  // - o functie care se executa la finalul upload-ului (pentru a trimite raspunsul si a restarta)
  // - o functie care se executa in timpul upload-ului (pentru a scrie datele in flash)
  server.on("/update", HTTP_POST, []() {
    // Trimite un raspuns simplu catre browser si restarteaza
    server.send(200, "text/plain", (Update.hasError()) ? "ESUAT" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Incepe procesul de scriere in flash
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
  // Procesam constant cererile venite pe web server si cele de la ArduinoOTA
  server.handleClient();
  ArduinoOTA.handle();
}
