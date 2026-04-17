# Firmware Personalizat pentru NOUS A5T (ESP8266 / ESP8285)

Acest firmware este o soluție personalizată pentru priza inteligentă NOUS A5T, bazată pe platforma ESP8266/ESP8285. Oferă control complet asupra celor 4 relee (inclusiv portul USB), monitorizare a consumului de energie (tensiune, curent, putere, factor de putere) și integrare extinsă cu sisteme de automatizare precum MQTT și Home Assistant.

**Versiune Firmware:** `v3.0.0` (Vezi Changelog)

## Cuprins

1.  Caracteristici Principale
2.  Hardware Suportat
3.  Instalare
4.  Modul de Configurare (AP)
5.  Configurare Web (Mod Normal)
    *   Setări WiFi
    *   Setări MQTT
    *   Setări Securitate
    *   Calibrare Energie
    *   Actualizare Firmware (OTA)
    *   Resetare la Setările din Fabrică
6.  Control și Funcționalitate
    *   Control Relee
    *   Blocare Butoane Fizice (Child Lock)
    *   Comportament la Pornire (Power-on Behavior)
    *   Monitorizare Energie
    *   Butoane Fizice
7.  Integrare MQTT
    *   Topicuri MQTT
    *   Home Assistant Discovery
    *   Generare Configurație OpenHAB / ESPHome
8.  Note Tehnice

---

## Caracteristici Principale

*   **Control Relee:** 4 relee individuale (3 prize, 1 port USB).
*   **Monitorizare Energie:** Măsurarea tensiunii (V), curentului (A), puterii (W) și factorului de putere (PF) prin cipul CSE7766.
*   **Conectivitate:** Suport WiFi (client STA și AP pentru configurare).
*   **Integrare MQTT:** Publicare și subscriere la topicuri MQTT pentru control și monitorizare.
*   **Home Assistant Discovery:** Suport complet pentru integrarea automată în Home Assistant.
*   **Interfață Web:** Pagini web responsive pentru configurare și monitorizare.
*   **Actualizare OTA:** Actualizare firmware Over-The-Air.
*   **Calibrare Energie:** Funcții de calibrare automată și manuală pentru măsurători precise.
*   **Blocare Butoane (Child Lock):** Opțiune pentru a dezactiva butoanele fizice.
*   **Comportament la Pornire:** Configurare pentru starea releelor la repornire (OFF, ON, PREVIOUS).
*   **Securitate:** Autentificare HTTP pentru paginile web și parolă pentru OTA.
*   **mDNS:** Accesibil prin hostname (ex: `http://nous-a5t.local`).

## Hardware Suportat

*   **Dispozitiv:** NOUS A5T (ESP8266 / ESP8285)
*   **Microcontroler:** ESP8266 sau ESP8285
*   **Relee:** GPIO14 (R1), GPIO12 (R2), GPIO13 (R3), GPIO5 (R4 - USB, inversat)
*   **Buton Fizic:** GPIO16 (pentru control global și reset)
*   **LED:** GPIO2 (LED Link, inversat)
*   **Senzor Energie:** CSE7766 (conectat via SoftwareSerial pe RX=GPIO3, TX=GPIO1)

## Instalare

Firmware-ul poate fi instalat inițial prin cablu serial (dacă este necesar) sau, după prima instalare, prin funcția OTA (Over-The-Air).

1.  **Compilare:** Deschideți fișierul `.ino` în Arduino IDE (sau PlatformIO). Asigurați-vă că aveți instalate bibliotecile necesare: `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266mDNS`, `Updater`, `ArduinoOTA`, `PubSubClient`, `LittleFS`, `SoftwareSerial`.
2.  **Flash Inițial:** Conectați dispozitivul la computer printr-un convertor USB-Serial. Selectați placa corectă (ex: `Generic ESP8266 Module` sau `NodeMCU 1.0 (ESP-12E Module)`). Asigurați-vă că setările de flash sunt corecte (Flash Size, CPU Frequency etc.). Încărcați firmware-ul.
3.  **Actualizare OTA:** După prima instalare, puteți actualiza firmware-ul accesând pagina web `/update` și încărcând fișierul `.bin` generat de Arduino IDE.

## Modul de Configurare (AP)

Dacă dispozitivul nu se poate conecta la rețeaua WiFi configurată (sau dacă nu a fost configurat niciodată), va intra în modul AP (Access Point).

