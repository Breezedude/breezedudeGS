// Maps of ID -> row DOM elements and last seen timestamp
const weatherStations = new Map();
const liveTracking = new Map();
const MAX_AGE_MS = 5 * 60 * 1000; // 5 minutes

let ws;
let ws_connected = false;
let reconnectInterval = 2000; // 2 seconds
let wsHeartbeatInterval;
let lastWsMessageTime = Date.now();
const WS_TIMEOUT_MS = 5000; // 5 seconds without message = disconnected
let got_settings = false;

// Section toggler
function toggleSection(header) {
  const content = header.nextElementSibling;
  content.style.display = content.style.display === "block" ? "none" : "block";
}

function show(id){
  const el = document.getElementById(id);
  if(el){
    el.style.display = 'block'
  }
}

function hide(id){
  const el = document.getElementById(id);
  if(el){
    el.style.display = 'none'
  }
}

function getVal(id) {
    const el = document.getElementById(id);
    if (!el) {
        console.warn(`Element with id '${id}' not found`);
        return null;
    }
    if (el.type === "checkbox") {
        return el.checked;
    }
    return el.value;
}

function saveWiFi() {
    const data = get_inputs_from_element('div_wifi')
    wsSend({ cmd: "save_wifi", ...data });
    setTimeout(loadFields, 150);
}


function get_inputs_from_element(id){
    const inputs =  Array.from(document.getElementById(id).querySelectorAll("input"));
    const fields = {};
    inputs.forEach(input => {
        const id = input.id || input.name;
        if (!id) return;
        fields[id] = getVal(id);
    });
    console.log(fields)
    return fields;
}

function saveSettings() {
  const data = get_inputs_from_element('div_settings')
  wsSend({ cmd: "save_settings", ...data });
  setTimeout(loadFields, 150);
}

function sendReboot() {
    wsSend({ cmd: "reboot"});
}

function onWsReady(callback) {
    if (ws.readyState === WebSocket.OPEN) {
        setTimeout(callback, 250);
    } else {
        ws.addEventListener('open', callback, { once: true });
    }
}

function wsSend(obj) {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(obj));
        //console.log(JSON.stringify(obj));
    } else {
        console.warn("WebSocket is not open. ReadyState:", ws.readyState);
    }
}

// Function to create or update a table row
function upsertRow(tableId, data, rowMap, columns) {
  const now = Date.now();
  let row = rowMap.get(data.id);

  if (!row) {
    // Create a new row
    const table = document.getElementById(tableId);
    row = document.createElement('tr');

    for (let i = 0; i < columns.length; i++) {
      const cell = document.createElement('td');
      cell.dataset.key = columns[i];
      row.appendChild(cell);
    }

    // Add hidden cell for lastSeen timestamp
    if(data.lastSeen){
      row.dataset.lastSeen = data.lastSeen;
    }
    

    // Append to table
    table.appendChild(row);
    rowMap.set(data.id, row);
  }

  // Update content
  columns.forEach(key => {
    const cell = row.querySelector(`[data-key="${key}"]`);
    if (cell) {
      if (key === "lastSeen") {
        cell.innerText = "-";
      } else {
        cell.innerText = data[key] !== undefined ? data[key] : "";
      }
    }
  });
}

// Clean up old entries
function removeOldRows(rowMap, maxAgeMs) {
  const now = Date.now();
  for (const [id, row] of rowMap.entries()) {
    const lastSeen = parseInt(row.dataset.lastSeen || "0", 10);
    if (now - lastSeen > maxAgeMs) {
      row.remove();
      rowMap.delete(id);
    }
  }
}

// Update last seen counters
function updateLastSeenCounters() {
  const now = Date.now();
  [weatherStations, liveTracking].forEach(rowMap => {
    for (const [id, row] of rowMap.entries()) {
      const cell = row.querySelector('[data-key="lastSeen"]');
      if (cell) {
        const lastSeen = parseInt(row.dataset.lastSeen || "0", 10);
        const delta = Math.floor((now - lastSeen) / 1000);
        cell.innerText = `${delta}s`;
      }
    }
  });
}

