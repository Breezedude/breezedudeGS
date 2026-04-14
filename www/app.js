// Maps of ID -> row DOM elements and last seen timestamp
const weatherStations = new Map();
const liveTracking = new Map();
const stationHwInfo = new Map(); // stationId -> { raw, merged }
const MAX_AGE_MS = 5 * 60 * 1000; // 5 minutes

const SENSOR_TYPE_NAMES = [
  'invalid',
  'WS80',
  'WS85',
  'WS85_UART',
  'DAVIS6410',
  'WINDNERD'
];

function sensorTypeToName(sensorType) {
  if (Number.isInteger(sensorType) && sensorType >= 0 && sensorType < SENSOR_TYPE_NAMES.length) {
    return SENSOR_TYPE_NAMES[sensorType];
  }
  return `unknown(${sensorType})`;
}

function decodeHwInfoDebug(rawBytes) {
  const result = { decode_type: null };
  if (!Array.isArray(rawBytes) || rawBytes.length === 0) return result;
  const bytes = rawBytes.map((v) => Number(v) & 0xFF);
  result.decode_type = bytes[0];
  // decode_type 0x01: vbatt, batt_perc, pv_state, cfg flags, lbt_counter, rssi_threshold, version_bcd
  if (result.decode_type === 0x01 && bytes.length >= 10) {
    result.vbatt_mv             = bytes[1] | (bytes[2] << 8);
    result.batt_perc            = bytes[3];
    result.pv_state             = bytes[4];
    const cfg                   = bytes[5];
    result.sensor_type          = cfg & 0x1F;
    result.sensor_type_text     = sensorTypeToName(result.sensor_type);
    result.use_baro             = ((cfg >> 5) & 0x01) === 1;
    result.uv_triggered         = ((cfg >> 6) & 0x01) === 1;
    result.lbt                  = ((cfg >> 7) & 0x01) === 1;
    result.lbt_counter          = bytes[6];
    result.lora_rssi_threshold  = -bytes[7];
    const vbcd                  = bytes[8] | (bytes[9] << 8);
    result.version_bcd          = vbcd;
    const vMajor                = (vbcd >> 8) & 0x0F;
    const vMinor                = (vbcd >> 4) & 0x0F;
    const vPatch                = vbcd & 0x0F;
    result.version              = `${vMajor}.${vMinor}.${vPatch}`;
  }
  // decode_type 0x02: gust_age, wind_age, sensor_integ_s, reduce_interval_voltage
  if (result.decode_type === 0x02 && bytes.length >= 7) {
    result.gust_age                 = bytes[1] | (bytes[2] << 8);
    result.wind_age                 = bytes[3] | (bytes[4] << 8);
    result.sensor_integ_s           = bytes[5];
    result.reduce_interval_voltage  = 2 + bytes[6] / 100.0;
  }
  return result;
}

function buildHwInfoHtml(hi, merged) {
  const row = (label, value) =>
    `<div class="hwinfo-row-item"><span class="hwinfo-label">${label}</span><span class="hwinfo-value">${value}</span></div>`;
  const buildDate = (hi.buildYear && hi.buildMonth && hi.buildDay)
    ? `${hi.buildYear}-${String(hi.buildMonth).padStart(2, '0')}-${String(hi.buildDay).padStart(2, '0')}`
    : null;
  let items = '';
  if (buildDate)                          items += row('Build Date', buildDate + (hi.isDevelopmentBuild ? ' <em>(dev)</em>' : ''));
  if (merged.version !== undefined)       items += row('FW Version', merged.version);
  if (hi.hasUptime && hi.uptimeMinutes !== undefined) items += row('Uptime', hi.uptimeMinutes + ' min');
  if (merged.vbatt_mv !== undefined)      items += row('Battery', (merged.vbatt_mv / 1000).toFixed(2) + ' V (' + merged.batt_perc + '%)');
  if (merged.pv_state !== undefined)      items += row('PV State', merged.pv_state);
  if (merged.sensor_type_text !== undefined) items += row('Sensor Type', merged.sensor_type_text);
  else if (merged.sensor_type !== undefined) items += row('Sensor Type', merged.sensor_type);
  if (merged.lora_rssi_threshold !== undefined) items += row('LoRa RSSI Thr.', merged.lora_rssi_threshold + ' dBm');
  if (merged.lbt !== undefined)           items += row('LBT', merged.lbt ? 'on (' + merged.lbt_counter + ')' : 'off');
  if (merged.gust_age !== undefined)      items += row('Gust Age', merged.gust_age + ' s');
  if (merged.wind_age !== undefined)      items += row('Wind Age', merged.wind_age + ' s');
  if (merged.sensor_integ_s !== undefined) items += row('Sensor Integ.', merged.sensor_integ_s + ' s');
  if (merged.reduce_interval_voltage !== undefined) items += row('Reduce V', merged.reduce_interval_voltage.toFixed(2) + ' V');
  return `<div class="hwinfo-grid">${items}</div>`;
}

