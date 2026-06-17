// Maps of ID -> row DOM elements and last seen timestamp
const weatherStations = new Map();
const liveTracking = new Map();
const stationHwInfo = new Map(); // stationId -> { raw, merged }
const MAX_AGE_MS = 5 * 60 * 1000; // 5 minutes
const HIDDEN_STATION_VIDS = new Set([0x66, 0x11]);

function isHiddenStationVid(vid) {
  return HIDDEN_STATION_VIDS.has(Number(vid));
}

function removeWeatherStationById(stationId) {
  const row = weatherStations.get(stationId);
  if (row) {
    const next = row.nextElementSibling;
    if (next && next.classList.contains('hwinfo-row')) next.remove();
    row.remove();
    weatherStations.delete(stationId);
  }
  stationHwInfo.delete(stationId);
}

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

  if (result.decode_type === 0x01 && bytes.length >= 11) {
    result.vbatt_mv = bytes[1] | (bytes[2] << 8);
    result.batt_perc = bytes[3];
    result.pv_state = bytes[4];
    result.rx_count = bytes[5] | (bytes[6] << 8);
    result.forward_count = bytes[7] | (bytes[8] << 8);
    result.lbt_counter = bytes[9];
  }

  if (result.decode_type === 0x02 && bytes.length >= 11) {
    const vbcd = bytes[1] | (bytes[2] << 8);
    result.version_bcd = vbcd;
    result.version = `${(vbcd >> 8) & 0x0F}.${(vbcd >> 4) & 0x0F}.${vbcd & 0x0F}`;
    result.nonce = (bytes[3]) | (bytes[4] << 8) | (bytes[5] << 16) | (bytes[6] << 24);
    result.slot_capacity_kb = bytes[7] | (bytes[8] << 8);
    result.ota_proto = bytes[9];
    result.ota_state = bytes[10] & 0x01;
    result.ota_state_text = result.ota_state ? 'B' : 'A';
  }

  if (result.decode_type === 0x03 && bytes.length >= 5) {
    const cfg = bytes[1];
    result.sensor_type = cfg & 0x1F;
    result.sensor_type_text = sensorTypeToName(result.sensor_type);
    result.use_baro = ((cfg >> 5) & 0x01) === 1;
    result.uv_triggered = ((cfg >> 6) & 0x01) === 1;
    result.lbt = ((cfg >> 7) & 0x01) === 1;
    result.lbt_rssi_threshold = -bytes[2];
    result.sensor_integ_s = bytes[3];
    result.reduce_interval_voltage = 2 + bytes[4] / 100.0;
  }

  return result;
}