1.  **Conectare:** Căutați o rețea WiFi numită `NOUS-Setup` și conectați-vă la ea.
2.  **Acces:** Deschideți un browser web și navigați la `http://192.168.4.1`.
3.  **Configurare WiFi:** Pe pagina de configurare, veți putea selecta rețeaua WiFi la care doriți să vă conectați și să introduceți parola. Există și o opțiune pentru a șterge fișierele inutile (ex: de la un firmware Tasmota anterior).
4.  **Repornire:** După salvare, dispozitivul va reporni și va încerca să se conecteze la rețeaua specificată.

## Configurare Web (Mod Normal)

Odată conectat la rețeaua WiFi, dispozitivul va fi accesibil prin adresa IP sau prin hostname-ul mDNS (implicit `http://nous-a5t-XXXXXX.local`).

### Setări WiFi
*   Configurația WiFi se face inițial în modul AP.

### Setări MQTT
Accesibilă la `/config`.
*   **Host, Port, Client ID, User, Pass:** Detalii pentru conexiunea la brokerul MQTT.
*   **Topic Principal:** Prefixul pentru toate topicurile MQTT (ex: `nous`).
*   **mDNS Hostname:** Numele sub care dispozitivul va fi vizibil în rețea (ex: `nous-a5t-XXXXXX.local`).
*   **Control MQTT:** Activează/dezactivează funcționalitatea MQTT.
*   **Retrimite HA Discovery:** Forțează retransmiterea mesajelor de descoperire pentru Home Assistant.
*   **Generare Config OpenHAB/ESPHome:** Instrumente pentru a genera automat fișiere de configurare pentru aceste platforme.

### Setări Securitate
Accesibilă la `/security`.
*   **Autentificare Pagina Status (Root):** Activează/dezactivează autentificarea pentru pagina principală (`/`).
*   **Autentificare Configurare & Administrare:** Activează/dezactivează autentificarea pentru paginile de configurare (`/config`, `/security`, `/calibration`, `/update`).
*   **Parola OTA:** Parola necesară pentru actualizările Over-The-Air.
*   **Mascare Parole:** Opțiuni pentru a afișa sau masca parolele în interfața web.

### Calibrare Energie
Accesibilă la `/calibration` și `/manual_calibration`.
*   **Calibrare Automată:** Introduceți o putere reală cunoscută (ex: un bec de 60W) și firmware-ul va calcula factorii de calibrare pentru putere și curent, menținând factorul de tensiune fix.
*   **Factori Manuali:** Ajustați direct factorii de calibrare pentru tensiune, curent și putere.
*   **Resetare Calibrare:** Resetează factorii de calibrare la valorile implicite din fabrică.

### Actualizare Firmware (OTA)
### Actualizare Firmware (OTA)
Accesibilă la `/update`.
*   Permite încărcarea unui nou fișier `.bin` pentru a actualiza firmware-ul fără a fi nevoie de o conexiune fizică.

### Resetare la Setările din Fabrică
Accesibilă la `/config` (secțiunea Administrare).
*   Șterge toate configurațiile salvate (WiFi, MQTT, aplicație) și repornește dispozitivul.

## Control și Funcționalitate

### Control Relee
*   **Interfață Web:** Pe pagina principală (`/`), există butoane pentru fiecare releu (P1, P2, P3, USB).
*   **MQTT:** Trimiteți comenzi `ON`/`OFF` sau `1`/`0` către topicurile specifice releelor.

### Blocare Butoane Fizice (Child Lock)
*   **Interfață Web:** Un buton "Child Lock" pe pagina principală permite activarea/dezactivarea blocării.
*   **MQTT:** Control prin topicul `child_lock/set`.
*   Când este activată, butoanele fizice (atât cel digital, cât și cele analogice) nu vor mai controla releele.

### Comportament la Pornire (Power-on Behavior)
*   **Interfață Web:** Pe pagina principală, puteți alege comportamentul releelor la repornirea dispozitivului:
    *   `OFF`: Toate releele se vor opri.
    *   `ON`: Toate releele se vor porni.
    *   `PREVIOUS`: Releele vor reveni la starea în care se aflau înainte de oprire.
*   **MQTT:** Control prin topicul `power_on_behavior/set` (valori `0`, `1`, `2` sau `OFF`, `ON`, `PREVIOUS`).