function toggleHwInfoRow(stationId, dataRow) {
  const next = dataRow.nextElementSibling;
  if (next && next.classList.contains('hwinfo-row')) {
    next.style.display = next.style.display === 'none' ? '' : 'none';
    return;
  }
  const detailRow = document.createElement('tr');
  detailRow.classList.add('hwinfo-row');
  const td = document.createElement('td');
  td.colSpan = 12;
  td.className = 'hwinfo-detail';
  const hwi = stationHwInfo.get(stationId);
  td.innerHTML = hwi ? buildHwInfoHtml(hwi.raw, hwi.merged) : '<em style="color:var(--muted-text)">No HW info received yet</em>';
  detailRow.appendChild(td);
  dataRow.parentNode.insertBefore(detailRow, dataRow.nextSibling);
}

const MOBILE_LABELS = {
  weatherTable: {
    name: "Name",
    dist: "Dist",
    temp: "Temp",
    windDir: "Wind Dir",
    windSpd: "Wind Spd",
    gust: "Gust",
    humidity: "Hum.",
    pressure: "Baro",
    soc: "SoC",
    rssi: "RSSI",
    lastSeen: "Last Seen"
  },
  trackingTable: {
    name: "Name",
    acft: "Type",
    state: "State",
    dist: "Dist",
    alt: "Alt",
    spd: "Speed",
    climb: "Climb",
    heading: "Heading",
    rssi: "RSSI",
    lastSeen: "Last Seen"
  }
};

let ws;
let ws_connected = false;
let reconnectInterval = 2000; // 2 seconds
let wsHeartbeatInterval;
let lastWsMessageTime = Date.now();
const WS_TIMEOUT_MS = 5000; // 5 seconds without message = disconnected
let got_settings = false;

const REQUIRED_DEFAULTS = {
  deviceName: "MyGS",
  lat: 47.0,
  lon: 12.0
};

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

function scanWiFi() {
  const select = document.getElementById("wifiScanResults");
  const scanBtn = document.getElementById("wifiScanBtn");
  if (select) {
    select.style.display = 'none';
    select.innerHTML = '';
  }
  if (scanBtn) {
    scanBtn.disabled = true;
    scanBtn.textContent = "Scanning...";
  }
  wsSend({ cmd: "wifi_scan" });
}

function applySelectedWiFi() {
  const select = document.getElementById("wifiScanResults");
  const ssidInput = document.getElementById("sta_ssid");
  if (!select || !ssidInput) return;
  if (select.value) {
    ssidInput.value = select.value;
  }
}

function togglePasswordVisibility(inputId, buttonEl) {
  const input = document.getElementById(inputId);
  if (!input) return;
  const isPassword = input.type === "password";
  input.type = isPassword ? "text" : "password";
  if (buttonEl) {
    buttonEl.classList.toggle("is-visible", isPassword);
    buttonEl.setAttribute("aria-label", isPassword ? "Passwort verbergen" : "Passwort anzeigen");
    buttonEl.setAttribute("title", isPassword ? "Passwort verbergen" : "Passwort anzeigen");
  }
}

function parseNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function isNameConfigured(name) {
  const value = (name || "").trim();
  const isValidLength = value.length >= 3 && value.length <= 9;
  const isValidChars = /^[A-Za-z0-9]+$/.test(value);
  return isValidLength && isValidChars && value !== REQUIRED_DEFAULTS.deviceName;
}

function isPositionConfigured(latValue, lonValue) {
  const lat = parseNumber(latValue);
  const lon = parseNumber(lonValue);
  if (lat === null || lon === null) return false;
  return Math.abs(lat - REQUIRED_DEFAULTS.lat) > 0.00001 && Math.abs(lon - REQUIRED_DEFAULTS.lon) > 0.00001;
}

function isRequiredSetupCompleteFromInputs() {
  const nameInput = document.getElementById("deviceName");
  const latInput = document.getElementById("lat");
  const lonInput = document.getElementById("lon");
  if (!nameInput || !latInput || !lonInput) return false;
  return isNameConfigured(nameInput.value) && isPositionConfigured(latInput.value, lonInput.value);
}