function buildHwInfoHtml(hi, merged) {
  const row = (label, value) =>
    `<div class="hwinfo-row-item"><span class="hwinfo-label">${label}</span><span class="hwinfo-value">${value}</span></div>`;
  const formatDateTime = (unixSec) => {
    if (!Number.isFinite(unixSec) || unixSec <= 0) return null;
    const d = new Date(unixSec * 1000);
    if (Number.isNaN(d.getTime())) return null;
    return d.toLocaleString();
  };

  const buildDate = (hi.buildYear && hi.buildMonth && hi.buildDay)
    ? `${hi.buildYear}-${String(hi.buildMonth).padStart(2, '0')}-${String(hi.buildDay).padStart(2, '0')}`
    : null;
  const latestRxText = formatDateTime(merged.newestHwTs);
  let items = '';
  if (latestRxText)                      items += row('Last Info', latestRxText);
  if (buildDate)                          items += row('FW Build Date', buildDate + (hi.isDevelopmentBuild ? ' <em>(dev)</em>' : ''));
  if (merged.version !== undefined)       items += row('FW Version', merged.version);
  if (hi.hasUptime && hi.uptimeMinutes !== undefined) {
    const uDays = Math.floor(hi.uptimeMinutes / 1440);
    const uHours = Math.floor((hi.uptimeMinutes % 1440) / 60);
    const uMins = hi.uptimeMinutes % 60;
    const uptimeStr = uDays > 0 ? `${uDays}d ${uHours}h ${uMins}m` : uHours > 0 ? `${uHours}h ${uMins}m` : `${uMins}m`;
    items += row('Uptime', uptimeStr);
  }
  if (merged.vbatt_mv !== undefined)      items += row('Battery', (merged.vbatt_mv / 1000).toFixed(2) + ' V (' + merged.batt_perc + '%)');
  if (merged.pv_state !== undefined)      items += row('PV State', merged.pv_state);
  if (merged.rx_count !== undefined)      items += row('FANET RX Count', merged.rx_count);
  if (merged.forward_count !== undefined) items += row('FANET FWD Count', merged.forward_count);
  if (merged.sensor_type_text !== undefined) items += row('Sensor Type', merged.sensor_type_text);
  else if (merged.sensor_type !== undefined) items += row('Sensor Type', merged.sensor_type);
  if (merged.lbt_rssi_threshold !== undefined) items += row('LBT RSSI Thr.', merged.lbt_rssi_threshold + ' dBm');
  if (merged.lbt !== undefined)           items += row('LBT', merged.lbt ? 'on (' + (merged.lbt_counter ?? 0) + ')' : 'off');
  if (merged.ota_state_text !== undefined) items += row('Active OTA Slot', merged.ota_state_text);
  if( merged.sensor_type !== undefined && ( merged.sensor_type === 4)) { // davis sensor
    if (merged.sensor_integ_s !== undefined) items += row('Sensor Integ.', merged.sensor_integ_s + ' s');
  }
  if (merged.reduce_interval_voltage !== undefined) items += row('Low Power Mode', merged.reduce_interval_voltage.toFixed(2) + ' V');
  // if (merged.slot_capacity_kb !== undefined) items += row('OTA Slot', merged.slot_capacity_kb + ' KiB');
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
  td.innerHTML = hwi
    ? buildHwInfoHtml(hwi.raw, hwi.merged)
    : '<div class="hwinfo-grid"><div class="hwinfo-row-item"><span class="hwinfo-label">No HW info received yet</span></div></div>';
  detailRow.appendChild(td);
  dataRow.parentNode.insertBefore(detailRow, dataRow.nextSibling);
}

const WEATHER_TABLE_COLUMNS = [
  'id', 'name', 'dist', 'temp', 'windDir', 'windSpd', 'gust',
  'humidity', 'pressure', 'soc', 'rssi', 'lastSeen'
];

function setupWeatherRow(rowData) {
  const row = weatherStations.get(rowData.id);
  if (!row) return;

  if (!row.dataset.hwExpandSetup) {
    row.dataset.hwExpandSetup = '1';
    const idCell = row.querySelector('[data-key="id"]');
    if (idCell) {
      idCell.classList.add('hw-expandable');
      idCell.title = 'Click to show/hide HW info';
      idCell.addEventListener('click', () => toggleHwInfoRow(rowData.id, row));
    }
  }

  if (stationHwInfo.has(rowData.id)) {
    const idCell = row.querySelector('[data-key="id"]');
    if (idCell) idCell.classList.add('hw-has-data');
  }
}

