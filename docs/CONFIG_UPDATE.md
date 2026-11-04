# Remote Config Updates - Ground Station Implementation

## Übersicht

Die Ground Station kann automatisch Config-Updates für Breezedude Wind-Sensoren über LoRa verteilen. Dies erfolgt durch regelmäßiges Polling des Backends und Transmission signierter Config-Pakete.

## Komponenten

### Backend API
- **GET /api/config/pending/:fanet_id** - Abfrage pending configs
- **POST /api/config/confirm** - Bestätigung nach Transmission

### Ground Station Files
- `src/config_gs.h` - Header-Datei
- `src/config_gs.cpp` - Implementation
- Integration in `src/main.cpp`

## Workflow

```
┌──────────────┐      ┌──────────────┐      ┌──────────────┐
│   Backend    │      │     GS       │      │   Device     │
└──────┬───────┘      └──────┬───────┘      └──────┬───────┘
       │                     │                     │
       │◄──── Poll /pending ─┤                     │
       │                     │                     │
       ├── signed packet ────►                     │
       │    + session_id     │                     │
       │                     │                     │
       │                     ├─── LoRa (0x0C) ────►│
       │                     │                     │
       │                     │                     ├─ Verify
       │                     │                     ├─ Apply
       │                     │                     │
       │◄── POST /confirm ───┤                     │
       │    (session_id)     │                     │
       │                     │                     │
```

## Konfiguration

### Backend URL
Standard: `http://fanet.breezedude.de/api/config`

Anpassung durch Definition in `config.h`:
```cpp
#define CONFIG_GS_BACKEND_URL "http://your-backend.com/api/config"
```

### Polling-Intervall
Standard: 60 Sekunden

Anpassung in `config_gs.cpp`:
```cpp
static constexpr uint32_t POLL_INTERVAL_MS = 60000UL;
```

## Polling-Logik

Die GS pollt automatisch alle bekannten Geräte:
- Durchläuft `weatherStore[]` und `trackingStore[]`
- Erstellt FANET-IDs: `sprintf("%02X%04X", vendor, address)`
- Ruft `/api/config/pending/:fanet_id` für jedes Gerät auf
- 100ms Delay zwischen Geräten

## Paket-Transmission

### Empfangenes Paket-Format (JSON)
```json
{
  "fanet_id": "BD1234",
  "pending": true,
  "count": 2,
  "session_id": "a1b2c3d4...",
  "packet": {
    "hex": "0cabcd1234024857...",
    "length": 140
  }
}
```

### LoRa-Transaction
1. Hex-String zu Bytes parsen
2. Paket-Typ verifizieren (0x0C = CONFIG_UPDATE_PKT_TYPE)
3. Transmission über `radio_phy->transmit()`
4. Normale FANET-Parameter (250kHz BW, SF7, CR5)

### Confirmation
Nach erfolgreicher Transmission:
```http
POST /api/config/confirm
Content-Type: application/json

{
  "session_id": "a1b2c3d4e5f6..."
}
```

## Fehlerbehandlung

### Backend-Fehler
- HTTP 404: Keine pending configs (normal, kein Error-Log)
- HTTP 4xx/5xx: Fehler geloggt + Operation abgebrochen
- JSON Parse Error: Fehler geloggt + Operation abgebrochen

### Paket-Fehler
- Ungültiges Hex-Format: Operation abgebrochen
- Falscher Paket-Typ: Fehler geloggt
- Längen-Mismatch: Fehler geloggt

### Transmission-Fehler
- Radio TX Fehler: Fehler geloggt, **aber keine Confirmation**, Session bleibt pending
- Confirmation-Fehler: Geloggt, aber Config wurde bereits gesendet

## Logging

Alle Aktivitäten werden geloggt:
- Serial Output (115200 baud)
- WebSocket zur Weboberfläche (`webconsole_print()`)

**Log-Beispiele:**
```
Config: update service initialized
Config: 2 config(s) pending for BD1234
Config: transmitting 140 bytes via LoRa
Config: packet transmitted successfully for BD1234
Config: transmission confirmed with session a1b2c3d4...
Config: update complete for BD1234
```

## Sicherheit

### Signatur-Verifizierung
- **GS**: Sendet nur das vom Backend empfangene Paket
- **Device**: Verifiziert ECDSA P-256 Signatur mit Backend Public Key
- **Device**: Prüft UUID-Suffix-Match

### Session-basierte Anti-Spoofing
- Jedes Paket hat eine einzigartige `session_id` (MD5 random)
- Nur die Session-ID vom `/pending` Call kann bestätigen
- Verhindert direktes Aufrufen von `/confirm` ohne echte Transmission

### Keine Speicherung sensibler Daten
- GS speichert keine Private Keys
- GS speichert keine Session-IDs langfristig
- Paket-Inhalte werden nicht persistiert

## Deaktivierung

Config-Updates können durch `#define` deaktiviert werden:

```cpp
// In config_gs.h oder config.h
#define BREEZEDUDE_GS_ENABLE_CONFIG_UPDATE 0
```

Kompiliert zu leeren Funktionen → kein Code-Overhead.

## Testing

### Manual Test
1. Config manuell in DB einfügen:
```sql
INSERT INTO bd_hardware.config_changes (device_id, key, value, requested_by)
VALUES ('BD1234', 'BROADCAST_INTERVAL_WEATHER', '120', 'admin');

UPDATE bd_hardware.devices 
SET config_update_pending = true 
WHERE fanet_id = 'BD1234';
```

2. GS Console monitoren:
   - Nach max. 60s sollte Polling erfolgen
   - Check "Config: X config(s) pending for BD1234"
   - Check "Config: packet transmitted successfully"

3. Backend DB prüfen:
```sql
SELECT * FROM bd_hardware.config_changes 
WHERE device_id = 'BD1234' 
ORDER BY created_at DESC;
```
Status sollte `'confirmed'` sein.

## Bekannte Einschränkungen

1. **Keine Acknowledgement vom Device**: GS sendet blind, keine Rückmeldung ob Device empfangen hat
2. **Kein Retry**: Wenn Transmission fehlschlägt, bleibt Config pending (wird beim nächsten Poll erneut versucht)
3. **Keine Priorisierung**: Alle Devices werden sequentiell gepollt
4. **WiFi-abhängig**: Kein Polling ohne WiFi-Verbindung

## Zukünftige Erweiterungen

- Device ACK handling (via FANET ACK packet)
- Prioritäts-Queue für dringende Configs
- Offline-Queue (Caching wenn Backend nicht erreichbar)
- UI-Integration (manuelles Triggern von Updates)
- Multi-Device Bulk Updates