function updateAprsGuardUi(showMessage = false) {
  const sendAprs = document.getElementById("sendAPRS");
  const sendBreezedude = document.getElementById("sendBreezedude");
  const hint = document.getElementById("aprsSetupHint");
  if (!sendAprs || !sendBreezedude) return;

  const setupComplete = isRequiredSetupCompleteFromInputs();
  sendAprs.disabled = !setupComplete;
  sendBreezedude.disabled = !setupComplete;
  if (!setupComplete) {
    sendAprs.checked = false;
    sendBreezedude.checked = false;
  }
  if (hint) {
    hint.style.display = setupComplete ? "none" : "block";
  }
  if (!setupComplete && showMessage) {
    updateFieldsFromData({ msg: "APRS und Send-to-Breezedude bleiben deaktiviert: Bitte Name sowie Latitude/Longitude sorgsam setzen." });
  }
}


function get_inputs_from_element(id){
  const inputs =  Array.from(document.getElementById(id).querySelectorAll("input, select"));
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
  if (!isRequiredSetupCompleteFromInputs()) {
    data.sendAPRS = false;
    data.sendBreezedude = false;
    const sendAprs = document.getElementById("sendAPRS");
    const sendBreezedude = document.getElementById("sendBreezedude");
    if (sendAprs) sendAprs.checked = false;
    if (sendBreezedude) sendBreezedude.checked = false;
    updateFieldsFromData({ msg: "APRS und Send-to-Breezedude bleiben deaktiviert bis Name und Position sorgsam gesetzt sind." });
  }
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
        //console.warn("WebSocket is not open. ReadyState:", ws.readyState);
    }
}

// Function to create or update a table row
function upsertRow(tableId, data, rowMap, columns) {
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
    
    // Append to table
    table.appendChild(row);
    rowMap.set(data.id, row);
  }

  // Update content
  columns.forEach(key => {
    const cell = row.querySelector(`[data-key="${key}"]`);
    if (cell) {
      if (key === "lastSeen") {
        row.dataset.lastSeen = parseInt(data.lastSeen || "0", 10);
      } else if(key === "dist"){
        cell.innerHTML = "<a href='https://www.google.com/maps/search/?api=1&query=" + data.lat + "," + data.lon + "' target='_blank' style='color: inherit; text-decoration: none;'>" + (data[key] !== undefined ? data[key] : "") + "</a>";
        } else{
        cell.innerText = data[key] !== undefined ? data[key] : "";
      }
    }
  });

  // Build compact mobile content inside 2nd column so small screens show: ID | Data
  if (columns.length > 1) {
    const secondKey = columns[1];
    const secondCell = row.querySelector(`[data-key="${secondKey}"]`);
    if (secondCell) {
      const labels = MOBILE_LABELS[tableId] || {};
      const details = columns
        .slice(1)
        .map((key) => {
          const label = labels[key] || key;
          if (key === "lastSeen") {
            const ts = Number(row.dataset.lastSeen || Date.now());
            const delta = Math.max(0, Math.floor((Date.now() - ts) / 1000));
            return `<div><strong>${label}:</strong> <span class="mobile-last-seen">${delta}s</span></div>`;
          }
          const value = data[key] !== undefined ? data[key] : "";
          return `<div><strong>${label}:</strong> ${value}</div>`;
        })
        .join("");

      secondCell.classList.add("summary-cell");
      secondCell.innerHTML = `
        <span class="desktop-only-value">${data[secondKey] !== undefined ? data[secondKey] : ""}</span>
        <div class="mobile-stack">${details}</div>
      `;
    }
  }
}