function upsertWeatherRow(rowData) {
  upsertRow('weatherTable', rowData, weatherStations, WEATHER_TABLE_COLUMNS);
  setupWeatherRow(rowData);
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
const MAX_CONSOLE_LINES = 1000;
let got_settings = false;
const domCache = new Map();

const REQUIRED_DEFAULTS = {
  deviceName: "MyGS",
  lat: 47.0,
  lon: 12.0
};

function normalizeConsoleText(text) {
  return String(text || '').replace(/\r\n/g, '\n').replace(/\r/g, '\n');
}

function getEl(id) {
  if (!id) return null;
  if (domCache.has(id)) return domCache.get(id);
  const el = document.getElementById(id);
  domCache.set(id, el || null);
  return el;
}

function classifyToastMessage(text) {
  const msg = String(text || '').toLowerCase();
  if (msg.includes('not connected') || msg.includes('nicht verbunden')) return 'info';
  if (msg.includes('error') || msg.includes('fehler') || msg.includes('failed') || msg.includes('deaktiviert')) return 'error';
  if (msg.includes('saved') || msg.includes('gespeichert') || msg.includes('connected') || msg.includes('verbunden') || msg.includes('success') || msg.includes('erfolgreich')) return 'success';
  return 'info';
}

function showToast(text, type) {
  const container = getEl('toastContainer');
  if (!container || !text) return;

  const toastType = type || classifyToastMessage(text);
  const toast = document.createElement('div');
  toast.className = `toast toast--${toastType}`;
  toast.innerText = text;
  container.appendChild(toast);

  requestAnimationFrame(() => toast.classList.add('toast--visible'));

  setTimeout(() => {
    toast.classList.remove('toast--visible');
    setTimeout(() => toast.remove(), 300);
  }, 4000);
}

function setConsoleLines(textarea, lines) {
  if (!textarea) return;
  const limitedLines = lines.slice(-MAX_CONSOLE_LINES);
  textarea.value = limitedLines.length > 0 ? `${limitedLines.join('\n')}\n` : '';
  textarea.scrollTop = textarea.scrollHeight;
}

function appendConsoleText(textarea, text) {
  if (!textarea) return;

  const incomingLines = normalizeConsoleText(text)
    .split('\n')
    .map((line) => line.trimEnd())
    .filter((line) => line.length > 0);

  if (incomingLines.length === 0) return;

  const existingLines = normalizeConsoleText(textarea.value)
    .split('\n')
    .filter((line) => line.length > 0);

  setConsoleLines(textarea, existingLines.concat(incomingLines));
}

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

// --- OTA live monitor (Tools -> Console -> OTA) -------------------------
// Visualizes streamFirmware()'s per-chunk transfer stats live, as they arrive
// over the websocket ({"ota_progress": {...}}, see main.cpp / ota_gs.cpp).
// Main chart: each chunk draws a bar above the zero-line for the SNR the GS
// measured on the device's ack, and a mirrored bar below the zero-line for
// the SNR the device measured on the chunk packet. The RSSI the device
// measured for the chunk packet is overlaid as a single line spanning the
// full chart height. A second, shorter chart below shows retries (with a
// full-height red bar marking the chunk that aborted the transfer, if any)
// and ack-resends (with a lighter stripe marking the proactive subset).
const OTA_LIVE_SNR_MIN = -20;     // fallback range until real values arrive
const OTA_LIVE_SNR_MAX = 11;
const OTA_LIVE_RETRY_ABORT = 15;  // sentinel: this chunk aborted the transfer
const OTA_LIVE_HIDE_DELAY_MS = 12000;

const otaLive = {
  total: 0,
  chunks: [],
  hideTimer: null,
  // Dynamic axis ranges, derived from the values seen so far this transfer
  snrMin: null,
  snrMax: null,
  deviceSnrMin: null,
  deviceSnrMax: null,
  rssiMin: null,
  rssiMax: null,
  retryMax: 1,
  ackResendMax: 1,
};

function otaLiveClamp01(v) {
  return Math.max(0, Math.min(1, v));
}

function otaLiveHexToRgba(color, alpha) {
  const hex = color.trim().replace('#', '');
  if (hex.length === 3 || hex.length === 6) {
    const full = hex.length === 3 ? hex.split('').map(c => c + c).join('') : hex;
    const r = parseInt(full.substring(0, 2), 16);
    const g = parseInt(full.substring(2, 4), 16);
    const b = parseInt(full.substring(4, 6), 16);
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }
  return color;
}

function otaLiveResizeCanvas(id) {
  const canvas = getEl(id);
  if (!canvas) return null;
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(1, Math.round(rect.width * dpr));
  const h = Math.max(1, Math.round(rect.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  return canvas;
}

function otaLiveDrawMain() {
  const canvas = otaLiveResizeCanvas('otaLiveCanvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  const mid = h / 2;

  ctx.clearRect(0, 0, w, h);

  const total = Math.max(otaLive.total, otaLive.chunks.length, 1);
  const barW = Math.max(w / total, 1);

  // Resolve theme-aware colors (data-theme lives on <body>)
  const themeStyle = getComputedStyle(document.body);
  const snrColor = themeStyle.getPropertyValue('--ota-snr-color').trim() || '#6f98d6';
  const deviceSnrColor = themeStyle.getPropertyValue('--ota-device-snr-color').trim() || '#2faa75';
  const deviceRssiColor = themeStyle.getPropertyValue('--ota-device-rssi-color').trim() || '#d6a900';
  const zeroColor = themeStyle.getPropertyValue('--ota-zero-line').trim() || 'rgba(0,0,0,0.3)';

  const snrGrad = ctx.createLinearGradient(0, 0, 0, mid);
  snrGrad.addColorStop(0, snrColor);
  snrGrad.addColorStop(1, otaLiveHexToRgba(snrColor, 0.25));

  const deviceSnrGrad = ctx.createLinearGradient(0, mid, 0, h);
  deviceSnrGrad.addColorStop(0, otaLiveHexToRgba(deviceSnrColor, 0.25));
  deviceSnrGrad.addColorStop(1, deviceSnrColor);

  // Dynamic axis ranges: scale to the values actually seen so far, so the
  // chart uses the full height instead of mostly-empty/mostly-full bars.
  const snrMin = otaLive.snrMin !== null ? otaLive.snrMin : OTA_LIVE_SNR_MIN;
  const snrMax = otaLive.snrMax !== null ? otaLive.snrMax : OTA_LIVE_SNR_MAX;
  const snrSpan = snrMax - snrMin;
  const deviceSnrMin = otaLive.deviceSnrMin !== null ? otaLive.deviceSnrMin : OTA_LIVE_SNR_MIN;
  const deviceSnrMax = otaLive.deviceSnrMax !== null ? otaLive.deviceSnrMax : OTA_LIVE_SNR_MAX;
  const deviceSnrSpan = deviceSnrMax - deviceSnrMin;
  const rssiMin = otaLive.rssiMin;
  const rssiMax = otaLive.rssiMax;
  const rssiSpan = (rssiMin !== null && rssiMax !== null) ? rssiMax - rssiMin : 0;

  for (let i = 0; i < otaLive.chunks.length; i++) {
    const c = otaLive.chunks[i];
    if (!c) continue;
    const x = i * (w / total);

    if (Number.isFinite(c.snr)) {
      const frac = snrSpan > 0 ? otaLiveClamp01((c.snr - snrMin) / snrSpan) : 1;
      const barH = frac * mid;
      ctx.fillStyle = snrGrad;
      ctx.fillRect(x, mid - barH, barW, barH);
    }

    if (Number.isFinite(c.deviceSnr)) {
      const frac = deviceSnrSpan > 0 ? otaLiveClamp01((c.deviceSnr - deviceSnrMin) / deviceSnrSpan) : 1;
      const barH = frac * (h - mid);
      ctx.fillStyle = deviceSnrGrad;
      ctx.fillRect(x, mid, barW, barH);
    }
  }

  // RSSI overlay: a single line spanning the full chart height, mapped from
  // [rssiMin..rssiMax] onto [h..0].
  if (rssiSpan > 0) {
    ctx.strokeStyle = deviceRssiColor;
    ctx.lineWidth = Math.max(1.5 * (window.devicePixelRatio || 1), 1.5);
    ctx.beginPath();
    let started = false;
    for (let i = 0; i < otaLive.chunks.length; i++) {
      const c = otaLive.chunks[i];
      if (!c || !Number.isFinite(c.deviceRssi)) continue;
      const x = (i + 0.5) * (w / total);
      const frac = otaLiveClamp01((c.deviceRssi - rssiMin) / rssiSpan);
      const y = h - frac * h;
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else {
        ctx.lineTo(x, y);
      }
    }
    if (started) ctx.stroke();
  }

  ctx.strokeStyle = zeroColor;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, mid + 0.5);
  ctx.lineTo(w, mid + 0.5);
  ctx.stroke();
}

function otaLiveDrawRetry() {
  const canvas = otaLiveResizeCanvas('otaLiveRetryCanvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  const total = Math.max(otaLive.total, otaLive.chunks.length, 1);
  const barW = Math.max(w / total, 1);

  const themeStyle = getComputedStyle(document.body);
  const retryColor = themeStyle.getPropertyValue('--ota-retry-color').trim() || '#e8743b';
  const ackResendColor = themeStyle.getPropertyValue('--ota-ackresend-color').trim() || '#9b59b6';
  const abortColor = themeStyle.getPropertyValue('--ota-abort-color').trim() || '#dc2626';

  const retryMax = Math.max(otaLive.retryMax, 1);
  const ackResendMax = Math.max(otaLive.ackResendMax, 1);

  for (let i = 0; i < otaLive.chunks.length; i++) {
    const c = otaLive.chunks[i];
    if (!c) continue;
    const x = i * (w / total);

    if (c.retries >= OTA_LIVE_RETRY_ABORT) {
      ctx.fillStyle = abortColor;
      ctx.fillRect(x, 0, barW, h);
    } else if (c.retries > 0) {
      const frac = otaLiveClamp01(c.retries / retryMax);
      const barH = frac * h;
      ctx.fillStyle = retryColor;
      ctx.fillRect(x, h - barH, barW, barH);
    }

    if (c.ackResends > 0) {
      const frac = otaLiveClamp01(c.ackResends / ackResendMax);
      const barH = frac * h;
      const stripeW = Math.max(barW * 0.4, 1);
      ctx.fillStyle = ackResendColor;
      ctx.fillRect(x + barW - stripeW, h - barH, stripeW, barH);

      if (c.proactiveAckResends > 0) {
        const pFrac = otaLiveClamp01(c.proactiveAckResends / ackResendMax);
        const pBarH = pFrac * h;
        ctx.fillStyle = otaLiveHexToRgba(ackResendColor, 0.45);
        ctx.fillRect(x + barW - stripeW, h - pBarH, stripeW, pBarH);
      }
    }
  }
}

function otaLiveDraw() {
  otaLiveDrawMain();
  otaLiveDrawRetry();
}

function handleOtaProgress(p) {
  const section = getEl('otaLiveSection');
  if (!section || !p || !p.phase) return;

  if (p.phase === 'start') {
    if (otaLive.hideTimer) {
      clearTimeout(otaLive.hideTimer);
      otaLive.hideTimer = null;
    }
    otaLive.total = Number(p.total) || 0;
    otaLive.chunks = [];
    otaLive.snrMin = null;
    otaLive.snrMax = null;
    otaLive.deviceSnrMin = null;
    otaLive.deviceSnrMax = null;
    otaLive.rssiMin = null;
    otaLive.rssiMax = null;
    otaLive.retryMax = 1;
    otaLive.ackResendMax = 1;
    section.classList.remove('ota-live-done', 'ota-live-failed');

    const status = getEl('otaLiveStatus');
    if (status) status.textContent = 'OTA in progress …';
    const prog = getEl('otaLiveProgress');
    if (prog) prog.textContent = `0 / ${otaLive.total}`;
    const snrChip = getEl('otaLiveSnr');
    if (snrChip) snrChip.textContent = 'SNR GS: – dB';
    const deviceSnrChip = getEl('otaLiveDeviceSnr');
    if (deviceSnrChip) deviceSnrChip.textContent = 'SNR Device: – dB';
    const deviceRssiChip = getEl('otaLiveDeviceRssi');
    if (deviceRssiChip) deviceRssiChip.textContent = 'RSSI Device: – dBm';
    const retryChip = getEl('otaLiveRetries');
    if (retryChip) retryChip.textContent = 'Retries: –';
    const ackResendChip = getEl('otaLiveAckResends');
    if (ackResendChip) ackResendChip.textContent = 'ACK-Resends: –';

    show('otaLiveSection');
    requestAnimationFrame(otaLiveDraw);
  } else if (p.phase === 'chunk') {
    if (section.style.display === 'none') {
      show('otaLiveSection');
    }
    const seq = Number(p.seq) || 0;
    const retries = Number(p.retries) || 0;
    const snr = Number(p.snr);
    const ackResends = Number(p.ackResends) || 0;
    const deviceSnr = Number(p.deviceSnr);
    const deviceRssi = Number(p.deviceRssi);
    const proactiveAckResends = Number(p.proactiveAckResends) || 0;
    otaLive.chunks[seq] = { retries, snr, ackResends, deviceSnr, deviceRssi, proactiveAckResends };

    // Track dynamic axis ranges from the values seen so far
    if (Number.isFinite(snr)) {
      otaLive.snrMin = otaLive.snrMin === null ? snr : Math.min(otaLive.snrMin, snr);
      otaLive.snrMax = otaLive.snrMax === null ? snr : Math.max(otaLive.snrMax, snr);
    }
    if (Number.isFinite(deviceSnr)) {
      otaLive.deviceSnrMin = otaLive.deviceSnrMin === null ? deviceSnr : Math.min(otaLive.deviceSnrMin, deviceSnr);
      otaLive.deviceSnrMax = otaLive.deviceSnrMax === null ? deviceSnr : Math.max(otaLive.deviceSnrMax, deviceSnr);
    }
    if (Number.isFinite(deviceRssi)) {
      otaLive.rssiMin = otaLive.rssiMin === null ? deviceRssi : Math.min(otaLive.rssiMin, deviceRssi);
      otaLive.rssiMax = otaLive.rssiMax === null ? deviceRssi : Math.max(otaLive.rssiMax, deviceRssi);
    }
    if (retries < OTA_LIVE_RETRY_ABORT) {
      otaLive.retryMax = Math.max(otaLive.retryMax, retries);
    }
    otaLive.ackResendMax = Math.max(otaLive.ackResendMax, ackResends);

    const prog = getEl('otaLiveProgress');
    if (prog) prog.textContent = `${seq + 1} / ${otaLive.total || (seq + 1)}`;
    const snrChip = getEl('otaLiveSnr');
    if (snrChip && Number.isFinite(snr)) snrChip.textContent = `SNR GS: ${snr.toFixed(1)} dB`;
    const deviceSnrChip = getEl('otaLiveDeviceSnr');
    if (deviceSnrChip && Number.isFinite(deviceSnr)) deviceSnrChip.textContent = `SNR Device: ${deviceSnr.toFixed(1)} dB`;
    const deviceRssiChip = getEl('otaLiveDeviceRssi');
    if (deviceRssiChip && Number.isFinite(deviceRssi)) deviceRssiChip.textContent = `RSSI Device: ${deviceRssi.toFixed(0)} dBm`;
    const retryChip = getEl('otaLiveRetries');
    if (retryChip) retryChip.textContent = retries >= OTA_LIVE_RETRY_ABORT ? 'Retries: Abort' : `Retries: ${retries}`;
    const ackResendChip = getEl('otaLiveAckResends');
    if (ackResendChip) ackResendChip.textContent = proactiveAckResends > 0 ? `ACK-Resends: ${ackResends} (proaktiv: ${proactiveAckResends})` : `ACK-Resends: ${ackResends}`;

    const otaVerboseToggle = getEl('otaVerboseToggle');
    const webCon = getEl('webconsole');
    if (webCon && otaVerboseToggle && otaVerboseToggle.checked) {
      const ts = new Date().toLocaleTimeString();
      const retriesText = retries >= OTA_LIVE_RETRY_ABORT ? 'Abort' : String(retries);
      const snrText = Number.isFinite(snr) ? `${snr.toFixed(1)} dB` : '–';
      appendConsoleText(webCon, `[${ts}] OTA chunk ${seq}: retries=${retriesText}, snr=${snrText}, ackResends=${ackResends}`);
    }

    otaLiveDraw();
  } else if (p.phase === 'end') {
    const success = !!p.success;
    section.classList.add(success ? 'ota-live-done' : 'ota-live-failed');
    const status = getEl('otaLiveStatus');
    if (status) status.textContent = success ? '✅ Update successful' : '❌ Update failed';

    otaLive.hideTimer = setTimeout(() => {
      hide('otaLiveSection');
    }, OTA_LIVE_HIDE_DELAY_MS);
  }
}

window.addEventListener('resize', () => {
  const section = getEl('otaLiveSection');
  if (section && section.style.display !== 'none') {
    otaLiveDraw();
  }
});

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

function checkBatteryApWarning() {
  const batteryEnabled = !!getVal('batteryPowered');
  const keepAP = !!getVal('keepAP');
  const visible = batteryEnabled && keepAP;
  const warnings = [getEl('batteryApWarning'), getEl('batteryApWarningSettings')];
  warnings.forEach((warning) => {
    if (warning) {
      warning.style.display = visible ? 'block' : 'none';
    }
  });
}

function toggleBatteryModeUi() {
  const batteryEnabled = !!getVal('batteryPowered');
  const batterySleepCard = getEl('batterySleepCard');
  const batteryInfoCard = getEl('batteryInfoCard');
  const batteryVoltage = getEl('batteryVoltage');

  if (batterySleepCard) {
    batterySleepCard.style.display = batteryEnabled ? '' : 'none';
  }
  if (batteryInfoCard) {
    batteryInfoCard.style.display = batteryEnabled ? '' : 'none';
  }
  if (!batteryEnabled) {
    const sleepEnabled = getEl('sleepScheduleEnabled');
    if (sleepEnabled) sleepEnabled.checked = false;
    if (batteryVoltage) batteryVoltage.innerText = '-';
  }
  toggleSleepScheduleUi();
  checkBatteryApWarning();
}

function toggleSleepScheduleUi() {
  const batteryEnabled = !!getVal('batteryPowered');
  const sleepEnabled = !!getVal('sleepScheduleEnabled');
  const sleepInputs = getEl('sleepScheduleInputs');
  if (sleepInputs) {
    sleepInputs.style.display = batteryEnabled && sleepEnabled ? 'flex' : 'none';
  }
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

  const updateLink = getEl("update_link");
  const updateNotice = getEl("update_notice");


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
      if (key === 'msg') {
        showToast(data[key]);
        return;
      }
      const el = getEl(key);
        const skipGenericUpdate = key === 'aprsconsole' || key === 'webconsole' || key === 'webconsole_history';
        if (el && !skipGenericUpdate) {
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

        if(key === "webconsole_history"){
          const con = getEl('webconsole');
          if (con && Array.isArray(data[key])) {
            const historyLines = data[key]
              .filter((e) => e && typeof e.msg === 'string')
              .map((e) => {
                const ts = e.ts && e.ts > 100000 ? new Date(e.ts * 1000).toLocaleTimeString() : (e.ts ? '+' + new Date(e.ts * 1000).toISOString().slice(11, 19) : '?');
                return `[${ts}] ${e.msg}`;
              });
            setConsoleLines(con, historyLines);
          }
        }
        if(key === "webconsole"){
          const con = getEl('webconsole');
          if(con && data[key]) {
            const ts = data.ts && data.ts > 100000 ? new Date(data.ts * 1000).toLocaleTimeString() : new Date().toLocaleTimeString();
            appendConsoleText(con, `[${ts}] ${data[key]}`);
          }
        }
        if(key === "aprsconsole"){
          const aprsCon = getEl('aprsconsole');
          if(aprsCon && data[key]) {
            const now = new Date();
            const time = now.toLocaleTimeString();
            appendConsoleText(aprsCon, `[${time}] ${data[key]}`);
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
          const warning = getEl('betaWarning');
          if (warning) warning.style.display = data[key] === 'beta' ? '' : 'none';
        }
        if(key === "batteryPowered") {
          toggleBatteryModeUi();
        }
        if(key === "sleepScheduleEnabled") {
          toggleSleepScheduleUi();
        }
        if(key === "keepAP") {
          checkBatteryApWarning();
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
    document.getElementById('batteryPowered').addEventListener('change', toggleBatteryModeUi);
    document.getElementById('sleepScheduleEnabled').addEventListener('change', toggleSleepScheduleUi);
    document.getElementById('keepAP').addEventListener('change', checkBatteryApWarning);
    const clearGeneralConsole = document.getElementById('clearGeneralConsole');
    if (clearGeneralConsole) {
      clearGeneralConsole.addEventListener('click', () => {
        const con = document.getElementById('webconsole');
        if (con) con.value = "";
      });
    }

    const clearAprsConsole = document.getElementById('clearAprsConsole');
    if (clearAprsConsole) {
      clearAprsConsole.addEventListener('click', () => {
        const con = document.getElementById('aprsconsole');
        if (con) con.value = "";
      });
    }

    const otaVerboseToggle = document.getElementById('otaVerboseToggle');
    if (otaVerboseToggle) {
      otaVerboseToggle.addEventListener('change', () => {
        if (ws && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({cmd: "set_ota_verbose", verbose: otaVerboseToggle.checked}));
        }
      });
    }

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

    // OTA verbose mode is per-websocket-session on the device, so re-send the
    // current checkbox state whenever a (re)connection is established.
    const otaVerboseToggle = document.getElementById('otaVerboseToggle');
    if (otaVerboseToggle && otaVerboseToggle.checked) {
      ws.send(JSON.stringify({cmd: "set_ota_verbose", verbose: true}));
    }

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
          const stationId = station.vid.toString(16).toUpperCase().padStart(2, "0") + station.fanet_id.toString(16).toUpperCase().padStart(4, "0");
          if (isHiddenStationVid(station.vid)) {
            removeWeatherStationById(stationId);
            return;
          }

          const rowData = {
              id: stationId,
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

          upsertWeatherRow(rowData);
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
        const fanetIdHex = Number(hi.fanet_id || 0).toString(16).toUpperCase().padStart(4, '0');
        if (isHiddenStationVid(hi.vid)) {
          const hiddenStationId = hi.vid.toString(16).toUpperCase().padStart(2, "0") + fanetIdHex;
          removeWeatherStationById(hiddenStationId);
          return;
        }
        const decodeType = Number.isFinite(Number(hi.decode_type))
          ? Number(hi.decode_type)
          : Number((Array.isArray(hi.rawBytes) && hi.rawBytes.length > 0) ? hi.rawBytes[0] : -1);
        console.log(`HWSTATUS fanet_id=${fanetIdHex} decode_type=${decodeType}`);

        const stationId = hi.vid.toString(16).toUpperCase().padStart(2, "0") +
                          hi.fanet_id.toString(16).toUpperCase().padStart(4, "0");
        const decoded = decodeHwInfoDebug(hi.rawBytes || []);
        const packetTs = Number(hi.tLastMsg || 0);
        const packetNewestTs = Number(hi.tNewestHwInfo || 0);
        // Merge: keep latest value of each field across decode types
        const prev = stationHwInfo.get(stationId);
        const previousNewest = prev && prev.merged && Number(prev.merged.newestHwTs || 0);
        const newestHwTs = Math.max(previousNewest || 0, packetTs || 0, packetNewestTs || 0);
        const merged = Object.assign({}, prev ? prev.merged : {}, decoded, { newestHwTs });
        stationHwInfo.set(stationId, { raw: hi, merged });

        if (!weatherStations.has(stationId)) {
          const lastSeen = newestHwTs > 0 ? newestHwTs * 1000 : Date.now();
          upsertWeatherRow({
            id: stationId,
            name: merged.sensor_type_text || '-',
            lat: '-',
            lon: '-',
            dist: '-',
            temp: '-',
            windDir: '-',
            windSpd: '-',
            gust: '-',
            humidity: '-',
            pressure: '-',
            soc: '-',
            rssi: hi.rssi !== undefined ? hi.rssi + ' dBm' : '-',
            lastSeen
          });
        } else {
          setupWeatherRow({ id: stationId });
        }

        // Update open detail row if visible
        const dataRow = weatherStations.get(stationId);
        if (dataRow) {
          const detailRow = dataRow.nextElementSibling;
          if (detailRow && detailRow.classList.contains('hwinfo-row') && detailRow.style.display !== 'none') {
            const td = detailRow.querySelector('td');
            if (td) td.innerHTML = buildHwInfoHtml(hi, merged);
          }
        }
      });
    }
    else if (data.ota_progress) {
      handleOtaProgress(data.ota_progress);
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