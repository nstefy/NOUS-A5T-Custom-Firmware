# Firmware de Salvare ESP8266 - Instrument de Actualizare OTA

## Prezentare Generală

Acest firmware permite actualizări de firmware **Over-The-Air (OTA)** pentru dispozitivele ESP8266. Acesta creează un punct de acces care vă permite să încărcați un firmware nou prin:
- **Interfață web în browser** - Încărcați fișiere `.bin` direct
- **Arduino IDE** - Fără fir, prin mecanismul OTA

Acest lucru este util în special pentru a "salva" dispozitivele cu firmware corupt sau pentru actualizări la distanță convenabile.

---

## Caracteristici

✅ **Mod Punct de Acces (AP)** - Dispozitivul devine un hotspot WiFi  
✅ **Interfață Web de Încărcare** - Încărcare firmware prin drag-and-drop direct din browser  
✅ **Suport OTA pentru Arduino IDE** - Încărcare directă din Arduino IDE  
✅ **Repornire Automată** - Dispozitivul repornește după o actualizare de firmware reușită  
✅ **Gestionare Erori** - Feedback privind succesul/eșecul încărcării  

---

## Cerințe Hardware

- **Microcontroler ESP8266** (NodeMCU, D1 Mini sau similar, inclusiv ESP8285)
- Cablu USB pentru programarea inițială (dacă este necesar)
- Dispozitiv cu WiFi (computer, smartphone) pentru actualizări

---

## Instalare (Dacă este necesară compilarea din sursă)

### 1. Configurare Arduino IDE

1. Instalați **Arduino IDE** (dacă nu este deja instalat)
2. Adăugați suport pentru plăcile ESP8266:
   - Accesați: `File → Preferences`
   - Adăugați la "Additional Boards Manager URLs":
     ```
     http://arduino.esp8266.com/stable/package_esp8266com_index.json
     ```
   - Accesați: `Tools → Board → Boards Manager`
   - Căutați și instalați: **esp8266**

3. Selectați placa: `Tools → Board → Generic ESP8266 Module`

### 2. Biblioteci

Instalați bibliotecile necesare prin `Sketch → Include Library → Manage Libraries`:
- **ESP8266WiFi** (integrată)
- **ESP8266WebServer** (integrată)
- **ArduinoOTA** (integrată)

### 3. Încărcare Firmware Inițială (prin cablu USB)

1. Conectați ESP8266 la computer prin USB
2. Deschideți fișierul `erase_firmware_RO.ino` în Arduino IDE
3. Selectați portul COM corect: `Tools → Port`
4. Faceți clic pe **Upload** (butonul săgeată dreapta)

---

## Utilizare

### Metoda 1: Încărcare prin Browser Web

1. **Conectați-vă la rețeaua WiFi:**
   - Căutați SSID-ul: `Firmware_Rescue_AP`
   - Nu este necesară parolă (rețea deschisă)

2. **Deschideți în Browser:**
   - Navigați la: `http://192.168.4.1`

3. **Încărcați Firmware-ul:**
   - Faceți clic pe **Choose File** și selectați fișierul `.bin` al firmware-ului dorit (de exemplu, `NOUS_A5T_firmware.bin`)
   - Faceți clic pe **Upload**
   - Așteptați finalizarea (dispozitivul va reporni automat)

### Metoda 2: Actualizare OTA din Arduino IDE

1. **Pregătiți Noul Firmware:**
   - Deschideți schița firmware-ului dvs. în Arduino IDE
   - Selectați: `Tools → Port → esp-rescue at 192.168.4.xx (network)`
   - Faceți clic pe **Upload** (butonul săgeată dreapta)

2. **Monitorizați Încărcarea:**
   - Ieșirea va afișa progresul încărcării
   - Dispozitivul repornește după finalizare

---

## Configurare (Personalizare)

Editați aceste linii în codul `erase_firmware_RO.ino` pentru a personaliza:

```cpp
const char* AP_SSID = "Firmware_Rescue_AP";    // Numele rețelei WiFi
const char* AP_PASS = NULL;                    // Parola (NULL = rețea deschisă)
const char* OTA_HOSTNAME = "esp-rescue";       // Numele dispozitivului pentru Arduino IDE
```

---

## Cum Funcționează

### La Pornire (`setup`):
1. Creează un **Punct de Acces (AP)** cu SSID-ul `Firmware_Rescue_AP`.
2. Inițializează **ArduinoOTA** pentru actualizări wireless din IDE.
3. Pornește **Serverul Web** pe portul 80 pentru încărcarea bazată pe fișiere.

### Procesul de Încărcare:
1. Utilizatorul încarcă fișierul `.bin` sau folosește Arduino IDE.
2. Firmware-ul este scris în memoria flash în bucăți.
3. Integritatea este verificată (implicit de către `Updater` în ESP8266 Arduino Core).
4. Dispozitivul repornește automat cu noul firmware.

### Operare Continuă (`loop`):
- Gestionează cererile web primite.
- Procesează comenzile ArduinoOTA.

---

## Depanare (Troubleshooting)

| Problemă | Soluție |
| :--- | :--- |
| **Nu găsesc rețeaua WiFi** | Asigurați-vă că ESP8266 este alimentat și rulează firmware-ul. |
| **Nu mă pot conecta la 192.168.4.1** | Asigurați-vă că sunteți conectat la rețeaua `Firmware_Rescue_AP`. |
| **Încărcarea eșuează** | Încercați un fișier firmware mai mic sau verificați alimentarea USB. |
| **Dispozitivul nu repornește** | Apăsați manual butonul de resetare sau întrerupeți/reconectați alimentarea. |
| **Arduino IDE nu găsește dispozitivul** | Asigurați-vă că ESP8266 este în aceeași rețea și că hostname-ul `esp-rescue` este accesibil. |

---

## Note de Securitate

⚠️ **Rețea Deschisă:** Implicit, punctul de acces nu are parolă.  
⚠️ **Pentru Producție:** Adăugați protecție prin parolă modificând `AP_PASS` în cod:
```cpp
const char* AP_PASS = "ParolaMeaSecreta";
```