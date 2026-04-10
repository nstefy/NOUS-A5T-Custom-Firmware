# Jurnal de Modificări (Changelog) - NOUS A5T Custom Firmware

Toate modificările notabile aduse acestui proiect vor fi documentate în acest fișier.

## [2.7.1] - 2026-04-10
### Adăugat
- Buton de testare dinamică a conexiunii MQTT în pagina de configurare (folosind AJAX).
- Generatoare automate de configurație pentru **OpenHAB** (.things și .items) și **ESPHome** (.yaml) pentru a facilita migrarea sau integrarea manuală.
- Suport pentru raportarea datelor brute (Raw Data) prin MQTT pentru depanare avansată.
- Opțiune de mascare a parolelor în interfața web pentru WiFi, MQTT și administrare.
- Indicator vizual pentru starea Child Lock și Power-on Behavior direct în tabloul de bord principal.

### Îmbunătățit
- **Filtrarea energiei:** Implementarea unui algoritm hibrid (Mediană + Trimmed Mean) pentru stabilizarea citirilor sub 15W, eliminând fluctuațiile cauzate de zgomotul electronic.
- **Managementul Memoriei:** Optimizarea utilizării memoriei RAM prin rezervarea buffer-elor pentru string-uri (`reserve()`) și utilizarea macro-ului `F()` pentru toate șirurile statice.
- **Home Assistant Discovery:** Extinderea entităților detectate automat (acum include Factorul de Putere și senzorul de date brute).

### Remediate (Fixed)
- Corecție la calculul Checksum pentru chip-ul CSE7766 (suma începe acum corect de la al treilea octet).
- Fix pentru stabilitatea WiFi pe modulele ESP8285 prin forțarea modului `WIFI_NONE_SLEEP`.
- Resetarea corectă a contoarelor de statistici (WiFi/MQTT) din interfața web.

## [2.7.0] - 2026-03-15
### Adăugat
- Suport complet pentru NOUS A5T (3 prize AC + 1 port USB).
- Monitorizare energie în timp real (V, A, W, PF) via CSE7766.
- Integrare MQTT cu suport Home Assistant Auto-Discovery.
- Funcție **Child Lock** pentru dezactivarea butoanelor fizice.
- Configurare **Power-on Behavior** (OFF, ON, PREVIOUS).
- Protecție hardware la supra-sarcină (deconectare automată la 3680W / 16A).
- Interfață web responsive cu actualizare automată la 2 secunde.
- Sistem de autentificare multi-nivel (Root vs. Configurare).

### Tehnic
- Utilizarea sistemului de fișiere **LittleFS** pentru stocarea persistentă și atomică a setărilor.
- Suport pentru actualizări firmware **OTA** (Over-The-Air) cu protecție prin parolă.
- Implementare mDNS pentru acces facil via `http://nous-a5t.local`.

## [1.0.0] - Erase/Rescue Bridge
### Adăugat
- Firmware intermediar minimal pentru a permite tranziția de la Tasmota (limitare 1MB Flash).
- Punct de acces WiFi de urgență (`Firmware_Rescue_AP`).
- Interfață simplă de încărcare HTTP pentru fișiere `.bin`.

---
*Notă: Versiunile sunt dezvoltate special pentru chip-ul ESP8285 cu 1MB Flash.*

---
**Legendă:**
- **Adăugat**: Pentru funcționalități noi.
- **Îmbunătățit**: Pentru modificări la funcționalitățile existente.
- **Remediate**: Pentru orice bug-uri reparate.