### Monitorizare Energie
*   **Interfață Web:** Pe pagina principală, sunt afișate în timp real valorile pentru tensiune, curent, putere și factor de putere. Acestea se actualizează automat la fiecare 2 secunde.
*   **MQTT:** Valorile sunt publicate periodic pe topicurile dedicate.

### Butoane Fizice
*   **Buton Digital (GPIO16):**
    *   **Apăsare scurtă (<5s):** Comută starea tuturor releelor (dacă cel puțin unul este OFF, toate se fac ON; dacă toate sunt ON, toate se fac OFF). Această funcție este dezactivată de Child Lock.
    *   **Apăsare lungă (>10s):** Resetează configurația WiFi și repornește dispozitivul. Această funcție este dezactivată de Child Lock.
*   **Butoane Analogice (ADC - A0):**
    *   Dispozitivul detectează apăsări pe butoanele fizice conectate la ADC (A0) pe baza pragurilor de tensiune.
    *   **Btn1 (720-770):** Comută Releul 1.
    *   **Btn2 (450-500):** Comută Releul 2.
    *   **Btn3 (200-250):** Comută Releul 3.
    *   Aceste funcții sunt dezactivate de Child Lock.

## Integrare MQTT

### Topicuri MQTT
Toate topicurile folosesc prefixul configurat (implicit `nous`).

**Control Relee:**
*   `[prefix]/relay/[0-3]/set` (Comandă: `ON`/`OFF` sau `1`/`0`)
*   `[prefix]/relay/all/set` (Comandă globală: `ON`/`OFF` sau `1`/`0`)

**Stare Relee:**
*   `[prefix]/relay/[0-3]` (Stare curentă: `ON`/`OFF`)

**Monitorizare Energie:**
*   `[prefix]/voltage` (Tensiune în Volți)
*   `[prefix]/current` (Curent în Amperi)
*   `[prefix]/power` (Putere în Wați)
*   `[prefix]/pf` (Factor de Putere)

**Stare Sistem:**
*   `[prefix]/stats/wifi` (Număr de conectări WiFi)
*   `[prefix]/stats/mqtt` (Număr de conectări MQTT)
*   `[prefix]/child_lock` (Stare Child Lock: `ON`/`OFF`)
*   `[prefix]/power_on_behavior` (Comportament la pornire: `OFF`, `ON`, `PREVIOUS`)

**Comenzi Sistem:**
*   `[prefix]/stats/reset` (Resetează contoarele WiFi și MQTT)
*   `[prefix]/child_lock/set` (Comandă Child Lock: `ON`/`OFF` sau `1`/`0`)
*   `[prefix]/power_on_behavior/set` (Comandă comportament la pornire: `OFF`, `ON`, `PREVIOUS` sau `0`, `1`, `2`)

### Home Assistant Discovery
Firmware-ul trimite automat mesaje de descoperire MQTT către Home Assistant, permițând integrarea rapidă a releelor, senzorilor de energie și a altor funcții.

### Generare Configurație OpenHAB / ESPHome
Pe pagina de configurare (`/config`), există butoane pentru a genera fragmente de cod pentru integrarea cu OpenHAB (fișiere `.things` și `.items`) și ESPHome (fișier `.yaml`), facilitând migrarea sau integrarea manuală.

## Note Tehnice

*   **ESP8285:** Firmware-ul dezactivează modul sleep (`WiFi.setSleepMode(WIFI_NONE_SLEEP)`) pentru a asigura stabilitatea pe ESP8285.
*   **LittleFS:** Sistemul de fișiere LittleFS este utilizat pentru stocarea configurației. Se folosește o metodă de salvare atomică (`.tmp` -> `.bin`) pentru a preveni coruperea datelor.
*   **CSE7766:** Comunicarea cu cipul CSE7766 se face prin SoftwareSerial la 4800 baud, 8E1. Se aplică o corecție pentru zgomotul de putere sub 0.1W.
*   **Protecție Supra-Sarcina:** Dacă puterea măsurată depășește `MAX_POWER_LIMIT` (3680W), toate releele sunt oprite automat, iar un mesaj de avertizare este afișat.
*   **Reconectare WiFi:** Dispozitivul încearcă să se reconecteze automat la WiFi. Dacă nu reușește timp de 5 minute, activează modul AP de urgență (`NOUS-Setup`).
