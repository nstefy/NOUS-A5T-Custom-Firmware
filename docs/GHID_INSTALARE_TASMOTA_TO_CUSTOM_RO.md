# Ghid de Instalare: De la Tasmota la Custom Firmware (NOUS A5T)

Acest document descrie procedura de instalare a firmware-ului custom v3.0.0 pe dispozitivele **NOUS A5T** care vin cu Tasmota preinstalat. 

## Problema Spațiului (Flash Limit)
Deoarece NOUS A5T utilizează cipul **ESP8285 cu 1MB Flash**, spațiul este insuficient pentru a face trecerea directă de la Tasmota la firmware-ul final complex. Este necesară utilizarea unui firmware intermediar „minimal” (Erase/Rescue) care servește drept punte.

---

## Fluxul de Lucru (Workflow)
1. **Tasmota UI** ➔ Încărcare `Erase_Firmware.bin`
2. **Erase/Rescue AP** ➔ Încărcare `NOUS_A5T_firmware.bin`
3. **Final Setup** ➔ Configurare WiFi și MQTT

---

## Pasul 1: Pregătirea Fișierelor
Pentru a începe, asigurați-vă că aveți la dispoziție cele două fișiere binare necesare:
- `Erase_Firmware.bin` (Firmware-ul intermediar de "rescue")
- `NOUS_A5T_firmware.bin` (Firmware-ul final v3.0.0 pentru NOUS A5T)


*Notă: Aceste fișiere sunt deja compilate și gata de utilizare. Nu este necesară compilarea lor manuală dacă le aveți deja. Asigurați-vă că fișierul `NOUS_A5T_firmware.bin` este accesibil de pe dispozitivul (telefon sau PC) pe care îl veți folosi pentru a vă conecta la rețeaua `Firmware_Rescue_AP` în Pasul 3.*

---

## Pasul 2: Încărcarea Firmware-ului Intermediar (Rescue)
1. Accesați interfața web Tasmota a dispozitivului.
2. Mergeți la **Firmware Upgrade**.
3. La secțiunea **Upgrade by File Upload**, selectați fișierul `Erase_Firmware.bin`.
4. Apăsați **Start Upgrade**.
5. După finalizare, dispozitivul va reporni și va crea un nou punct de acces WiFi numit `Firmware_Rescue_AP`.

---

## Pasul 3: Încărcarea Firmware-ului Final
1. Conectați-vă cu telefonul sau PC-ul la rețeaua WiFi `Firmware_Rescue_AP` (fără parolă).
2. Deschideți browserul și accesați adresa: `http://192.168.4.1`.
3. În interfața de rescue, apăsați pe **Choose File** și selectați fișierul final `NOUS_A5T_firmware.bin`.
4. Apăsați **Upload**.
5. Așteptați finalizarea. Dispozitivul se va restarta automat cu noul firmware custom.

---

## Pasul 4: Configurarea Inițială (Custom Firmware)
După restart, dispozitivul va intra în modul de configurare specific firmware-ului NOUS:
1. Conectați-vă la rețeaua WiFi `NOUS-Setup-XXXXXX` (unde XXXXXX este ID-ul unic al cipului).
2. Accesați `http://192.168.4.1`.
3. Configurați credențialele WiFi ale casei dumneavoastră.
4. După salvare, dispozitivul va fi accesibil la adresa `http://nous-a5t-XXXXXX.local` (unde XXXXXX este ID-ul unic al cipului) (mDNS).

---

## Caracteristici Firmware Final (v3.0.0)
- **Control Multi-Socket**: 3 prize AC + 1 port USB (independent).
- **Monitorizare Energie**: Tensiune (V), Curent (A), Putere (W) și Factor de Putere (PF) via CSE7766.
- **Protecție**: Oprire automată la depășirea puterii de **3680W (16A)**.
- **Integrare HA**: Auto-discovery prin MQTT pentru Home Assistant.
- **Securitate**: Child Lock (blocare butoane fizice) și autentificare Web.

---


## Instrucțiuni Avansate / Pentru Dezvoltatori

Această secțiune este destinată utilizatorilor avansați sau celor care doresc să modifice, să compileze sau să înțeleagă mai profund funcționarea firmware-ului.

### 1. Compilarea Firmware-ului din Sursă
Dacă doriți să compilați singur firmware-ul (fie cel de rescue, fie cel final), veți avea nevoie de:
*   **Arduino IDE** (sau PlatformIO).
*   **Suport pentru plăci ESP8266** instalat în IDE.
*   **Biblioteci necesare**: `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266mDNS`, `Updater`, `ArduinoOTA`, `PubSubClient`, `LittleFS`, `SoftwareSerial`.
*   **Setări specifice pentru `Erase_Firmware`**: Pentru a asigura o dimensiune minimă, selectați `Tools -> Flash Size: 1MB (FS:64KB)` în Arduino IDE.

### 2. Instalare Inițială prin Cablu Serial
În cazul în care dispozitivul nu răspunde sau pentru prima instalare pe un cip gol, puteți folosi o conexiune serială:
*   Conectați dispozitivul la computer printr-un convertor USB-Serial (FTDI).
*   Identificați pinii `RX`, `TX`, `VCC`, `GND` și `GPIO0` (pentru modul flash).
*   Selectați portul COM corect în Arduino IDE și încărcați firmware-ul.

### 3. Personalizarea Firmware-ului Intermediar (Erase/Rescue)
Dacă doriți să schimbați numele rețelei WiFi sau să adăugați o parolă pentru `Firmware_Rescue_AP`, puteți edita fișierul `.ino` al `Erase_Firmware` înainte de compilare:
```cpp
const char* AP_SSID = "Nume_Retea_Rescue_Personalizat"; // Numele rețelei WiFi
const char* AP_PASS = "Parola_Mea_Secreta";           // Parola (NULL pentru rețea deschisă)
const char* OTA_HOSTNAME = "esp-rescue-custom";       // Numele dispozitivului pentru OTA în Arduino IDE
```
*Atenție: Adăugarea unei parole la `AP_PASS` este recomandată pentru securitate, mai ales în medii publice.*

### 4. Integrarea cu Alte Platforme Smart Home
Firmware-ul final oferă instrumente pentru a facilita integrarea cu alte sisteme:
*   **OpenHAB**: Pe pagina de configurare (`/config`) a interfeței web, puteți genera fragmente de cod pentru fișierele `.things` și `.items`.
*   **ESPHome**: De asemenea, pe pagina `/config`, puteți genera un fișier `.yaml` compatibil cu ESPHome, util pentru migrarea sau înțelegerea structurii.

### 5. Detalii Tehnice Suplimentare
*   **ESP8285**: Firmware-ul dezactivează modul sleep (`WiFi.setSleepMode(WIFI_NONE_SLEEP)`) pentru a asigura stabilitatea pe ESP8285.
*   **LittleFS**: Sistemul de fișiere LittleFS este utilizat pentru stocarea configurației, cu o metodă de salvare atomică pentru a preveni coruperea datelor.
*   **CSE7766**: Comunicarea cu cipul de măsurare a energiei se face prin SoftwareSerial la 4800 baud, 8E1.
*   **Protecție Supra-Sarcina**: Firmware-ul include o protecție automată care oprește toate releele dacă puterea depășește 3680W (16A), afișând un mesaj de avertizare.