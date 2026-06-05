#include "recovery.h"

#include <LittleFS.h>
#include <Update.h>
#include <esp_partition.h>
#include "esp32/rom/spi_flash.h"

#include "target_partitions_bin.h"

extern void webconsole_print(String in);

namespace {

static constexpr uint32_t EXPECTED_FS_OFFSET = 0x670000;
static constexpr uint32_t EXPECTED_FS_SIZE = 0x180000;
static constexpr uint32_t PARTITION_TABLE_OFFSET = 0x8000;
static constexpr uint32_t PARTITION_TABLE_SECTOR_SIZE = 0x1000;

// Survives ESP.restart(); prevents migration-triggered reset loops.
RTC_DATA_ATTR static uint32_t s_partitionMigrationRestartGuard = 0;

static const char FALLBACK_FS_UPDATER_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Breezedude Recovery</title>
  <style>
    body{font-family:sans-serif;margin:1.5rem;background:#f7f7f7;color:#111}
    .card{max-width:560px;margin:0 auto;background:#fff;padding:1rem 1.2rem;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,.08)}
    h1{margin-top:0;font-size:1.2rem}
    h2{margin:.9rem 0 .4rem 0;font-size:1rem}
    .row{display:flex;align-items:center;gap:.5rem;margin:.4rem 0;font-size:.9rem}
    .dot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
    .ok{background:#22c55e}.bad{background:#ef4444}.unkn{background:#94a3b8}
    label{display:block;margin-top:.5rem;font-size:.9rem;color:#444}
    input[type=file]{display:block;margin:.3rem 0 .5rem;width:100%;padding:.5rem;box-sizing:border-box;font-size:1rem}
    input[type=text],input[type=password]{display:block;margin:.25rem 0 .4rem;width:100%;padding:.5rem;box-sizing:border-box;font-size:1rem}
    button{margin-top:.4rem;width:100%;padding:.65rem;font-size:1rem;cursor:pointer}
    .secondary{background:#f0f0f0;border:1px solid #cfcfcf}
    .wifi{background:#f8fafc;border:1px solid #e2e8f0;border-radius:8px;padding:.7rem .8rem;margin:.6rem 0}
    .hint{font-size:.82rem;color:#4b5563;line-height:1.3}
    progress{width:100%;height:18px;margin-top:.6rem;display:none}
    #pct{font-size:.85rem;color:#555;display:none}
    #status{margin-top:.6rem;font-weight:600;font-size:.95rem}
    #wifiStatus{margin-top:.45rem;font-size:.9rem;font-weight:600}
    .note{background:#fef9c3;border:1px solid #fde047;border-radius:6px;padding:.5rem .7rem;font-size:.82rem;margin:.8rem 0;line-height:1.4}
    hr{border:none;border-top:1px solid #e5e7eb;margin:.8rem 0}
  </style>
</head>
<body>
<div class="card">
  <h1>Breezedude Recovery</h1>
  <div class="row"><div class="dot unkn" id="ptDot"></div><span id="ptText">Checking partition table...</span></div>

  <div class="wifi">
    <h2>WiFi STA Setup</h2>
    <div class="hint">Set STA credentials</div>
    <label for="staSsid">STA SSID</label>
    <input id="staSsid" type="text" autocomplete="off"/>
    <label for="staPass">STA Password</label>
    <input id="staPass" type="password" autocomplete="off"/>
    <button id="wifiSaveBtn" type="button">Save WiFi and Reconnect STA</button>
    <div id="wifiStatus"></div>
  </div>

  <div class="note">
    <strong>GXAirCom migration:</strong> After the first firmware flash via GXAirCom the partition table is migrated automatically and the device reboots. A <em>second</em> firmware flash via GXAirCom may be required &ndash; after that upload littlefs.bin here.
  </div>
  <hr>
  <h2>LittleFS Upload</h2>
  <label for="file">Upload <strong>littlefs.bin</strong> to restore the web UI:</label>
  <input id="file" type="file" accept=".bin"/>
  <button id="upbtn" type="button">Upload littlefs.bin</button>
  <progress id="prog" value="0" max="100"></progress>
  <div id="pct">0%</div>
  <div id="status"></div>
  <hr>
  <button id="rebootBtn" class="secondary" type="button">Reboot Device</button>
</div>
<script>
  const wifiStatus=document.getElementById('wifiStatus');
  const staSsid=document.getElementById('staSsid');
  const staPass=document.getElementById('staPass');
  const wifiState={
    ap_ssid:'BD-Groundstation',
    ap_password:'configureme',
    keepAP:true,
    sta_ssid:'',
    sta_password:''
  };
  let ws=null;

  function setWifiStatus(t){wifiStatus.textContent=t;}

  function connectWs(){
    const proto=(location.protocol==='https:')?'wss':'ws';
    ws=new WebSocket(proto+'://'+location.host+'/ws');
    ws.onopen=()=>{
      setWifiStatus('WS connected');
      ws.send(JSON.stringify({cmd:'get_wifi'}));
    };
    ws.onmessage=(ev)=>{
      let d=null;
      try{d=JSON.parse(ev.data);}catch(e){return;}
      if(d && typeof d==='object'){
        if(typeof d.ap_ssid==='string') wifiState.ap_ssid=d.ap_ssid;
        if(typeof d.ap_password==='string') wifiState.ap_password=d.ap_password;
        if(typeof d.keepAP==='boolean') wifiState.keepAP=d.keepAP;
        if(typeof d.sta_ssid==='string'){
          wifiState.sta_ssid=d.sta_ssid;
          staSsid.value=d.sta_ssid;
        }
        if(typeof d.sta_password==='string'){
          wifiState.sta_password=d.sta_password;
          staPass.value=d.sta_password;
        }
        if(typeof d.msg==='string') setWifiStatus(d.msg);
      }
    };
    ws.onclose=()=>{
      setWifiStatus('WS disconnected - retrying...');
      setTimeout(connectWs,1500);
    };
    ws.onerror=()=>{setWifiStatus('WS error');};
  }

  document.getElementById('wifiSaveBtn').addEventListener('click',()=>{
    if(!ws || ws.readyState!==WebSocket.OPEN){
      setWifiStatus('WS not connected yet');
      return;
    }
    const ssid=staSsid.value||'';
    const pass=staPass.value||'';
    wifiState.sta_ssid=ssid;
    wifiState.sta_password=pass;
    ws.send(JSON.stringify({
      cmd:'save_wifi',
      sta_ssid:ssid,
      sta_password:pass,
      ap_ssid:wifiState.ap_ssid,
      ap_password:wifiState.ap_password,
      keepAP:wifiState.keepAP
    }));
    setWifiStatus('Saving WiFi...');
  });

  connectWs();

  fetch('/partition_status').then(r=>r.json()).then(d=>{
    const dot=document.getElementById('ptDot');
    const txt=document.getElementById('ptText');
    if(d.correct){
      dot.className='dot ok';
      txt.textContent='Partition table: OK (Breezedude layout, FS @'+d.fs_addr+')';
    } else {
      dot.className='dot bad';
      txt.textContent='Partition table: MISMATCH (FS @'+(d.fs_addr||'?')+', expected @'+d.expected_addr+')';
    }
  }).catch(()=>{
    document.getElementById('ptText').textContent='Partition status unavailable';
  });

  const fileEl=document.getElementById('file');
  const statusEl=document.getElementById('status');
  const prog=document.getElementById('prog');
  const pct=document.getElementById('pct');

  document.getElementById('upbtn').addEventListener('click',()=>{
    const file=fileEl.files&&fileEl.files[0];
    if(!file){statusEl.textContent='Please select littlefs.bin';return;}
    if(file.name.toLowerCase()!=='littlefs.bin'){statusEl.textContent='Invalid filename \u2013 select littlefs.bin';return;}
    statusEl.textContent='';
    prog.style.display='block';
    prog.value=0;
    pct.style.display='block';
    pct.textContent='0%';
    const fd=new FormData();
    fd.append('update',file,file.name);
    const xhr=new XMLHttpRequest();
    xhr.open('POST','/update');
    xhr.upload.onprogress=e=>{
      if(e.lengthComputable){
        const p=Math.round(e.loaded/e.total*100);
        prog.value=p;
        pct.textContent=p+'%';
      }
    };
    xhr.onload=()=>{
      prog.value=100;
      pct.textContent='100%';
      statusEl.textContent=xhr.responseText;
    };
    xhr.onerror=()=>{statusEl.textContent='Upload failed (connection error)';};
    xhr.send(fd);
  });

  document.getElementById('rebootBtn').addEventListener('click',async()=>{
    statusEl.textContent='Rebooting...';
    try{await fetch('/reboot',{method:'POST'});}catch(e){}
  });
</script>
</body>
</html>
)rawliteral";

const esp_partition_t* getLittleFsPartition() {
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      NULL
  );
}

bool isExpectedPartitionLayoutPresent() {
  const esp_partition_t* fs = getLittleFsPartition();
  if (fs == nullptr) {
    Serial.println("Layout check: no FS partition found");
    return false;
  }

  bool match = (fs->address == EXPECTED_FS_OFFSET) && (fs->size == EXPECTED_FS_SIZE);
  Serial.printf("Layout check: FS @0x%06lX size=0x%06lX expected @0x%06lX size=0x%06lX -> %s\n",
                (unsigned long)fs->address,
                (unsigned long)fs->size,
                (unsigned long)EXPECTED_FS_OFFSET,
                (unsigned long)EXPECTED_FS_SIZE,
                match ? "OK" : "MISMATCH");
  return match;
}

// Buffers must live in DRAM - kTargetPartitionsBin is in DROM (flash) and must be
// copied to DRAM before any flash-write operation that may stall the data cache.
static uint8_t DRAM_ATTR s_ptWriteBuf[PARTITION_TABLE_SECTOR_SIZE];
static uint8_t DRAM_ATTR s_ptVerifyBuf[PARTITION_TABLE_SECTOR_SIZE];

// This inner function executes from IRAM so the CPU never needs to fetch instructions
// from flash while the SPI0 bus is busy doing the erase/write.
// esp_rom_spiflash_* are ROM functions and bypass the IDF dangerous-write safety check
// that aborts on addresses < 0x10000 (which includes the partition table at 0x8000).
IRAM_ATTR static bool doPartitionTableWrite(uint32_t sector, uint32_t alignedLen) {
  if (esp_rom_spiflash_erase_sector(sector) != ESP_ROM_SPIFLASH_RESULT_OK) return false;
  if (esp_rom_spiflash_write(PARTITION_TABLE_OFFSET, (const uint32_t*)s_ptWriteBuf, alignedLen) != ESP_ROM_SPIFLASH_RESULT_OK) return false;
  if (esp_rom_spiflash_read(PARTITION_TABLE_OFFSET, (uint32_t*)s_ptVerifyBuf, alignedLen) != ESP_ROM_SPIFLASH_RESULT_OK) return false;
  return true;
}

bool rewritePartitionTableRaw() {
  if (kTargetPartitionsBinLen == 0 || kTargetPartitionsBinLen > PARTITION_TABLE_SECTOR_SIZE) {
    Serial.println("Partition rewrite: invalid embedded partition table length");
    return false;
  }

  // Copy source data from DROM to DRAM before we touch the SPI bus
  const uint32_t alignedLen = (kTargetPartitionsBinLen + 3) & ~3u;
  memset(s_ptWriteBuf, 0xFF, PARTITION_TABLE_SECTOR_SIZE);
  memcpy(s_ptWriteBuf, kTargetPartitionsBin, kTargetPartitionsBinLen);

  Serial.println("Partition rewrite: writing via ROM spiflash (bypasses IDF dangerous-write check)");
  Serial.flush();

  const uint32_t sector = PARTITION_TABLE_OFFSET / PARTITION_TABLE_SECTOR_SIZE;
  if (!doPartitionTableWrite(sector, alignedLen)) {
    Serial.println("Partition rewrite: ROM spiflash operation failed");
    return false;
  }

  if (memcmp(s_ptVerifyBuf, kTargetPartitionsBin, kTargetPartitionsBinLen) != 0) {
    Serial.println("Partition rewrite: verify mismatch");
    return false;
  }

  Serial.println("Partition rewrite: success");
  return true;
}

}  // namespace

void migratePartitionLayoutIfNeeded() {
  if (isExpectedPartitionLayoutPresent()) {
    s_partitionMigrationRestartGuard = 0;
    return;
  }

  if (s_partitionMigrationRestartGuard > 0) {
    Serial.println("Partition migration: restart guard active, skipping auto-restart to avoid bootloop");
    webconsole_print("Recovery: partition mismatch persists after migration reboot attempt; skipping further auto-restarts");
    return;
  }

  Serial.println("Partition layout mismatch detected - attempting raw partition table migration");
  webconsole_print("Recovery: partition layout mismatch detected, migrating partition table...");
  if (rewritePartitionTableRaw()) {
    s_partitionMigrationRestartGuard = 1;
    webconsole_print("Recovery: partition table migrated, rebooting now");
    Serial.flush();
    delay(300);
    ESP.restart();
  }

  webconsole_print("Recovery: partition table migration failed");
}

void registerRecoveryRoutes(
  AsyncWebServer& server,
  AsyncWebSocket& ws,
  bool& littlefsMounted,
  volatile bool& otaUploadInProgress,
  volatile bool& otaUploadRejected
) {
  auto* wsRef = &ws;
  auto* littlefsMountedRef = &littlefsMounted;
  auto* otaUploadInProgressRef = &otaUploadInProgress;
  auto* otaUploadRejectedRef = &otaUploadRejected;

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Rebooting...");
    webconsole_print("Recovery: reboot requested from fallback updater page");
    delay(200);
    ESP.restart();
  });

  server.on("/partition_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const esp_partition_t* fs = getLittleFsPartition();
    bool correct = (fs != nullptr)
                   && (fs->address == EXPECTED_FS_OFFSET)
                   && (fs->size == EXPECTED_FS_SIZE);
    char json[200];
    if (fs) {
      snprintf(json, sizeof(json),
        "{\"correct\":%s,\"fs_addr\":\"0x%06lX\",\"fs_size\":\"0x%06lX\","
        "\"expected_addr\":\"0x%06lX\",\"expected_size\":\"0x%06lX\"}",
        correct ? "true" : "false",
        (unsigned long)fs->address, (unsigned long)fs->size,
        (unsigned long)EXPECTED_FS_OFFSET, (unsigned long)EXPECTED_FS_SIZE);
    } else {
      snprintf(json, sizeof(json),
        "{\"correct\":false,\"fs_addr\":null,\"fs_size\":null,"
        "\"expected_addr\":\"0x%06lX\",\"expected_size\":\"0x%06lX\"}",
        (unsigned long)EXPECTED_FS_OFFSET, (unsigned long)EXPECTED_FS_SIZE);
    }
    request->send(200, "application/json", json);
  });

  server.on("/update", HTTP_POST,
    [wsRef, otaUploadInProgressRef, otaUploadRejectedRef](AsyncWebServerRequest *request) {
      bool shouldReboot = !Update.hasError() && !*otaUploadRejectedRef;
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "Update Success. Rebooting..." : "Update Failed!");
      response->addHeader("Connection", "close");
      request->send(response);

      Serial.printf("content: %lu\r\n", request->contentLength());

      if (shouldReboot) {
        webconsole_print("update upload finished successfully. Rebooting...");
        wsRef->textAll("{\"otaStatus\":\"Update complete. Rebooting...\"}");
        Serial.println("Rebooting");
        wsRef->textAll("{\"disconnect\":\"true\"}");
        // Keep otaUploadInProgress=true so loop() doesn't trigger WiFi.begin during delay
        delay(300);
        ESP.restart();
      } else {
        *otaUploadInProgressRef = false;
        *otaUploadRejectedRef = false;
        webconsole_print("update upload failed. See serial log for details.");
      }
    },
    [wsRef, littlefsMountedRef, otaUploadInProgressRef, otaUploadRejectedRef](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      size_t uploaded = index + len;
      size_t uploadSize = request->contentLength();
      int progress = ((uploaded * 100) / uploadSize) / 5;
      static int last_perc = 0;

      if (!index) {
        *otaUploadInProgressRef = true;
        *otaUploadRejectedRef = false;
        int type = 0; // U_FS, see Update.begin(...)
        size_t beginSize = UPDATE_SIZE_UNKNOWN;
        if (filename == "littlefs.bin") {
          Serial.println("Updating LittleFS");
          const esp_partition_t* fs = getLittleFsPartition();
          if (!fs) {
            *otaUploadRejectedRef = true;
            Update.abort();
            webconsole_print("littlefs update rejected: no SPIFFS/LittleFS partition found");
            wsRef->textAll("{\"otaStatus\":\"LittleFS update rejected: no FS partition\"}");
            Serial.println("LittleFS update rejected: no FS partition found");
            return;
          }
          if ((fs->address != EXPECTED_FS_OFFSET) || (fs->size != EXPECTED_FS_SIZE)) {
            *otaUploadRejectedRef = true;
            Update.abort();
            webconsole_print("littlefs update rejected: partition mismatch (likely GXAirCom layout still active)");
            wsRef->textAll("{\"otaStatus\":\"LittleFS rejected: wrong partition layout. Flash firmware again via GXAirCom first.\"}");
            Serial.printf("LittleFS update rejected: partition mismatch fs@0x%06lX size=0x%06lX expected@0x%06lX size=0x%06lX\n",
                          (unsigned long)fs->address, (unsigned long)fs->size,
                          (unsigned long)EXPECTED_FS_OFFSET, (unsigned long)EXPECTED_FS_SIZE);
            return;
          }
          // Release LittleFS partition before OTA overwrites it.
          // Even a failed begin() can hold internal state that blocks the write.
          LittleFS.end();
          *littlefsMountedRef = false;
          type = 100; // U_SPIFFS
          // Use UPDATE_SIZE_UNKNOWN so the library derives size from the partition.
          // Passing 0 (when getLittleFSPartitionSize fails) would silently abort.
          beginSize = UPDATE_SIZE_UNKNOWN;
        }
        Serial.printf("Update Start: %s, %lu\n", filename.c_str(), uploadSize);
        webconsole_print("update upload started: " + filename + " (" + String(uploadSize) + " bytes)");
        if (!Update.begin(beginSize, type)) {
          Update.printError(Serial);
          webconsole_print("update init failed. See serial log for details.");
        }
      }

      if (*otaUploadRejectedRef) {
        if (final) {
          *otaUploadInProgressRef = false;
        }
        return;
      }

      if (!Update.hasError()) {
        if (last_perc != progress) {
          last_perc = progress;
          String msg = "{\"otaStatus\":\"" + String(progress * 5) + "%\"}";
          Serial.println(msg);
          webconsole_print("update upload progress: " + String(progress * 5) + "%");
        }
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
          webconsole_print("update write failed. See serial log for details.");
        }
      }

      if (final) {
        if (Update.end(true)) {
          Serial.printf("Update Success: %uB\n", index + len);
          webconsole_print("update image validated and written: " + String(index + len) + " bytes");
        } else {
          Update.printError(Serial);
          webconsole_print("update finalize failed. See serial log for details.");
        }
        // Do not clear otaUploadInProgress here.
        // The HTTP POST completion handler decides whether we reboot or clear flags.
      }
    }
  );

  server.on("/", HTTP_GET, [littlefsMountedRef](AsyncWebServerRequest *request) {
    if (*littlefsMountedRef && LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
      return;
    }
    if (*littlefsMountedRef && LittleFS.exists("/index.html.gz")) {
      AsyncWebServerResponse *resp = request->beginResponse(LittleFS, "/index.html.gz", "text/html");
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
      return;
    }
    request->send(200, "text/html", FALLBACK_FS_UPDATER_HTML);
  });
}