// Call every 1 second
setInterval(() => {
  updateLastSeenCounters();
  removeOldRows(weatherStations, MAX_AGE_MS);
  removeOldRows(liveTracking, MAX_AGE_MS);
}, 1000);


function updateFieldsFromData(data) {
    Object.keys(data).forEach(key => {
        const el = document.getElementById(key);
        if (el) {
            if (el.tagName === 'INPUT') {
                if (el.type === 'checkbox') {
                    el.checked = !!data[key];
                } else {
                    el.value = data[key];
                }
            } else {
                el.innerText = data[key];
            }
        } else {
            console.warn(`Element with id '${key}' not found.`);
        }

        if(key === "msg"){
          setInterval(() => {el.innerText = "";}, 3000);
        }

        if(key === "disconnect"){
          updateWsStatus(false);
          got_settings = false;
        }
        if(key === "sendAPRS" && data[key] == false){
          toggleAPRSInputs();
        }
        if(key === "deviceName"){
          updateFieldsFromData({'deviceNamedisplay': data[key]});
          got_settings =true;
        }

        deviceNamedisplay
    });
}


window.addEventListener('load', () => {
  //document.getElementById("getLocationBtn").addEventListener("click", getUserLocation);
    onWsReady(() => {
       // console.log("WebSocket connected, initializing requests...");
        loadFields();
        wsSend({ cmd: "get_fanet", "min": 5}); // get not older than 5min

        setInterval(() => {
          wsSend({ cmd: "get_info_update"});
          if(!got_settings){
            loadFields();
          }
        }, 1000);
    });

    document.getElementById('deviceName').addEventListener('input', function() {
    validateNameField('deviceName', 'saveSettingsBtn');
});
});

function loadFields(){
  wsSend({ cmd: "get_info"});
  setTimeout(() => {wsSend({ cmd: "get_wifi"});}, 100);
  setTimeout(() => {wsSend({ cmd: "get_settings"});}, 200);
}


function validateNameField(fieldId, submitButtonId) {
    const field = document.getElementById(fieldId);
    const submitButton = document.getElementById(submitButtonId);
    const value = field.value.trim();

    const isValidLength = value.length >= 3 && value.length <= 9;
    const isValidChars = /^[A-Za-z0-9]+$/.test(value);

    const errorMsg = document.getElementById('deviceNameError');
if (isValidLength && isValidChars) {
    field.style.borderColor = '';
    errorMsg.style.display = 'none';
    submitButton.disabled = false;
    return true;
} else {
    field.style.borderColor = 'red';
    errorMsg.style.display = 'inline';
    submitButton.disabled = true;
    return false;
}
}

function calculateDistanceKm(lat1, lon1, lat2, lon2) {
    const R = 6371; // Earth radius in km
    const toRad = angle => angle * Math.PI / 180;

    const dLat = toRad(lat2 - lat1);
    const dLon = toRad(lon2 - lon1);

    const a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
              Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) *
              Math.sin(dLon / 2) * Math.sin(dLon / 2);

    const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
    const distance = R * c;
    return distance;
}


function updateWsStatus(connected) {
    ws_connected = connected
    const wsStatus = document.getElementById('wsStatus');
    wsStatus.textContent = connected ? "Connected" : "Disconnected";
    wsStatus.style.color = connected ? "green" : "red";
}

// Display temporary error/status message
function showError(message) {
    const wsError = document.getElementById('wsError');
    wsError.textContent = message;
    wsError.style.color = "red";
    setTimeout(() => {
        wsError.textContent = "-";
    }, 5000);
}