// Clean up old entries
function removeOldRows(rowMap, maxAgeMs) {
  const now = Date.now();
  for (const [id, row] of rowMap.entries()) {
    if (now - row.dataset.lastSeen > maxAgeMs) {
      const next = row.nextElementSibling;
      if (next && next.classList.contains('hwinfo-row')) next.remove();
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
      const delta = Math.max(0, Math.floor((now - Number(row.dataset.lastSeen || now)) / 1000));

      const cell = row.querySelector('[data-key="lastSeen"]');
      if (cell) {
        cell.innerText = `${delta}s`;
      }

      const mobileLastSeen = row.querySelector('.mobile-last-seen');
      if (mobileLastSeen) {
        mobileLastSeen.innerText = `${delta}s`;
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

    const updateLink = document.getElementById("update_link");
    const updateNotice = document.getElementById("update_notice");


    if (updateLink && updateNotice) {
      const hasUpdate = !!data.update_available;
      const updateVersion = data.update_version || "";
      if (hasUpdate && updateVersion) {
        updateNotice.innerText = `-> ${updateVersion} Update verfügbar`;
        updateLink.style.display = "inline";
      } else if (data.update_available === false || data.update_version !== undefined) {
        updateNotice.innerText = "";
        updateLink.style.display = "none";
      }
    }

    Object.keys(data).forEach(key => {
        const el = document.getElementById(key);
        if (el) {
            if (el.tagName === 'INPUT') {
                if (el.type === 'checkbox') {
                    el.checked = !!data[key];
                } else {
                    el.value = data[key];
                }
          } else if (el.tagName === 'SELECT') {
            el.value = data[key];
            } else {
                el.innerText = data[key];
            }
        } else {
            //console.warn(`Element with id '${key}' not found.`);
        }

        if(key === "msg"){
          setInterval(() => {el.innerText = "";}, 3000);
        }
        if(key === "webconsole"){
          if(data[key]) {
            const now = new Date();
            const time = now.toLocaleTimeString();
            if(el.value && !el.value.endsWith('\n')) el.value += '\r\n';
            el.value += `[${time}] ${data[key]}\r\n`;
          }
        }

        if(key === "disconnect"){
          updateWsStatus(false);
          got_settings = false;
        }
        if(key === "sendAPRS" && data[key] == false){
          toggleAPRSInputs();
        }
        if(key === "updateBranch") {
          const warning = document.getElementById('betaWarning');
          if (warning) warning.style.display = data[key] === 'beta' ? '' : 'none';
        }
        if(key === "deviceName"){
          updateFieldsFromData({'deviceNamedisplay': data[key]});
          got_settings =true;
        }
    });

    updateAprsGuardUi(false);
}

window.addEventListener('load', () => {
  //document.getElementById("getLocationBtn").addEventListener("click", getUserLocation);
    onWsReady(() => {
       // console.log("WebSocket connected, initializing requests...");
        loadFields();
        setTimeout(() => {wsSend({ cmd: "get_fanet", "min": 5});}, 700); // get not older than 5min, after station gps position is reveived

        setInterval(() => {
          wsSend({ cmd: "get_info_update"});
          if(!got_settings){
            loadFields();
          }
        }, 1000);
    });

    document.getElementById('deviceName').addEventListener('input', function() {
      validateNameField('deviceName', 'saveSettingsBtn');
      updateAprsGuardUi(false);
    });
    document.getElementById('lat').addEventListener('input', () => updateAprsGuardUi(false));
    document.getElementById('lon').addEventListener('input', () => updateAprsGuardUi(false));
    const clearButton  = document.getElementById('clearButton');
    clearButton.addEventListener('click', () => {
      let con = document.getElementById('webconsole');
      con.value = "";
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

    //ws = new WebSocket('ws://192.168.178.57/ws');
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
    console.log(data)

    if(data.error){
      const scanBtn = document.getElementById("wifiScanBtn");
      if (scanBtn) {
        scanBtn.disabled = false;
        scanBtn.textContent = "Scan WiFi";
      }
      showError(data.error);
    }
    else if (data.wifi_scan && Array.isArray(data.wifi_scan)) {
      const scanBtn = document.getElementById("wifiScanBtn");
      const select = document.getElementById("wifiScanResults");
      if (scanBtn) {
        scanBtn.disabled = false;
        scanBtn.textContent = "Scan WiFi";
      }
      if (select) {
        const previous = select.value;
        select.innerHTML = '<option value="">-- Select network --</option>';
        select.style.display = '';
        data.wifi_scan.forEach(net => {
          const opt = document.createElement("option");
          opt.value = net.ssid || "";
          const lock = net.open ? "" : " 🔒";
          const rssi = (net.rssi !== undefined && net.rssi !== null) ? ` (${net.rssi} dBm)` : "";
          opt.textContent = `${net.ssid || "<hidden>"}${lock}${rssi}`;
          select.appendChild(opt);
        });
        if (previous) {
          select.value = previous;
        }
      }
    }
    else if (data.weather && Array.isArray(data.weather)) {
      data.weather.forEach(station => {
          const rowData = {
              id: (station.vid.toString(16).toUpperCase().padStart(2, "0") + station.fanet_id.toString(16).toUpperCase().padStart(4, "0")),
              name: station.name,
              lat: station.lat !== undefined ? station.lat.toFixed(5) : "-",
              lon: station.lon !== undefined ? station.lon.toFixed(5) : "-",
              dist: station.lat !== 0 ? calculateDistanceKm(station.lat, station.lon, getVal("lat"), getVal("lon")).toFixed(1) +" km" : "-",
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

          if(rowData.lastSeen < 100000000){
            rowData.lastSeen = Date.now();
          }

          upsertRow("weatherTable", rowData, weatherStations, [
              "id", "name","dist", "temp", "windDir", "windSpd", "gust",
              "humidity", "pressure", "soc", "rssi", "lastSeen" //  "lat", "lon", 
          ]);

          // Set up click handler on ID cell for hw_status expand/collapse
          const row = weatherStations.get(rowData.id);
          if (row && !row.dataset.hwExpandSetup) {
            row.dataset.hwExpandSetup = '1';
            const idCell = row.querySelector('[data-key="id"]');
            if (idCell) {
              idCell.classList.add('hw-expandable');
              idCell.title = 'Click to show/hide HW info';
              idCell.addEventListener('click', () => toggleHwInfoRow(rowData.id, row));
            }
          }
      }); // {"weather":[{"vid":189,"fanet_id":50097,"name":"","rssi":-103,"snr":-9,"lat":47.74839,"lon":12.25035,"tLastMsg":1760421160,"temp":2,"wHeading":237.6563,"wSpeed":4.4,"wGust":8.6,"humidity":99.2,"baro":1028,"charge":93.33334}]}

    } else if (data.tracking && Array.isArray(data.tracking)) {
      data.tracking.forEach(trk => {
          const rowData = {
              id: (trk.vid.toString(16).toUpperCase().padStart(2, "0") + trk.fanet_id.toString(16).toUpperCase().padStart(4, "0")),
              name: trk.name,
              lat: trk.lat !== undefined ? trk.lat.toFixed(5) : "-",
              lon: trk.lon !== undefined ? trk.lon.toFixed(5) : "-",
              dist: (trk.lat !== undefined && trk.lon !== undefined) ?
                  (calculateDistanceKm(trk.lat, trk.lon, getVal("lat"), getVal("lon")).toFixed(1) + " km") : "-",
              alt: (trk.alt !== undefined) && (trk.alt !== "-") ? trk.alt.toFixed(0) + " m" : "-",
              hdop: trk.hdop !== undefined ? (trk.hdop / 100).toFixed(2) : "-", // assuming hdop is in 1/100 m
              acft: trk.acft !== undefined ? trk.acft : "-", // aircraft type as raw value
              spd: (trk.spd !== undefined) && (trk.spd !== "-") ? trk.spd.toFixed(1) + " km/h" : "-",
              climb: (trk.climb !== undefined) && (trk.climb !== "-") ? trk.climb.toFixed(1) + " m/s" : "-",
              heading: (trk.heading !== undefined) && (trk.heading !== "-") ? trk.heading.toFixed(0) + "°" : "-",
              track: trk.track ? "Yes" : "No",
              rssi: trk.rssi !== undefined ? trk.rssi + " dBm" : "-",
              snr: trk.snr !== undefined ? trk.snr + " dB" : "-",
              lastSeen: trk.tLastMsg !== undefined ? trk.tLastMsg*1000 : "-",
              state: trk.state !== undefined ? trk.state: "-",
          };

          upsertRow("trackingTable", rowData, liveTracking, [
              "id", "name", "acft", "state", "dist", "alt", "spd", "climb", "heading", "rssi", "lastSeen" // "lat", "lon",
          ]);
      });
    }
    else if (data.hwinfo && Array.isArray(data.hwinfo)) {
      data.hwinfo.forEach(hi => {
        const stationId = hi.vid.toString(16).toUpperCase().padStart(2, "0") +
                          hi.fanet_id.toString(16).toUpperCase().padStart(4, "0");
        const decoded = decodeHwInfoDebug(hi.rawBytes || []);
        // Merge: keep latest value of each field across decode types
        const prev = stationHwInfo.get(stationId);
        const merged = Object.assign({}, prev ? prev.merged : {}, decoded);
        stationHwInfo.set(stationId, { raw: hi, merged });

        // Update open detail row if visible
        const dataRow = weatherStations.get(stationId);
        if (dataRow) {
          const detailRow = dataRow.nextElementSibling;
          if (detailRow && detailRow.classList.contains('hwinfo-row') && detailRow.style.display !== 'none') {
            const td = detailRow.querySelector('td');
            if (td) td.innerHTML = buildHwInfoHtml(hi, merged);
          }
          // Mark ID cell with indicator that hw info is available
          const idCell = dataRow.querySelector('[data-key="id"]');
          if (idCell) idCell.classList.add('hw-has-data');
        }
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
  updateAprsGuardUi(true);
  const sendAprs = document.getElementById("sendAPRS");
  const enabled = sendAprs && sendAprs.checked && !sendAprs.disabled;
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