function initWebSocket() {
    const wsProtocol = (location.protocol === "https:") ? "wss://" : "ws://";
    const wsUri = wsProtocol + location.host + "/ws";

    //ws = new WebSocket('ws://192.168.178.92/ws');
    ws = new WebSocket(wsUri);


  ws.onopen = function(){
    updateWsStatus(true);
    updateFieldsFromData({"otaStatus": ""});
    lastWsMessageTime = Date.now();
    if (wsHeartbeatInterval) clearInterval(wsHeartbeatInterval);

    wsHeartbeatInterval = setInterval(() => {
        if (Date.now() - lastWsMessageTime > WS_TIMEOUT_MS) {
            updateWsStatus(false);
        }
    }, 1000); // check every second
  };

  ws.onclose = function(event) {
    updateWsStatus(false); clearInterval(wsHeartbeatInterval);
    setTimeout(initWebSocket, reconnectInterval);
    got_settings = false;
  };

  ws.onerror = function(err) {
        showError("WebSocket error");
        //ws.close(); // Force reconnect on error
  };

  // Handle incoming WebSocket message
  ws.onmessage = function (event) {

    lastWsMessageTime = Date.now();
      if (document.getElementById('wsStatus').textContent !== "WS: Connected") {
          updateWsStatus(true); // auto-recover to connected if previously disconnected
      }

    const data = JSON.parse(event.data);
    //console.log(data)

    if(data.error){
      showError(data.error);
    }
    else if (data.weather && Array.isArray(data.weather)) {
      data.weather.forEach(station => {
          const rowData = {
              id: (station.vid.toString(16).toUpperCase().padStart(2, "0") + station.fanet_id.toString(16).toUpperCase().padStart(4, "0")),
              name: station.name,
              lat: station.lat !== undefined ? station.lat.toFixed(5) : "-",
              lon: station.lon !== undefined ? station.lon.toFixed(5) : "-",
              dist: calculateDistanceKm(station.lat, station.lon, getVal("lat"), getVal("lon")).toFixed(1) +" km",
              temp: station.temp !== undefined ? station.temp.toFixed(1) + "°C" : "-",
              windDir: station.wHeading !== undefined ? station.wHeading.toFixed(0) + "°" : "-",
              windSpd: station.wSpeed !== undefined ? station.wSpeed.toFixed(1) + " km/h" : "-",
              gust: station.wGust !== undefined ? station.wGust.toFixed(1) + " km/h" : "-",
              humidity: station.humidity !== undefined ? station.humidity.toFixed(0) + "%" : "-",
              pressure: station.baro !== undefined ? station.baro.toFixed(1) + " hPa" : "-",
              soc: station.charge !== undefined ? station.charge.toFixed(0) + "%" : "-",
              rssi: station.rssi !== undefined ? station.rssi + " dBm" : "-",
              lastSeen: station.tLastMsg !== undefined ? station.tLastMsg*1000 : "-"
          };

          upsertRow("weatherTable", rowData, weatherStations, [
              "id", "name","dist", "temp", "windDir", "windSpd", "gust",
              "humidity", "pressure", "soc", "rssi", "lastSeen" //  "lat", "lon", 
          ]);
      });

    } else if (data.tracking && Array.isArray(data.tracking)) {
      data.tracking.forEach(trk => {
          const rowData = {
              id: (trk.vid.toString(16).toUpperCase().padStart(2, "0") + trk.fanet_id.toString(16).toUpperCase().padStart(4, "0")),
              name: trk.name,
              lat: trk.lat !== undefined ? trk.lat.toFixed(5) : "-",
              lon: trk.lon !== undefined ? trk.lon.toFixed(5) : "-",
              dist: (trk.lat !== undefined && trk.lon !== undefined) ?
                  (calculateDistanceKm(trk.lat, trk.lon, getVal("lat"), getVal("lon")).toFixed(1) + " km") : "-",
              alt: trk.alt !== undefined ? trk.alt.toFixed(0) + " m" : "-",
              hdop: trk.hdop !== undefined ? (trk.hdop / 100).toFixed(2) : "-", // assuming hdop is in 1/100 m
              acft: trk.acft !== undefined ? trk.acft : "-", // aircraft type as raw value
              spd: trk.spd !== undefined ? trk.spd.toFixed(1) + " km/h" : "-",
              climb: trk.climb !== undefined ? trk.climb.toFixed(1) + " m/s" : "-",
              heading: trk.heading !== undefined ? trk.heading.toFixed(0) + "°" : "-",
              track: trk.track ? "Yes" : "No",
              rssi: trk.rssi !== undefined ? trk.rssi + " dBm" : "-",
              snr: trk.snr !== undefined ? trk.snr + " dB" : "-",
              lastSeen: trk.tLastMsg !== undefined ? trk.tLastMsg*1000 : "-",
              state: "-"
          };

          upsertRow("trackingTable", rowData, liveTracking, [
              "id", "name", "acft", "state", "dist", "alt", "speed", "climb", "heading", "rssi", "lastSeen" // "lat", "lon",
          ]);
      });
    }
    // update static info
    else {
      updateFieldsFromData(data);
    }
  };
}

initWebSocket();

function toggleAPRSInputs() {
    const enabled = document.getElementById("sendAPRS").checked;
    document.getElementById("aprsServer").disabled = !enabled;
    document.getElementById("aprsPort").disabled = !enabled;
}

function backupUserData() {
    const inputs = document.querySelectorAll("input");
    const data = {};

    inputs.forEach(input => {
        const id = input.id || input.name;
        if (!id) return;
        data[id] = getVal(id);
    });

    return JSON.stringify(data, null, 2);
}

function downloadUserData() {
    const jsonData = backupUserData(); // use the function we made earlier
    const blob = new Blob([jsonData], { type: "application/json" });
    const url = URL.createObjectURL(blob);

    const a = document.createElement("a");
    a.href = url;

    let deviceName = getVal("deviceName") || "BDGs";
    deviceName = deviceName.replace(/[^a-zA-Z0-9_-]/g, "_"); // sanitize filename

    // Format current date as YYYY-MM-DD_HH-MM
    const now = new Date();
    const dateStr = now.toISOString().slice(0,16).replace("T", "_").replace(/:/g, "-");
    a.download = `${deviceName}_${dateStr}_backup.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);

    URL.revokeObjectURL(url); // clean up
}

function importUserData(event) {
    const file = event.target.files ? event.target.files[0] : null;
    if (!file) {
        return;
    }
    show('import_status')

    if(!ws_connected){
      updateFieldsFromData({"import_status": "❌ Not connected. Connect first"});
      return;
    }

    const reader = new FileReader();
    reader.onload = function(e) {
        try {
            const data = JSON.parse(e.target.result);
            if (typeof data !== "object" || data === null) {
                throw new Error("Invalid JSON structure");
            }
            // Apply values using your existing updater
            updateFieldsFromData(data);
            updateFieldsFromData({"import_status": "✅ Import successful! Click 'Save' to write config"});
            hide('import_config')
            show('btn_save_all')
            

            console.log("User data restored:", data);
        } catch (err) {
            console.error("Error parsing JSON file:", err);
            alert("Invalid JSON file.");

            updateFieldsFromData({"import_status": "❌ Import failed!"});
        }
    };
    reader.readAsText(file);
}

function save_all(){
  saveSettings();
  saveWiFi();
  show('import_config')
  hide('btn_save_all')
  updateFieldsFromData({"import_status": "✅ Config Saved"});
  setTimeout(() => {
       hide('import_status')
    }, 3000);

}

function uploadFirmware() {
    const fileInput = document.getElementById("fwfile");
    const file = fileInput.files[0];
    if (!file) return alert("Select a file");
    const update_url =  "/update";
    const formData = new FormData();
    formData.append("file", file);

    const xhr = new XMLHttpRequest();
    xhr.open("POST", update_url, true);

    xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          let percent = (e.loaded / e.total) * 100;
          console.log(`Upload: ${percent.toFixed(2)}%`);
          // Optionally update a progress bar here
          if(percent <100){
            updateFieldsFromData({"otaStatus": `Upload: ${percent.toFixed(0)}%`});
          }
        }
    };

    xhr.onload = () => {
        console.log(xhr.responseText);
    };
    xhr.send(formData);
}