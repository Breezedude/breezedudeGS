'use strict';
// Breezedude ground station web UI.
//
// One websocket to /ws. Requests are {"cmd": ...}; the station answers
// info/status/stats/config with a "type" field and pushes live data in keyed
// messages: {"weather":[...]}, {"tracking":[...]}, {"hwinfo":[...]},
// {"ota_history":[...]}, {"ota_progress":{...}}, {"webconsole":...},
// {"aprsconsole":...}, {"msg":...}.

// ---------------------------------------------------------------- utilities --
const $ = (id) => document.getElementById(id);

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (c) => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fanetId(vendor, address) {
  return Number(vendor).toString(16).toUpperCase().padStart(2, '0') +
         Number(address).toString(16).toUpperCase().padStart(4, '0');
}

function num(value, digits = 0, unit = '') {
  if (value === undefined || value === null || value === '-' || !Number.isFinite(Number(value))) return '-';
  return Number(value).toFixed(digits) + (unit ? ' ' + unit : '');
}

function durationText(seconds) {
  seconds = Math.max(0, Math.floor(seconds));
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d} d ${h} h`;
  if (h > 0) return `${h} h ${m} min`;
  if (m > 0) return `${m} min`;
  return `${seconds} s`;
}

function agoText(ms) {
  const s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)} min`;
  return durationText(s);
}

function dateTimeText(unixSeconds) {
  if (!unixSeconds || unixSeconds < 1e9) return '-';
  return new Date(unixSeconds * 1000).toLocaleString([], {
    day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
}

function distanceKm(lat1, lon1, lat2, lon2) {
  const rad = (a) => a * Math.PI / 180;
  const dLat = rad(lat2 - lat1);
  const dLon = rad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(rad(lat1)) * Math.cos(rad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 6371 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function setText(id, text) {
  const el = $(id);
  if (el && el.textContent !== String(text)) el.textContent = text;
}

function setPill(id, level, text, title) {
  const pill = $(id);
  if (!pill) return;
  pill.querySelector('.dot').className = 'dot' + (level ? ' ' + level : '');
  pill.querySelector('.txt').textContent = text;
  pill.title = title || '';
}

function toast(text, type) {
  if (!text) return;
  const lower = String(text).toLowerCase();
  type = type || (/(error|fail|could not|deaktiviert)/.test(lower) ? 'error'
    : /(saved|success|connected|gespeichert)/.test(lower) ? 'success' : 'info');
  const el = document.createElement('div');
  el.className = 'toast ' + type;
  el.textContent = text;
  $('toasts').appendChild(el);
  requestAnimationFrame(() => el.classList.add('visible'));
  setTimeout(() => {
    el.classList.remove('visible');
    setTimeout(() => el.remove(), 250);
  }, 4500);
}

// ------------------------------------------------------------------- state --
const state = {
  info: null,
  status: null,
  config: null,
  stats: null,
  weather: new Map(),     // id -> station record
  tracking: new Map(),    // id -> tracker record
  hwinfo: new Map(),      // id -> merged decoded HW info
  otaHistory: new Map(),  // id -> [{ts, from, to, outcome, detail}]
  repeatSources: new Map(),  // id -> packets per reception kind
  repeatEvents: [],          // newest first
  repeatWindowMin: 15,
  clockOffset: 0,            // station clock minus browser clock, s
  channels: [],           // update channels of the last manifest: [{name, version, description}]
  open: new Set(),        // ids of expanded weather stations
};

const WEATHER_KEEP_MS = 2 * 3600 * 1000;
const TRACKING_KEEP_MS = 5 * 60 * 1000;
// Vendors that announce themselves but are not weather stations.
const HIDDEN_VENDORS = new Set([0x66, 0x11]);

function stationPosition() {
  const c = state.config;
  if (!c || !Number.isFinite(c.lat) || !Number.isFinite(c.lon)) return null;
  if (Math.abs(c.lat - 47) < 1e-5 && Math.abs(c.lon - 12) < 1e-5) return null;  // defaults
  return { lat: c.lat, lon: c.lon };
}

// ---------------------------------------------------------------- theming --
function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  try { localStorage.setItem('theme', theme); } catch (e) { /* private mode */ }
  redrawCharts();
  if (locMap) locMap.setStyle(mapStyle());
}

$('themeBtn').addEventListener('click', () => {
  applyTheme(document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark');
});

// ------------------------------------------------------------------- tabs --
const TABS = ['dashboard', 'tools', 'settings', 'system'];

function showTab() {
  let tab = location.hash.replace('#', '');
  if (!TABS.includes(tab)) tab = 'dashboard';
  TABS.forEach((t) => $('tab-' + t).classList.toggle('active', t === tab));
  document.querySelectorAll('.tabs a').forEach((a) => a.classList.toggle('active', a.dataset.tab === tab));
  if (tab === 'dashboard') requestAnimationFrame(redrawCharts);
  if (tab === 'tools') requestAnimationFrame(otaLiveDraw);
}
window.addEventListener('hashchange', showTab);

// -------------------------------------------------------------- websocket --
let ws = null;
let lastMessageMs = 0;
let connected = false;

function send(obj) {
  if (!(ws && ws.readyState === WebSocket.OPEN)) return;
  if (auth.token) obj.auth = auth.token;
  ws.send(JSON.stringify(obj));
}

function setConnected(value) {
  connected = value;
  $('offline').classList.toggle('visible', !value);
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws');

  ws.onopen = () => {
    lastMessageMs = Date.now();
    setConnected(true);
    send({ cmd: 'auth_check' });
    send({ cmd: 'info' });
    send({ cmd: 'config' });
    send({ cmd: 'status' });
    loadSnapshot();
    // Verbose modes live per websocket session on the station.
    ['ota', 'flarm', 'adsl'].forEach((p) => {
      if ($(p + 'Verbose').checked) send({ cmd: `set_${p}_verbose`, verbose: true });
    });
  };

  ws.onclose = () => {
    setConnected(false);
    setTimeout(connect, 2000);
  };

  ws.onmessage = (event) => {
    lastMessageMs = Date.now();
    if (!connected) setConnected(true);
    let data;
    try { data = JSON.parse(event.data); } catch (e) { return; }
    // What arrives during a snapshot is newer than it, so it waits for it.
    if (snapshotLoading) pendingLive.push(data);
    else handleMessage(data);
  };
}

// The whole state comes from /api/snapshot, the websocket only carries what
// changes after it, see docs/web-api.md.
const WEB_API_PROTO = 2;
const SNAPSHOT_TIMEOUT_MS = 20000;
const SNAPSHOT_RETRY_MS = 5000;
let snapshotLoading = false;
let snapshotTimer = null;
let lastSnapshotMs = 0;
let pendingLive = [];

async function fetchSnapshot(parts) {
  const query = `min=${WEATHER_KEEP_MS / 60000}` + (parts ? `&parts=${parts}` : '');
  const resp = await fetch(`/api/snapshot?${query}`, { cache: 'no-store', signal: AbortSignal.timeout(SNAPSHOT_TIMEOUT_MS) });
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const snap = await resp.json();
  if (!snap.complete) throw new Error('incomplete');
  return snap;
}

function applySnapshot(snap) {
  (snap.weather || []).forEach(onWeather);
  (snap.tracking || []).forEach(onTracking);
  (snap.hwinfo || []).forEach(onHwInfo);
  (snap.ota_history || []).forEach(onOtaHistory);
  if (snap.repeat) onRepeatSnapshot(snap.repeat);
  if (snap.stats) onStats(snap.stats);
  if (snap.console) {
    const lines = snap.console.filter((e) => e && typeof e.msg === 'string').map((e) => `[${consoleTime(e.ts)}] ${e.msg}`);
    setConsole($('systemConsole'), lines);
  }
}

async function loadSnapshot() {
  if (snapshotLoading) return;
  clearTimeout(snapshotTimer);
  snapshotLoading = true;
  lastSnapshotMs = Date.now();
  try {
    applySnapshot(await fetchSnapshot());
  } catch (e) {
    snapshotTimer = setTimeout(loadSnapshot, SNAPSHOT_RETRY_MS);
  }
  snapshotLoading = false;
  const waiting = pendingLive;
  pendingLive = [];
  waiting.forEach(handleMessage);
}

// A busy station may ask for this often; it should not get a download each time.
function requestResync() {
  clearTimeout(snapshotTimer);
  snapshotTimer = setTimeout(loadSnapshot, Math.max(0, lastSnapshotMs + SNAPSHOT_RETRY_MS - Date.now()));
}

function handleMessage(data) {
  if (data.hello) {
    if (data.hello.proto !== WEB_API_PROTO) location.reload();
    return;
  }
  if (data.resync) return requestResync();
  if (data.diag_claim) return onClaim(data.diag_claim);
  if (data.auth) return onAuth(data.auth);
  if (data.authButton) return onAuthButton(data.authButton);
  if (data.locked) {
    toast(data.error, 'error');
    return openAuth();
  }
  if (data.repeat_reset) return onRepeatSnapshot({});
  if (data.type === 'status') return onStatus(data);
  if (data.type === 'info') return onInfo(data);
  if (data.type === 'config') return onConfig(data);
  if (data.repeat) return onRepeatLive(data.repeat);
  if (Array.isArray(data.weather)) return data.weather.forEach(onWeather);
  if (Array.isArray(data.tracking)) return data.tracking.forEach(onTracking);
  if (Array.isArray(data.hwinfo)) return data.hwinfo.forEach(onHwInfo);
  if (Array.isArray(data.ota_history)) return data.ota_history.forEach(onOtaHistory);
  if (Array.isArray(data.update_channels)) return onUpdateChannels(data.update_channels);
  if (data.ota_progress) return onOtaProgress(data.ota_progress);
  if (Array.isArray(data.wifi_scan)) return onWifiScan(data.wifi_scan);
  if (data.wifi_scan_error) return onWifiScanError(data.wifi_scan_error);
  if (data.webconsole) appendConsole($('systemConsole'), `[${consoleTime(data.ts)}] ${data.webconsole}`);
  if (data.aprsconsole) appendConsole($('aprsConsole'), `[${consoleTime()}] ${data.aprsconsole}`);
  if (data.ota_verbose !== undefined) $('otaVerbose').checked = !!data.ota_verbose;
  if (data.flarm_verbose !== undefined) $('flarmVerbose').checked = !!data.flarm_verbose;
  if (data.adsl_verbose !== undefined) $('adslVerbose').checked = !!data.adsl_verbose;
  if (data.msg) toast(data.msg);
  if (data.error) {
    toast(data.error, 'error');
    resetScanButton();
  }
}

setInterval(() => {
  if (connected && Date.now() - lastMessageMs > 6000) setConnected(false);
}, 1000);

setInterval(() => send({ cmd: 'status' }), 2000);
setInterval(() => {
  if (connected) fetchSnapshot('stats').then((snap) => onStats(snap.stats)).catch(() => {});
}, 60000);
setInterval(() => {
  expireRecords();
  renderWeather();
  renderTracking();
  renderRepeat();
  updateAgoCells();
  renderKpis();
}, 1000);

// ----------------------------------------------------------------- status --
function onStatus(s) {
  state.status = s;
  renderDiag(s.diag);
  if (s.time) state.clockOffset = s.time - Date.now() / 1000;
  const wifi = s.wifi || {};

  if (wifi.sta) {
    const weak = wifi.rssi < -80;
    setPill('pillWifi', weak ? 'warn' : 'ok', `${wifi.ssid || 'WiFi'} · ${wifi.rssi} dBm`,
      `Connected, IP ${wifi.ip}` + (wifi.ap ? `, access point open (${wifi.apIp})` : ''));
  } else if (wifi.ap) {
    setPill('pillWifi', 'warn', 'Access point only', `No WiFi connection, AP at ${wifi.apIp}`);
  } else {
    setPill('pillWifi', 'bad', 'No WiFi', '');
  }

  if (!wifi.sta) setPill('pillInternet', '', 'Internet', 'No WiFi connection');
  else setPill('pillInternet', s.internet ? 'ok' : 'bad', 'Internet', s.internet ? 'Reachable' : 'Not reachable');

  if (s.aprs === 'off') setPill('pillAprs', '', 'OGN off', 'Sending to APRS/OGN is disabled');
  else setPill('pillAprs', s.aprs === 'connected' ? 'ok' : 'bad', 'OGN',
    s.aprs === 'connected' ? 'Connected to the APRS server' : 'Not connected to the APRS server');

  const bd = s.breezedude;
  if (!bd) setPill('pillBreezedude', '', 'Breezedude off', 'Forwarding to Breezedude is disabled');
  else if (bd.code === 0) setPill('pillBreezedude', '', 'Breezedude', 'Nothing forwarded yet');
  else if (bd.code >= 200 && bd.code < 300) {
    setPill('pillBreezedude', 'ok', 'Breezedude', `Last upload ${durationText(bd.age)} ago`);
  } else {
    setPill('pillBreezedude', 'bad', 'Breezedude',
      `Last upload failed (${bd.code < 0 ? 'no connection' : 'HTTP ' + bd.code}), ${durationText(bd.age)} ago`);
  }

  renderKpis();
  renderBanners();
  renderFirmware();
  renderDeviceInfo();
}

function renderBanners() {
  const out = [];
  const s = state.status;
  const c = state.config;
  if (s && s.update && s.update.available && s.update.version) {
    out.push(`<div class="banner info"><span class="grow">Firmware <b>${esc(s.update.version)}</b> is available.
      ${c && c.autoUpdate ? 'It will be installed automatically.' : ''}</span>
      <a href="#system">System</a></div>`);
  }
  if (c && !setupComplete(c.deviceName, c.lat, c.lon)) {
    out.push(`<div class="banner warn"><span class="grow">Name and position are not set yet - OGN, Breezedude
      and FLARM stay off until they are.</span><a href="#settings">Settings</a></div>`);
  }
  if (s && s.diag && s.diag.rejected) {
    out.push(`<div class="banner warn"><span class="grow">The Breezedude server rejects this station's device key -
      diagnostic reports and linking to an account do not work.</span><a href="#system">System</a></div>`);
  }
  if (otaLive.active) {
    out.push(`<div class="banner info"><span class="grow">Updating sensor firmware:
      ${otaLive.chunks.length} of ${otaLive.total} packets</span><a href="#tools">Details</a></div>`);
  }
  const html = out.join('');
  const el = $('banners');
  if (el.dataset.html !== html) {
    el.innerHTML = html;
    el.dataset.html = html;
  }
}

function renderKpis() {
  const s = state.status;
  const stats = state.stats;

  if (stats) {
    const sum = (col) => stats.rx.reduce((a, r) => a + (r[col] || 0), 0);
    const fanet = sum(0);
    const flarm = sum(1);
    const adsl = sum(2);
    setText('kpiPackets', (fanet + flarm + adsl).toLocaleString());
    const parts = [`FANET ${fanet.toLocaleString()}`];
    if (flarm || (s && s.radio && s.radio.flarm)) parts.push(`FLARM ${flarm.toLocaleString()}`);
    if (adsl || (s && s.radio && s.radio.adsl)) parts.push(`ADS-L ${adsl.toLocaleString()}`);
    setText('kpiPacketsSub', parts.join(' · '));

    const q = qualitySummary(stats);
    $('kpiSnr').innerHTML = q.snr === null ? '-' : `${q.snr.toFixed(1)}<span class="unit">dB SNR</span>`;
    setText('kpiSnrSub', q.noise === null ? 'average of the last 24 h' : `noise floor ${q.noise} dBm`);
  }

  const now = Date.now();
  let active = 0;
  state.weather.forEach((w) => { if (now - w.lastSeen < 10 * 60 * 1000) active++; });
  setText('kpiStations', String(active));
  setText('kpiStationsSub', state.weather.size > active ? `${state.weather.size} heard in 2 h` : 'heard in the last 10 min');

  let flying = 0;
  state.tracking.forEach((t) => { if (t.flying) flying++; });
  setText('kpiTraffic', String(state.tracking.size));
  setText('kpiTrafficSub', state.tracking.size ? `${flying} flying` : 'nothing in the last 5 min');

  if (s) {
    setText('kpiUptime', durationText(s.uptime));
    setText('kpiUptimeSub', s.sleep ? `power: ${s.sleep}` : '');
  }

  const battery = s && Number.isFinite(s.battery) ? s.battery : null;
  $('kpiBatteryCard').hidden = !(state.config && state.config.batteryPowered);
  if (battery !== null) {
    $('kpiBattery').innerHTML = `${battery.toFixed(2)}<span class="unit">V</span>`;
    const trend = batteryTrend();
    setText('kpiBatterySub', `about ${socFromVoltage(battery)} %` + (trend ? ' · ' + trend : ''));
  }
}

// --------------------------------------------------------------- info/system --
function onInfo(info) {
  state.info = info;
  setText('hdrVersion', 'Firmware ' + info.version);
  renderChannelOptions();
  renderDeviceInfo();
  renderFirmware();
}

function renderDeviceInfo() {
  const i = state.info;
  if (!i) return;
  const s = state.status || {};
  const wifi = s.wifi || {};
  const rows = [
    ['Firmware', `${i.version} (${i.compileTime})`],
    ['ESP-IDF', i.idf],
    ['Bootloader', i.bootloader],
    ['Chip', `${String(i.chip).toUpperCase()} v${Math.floor(i.chipRev / 100)}.${i.chipRev % 100}, ${i.cores} cores`],
    ['Flash', i.flashSize ? `${i.flashSize / 1048576} MB` : '-'],
    ['Sensor images', i.fwstore && i.fwstore.total ? `${Math.round(i.fwstore.free / 1024)} of ${Math.round(i.fwstore.total / 1024)} kB free` : '-'],
    ['Running slot', i.partition],
    ['Last reset', i.resetReason],
    ['Radio', i.radio],
    ['MAC', i.mac],
    ['IP', wifi.ip || '-'],
    ['Uptime', s.uptime !== undefined ? durationText(s.uptime) : '-'],
    ['Free memory', s.heap ? `${Math.round(s.heap / 1024)} kB (lowest ${Math.round(s.heapMin / 1024)} kB)` : '-'],
    ['Largest free block', s.heapBlock ? `${Math.round(s.heapBlock / 1024)} kB` : '-'],
    ['Live data lost', s.ws ? `${s.ws.dropped} messages, ${s.ws.resyncs} resyncs` : '-'],
    ['Power', s.sleep || '-'],
  ];
  const html = rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v ?? '-')}</dd>`).join('');
  const el = $('deviceInfo');
  if (el.dataset.html !== html) {
    el.innerHTML = html;
    el.dataset.html = html;
  }
}

function renderFirmware() {
  if (state.info) setText('fwInstalled', state.info.version);
  const u = state.status && state.status.update;
  if (u) setText('fwOnline', u.version ? (u.available ? `${u.version} - update available` : u.version) : '-');
}

// ------------------------------------------------------------------ charts --
// Minimal canvas charts: stacked bars and lines over equally spaced time slots,
// with an optional shaded band per slot and a hover tooltip.
class Chart {
  constructor(container) {
    this.container = container;
    this.canvas = document.createElement('canvas');
    this.tip = document.createElement('div');
    this.tip.className = 'chart-tip';
    container.append(this.canvas, this.tip);
    this.opts = null;
    this.hover = -1;
    this.canvas.addEventListener('mousemove', (e) => this.onMove(e));
    this.canvas.addEventListener('mouseleave', () => { this.hover = -1; this.tip.style.display = 'none'; this.draw(); });
  }

  set(opts) {
    this.opts = opts;
    this.draw();
  }

  layout() {
    const rect = this.canvas.getBoundingClientRect();
    return { w: rect.width, h: rect.height, left: 40, right: 8, top: this.opts.yTitle ? 20 : 8,
      bottom: this.opts.noXAxis ? 6 : 20 };
  }

  onMove(e) {
    if (!this.opts) return;
    const l = this.layout();
    const rect = this.canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const n = this.opts.count;
    const idx = Math.floor((x - l.left) / ((l.w - l.left - l.right) / n));
    if (idx < 0 || idx >= n) {
      this.hover = -1;
      this.tip.style.display = 'none';
    } else {
      this.hover = idx;
      const html = this.opts.tooltip ? this.opts.tooltip(idx) : '';
      if (html) {
        this.tip.innerHTML = html;
        this.tip.style.display = 'block';
        const tipW = this.tip.offsetWidth;
        let left = x + 12;
        if (left + tipW > l.w) left = x - tipW - 12;
        this.tip.style.left = Math.max(0, left) + 'px';
        this.tip.style.top = '4px';
      }
    }
    this.draw();
  }

  draw() {
    const o = this.opts;
    if (!o || !this.canvas.isConnected || this.canvas.offsetParent === null) return;
    const dpr = window.devicePixelRatio || 1;
    const l = this.layout();
    if (l.w <= 0 || l.h <= 0) return;
    this.canvas.width = Math.round(l.w * dpr);
    this.canvas.height = Math.round(l.h * dpr);
    const ctx = this.canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, l.w, l.h);

    const plotW = l.w - l.left - l.right;
    const plotH = l.h - l.top - l.bottom;
    const n = o.count;
    const slotW = plotW / n;
    const font = '11px ' + cssVar('--font');
    const muted = cssVar('--faint');
    const grid = cssVar('--grid');

    // value range
    let min = o.yMin;
    let max = o.yMax;
    if (min === undefined || max === undefined) {
      let lo = Infinity;
      let hi = -Infinity;
      (o.bars || []).length && (lo = 0);
      for (let i = 0; i < n; i++) {
        let stack = 0;
        (o.bars || []).forEach((b) => { stack += b.values[i] || 0; });
        if (o.bars) hi = Math.max(hi, stack);
        (o.lines || []).forEach((ln) => {
          const v = ln.values[i];
          if (v !== null && v !== undefined && Number.isFinite(v)) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
        });
      }
      if (!Number.isFinite(lo)) { lo = 0; hi = 1; }
      if (hi === lo) { hi += 1; lo -= o.bars ? 0 : 1; }
      const pad = o.bars ? 0 : (hi - lo) * 0.1;
      min = min !== undefined ? min : lo - pad;
      max = max !== undefined ? max : hi + pad;
      if (o.bars) max = Math.max(max, o.minMax || 1);
    }
    const ticks = niceTicks(min, max, plotH > 120 ? 4 : 2);
    min = Math.min(min, ticks[0]);
    max = Math.max(max, ticks[ticks.length - 1]);
    const y = (v) => l.top + plotH - ((v - min) / (max - min)) * plotH;

    // bands (charging periods etc.)
    // one rectangle per run of slots - per-slot rectangles leave seams
    (o.bands || []).forEach((band) => {
      ctx.fillStyle = band.color;
      for (let i = 0; i < n; i++) {
        if (!band.values[i]) continue;
        let j = i;
        while (j + 1 < n && band.values[j + 1]) j++;
        ctx.fillRect(l.left + i * slotW, l.top, (j - i + 1) * slotW, plotH);
        i = j;
      }
    });

    // grid + y labels
    ctx.font = font;
    ctx.fillStyle = muted;
    ctx.strokeStyle = grid;
    ctx.lineWidth = 1;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ticks.forEach((t) => {
      const yy = Math.round(y(t)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(l.left, yy);
      ctx.lineTo(l.w - l.right, yy);
      ctx.stroke();
      ctx.fillText(o.yFormat ? o.yFormat(t) : String(t), l.left - 6, yy);
    });

    // axis title, above the y labels
    if (o.yTitle) {
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      ctx.fillText(o.yTitle, 2, 1);
    }

    // x labels
    if (!o.noXAxis && o.xLabel) {
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      for (let i = 0; i < n; i++) {
        const label = o.xLabel(i);
        if (label) ctx.fillText(label, l.left + i * slotW, l.h - l.bottom + 5);
      }
    }

    // stacked bars
    if (o.bars) {
      const gap = slotW > 4 ? 1 : 0;
      for (let i = 0; i < n; i++) {
        let base = 0;
        o.bars.forEach((b) => {
          const v = b.values[i] || 0;
          if (v <= 0) return;
          ctx.fillStyle = b.color;
          const y0 = y(base);
          const y1 = y(base + v);
          ctx.fillRect(l.left + i * slotW + gap / 2, y1, Math.max(slotW - gap, 1), Math.max(y0 - y1, 1));
          base += v;
        });
      }
    }

    // lines (gaps where a value is null)
    (o.lines || []).forEach((ln) => {
      ctx.strokeStyle = ln.color;
      ctx.lineWidth = ln.width || 1.75;
      ctx.setLineDash(ln.dash || []);
      ctx.beginPath();
      let pen = false;
      for (let i = 0; i < n; i++) {
        const v = ln.values[i];
        if (v === null || v === undefined || !Number.isFinite(v)) { pen = false; continue; }
        const xx = l.left + (i + 0.5) * slotW;
        if (pen) ctx.lineTo(xx, y(v)); else ctx.moveTo(xx, y(v));
        pen = true;
      }
      ctx.stroke();
      ctx.setLineDash([]);
      // single points between gaps would be invisible as a path
      ctx.fillStyle = ln.color;
      for (let i = 0; i < n; i++) {
        const v = ln.values[i];
        const prev = ln.values[i - 1];
        const next = ln.values[i + 1];
        if (Number.isFinite(v) && !Number.isFinite(prev) && !Number.isFinite(next)) {
          ctx.beginPath();
          ctx.arc(l.left + (i + 0.5) * slotW, y(v), 1.8, 0, Math.PI * 2);
          ctx.fill();
        }
      }
    });

    if (this.hover >= 0) {
      ctx.fillStyle = cssVar('--accent-soft');
      ctx.fillRect(l.left + this.hover * slotW, l.top, Math.max(slotW, 2), plotH);
    }
  }
}

function niceTicks(min, max, count) {
  const span = max - min || 1;
  const raw = span / count;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => span / s <= count + 0.5) || mag * 10;
  const ticks = [];
  for (let t = Math.floor(min / step) * step; t <= max + step * 0.001; t += step) {
    ticks.push(Math.round(t / step) * step);
  }
  if (ticks.length < 2) ticks.push(ticks[0] + step);
  return ticks;
}

const charts = {};
function chartFor(id) {
  if (!charts[id]) charts[id] = new Chart($(id));
  return charts[id];
}

function redrawCharts() {
  Object.values(charts).forEach((c) => c.draw());
}
window.addEventListener('resize', () => { redrawCharts(); otaLiveDraw(); });

// Time of slot i (0 = oldest) of an n-slot history ending at `end` (epoch s).
function slotStart(end, slotSeconds, n, i) {
  return end ? (end - (n - i) * slotSeconds) * 1000 : null;
}

function slotLabel(end, slotSeconds, n, i) {
  const t = slotStart(end, slotSeconds, n, i);
  if (t === null) {
    const hoursAgo = ((n - i) * slotSeconds) / 3600;
    return `-${hoursAgo.toFixed(hoursAgo < 10 ? 1 : 0)} h`;
  }
  const d = new Date(t);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

// Axis label every `everyHours`, on the full hour where the clock is known.
function hourAxis(end, slotSeconds, n, everyHours) {
  const perLabel = (everyHours * 3600) / slotSeconds;
  return (i) => {
    const t = slotStart(end, slotSeconds, n, i);
    if (t === null) return (n - i) % perLabel === 0 ? `-${((n - i) * slotSeconds) / 3600} h` : '';
    const d = new Date(t);
    if (d.getMinutes() !== 0 || d.getHours() % everyHours !== 0) return '';
    if (everyHours >= 24 || d.getHours() === 0) return d.toLocaleDateString([], { weekday: 'short' });
    return `${String(d.getHours()).padStart(2, '0')}:00`;
  };
}

// ------------------------------------------------------------- statistics --
// stats.rx[i] = [fanet, flarm, adsl, crcErr, decodeFail, fskErr, snr*10, rssi, noiseMin, noiseAvg]
function qualitySummary(stats) {
  let snrW = 0, snrSum = 0, rssiSum = 0, crc = 0, dec = 0, fsk = 0, fanet = 0;
  const noise = [];
  let noiseMin = null;
  stats.rx.forEach((r) => {
    fanet += r[0];
    crc += r[3];
    dec += r[4];
    fsk += r[5];
    if (r[6] !== null && r[0] > 0) {
      snrSum += (r[6] / 10) * r[0];
      rssiSum += r[7] * r[0];
      snrW += r[0];
    }
    if (r[9] !== null) noise.push(r[9]);
    if (r[8] !== null) noiseMin = noiseMin === null ? r[8] : Math.min(noiseMin, r[8]);
  });
  noise.sort((a, b) => a - b);
  return {
    snr: snrW ? snrSum / snrW : null,
    rssi: snrW ? Math.round(rssiSum / snrW) : null,
    noise: noise.length ? noise[Math.floor(noise.length / 2)] : null,
    noiseMin,
    crc,
    crcRate: fanet + crc ? (crc / (fanet + crc)) * 100 : 0,
    decodeFail: dec,
    fskErr: fsk,
  };
}

function onStats(stats) {
  state.stats = stats;
  const rx = stats.rx;
  const n = rx.length;
  const slot = stats.slot;
  const end = stats.end;
  const s = state.status || {};
  const radio = s.radio || {};
  const col = (k, f = (v) => v) => rx.map((r) => f(r[k]));

  const bars = [{ label: 'FANET', color: cssVar('--c-fanet'), values: col(0) }];
  const flarmOn = rx.some((r) => r[1]) || radio.flarm;
  const adslOn = rx.some((r) => r[2]) || radio.adsl;
  if (flarmOn) bars.push({ label: 'FLARM', color: cssVar('--c-flarm'), values: col(1) });
  if (adslOn) bars.push({ label: 'ADS-L', color: cssVar('--c-adsl'), values: col(2) });
  bars.push({ label: 'CRC errors', color: cssVar('--c-error'), values: col(3) });
  $('activityLegend').innerHTML = bars.map((b) => `<span><i style="background:${b.color}"></i>${b.label}</span>`).join('');

  chartFor('chartActivity').set({
    count: n,
    bars,
    minMax: 4,
    yTitle: 'packets / 15 min',
    yFormat: (v) => String(v),
    xLabel: hourAxis(end, slot, n, 6),
    tooltip: (i) => {
      const r = rx[i];
      const from = slotLabel(end, slot, n, i);
      let html = `<b>${from}</b> (15 min)<br>FANET ${r[0]}`;
      if (flarmOn) html += `<br>FLARM ${r[1]}`;
      if (adslOn) html += `<br>ADS-L ${r[2]}`;
      if (r[3]) html += `<br>CRC errors ${r[3]}`;
      if (r[4]) html += `<br>Undecodable ${r[4]}`;
      if (r[5]) html += `<br>FLARM/ADS-L errors ${r[5]}`;
      return html;
    },
  });

  const signalTip = (i) => {
    const r = rx[i];
    const parts = [`<b>${slotLabel(end, slot, n, i)}</b>`];
    if (r[7] !== null) parts.push(`RSSI ${r[7]} dBm, SNR ${(r[6] / 10).toFixed(1)} dB`);
    if (r[9] !== null) parts.push(`Noise ${r[9]} dBm (min ${r[8]})`);
    if (parts.length === 1) parts.push('no data');
    return parts.join('<br>');
  };
  chartFor('chartSignal').set({
    count: n,
    lines: [
      { color: cssVar('--c-noise'), values: col(9), width: 1.5 },
      { color: cssVar('--c-noise'), values: col(8), width: 1, dash: [3, 3] },
      { color: cssVar('--c-rssi'), values: col(7) },
    ],
    yTitle: 'signal dBm',
    yFormat: (v) => String(v),
    noXAxis: true,
    tooltip: signalTip,
  });
  chartFor('chartSnr').set({
    count: n,
    lines: [{ color: cssVar('--c-snr'), values: col(6, (v) => (v === null ? null : v / 10)) }],
    yTitle: 'SNR dB',
    yFormat: (v) => `${v}`,
    xLabel: hourAxis(end, slot, n, 6),
    tooltip: signalTip,
  });

  const q = qualitySummary(stats);
  const facts = [
    ['Average SNR', q.snr === null ? '-' : `${q.snr.toFixed(1)} dB`],
    ['Average RSSI', q.rssi === null ? '-' : `${q.rssi} dBm`],
    ['Noise floor', q.noise === null ? '-' : `${q.noise} dBm`],
    ['Lowest noise', q.noiseMin === null ? '-' : `${q.noiseMin} dBm`],
    ['CRC errors', `${q.crc} (${q.crcRate.toFixed(1)} %)`],
    ['Undecodable', String(q.decodeFail)],
  ];
  if (flarmOn || adslOn) facts.push(['FLARM/ADS-L errors', String(q.fskErr)]);
  $('qualityFacts').innerHTML = facts.map(([k, v]) =>
    `<div><div class="fact-label">${k}</div><div class="fact-value">${v}</div></div>`).join('');

  renderWifi();
  renderBattery();
  renderKpis();
}

// WiFi STA signal: columns 10 (slot average) and 11 (weakest sample).
function renderWifi() {
  const stats = state.stats;
  if (!stats) return;
  const rx = stats.rx;
  const n = rx.length;
  const avg = rx.map((r) => (r[10] === undefined ? null : r[10]));
  const weakest = rx.map((r) => (r[11] === undefined ? null : r[11]));
  chartFor('chartWifi').set({
    count: n,
    lines: [
      { color: cssVar('--c-noise'), values: weakest, width: 1, dash: [3, 3] },
      { color: cssVar('--c-rssi'), values: avg },
    ],
    yTitle: 'RSSI dBm',
    yFormat: (v) => String(v),
    xLabel: hourAxis(stats.end, stats.slot, n, 6),
    tooltip: (i) => {
      const head = `<b>${slotLabel(stats.end, stats.slot, n, i)}</b>`;
      return avg[i] === null ? `${head}<br>not connected` : `${head}<br>average ${avg[i]} dBm<br>weakest ${weakest[i]} dBm`;
    },
  });
  const valid = avg.filter((v) => v !== null);
  const wifi = (state.status && state.status.wifi) || {};
  const facts = [
    ['Now', wifi.sta ? `${wifi.rssi} dBm` : 'not connected'],
    ['Average (24 h)', valid.length ? `${Math.round(valid.reduce((a, b) => a + b, 0) / valid.length)} dBm` : '-'],
    ['Weakest (24 h)', valid.length ? `${Math.min(...weakest.filter((v) => v !== null))} dBm` : '-'],
  ];
  $('wifiFacts').innerHTML = facts.map(([k, v]) =>
    `<div><div class="fact-label">${k}</div><div class="fact-value">${v}</div></div>`).join('');
}

// ---------------------------------------------------------------- battery --
// Single-cell Li-ion/LiPo open-circuit curve; only an estimate under load.
const SOC_CURVE = [
  [3.30, 0], [3.61, 5], [3.69, 10], [3.71, 15], [3.73, 20], [3.75, 25], [3.77, 30], [3.79, 35],
  [3.80, 40], [3.82, 45], [3.84, 50], [3.85, 55], [3.87, 60], [3.91, 65], [3.95, 70], [3.98, 75],
  [4.02, 80], [4.08, 85], [4.11, 90], [4.15, 95], [4.20, 100]];

function socFromVoltage(v) {
  if (v <= SOC_CURVE[0][0]) return 0;
  for (let i = 1; i < SOC_CURVE.length; i++) {
    const [v1, p1] = SOC_CURVE[i];
    const [v0, p0] = SOC_CURVE[i - 1];
    if (v <= v1) return Math.round(p0 + ((v - v0) / (v1 - v0)) * (p1 - p0));
  }
  return 100;
}

// Per battery slot: 1 = charging, 2 = full, 0 = neither. Derived from the
// voltage: rising by more than 10 mV per 15 min means charging; a plateau near
// the charge voltage means full.
function batteryPhases(mv) {
  return mv.map((cur, i) => {
    if (!cur) return 0;
    const prev = mv[i - 1];
    const last = cur[2];
    if (cur[1] >= 4150 && (!prev || Math.abs(last - prev[2]) <= 10)) return 2;
    if (prev && last - prev[2] > 10) return 1;
    return 0;
  });
}

function batteryTrend() {
  const b = state.stats && state.stats.bat;
  if (!b) return '';
  const mv = b.mv;
  const recent = mv.slice(-5).filter(Boolean);
  if (recent.length < 2) return '';
  const delta = recent[recent.length - 1][2] - recent[0][2];
  if (state.config && state.config.solarPowered) {
    const phase = batteryPhases(mv).slice(-2);
    if (phase.includes(2)) return 'full';
    if (phase.includes(1)) return 'charging';
  }
  if (delta > 15) return 'rising';
  if (delta < -15) return 'falling';
  return 'steady';
}

function renderBattery() {
  const c = state.config;
  const b = state.stats && state.stats.bat;
  const show = !!(c && c.batteryPowered);
  $('batteryCard').hidden = !show;
  if (!show) return;
  if (!b) {
    $('batteryFacts').innerHTML = '<div class="muted">No measurements yet.</div>';
    return;
  }
  const mv = b.mv;
  const n = mv.length;
  const solar = !!c.solarPowered;
  const phases = batteryPhases(mv);
  const values = mv.map((m) => (m ? m[2] / 1000 : null));
  const valid = values.filter((v) => v !== null);
  const lo = valid.length ? Math.min(...valid) : 3.5;
  const hi = valid.length ? Math.max(...valid) : 4.2;

  const legend = [`<span><i class="line" style="background:var(--c-battery)"></i>Voltage</span>`];
  if (solar) {
    legend.push(`<span><i style="background:var(--c-charge)"></i>Charging</span>`);
    legend.push(`<span><i style="background:var(--c-full)"></i>Full</span>`);
  }
  $('batteryLegend').innerHTML = legend.join('');

  chartFor('chartBattery').set({
    count: n,
    lines: [{ color: cssVar('--c-battery'), values }],
    bands: solar ? [
      { color: cssVar('--c-charge'), values: phases.map((p) => p === 1) },
      { color: cssVar('--c-full'), values: phases.map((p) => p === 2) },
    ] : [],
    yMin: Math.min(3.4, Math.floor(lo * 10) / 10),
    yMax: Math.max(4.25, Math.ceil(hi * 10) / 10),
    yTitle: 'volts',
    yFormat: (v) => v.toFixed(1),
    xLabel: hourAxis(b.end, b.slot, n, 24),
    tooltip: (i) => {
      const m = mv[i];
      const head = `<b>${slotLabel(b.end, b.slot, n, i)}</b>`;
      if (!m) return `${head}<br>no data`;
      const phase = solar ? ['', '<br>charging', '<br>full'][phases[i]] : '';
      return `${head}<br>${(m[2] / 1000).toFixed(2)} V (${socFromVoltage(m[2] / 1000)} %)` +
        `<br>min ${(m[0] / 1000).toFixed(2)} / max ${(m[1] / 1000).toFixed(2)} V${phase}`;
    },
  });

  const facts = [];
  const lastIdx = values.map((v, i) => (v !== null ? i : -1)).filter((i) => i >= 0).pop();
  if (lastIdx !== undefined) {
    const v = values[lastIdx];
    facts.push(['Now', `${v.toFixed(2)} V · ${socFromVoltage(v)} %`]);
  }
  if (valid.length) {
    facts.push(['Lowest (3 days)', `${lo.toFixed(2)} V`]);
    facts.push(['Highest (3 days)', `${hi.toFixed(2)} V`]);
  }
  if (solar) {
    const lastFull = phases.lastIndexOf(2);
    const lastCharge = phases.lastIndexOf(1);
    const ago = (i) => (i < 0 ? 'not in 3 days' : i >= n - 1 ? 'now' : `${durationText((n - 1 - i) * b.slot)} ago`);
    facts.push(['Last full', ago(lastFull)]);
    facts.push(['Last charging', ago(lastCharge)]);
  }
  $('batteryFacts').innerHTML = facts.map(([k, v]) =>
    `<div><div class="fact-label">${k}</div><div class="fact-value">${v}</div></div>`).join('');
}

// ------------------------------------------------------- weather stations --
const RESET_CAUSE_TEXT = ['power-on', 'watchdog', 'software', 'brown-out', 'external', 'several', 'unknown', 'unknown'];
const AWAKE_CLASS_TEXT = ['< 1 %', '1-5 %', '5-20 %', '>= 20 %'];
const SCHED_STATE_TEXT = ['free', 'learning', 'checking', 'synced', 'resync', '?', '?', '?'];
const REPEATER_MODE_TEXT = ['off', 'classic', 'scheduled (listening)', 'scheduled'];
const SENSOR_TYPE_TEXT = ['invalid', 'WS80', 'WS85', 'WS85 UART', 'Davis 6410', 'Windnerd'];
const PV_STATE_TEXT = ['not charging', 'charging', 'full', 'full'];

// HW-info packets come in several decode types; each carries different fields.
function decodeHwInfo(rawBytes) {
  const out = {};
  if (!Array.isArray(rawBytes) || !rawBytes.length) return out;
  const b = rawBytes.map((v) => Number(v) & 0xff);
  const type = b[0];

  if (type === 0x01 && b.length >= 11) {
    out.vbatt_mv = b[1] | (b[2] << 8);
    out.batt_perc = b[3];
    out.pv_state = b[4] & 0x03;
    out.awake_class = (b[4] >> 2) & 0x03;
    out.rx_count = b[5] | (b[6] << 8);
    out.forward_count = b[7] | (b[8] << 8);
    out.lbt_counter = b[9];
    out.resets = b[10] & 0x1f;
    out.reset_cause = (b[10] >> 5) & 0x07;
  }
  if (type === 0x02 && b.length >= 11) {
    const v = b[1] | (b[2] << 8);
    out.version = `${(v >> 8) & 0x0f}.${(v >> 4) & 0x0f}.${v & 0x0f}`;
    out.slot_capacity_kb = b[7] | (b[8] << 8);
    out.ota_proto = b[9];
    out.ota_slot = (b[10] & 0x01) ? 'B' : 'A';
  }
  if (type === 0x03 && b.length >= 5) {
    out.sensor_type = b[1] & 0x1f;
    out.use_baro = ((b[1] >> 5) & 1) === 1;
    out.lbt = ((b[1] >> 7) & 1) === 1;
    out.lbt_threshold = -b[2];
    out.sensor_integ_s = b[3];
    out.low_power_v = 2 + b[4] / 100;
    if (b.length >= 8) {
      const hw = b[5];
      out.hw_rev = (hw & 0x0f) === 1 ? '2.5' : '2.4 or earlier';
      out.hw_forced = ((hw >> 4) & 1) === 1;
      out.hw_overruled = ((hw >> 5) & 1) === 1;
      const bl = b[6] | (b[7] << 8);
      out.bootloader = bl ? `${(bl >> 12) & 0x0f}.${(bl >> 6) & 0x3f}.${bl & 0x3f}` : 'unknown';
    }
  }
  if (type === 0x04 && b.length >= 4) {
    const flags = b[1];
    out.repeater_mode = flags & 0x03;
    out.repeater_scanning = ((flags >> 2) & 1) === 1;
    out.repeater_table = (flags >> 3) & 0x07;
    out.repeater_dropped = ((flags >> 6) & 1) === 1;
    out.repeater_rx_off = ((flags >> 7) & 1) === 1;
    out.rx_duty = b[2];
    out.forwarded = b[3];
    out.repeater_stations = [];
    for (let i = 4; i + 5 <= b.length; i += 5) {
      const st = b[i + 4];
      out.repeater_stations.push({
        id: fanetId(b[i], b[i + 1] | (b[i + 2] << 8)),
        period: b[i + 3],
        state: SCHED_STATE_TEXT[st & 0x07],
        misses: (st >> 3) & 0x03,
        hwState: SCHED_STATE_TEXT[(st >> 5) & 0x03],
      });
    }
  }
  return out;
}

function onWeather(w) {
  if (HIDDEN_VENDORS.has(Number(w.vid))) return;
  const id = fanetId(w.vid, w.fanet_id);
  const rec = state.weather.get(id) || { id };
  Object.assign(rec, {
    vid: w.vid,
    address: w.fanet_id,
    name: w.name || rec.name || '',
    lat: w.lat,
    lon: w.lon,
    temp: w.temp,
    heading: w.wHeading,
    speed: w.wSpeed,
    gust: w.wGust,
    humidity: w.humidity,
    baro: w.baro,
    charge: w.charge,
    rssi: w.rssi,
    snr: w.snr,
    hasWeather: true,
    lastSeen: w.tLastMsg > 1e9 ? w.tLastMsg * 1000 : Date.now(),
  });
  state.weather.set(id, rec);
  renderWeather();
}

function onHwInfo(hi) {
  if (HIDDEN_VENDORS.has(Number(hi.vid))) return;
  const id = fanetId(hi.vid, hi.fanet_id);
  const prev = state.hwinfo.get(id) || {};
  const decoded = decodeHwInfo(hi.rawBytes);
  const merged = Object.assign({}, prev, decoded);
  merged.newest = Math.max(prev.newest || 0, Number(hi.tLastMsg) || 0, Number(hi.tNewestHwInfo) || 0);
  // tNewestHwInfo covers all packets of the station, tLastMsg only this one.
  const packetTs = Number(hi.tLastMsg) || 0;
  if (decoded.repeater_mode !== undefined) merged.repeaterReport = packetTs > 1e9 ? packetTs * 1000 : Date.now();
  if (hi.buildYear && hi.buildMonth && hi.buildDay) {
    merged.buildDate = `${hi.buildYear}-${String(hi.buildMonth).padStart(2, '0')}-${String(hi.buildDay).padStart(2, '0')}` +
      (hi.isDevelopmentBuild ? ' (dev)' : '');
  }
  if (hi.hasUptime && packetTs >= (prev.uptimeAt || 0)) {
    merged.uptimeMin = hi.uptimeMinutes;
    merged.uptimeAt = packetTs;
  }
  merged.rssi = hi.rssi;
  merged.snr = hi.snr;
  state.hwinfo.set(id, merged);

  if (!state.weather.has(id)) {
    state.weather.set(id, {
      id, vid: hi.vid, address: hi.fanet_id, name: '', rssi: hi.rssi, snr: hi.snr, hasWeather: false,
      lastSeen: merged.newest > 1e9 ? merged.newest * 1000 : Date.now(),
    });
  }
  renderWeather();
}

function onOtaHistory(h) {
  state.otaHistory.set(h.id, h.entries || []);
  if (state.open.has(h.id)) renderWeather(true);
}

const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
function windHtml(heading, speed) {
  if (!Number.isFinite(speed)) return '-';
  if (!Number.isFinite(heading)) return num(speed, 0, 'km/h');
  const dir = COMPASS[Math.round(heading / 45) % 8];
  // The arrow points where the wind blows to.
  return `<span class="wind"><svg viewBox="0 0 16 16" style="transform:rotate(${(heading + 180) % 360}deg)">
    <path d="M8 1v14M8 15l-4-4M8 15l4-4" fill="none" stroke="currentColor" stroke-width="1.8"
    stroke-linecap="round" stroke-linejoin="round" transform="rotate(180 8 8)"/></svg>${dir} ${num(speed, 0, 'km/h')}</span>`;
}

function signalHtml(rssi, snr, fsk) {
  if (!Number.isFinite(rssi)) return '-';
  let bars;
  if (fsk) bars = rssi > -80 ? 4 : rssi > -95 ? 3 : rssi > -105 ? 2 : 1;
  else bars = snr > 5 ? 4 : snr > 0 ? 3 : snr > -7 ? 2 : 1;
  const cls = bars >= 3 ? '' : bars === 2 ? ' weak' : ' poor';
  const title = fsk ? `${rssi} dBm` : `${rssi} dBm, SNR ${snr} dB`;
  return `<span class="signal${cls}" title="${title}">${[4, 7, 10, 14].map((h, i) =>
    `<i style="height:${h}px" class="${i < bars ? 'on' : ''}"></i>`).join('')}</span>${rssi} dBm`;
}

function ageDot(lastSeen) {
  const age = Date.now() - lastSeen;
  const cls = age < 10 * 60e3 ? 'ok' : age < 30 * 60e3 ? 'warn' : '';
  return `<span class="dot ${cls}"></span>`;
}

// "Last seen" cells are filled in separately, so the rows themselves only
// change - and get rebuilt - when their data does.
function agoCell(lastSeen) {
  return `<td data-label="Last seen" class="ago" data-ts="${lastSeen}">${agoText(lastSeen)}</td>`;
}

function updateAgoCells() {
  document.querySelectorAll('td.ago').forEach((td) => {
    const text = agoText(Number(td.dataset.ts));
    if (td.textContent !== text) td.textContent = text;
  });
}

function setRows(body, html) {
  if (body.dataset.html === html) return;
  body.innerHTML = html;
  body.dataset.html = html;
}

const CHEVRON = '<svg class="chevron" viewBox="0 0 16 16"><path d="M6 3l5 5-5 5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>';

function stationName(rec) {
  const hw = state.hwinfo.get(rec.id);
  if (rec.name) return rec.name;
  if (hw && hw.sensor_type !== undefined) return SENSOR_TYPE_TEXT[hw.sensor_type] || 'Sensor';
  return 'Unnamed';
}

function stationDistance(rec) {
  const pos = stationPosition();
  if (!pos || !Number.isFinite(rec.lat) || !rec.lat) return null;
  return distanceKm(pos.lat, pos.lon, rec.lat, rec.lon);
}

let weatherRenderPending = false;
function renderWeather(force) {
  if (weatherRenderPending && !force) return;
  weatherRenderPending = true;
  requestAnimationFrame(() => {
    weatherRenderPending = false;
    const body = $('weatherBody');
    const rows = [...state.weather.values()];
    rows.sort((a, b) => {
      const aa = Date.now() - a.lastSeen < 30 * 60e3 ? 0 : 1;
      const bb = Date.now() - b.lastSeen < 30 * 60e3 ? 0 : 1;
      if (aa !== bb) return aa - bb;
      const da = stationDistance(a);
      const db = stationDistance(b);
      if (da !== null && db !== null) return da - db;
      return a.id.localeCompare(b.id);
    });
    if (!rows.length) {
      setRows(body, '<tr class="empty-row"><td colspan="10">No weather station heard yet.</td></tr>');
      return;
    }
    setRows(body, rows.map((r) => {
      const open = state.open.has(r.id);
      const dist = stationDistance(r);
      const hw = state.hwinfo.get(r.id) || {};
      const charge = Number.isFinite(r.charge) ? r.charge : hw.batt_perc;
      const main = `<tr class="row${open ? ' open' : ''}" data-id="${r.id}">
        <td><div class="name-cell">${CHEVRON}${ageDot(r.lastSeen)}<div><div class="name">${esc(stationName(r))}</div>
          <div class="id">${r.id}</div></div></div></td>
        <td data-label="Wind">${r.hasWeather ? windHtml(r.heading, r.speed) : '-'}</td>
        <td data-label="Gust">${num(r.gust, 0, 'km/h')}</td>
        <td data-label="Temp">${num(r.temp, 1, '°C')}</td>
        <td data-label="Humidity" class="hide-sm">${num(r.humidity, 0, '%')}</td>
        <td data-label="Pressure" class="hide-sm">${num(r.baro, 0, 'hPa')}</td>
        <td data-label="Battery">${num(charge, 0, '%')}</td>
        <td data-label="Distance">${dist === null ? '-' : num(dist, 1, 'km')}</td>
        <td data-label="Signal">${signalHtml(r.rssi, r.snr, false)}</td>
        ${agoCell(r.lastSeen)}
      </tr>`;
      return open ? main + `<tr class="detail"><td colspan="10">${stationDetailHtml(r)}</td></tr>` : main;
    }).join(''));
  });
}

$('weatherBody').addEventListener('click', (e) => {
  const row = e.target.closest('tr.row');
  if (!row || e.target.closest('a')) return;
  const id = row.dataset.id;
  if (state.open.has(id)) state.open.delete(id); else state.open.add(id);
  renderWeather(true);
});

const OTA_OUTCOME_TEXT = {
  success: ['Updated', 'ok'],
  no_update: ['No update needed', ''],
  other_gs: ['Served by another station', ''],
  backend_unreachable: ['Server unreachable', 'warn'],
  backend_error: ['Server error', 'bad'],
  unsupported_proto: ['Unsupported OTA protocol', 'warn'],
  image_too_large: ['Image too large', 'bad'],
  download_failed: ['Download failed', 'bad'],
  fanet_ack_failed: ['Radio send failed', 'bad'],
  start_send_failed: ['Radio send failed', 'bad'],
  start_not_acked: ['Sensor did not answer', 'bad'],
  transfer_failed: ['Transfer aborted', 'bad'],
  finish_send_failed: ['Finish not sent', 'bad'],
  finish_not_ready: ['Finish not confirmed', 'bad'],
  finish_rejected: ['Sensor rejected the image', 'bad'],
};

function kvHtml(pairs) {
  const items = pairs.filter(([, v]) => v !== undefined && v !== null && v !== '' && v !== '-');
  if (!items.length) return '<p class="muted" style="margin:0">Not received yet</p>';
  return `<dl class="kv">${items.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join('')}</dl>`;
}

function stationDetailHtml(r) {
  const hw = state.hwinfo.get(r.id) || {};
  const dist = stationDistance(r);
  const mapLink = Number.isFinite(r.lat) && r.lat
    ? `<a href="https://www.openstreetmap.org/?mlat=${r.lat}&mlon=${r.lon}#map=14/${r.lat}/${r.lon}" target="_blank" rel="noopener">${r.lat.toFixed(4)}, ${r.lon.toFixed(4)}</a>`
    : null;

  const weather = kvHtml([
    ['Wind', r.hasWeather && Number.isFinite(r.heading) ? `${num(r.heading, 0, '°')} · ${num(r.speed, 1, 'km/h')}` : null],
    ['Gust', Number.isFinite(r.gust) ? num(r.gust, 1, 'km/h') : null],
    ['Temperature', Number.isFinite(r.temp) ? num(r.temp, 1, '°C') : null],
    ['Humidity', Number.isFinite(r.humidity) ? num(r.humidity, 0, '%') : null],
    ['Pressure', Number.isFinite(r.baro) ? num(r.baro, 1, 'hPa') : null],
    ['Position', mapLink],
    ['Distance', dist === null ? null : num(dist, 1, 'km')],
  ]);

  let uptime = null;
  if (hw.uptimeMin !== undefined) uptime = durationText(hw.uptimeMin * 60);
  const sensor = kvHtml([
    ['Firmware', hw.version ? esc(hw.version) : null],
    ['Build', hw.buildDate ? esc(hw.buildDate) : null],
    ['Sensor', hw.sensor_type !== undefined ? esc(SENSOR_TYPE_TEXT[hw.sensor_type] || hw.sensor_type) : null],
    ['Hardware', hw.hw_rev ? esc(hw.hw_rev + (hw.hw_forced ? ' (pinned)' : '') + (hw.hw_overruled ? ' (divider)' : '')) : null],
    ['Bootloader', hw.bootloader ? esc(hw.bootloader) : null],
    ['Active slot', hw.ota_slot || null],
    ['Uptime', uptime],
    ['Last HW info', hw.newest ? dateTimeText(hw.newest) : null],
  ]);

  const power = kvHtml([
    ['Battery', hw.vbatt_mv !== undefined ? `${(hw.vbatt_mv / 1000).toFixed(2)} V · ${hw.batt_perc} %`
      : Number.isFinite(r.charge) ? num(r.charge, 0, '%') : null],
    ['Solar charger', hw.pv_state !== undefined ? PV_STATE_TEXT[hw.pv_state] : null],
    ['Awake', hw.awake_class !== undefined ? AWAKE_CLASS_TEXT[hw.awake_class] : null],
    ['Low power below', hw.low_power_v !== undefined ? num(hw.low_power_v, 2, 'V') : null],
    ['Resets', hw.resets !== undefined ? `${hw.resets} (last: ${RESET_CAUSE_TEXT[hw.reset_cause]})` : null],
  ]);

  const radioPairs = [
    ['Signal here', Number.isFinite(r.rssi) ? `${r.rssi} dBm · SNR ${r.snr} dB` : null],
    ['Received', hw.rx_count !== undefined ? String(hw.rx_count) : null],
    ['Forwarded', hw.forward_count !== undefined ? String(hw.forward_count) : null],
    ['Listen before talk', hw.lbt !== undefined ? (hw.lbt ? `on, ${hw.lbt_threshold} dBm (${hw.lbt_counter ?? 0}×)` : 'off') : null],
  ];
  if (hw.repeater_mode !== undefined) {
    radioPairs.push(['Repeater', esc(REPEATER_MODE_TEXT[hw.repeater_mode] +
      (hw.repeater_scanning ? ', scanning' : '') + (hw.repeater_rx_off ? ', RX off (undervoltage)' : ''))]);
    radioPairs.push(['Repeater RX duty', `${hw.rx_duty} %`]);
    (hw.repeater_stations || []).forEach((s) => {
      radioPairs.push([`↳ ${s.id}`, esc(`${s.period ? s.period + ' s' : '?'}, ${s.state}` +
        (s.misses ? `, ${s.misses} missed` : ''))]);
    });
  }
  const radio = kvHtml(radioPairs);

  const history = state.otaHistory.get(r.id) || [];
  const updates = history.length
    ? `<ul class="timeline">${history.slice().reverse().map((e) => {
        const [text, cls] = OTA_OUTCOME_TEXT[e.outcome] || [e.outcome, ''];
        const versions = e.to && e.outcome !== 'no_update' ? `${esc(e.from)} → ${esc(e.to)}` : esc(e.from);
        // The server's wording says nothing more than the tag when there was nothing to do.
        const detail = e.detail && e.outcome !== 'no_update' ? ' · ' + esc(e.detail) : '';
        return `<li title="${esc(e.detail || '')}"><span class="when">${dateTimeText(e.ts)}</span>
          <span class="what"><span class="tag ${cls}">${esc(text)}</span></span>
          <span class="why">${versions}${detail}</span></li>`;
      }).join('')}</ul>`
    : '<p class="muted" style="margin:0">No update request since the station started.</p>';

  return `<div class="detail-grid">
    <div class="detail-box"><h3>Weather</h3>${weather}</div>
    <div class="detail-box"><h3>Sensor</h3>${sensor}</div>
    <div class="detail-box"><h3>Power</h3>${power}</div>
    <div class="detail-box"><h3>Radio</h3>${radio}</div>
    <div class="detail-box"><h3>Firmware updates</h3>${updates}</div>
  </div>`;
}

// ------------------------------------------------------------ live traffic --
function onTracking(t) {
  const id = fanetId(t.vid, t.fanet_id);
  const motion = (v) => (v === '-' || v === undefined ? null : v);
  state.tracking.set(id, {
    id,
    name: t.name || '',
    src: t.src || 'FANET',
    noTrack: !!t.notrack,
    acft: t.acft && t.acft !== '-' ? t.acft : '',
    state: t.state && t.state !== '-' ? t.state : '',
    flying: motion(t.spd) !== null,
    lat: t.lat,
    lon: t.lon,
    alt: motion(t.alt),
    speed: motion(t.spd),
    climb: motion(t.climb),
    heading: motion(t.heading),
    rssi: t.rssi,
    snr: t.snr,
    lastSeen: t.tLastMsg > 1e9 ? t.tLastMsg * 1000 : Date.now(),
  });
  renderTracking();
}

let trackingRenderPending = false;
function renderTracking() {
  if (trackingRenderPending) return;
  trackingRenderPending = true;
  requestAnimationFrame(() => {
    trackingRenderPending = false;
    const pos = stationPosition();
    const rows = [...state.tracking.values()].sort((a, b) => b.lastSeen - a.lastSeen);
    if (!rows.length) {
      setRows($('trackingBody'), '<tr class="empty-row"><td colspan="11">No traffic in the last 5 minutes.</td></tr>');
      return;
    }
    setRows($('trackingBody'), rows.map((t) => {
      const dist = pos && Number.isFinite(t.lat) ? distanceKm(pos.lat, pos.lon, t.lat, t.lon) : null;
      const map = Number.isFinite(t.lat)
        ? `<a href="https://www.openstreetmap.org/?mlat=${t.lat}&mlon=${t.lon}#map=13/${t.lat}/${t.lon}" target="_blank" rel="noopener">${dist === null ? 'map' : num(dist, 1, 'km')}</a>`
        : '-';
      return `<tr class="row" style="cursor:default">
        <td><div class="name-cell"><div><div class="name">${esc(t.name || t.id)}</div>
          <div class="id">${t.id}</div></div></div></td>
        <td data-label="Source"><span class="tag src-${t.src}">${t.src}</span>${t.noTrack ? ' <span class="tag" title="Not forwarded to OGN">no-track</span>' : ''}</td>
        <td data-label="Type">${esc(t.acft || '-')}</td>
        <td data-label="State" class="hide-sm">${esc(t.state || '-')}</td>
        <td data-label="Distance">${map}</td>
        <td data-label="Altitude">${num(t.alt, 0, 'm')}</td>
        <td data-label="Speed">${num(t.speed, 0, 'km/h')}</td>
        <td data-label="Climb">${num(t.climb, 1, 'm/s')}</td>
        <td data-label="Heading" class="hide-sm">${num(t.heading, 0, '°')}</td>
        <td data-label="Signal">${signalHtml(t.rssi, t.snr, t.src !== 'FANET')}</td>
        ${agoCell(t.lastSeen)}
      </tr>`;
    }).join(''));
  });
}

function expireRecords() {
  const now = Date.now();
  state.weather.forEach((w, id) => {
    if (now - w.lastSeen > WEATHER_KEEP_MS) {
      state.weather.delete(id);
      state.open.delete(id);
    }
  });
  state.tracking.forEach((t, id) => { if (now - t.lastSeen > TRACKING_KEEP_MS) state.tracking.delete(id); });
}

// --------------------------------------------------------------- settings --
// Fields edited on the Settings tab; saved together through the save bar.
const SETTINGS_FIELDS = ['deviceName', 'lat', 'lon', 'elevation', 'sendAPRS', 'aprsServer', 'aprsPort',
  'sendBreezedude', 'sendDiagnostics', 'receiveFanet', 'receiveFlarm', 'receiveAdsl', 'flarmDutyProfile', 'batteryPowered',
  'solarPowered', 'sleepScheduleEnabled', 'sleepOffTime', 'sleepOnTime'];
// Saved with save_settings too, but immediately, from the System tab.
const UPDATE_FIELDS = ['autoUpdate', 'updateBranch'];
const WIFI_FIELDS = ['sta_ssid', 'sta_password', 'ap_ssid', 'ap_password', 'keepAP', 'apAutoOff'];
const NUMERIC_FIELDS = new Set(['lat', 'lon', 'elevation', 'aprsPort', 'flarmDutyProfile']);

function readField(id) {
  const el = $(id);
  if (el.type === 'checkbox') return el.checked;
  if (NUMERIC_FIELDS.has(id)) return el.value === '' ? null : Number(el.value);
  return el.value;
}

function writeField(id, value) {
  const el = $(id);
  if (!el || value === undefined) return;
  if (el.type === 'checkbox') el.checked = !!value;
  else el.value = value;
}

function nameValid(name) {
  return /^[A-Za-z0-9]{3,9}$/.test((name || '').trim());
}

function setupComplete(name, lat, lon) {
  return nameValid(name) && name !== 'MyGS' && Number.isFinite(lat) && Number.isFinite(lon) &&
    Math.abs(lat - 47) > 1e-5 && Math.abs(lon - 12) > 1e-5;
}

function onConfig(c) {
  // Do not overwrite what the user is editing - judged against the previous
  // configuration, before this one replaces it (none yet = nothing edited).
  const keepSettings = settingsDirty();
  const keepWifi = wifiDirty();
  // Without a session the station does not send the WiFi passwords.
  ['sta_password', 'ap_password'].forEach((f) => { if (c[f] === undefined) c[f] = ''; });
  state.config = c;
  if (c.authRequired !== undefined) {
    auth.required = c.authRequired;
    applyLock();
  }
  renderChannelOptions();
  $('aprsOffHint').hidden = !!c.sendAPRS;
  if (!keepSettings) SETTINGS_FIELDS.forEach((f) => writeField(f, c[f]));
  UPDATE_FIELDS.forEach((f) => writeField(f, c[f]));
  if (!keepWifi) WIFI_FIELDS.forEach((f) => writeField(f, c[f]));
  setText('hdrName', c.deviceName || 'Groundstation');
  document.title = (c.deviceName ? c.deviceName + ' · ' : '') + 'Breezedude Groundstation';
  updateFormUi();
  updateBranchWarning();
  renderBanners();
  renderBattery();
  renderKpis();
  renderWeather(true);
}

const SETUP_GATED = ['sendAPRS', 'sendBreezedude', 'receiveFlarm'];

function settingsDirty() {
  const c = state.config;
  if (!c) return false;
  // Until name and position are set, updateFormUi unchecks these boxes; a
  // configuration that has them on (e.g. from the Arduino firmware) is no edit.
  const ready = setupComplete(readField('deviceName'), readField('lat'), readField('lon'));
  return SETTINGS_FIELDS.some((f) => {
    if (!ready && SETUP_GATED.includes(f)) return false;
    const v = readField(f);
    return NUMERIC_FIELDS.has(f) ? Math.abs((v ?? NaN) - c[f]) > 1e-6 || (v === null) !== (c[f] === null)
      : v !== c[f];
  });
}

function wifiDirty() {
  const c = state.config;
  return !!c && WIFI_FIELDS.some((f) => readField(f) !== c[f]);
}

// Dependent visibility and the "name and position first" guard.
function updateFormUi() {
  const name = readField('deviceName');
  const ready = setupComplete(name, readField('lat'), readField('lon'));
  $('fieldDeviceName').classList.toggle('invalid', !nameValid(name));
  $('setupHint').hidden = ready;
  SETUP_GATED.forEach((id) => {
    $(id).disabled = !ready;
    if (!ready) $(id).checked = false;
  });
  const aprs = readField('sendAPRS');
  $('aprsServer').disabled = !aprs;
  $('aprsPort').disabled = !aprs;

  const fanet = readField('receiveFanet');
  const fsk = readField('receiveFlarm') || readField('receiveAdsl');
  $('fanetOffWarning').hidden = fanet;
  $('dutyProfileField').hidden = !(fanet && fsk);

  const battery = readField('batteryPowered');
  $('batteryFields').hidden = !battery;
  $('sleepFields').hidden = !readField('sleepScheduleEnabled');
  $('batteryApWarning').hidden = !(battery && readField('keepAP'));

  const dirty = settingsDirty();
  $('saveBar').classList.toggle('visible', dirty || wifiDirty());
  applyLock();
  $('saveSettingsBtn').disabled = dirty && !nameValid(name);
}

function settingsPayload(overrides) {
  const c = state.config || {};
  const data = { cmd: 'save_settings' };
  SETTINGS_FIELDS.concat(UPDATE_FIELDS).forEach((f) => { data[f] = c[f]; });
  Object.assign(data, overrides);
  data.flarmDutyProfile = Number(data.flarmDutyProfile) || 0;
  return data;
}

// One save for the whole Settings tab. WiFi goes last: a changed network
// makes the station reconnect, which may cut this browser off.
function saveSettings() {
  if (settingsDirty()) {
    const values = {};
    SETTINGS_FIELDS.forEach((f) => { values[f] = readField(f); });
    values.deviceName = String(values.deviceName || '').trim();
    send(settingsPayload(values));
  }
  if (wifiDirty()) {
    const data = { cmd: 'save_wifi' };
    WIFI_FIELDS.forEach((f) => { data[f] = readField(f); });
    send(data);
  }
  setTimeout(() => send({ cmd: 'config' }), 400);
}

$('settingsForm').addEventListener('input', (e) => {
  if (SETTINGS_FIELDS.includes(e.target.id) || WIFI_FIELDS.includes(e.target.id)) updateFormUi();
});
$('settingsForm').addEventListener('change', (e) => {
  if (e.target.id === 'keepAP' && e.target.checked) $('apAutoOff').checked = false;
  if (e.target.id === 'apAutoOff' && e.target.checked) $('keepAP').checked = false;
  updateFormUi();
});
$('saveSettingsBtn').addEventListener('click', saveSettings);
$('discardBtn').addEventListener('click', () => {
  if (state.config) SETTINGS_FIELDS.concat(WIFI_FIELDS).forEach((f) => writeField(f, state.config[f]));
  $('wifiScanResults').hidden = true;
  updateFormUi();
});

document.querySelectorAll('.pw-toggle').forEach((btn) => {
  btn.addEventListener('click', () => {
    const input = $(btn.dataset.for);
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    btn.textContent = show ? 'Hide' : 'Show';
  });
});

// A scan takes about 4 s. Connected through the station's own AP, the browser
// can miss the result while the radio visits the other channels - the timeout
// then frees the button for another try.
let scanTimer = null;

function resetScanButton() {
  clearTimeout(scanTimer);
  $('wifiScanBtn').disabled = false;
  $('wifiScanBtn').textContent = 'Scan';
}

$('wifiScanBtn').addEventListener('click', () => {
  $('wifiScanBtn').disabled = true;
  $('wifiScanBtn').textContent = 'Scanning…';
  send({ cmd: 'wifi_scan' });
  clearTimeout(scanTimer);
  scanTimer = setTimeout(() => {
    resetScanButton();
    toast('No scan result arrived - please try again', 'error');
  }, 20000);
});

function onWifiScan(list) {
  resetScanButton();
  const select = $('wifiScanResults');
  if (!list.length) {
    select.hidden = true;
    toast('No WiFi networks found', 'error');
    return;
  }
  select.innerHTML = `<option value="">${list.length} ${list.length === 1 ? 'network' : 'networks'} found - choose one…</option>` + list.map((n) =>
    `<option value="${esc(n.ssid)}">${esc(n.ssid)}${n.open ? ' (open)' : ''} · ${n.rssi} dBm</option>`).join('');
  select.hidden = false;
}

function onWifiScanError(reason) {
  resetScanButton();
  toast('WiFi scan failed: ' + reason, 'error');
}

$('wifiScanResults').addEventListener('change', (e) => {
  if (e.target.value) {
    $('sta_ssid').value = e.target.value;
    if (e.target.value !== (state.config || {}).sta_ssid) $('sta_password').value = '';
    updateFormUi();
    $('sta_password').focus();
  }
});

// ------------------------------------------------------- repeater analysis --
const REPEAT_KIND_TEXT = [
  ['Direct + repeated', 'ok'], ['Direct only', ''], ['Repeated only', 'warn'], ['No flag', ''],
];
const FANET_TYPE_TEXT = { 1: 'Tracking', 2: 'Name', 4: 'Weather', 7: 'Ground', 10: 'HW info' };
// The station keeps the last 120; this page keeps collecting while it is open.
const MAX_REPEAT_EVENTS = 800;
const TIMELINE_TYPE_CLASS = { 1: 't-track', 2: 't-name', 4: 't-weather', 7: 't-track', 10: 't-hw' };
const TIMELINE_HOVER_PX = 14;

function onRepeatSnapshot(data) {
  state.repeatSources = new Map();
  (data.sources || []).forEach(storeRepeatSource);
  // An empty list means a reset or a restarted station. Otherwise what this page
  // collected before the station's oldest packet stays.
  const events = (data.events || []).filter(repeatVisible).reverse();
  const older = events.length ? state.repeatEvents.filter((e) => e.ts < events[events.length - 1].ts) : [];
  // The station keeps receiving while it streams, so the order is not guaranteed.
  state.repeatEvents = events.concat(older).sort((a, b) => b.ts - a.ts).slice(0, MAX_REPEAT_EVENTS);
  renderRepeat();
}

function onRepeatLive(live) {
  if (live.source) storeRepeatSource(live.source);
  if (live.event && repeatVisible(live.event)) {
    // A late copy updates a packet that is already listed.
    const known = state.repeatEvents.findIndex((e) => e.seq === live.event.seq && e.ts === live.event.ts);
    if (known >= 0) {
      state.repeatEvents[known] = live.event;
    } else {
      state.repeatEvents.unshift(live.event);
      state.repeatEvents.length = Math.min(state.repeatEvents.length, MAX_REPEAT_EVENTS);
    }
  }
  renderRepeat();
}

function repeatVisible(e) {
  return !HIDDEN_VENDORS.has(Number(e.vid));
}

function storeRepeatSource(s) {
  if (repeatVisible(s)) state.repeatSources.set(fanetId(s.vid, s.addr), s);
}

function senderCell(id) {
  const weather = state.weather.get(id);
  const tracker = state.tracking.get(id);
  const source = state.repeatSources.get(id);
  const name = (weather && weather.name) || (tracker && tracker.name) || (source && source.name) ||
    (weather ? stationName(weather) : 'Unnamed');
  return `<td><div class="name">${esc(name)}</div><div class="id">${id}</div></td>`;
}

function signalText(rssi, snr) {
  if (!Number.isFinite(rssi)) return '-';
  return `${Math.round(rssi)} dBm` + (Number.isFinite(snr) ? ` · ${snr.toFixed(1)} dB` : '');
}

function delayText(ms) {
  return (ms / 1000).toFixed(1) + ' s';
}

function shortName(id) {
  const weather = state.weather.get(id);
  const source = state.repeatSources.get(id);
  return (weather && weather.name) || (source && source.name) || id;
}

// Who can have repeated this sender: repeaters that report following it, else
// any station that reports having forwarded something.
function repeaterCandidates(id) {
  const all = [...state.hwinfo.entries()].filter(([other]) => other !== id);
  const listed = all.filter(([, hw]) => (hw.repeater_stations || []).some((s) => s.id === id));
  if (listed.length) return listed.map(([other]) => shortName(other));
  return all.filter(([, hw]) => hw.repeater_mode || hw.forward_count).map(([other]) => shortName(other) + '?');
}

function isRepeaterStation(id) {
  const hw = state.hwinfo.get(id);
  return !!hw && !!(hw.repeater_mode || hw.forward_count);
}

function meter(percent) {
  return `<div class="rp-meter"><i style="width:${Math.min(100, percent)}%"></i></div>`;
}

function renderRepeat() {
  const c = state.config;
  $('repeatFskHint').hidden = !(c && (c.receiveFlarm || c.receiveAdsl));
  renderRepeaterCards();
  renderRepeatSources();
  renderRepeatEvents();
  renderTimeline();
}

// A repeater reports every 15 minutes, the first time 5 minutes after boot.
function reportMinutes(hw) {
  if (!hw.uptimeAt || !hw.repeaterReport) return 15;
  const uptimeAtReport = hw.uptimeMin + (hw.repeaterReport / 1000 - hw.uptimeAt) / 60;
  return uptimeAtReport < 12 ? 5 : 15;
}

// The report says how much was forwarded since the previous one and the learned
// weather period of every followed station, which gives the packets to expect.
// HWInfo reports are forwarded too, so a healthy repeater can land above that.
function forwardRate(hw) {
  if (hw.repeater_mode === undefined) return null;
  // A period still being learned can be far off, so no rate from it.
  if ((hw.repeater_stations || []).some((s) => s.state !== 'synced')) return null;
  const expected = (hw.repeater_stations || [])
    .reduce((sum, s) => sum + (s.period ? reportMinutes(hw) * 60 / s.period : 0), 0);
  if (!expected) return null;
  return { expected, percent: Math.round(hw.forwarded / expected * 100) };
}

function followedStationRow(s) {
  const seen = state.repeatSources.get(s.id);
  const heard = seen ? seen.both + seen.direct + seen.repeated : 0;
  const here = heard ? `${Math.round((seen.both + seen.repeated) / heard * 100)} % repeated as seen here` : '';
  const cls = s.state === 'synced' ? 'ok' : s.state === 'resync' ? 'warn' : '';
  const hwRhythm = s.hwState === 'free' ? 'HW info rhythm not learned yet' : `HW info ${s.hwState}`;
  const facts = [s.period ? `every ${s.period} s` : 'period unknown', s.misses ? `${s.misses} missed` : '', here, hwRhythm]
    .filter(Boolean).join(' · ');
  return `<div class="rp-follow"><span class="name">${esc(shortName(s.id))}</span>
    <span class="tag ${cls}">${esc(s.state)}</span><span class="muted">${esc(facts)}</span></div>`;
}

// What the share of receive time means depends on the mode. It is about power,
// not quality: whether packets get through is the forward rate. A scan window of
// 6 of 15 minutes alone gives 40 %.
function rxDutyVerdict(hw) {
  const stations = hw.repeater_stations || [];
  const settling = hw.repeater_scanning || hw.repeater_mode === 2 || stations.some((s) => s.state !== 'synced');
  if (hw.repeater_mode === 1) return ['Classic mode listens all the time', ''];
  if (hw.rx_duty <= 10) return ['Sleeps between packets, as intended', 'ok'];
  if (hw.rx_duty <= 50) return ['Includes a scan window, otherwise it sleeps', ''];
  if (settling) return ['Still learning or scanning, drops once synced', ''];
  return ['Listens more than a synced repeater should', 'warn'];
}

function renderRepeaterCards() {
  const repeaters = [...state.hwinfo.entries()].filter(([id]) => isRepeaterStation(id));
  setRows($('repeaterCards'), repeaters.length ? repeaters.map(([id, hw]) => {
    const reported = hw.repeater_mode !== undefined;
    const rate = forwardRate(hw);
    const [dutyText, dutyClass] = reported ? rxDutyVerdict(hw) : ['', ''];
    const mode = reported ? REPEATER_MODE_TEXT[hw.repeater_mode] + (hw.repeater_scanning ? ', scanning' : '') : 'no report yet';
    const stats = reported ? `<div class="rp-stats">
        <div class="rp-stat" title="Packets forwarded since the previous report, against that time divided by the learned weather period of each followed station. HW info counts as forwarded too.">
          <div class="kpi-label">Forward rate</div>
          <div class="rp-value">${rate ? Math.min(100, rate.percent) + ' %' : '-'}</div>
          ${rate ? meter(rate.percent) : ''}
          <div class="kpi-sub">${rate ? `${hw.forwarded} of about ${Math.round(rate.expected)}` : hw.forwarded} in ${reportMinutes(hw)} min${hw.forward_count ? ` · ${hw.forward_count} since boot` : ''}</div>
        </div>
        <div class="rp-stat" title="Share of the time the receiver was on since the previous report. Lower saves power; whether packets still get through is the forward rate.">
          <div class="kpi-label">RX duty (power)</div>
          <div class="rp-value">${hw.rx_duty} %</div>
          <div class="kpi-sub"><span class="tag ${dutyClass}">${esc(dutyText)}</span></div>
        </div>
      </div>` : `<div class="kpi-sub">${hw.forward_count} forwarded since boot. Repeaters report every 15 minutes.</div>`;
    const follows = (hw.repeater_stations || []).map(followedStationRow).join('');
    return `<div class="rp-card">
      <div class="rp-card-head">
        <div><span class="name">${esc(shortName(id))}</span> <span class="id">${id}</span></div>
        <span class="tag">${esc(mode)}</span>
      </div>
      ${hw.repeater_rx_off ? '<div class="hint warn">RX off (undervoltage)</div>' : ''}
      ${stats}
      ${follows ? `<div class="rp-follows">${follows}</div>` : ''}
      ${hw.repeaterReport ? `<div class="kpi-sub">Last report ${agoText(hw.repeaterReport)}</div>` : ''}
    </div>`;
  }).join('') : '<p class="hint">No repeater heard yet. Repeaters report every 15 minutes.</p>');
}

function repeatShareCell(id, s) {
  const flagged = s.both + s.direct + s.repeated;
  const repeated = s.both + s.repeated;
  if (!flagged) {
    return `<td data-label="Repeat rate"><span class="muted">Not requested</span><div class="muted small">${s.plain} packets without forward flag</div></td>`;
  }
  // Nobody is meant to repeat a repeater, so this is not a rate of zero.
  if (!repeated && isRepeaterStation(id)) {
    return `<td data-label="Repeat rate"><span class="muted">Repeater itself</span><div class="muted small">${s.direct} packets with forward flag</div></td>`;
  }
  const parts = [
    [s.both, 'Direct + repeated', 'both'], [s.repeated, 'Repeated only', 'copy'], [s.direct, 'Direct only', 'direct'],
    [s.plain, 'No flag', 'plain'],
  ].filter(([n]) => n);
  const total = parts.reduce((sum, [n]) => sum + n, 0);
  return `<td data-label="Repeat rate"><div class="rp-share">
    <b>${Math.round(repeated / flagged * 100)} %</b>
    <div class="rp-bar">${parts.map(([n, , cls]) => `<i class="${cls}" style="flex-grow:${n / total}"></i>`).join('')}</div>
  </div><div class="muted small">${parts.map(([n, text]) => `${n} ${text.toLowerCase()}`).join(' · ')}</div></td>`;
}

function renderRepeatSources() {
  const sources = [...state.repeatSources.entries()].sort((a, b) => a[0].localeCompare(b[0]));
  setRows($('repeatSourceBody'), sources.length ? sources.map(([id, s]) => {
    const repeated = s.both + s.repeated;
    const by = repeated ? repeaterCandidates(id).join(' or ') || 'unknown' : '-';
    return `<tr class="row static">${senderCell(id)}
      ${repeatShareCell(id, s)}
      <td data-label="Direct signal">${signalText(s.rd, s.sd)}</td>
      <td data-label="Repeated signal">${signalText(s.rc, s.sc)}</td>
      <td data-label="Delay">${s.dl !== undefined ? `${delayText(s.dl)}<br><span class="muted">${delayText(s.dlMin)} to ${delayText(s.dlMax)}</span>` : '-'}</td>
      <td data-label="Repeated by">${esc(by)}${s.extra ? `<br><span class="muted">${s.extra} extra ${s.extra === 1 ? 'copy' : 'copies'}: another repeater is active too</span>` : ''}</td>
      <td data-label="Last">${consoleTime(s.ts)}</td>
    </tr>`;
  }).join('') : '<tr class="empty-row"><td colspan="7">No FANET packet received since the last reset.</td></tr>');
}

function renderRepeatEvents() {
  if (!document.querySelector('.rt-list').open) return;
  setRows($('repeatEventBody'), state.repeatEvents.length ? state.repeatEvents.slice(0, 100).map((e) => {
    const [text, cls] = REPEAT_KIND_TEXT[e.k] || ['?', ''];
    return `<tr class="row static">${senderCell(fanetId(e.vid, e.addr))}
      <td data-label="Time">${consoleTime(e.ts)}</td>
      <td data-label="Type">${FANET_TYPE_TEXT[e.t] || e.t}</td>
      <td data-label="Reception"><span class="tag ${cls}">${text}${e.n > 1 ? ` ×${e.n}` : ''}</span></td>
      <td data-label="Direct">${signalText(e.rd, e.sd)}</td>
      <td data-label="Repeated">${signalText(e.rc, e.sc)}</td>
      <td data-label="Delay">${e.dl !== undefined ? delayText(e.dl) : '-'}</td>
    </tr>`;
  }).join('') : '<tr class="empty-row"><td colspan="7">Nothing received yet.</td></tr>');
}

// ---------------------------------------------- repeater analysis: timeline --
let timelineView = { from: 0, to: 1 };
let timelineHover = false;

function copyTime(e) {
  return e.ts + (e.dl || 0) / 1000;
}

function medianInterval(events) {
  const counts = {};
  events.forEach((e) => { counts[e.t] = (counts[e.t] || 0) + 1; });
  const type = Object.keys(counts).sort((a, b) => counts[b] - counts[a])[0];
  const times = events.filter((e) => String(e.t) === type).map((e) => e.ts);
  if (times.length < 3) return '';
  const gaps = times.slice(1).map((t, i) => times[i] - t).sort((a, b) => a - b);
  return `${FANET_TYPE_TEXT[type] || type} every ${gaps[gaps.length >> 1]} s`;
}

function timelineRowHtml(id, events, percentAt) {
  const flagged = events.filter((e) => e.k !== 3);
  const copies = events.filter((e) => e.k === 0 || e.k === 2).length;
  // Nobody repeats a repeater's own packets, so that lane would stay empty.
  const copyLane = copies > 0 || (flagged.length > 0 && !isRepeaterStation(id));
  let upper = '';
  let lower = '';
  events.forEach((e) => {
    const cls = TIMELINE_TYPE_CLASS[e.t] || 't-track';
    const mark = (time, extra) => `<i class="rt-mark ${extra}" data-seq="${e.seq}" style="left:${percentAt(time).toFixed(2)}%"></i>`;
    if (e.k === 0 || e.k === 1) upper += mark(e.ts, cls);
    if (e.k === 3) upper += mark(e.ts, `${cls} plain`);
    if (e.k === 0) lower += mark(copyTime(e), cls + (e.n > 1 ? ' multi' : ''));
    if (e.k === 2) lower += mark(e.ts, cls + (e.n > 1 ? ' multi' : ''));
  });
  const rate = flagged.length && copyLane ? `${Math.round(copies / flagged.length * 100)} % repeated` : '';
  const facts = [rate, medianInterval(events)].filter(Boolean).join(' · ');
  const nameCell = senderCell(id).replace(/^<td>|<\/td>$/g, '');
  return `<div class="rt-row"><div class="rt-label">${nameCell}<div class="rt-facts">${esc(facts)}</div></div>
    <div class="rt-plot" data-id="${id}"><div class="rt-lane">${upper}</div>${copyLane ? `<div class="rt-lane copy">${lower}</div>` : ''}</div></div>`;
}

function timelineTickLabel(ts) {
  if (ts > 1e9) return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return '+' + new Date(ts * 1000).toISOString().slice(11, 16);
}

function renderTimeline() {
  if (timelineHover || !$('tab-tools').classList.contains('active')) return;
  const to = Date.now() / 1000 + state.clockOffset;
  const from = to - state.repeatWindowMin * 60;
  timelineView = { from, to };
  const percentAt = (t) => (t - from) / (to - from) * 100;

  const rows = new Map();
  state.repeatEvents.forEach((e) => {
    if (copyTime(e) < from) return;
    const id = fanetId(e.vid, e.addr);
    if (!rows.has(id)) rows.set(id, []);
    rows.get(id).push(e);
  });
  if (!rows.size) {
    $('repeatTimeline').innerHTML = '<p class="hint">No FANET packet in this time range.</p>';
    return;
  }

  const step = { 5: 60, 15: 300, 30: 300, 60: 600 }[state.repeatWindowMin];
  let grid = '';
  let axis = '';
  for (let t = Math.ceil(from / step) * step; t < to; t += step) {
    const at = percentAt(t).toFixed(2);
    grid += `<i style="left:${at}%"></i>`;
    if (at > 4 && at < 96) axis += `<span style="left:${at}%">${timelineTickLabel(t)}</span>`;
  }
  // Before the oldest known packet this page does not know what was received.
  const oldest = state.repeatEvents[state.repeatEvents.length - 1].ts;
  const noData = oldest > from ? percentAt(oldest) : 0;
  const sorted = [...rows.entries()].sort((a, b) => a[0].localeCompare(b[0]));
  $('repeatTimeline').innerHTML = `<div class="rt-area">
      ${noData > 0 ? `<div class="rt-nodata" style="width:${noData.toFixed(2)}%">${noData > 12 ? '<span>no history</span>' : ''}</div>` : ''}
      <div class="rt-grid">${grid}</div>
    </div>
    ${sorted.map(([id, events]) => timelineRowHtml(id, events, percentAt)).join('')}
    <div class="rt-axis">${axis}</div>`;
}

function timelineTipHtml(id, e) {
  const [text, cls] = REPEAT_KIND_TEXT[e.k] || ['?', ''];
  const previous = state.repeatEvents.find((o) => o.ts < e.ts && o.t === e.t && fanetId(o.vid, o.addr) === id);
  const flag = ['Set', 'Set', 'Cleared on the copy, original not heard', 'Not set'][e.k];
  const lines = [
    ['Type', FANET_TYPE_TEXT[e.t] || e.t],
    ['Time', consoleTime(e.ts)],
    ['Forward flag', flag],
    ['Direct', signalText(e.rd, e.sd)],
    ['Repeated', signalText(e.rc, e.sc)],
    ['Delay', e.dl !== undefined ? delayText(e.dl) : '-'],
    ['Since previous', previous ? `${e.ts - previous.ts} s` : '-'],
  ];
  return `<div class="name">${esc(shortName(id))} <span class="id">${id}</span></div>
    <span class="tag ${cls}">${text}${e.n > 1 ? ` ×${e.n}` : ''}</span>
    <dl>${lines.map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join('')}</dl>`;
}

function timelineLeave() {
  timelineHover = false;
  $('repeatTip').hidden = true;
  document.querySelectorAll('.rt-mark.hot').forEach((m) => m.classList.remove('hot'));
}

// Marks are only a few pixels wide, so the nearest one in the row answers.
function timelineMove(ev) {
  const plot = ev.target.closest && ev.target.closest('.rt-plot');
  if (!plot) return timelineLeave();
  timelineHover = true;
  const box = plot.getBoundingClientRect();
  const id = plot.dataset.id;
  const xOf = (t) => (t - timelineView.from) / (timelineView.to - timelineView.from) * box.width;
  const x = ev.clientX - box.left;
  let best = null;
  let bestDist = TIMELINE_HOVER_PX;
  state.repeatEvents.forEach((e) => {
    if (fanetId(e.vid, e.addr) !== id) return;
    const dist = Math.min(Math.abs(xOf(e.ts) - x), Math.abs(xOf(copyTime(e)) - x));
    if (dist < bestDist) {
      best = e;
      bestDist = dist;
    }
  });
  document.querySelectorAll('.rt-mark.hot').forEach((m) => m.classList.remove('hot'));
  const tip = $('repeatTip');
  if (!best) {
    tip.hidden = true;
    return;
  }
  plot.querySelectorAll(`[data-seq="${best.seq}"]`).forEach((m) => m.classList.add('hot'));
  tip.innerHTML = timelineTipHtml(id, best);
  tip.hidden = false;
  const left = Math.min(ev.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
  const below = ev.clientY + 14 + tip.offsetHeight < window.innerHeight;
  tip.style.left = `${Math.max(8, left)}px`;
  tip.style.top = `${below ? ev.clientY + 14 : ev.clientY - tip.offsetHeight - 14}px`;
}

$('repeatTimeline').addEventListener('pointermove', timelineMove);
$('repeatTimeline').addEventListener('pointerdown', timelineMove);
$('repeatTimeline').addEventListener('pointerleave', timelineLeave);

$('repeatWindow').addEventListener('click', (ev) => {
  const min = Number(ev.target.dataset.min);
  if (!min) return;
  state.repeatWindowMin = min;
  $('repeatWindow').querySelectorAll('button').forEach((b) => b.classList.toggle('active', b === ev.target));
  renderTimeline();
});

document.querySelector('.rt-list').addEventListener('toggle', renderRepeatEvents);

$('repeatReset').addEventListener('click', () => send({ cmd: 'repeat_reset' }));

// ------------------------------------------------------- portal reports --
const DIAG_CODE_TEXT = { 204: 'accepted', 400: 'rejected as invalid', 401: 'key rejected', 429: 'too frequent' };
let claimWindow = null;

function renderDiag(d) {
  if (!d) return;
  $('diagRejected').hidden = !d.rejected;
  let text = d.enabled ? 'On' : 'Off';
  if (d.enabled && d.lastTs) {
    const result = d.lastCode > 0 ? DIAG_CODE_TEXT[d.lastCode] || `HTTP ${d.lastCode}`
      : 'HTTPS failed (certificate or connection), report skipped';
    text = `Last report ${agoText(d.lastTs * 1000)}: ${result}`;
  } else if (d.enabled) {
    text = 'On, first report about a minute after the start';
  }
  setText('diagState', text);
  // The server only knows a station that has reported.
  $('claimBtn').hidden = !d.enabled;
  $('claimBtn').disabled = !d.accepted || d.rejected || !unlocked();
  $('claimBtn').title = d.accepted ? '' : 'Possible once a report was accepted';
}

function onClaim(c) {
  $('claimBtn').disabled = false;
  if (c.error) {
    if (claimWindow) claimWindow.close();
    return toast(c.error, 'error');
  }
  $('claimUrl').value = c.url;
  $('claimResult').hidden = false;
  if (claimWindow && !claimWindow.closed) claimWindow.location.href = c.url;
}

$('claimBtn').addEventListener('click', () => {
  // Opened now, while the click still counts as a user action; the link follows.
  claimWindow = window.open('', '_blank');
  $('claimBtn').disabled = true;
  $('claimResult').hidden = true;
  send({ cmd: 'diag_claim' });
});
$('claimCopy').addEventListener('click', () => {
  $('claimUrl').select();
  if (navigator.clipboard) navigator.clipboard.writeText($('claimUrl').value).catch(() => document.execCommand('copy'));
  else document.execCommand('copy');
  toast('Link copied', 'success');
});

// ------------------------------------------------------------ write lock --
// While a password is set, changes need a session, see core/bd_auth.cpp. The
// session token is kept in this browser, so it survives a reload and a restart
// of the station, until it expires after an hour.
const AUTH_KEY = 'bd-auth';
const auth = { required: false, token: '', expiresAt: 0, buttonUntil: 0 };
let authTimer = null;
try {
  const saved = JSON.parse(localStorage.getItem(AUTH_KEY) || '{}');
  if (saved.token && saved.expiresAt > Date.now()) Object.assign(auth, saved);
} catch (e) { /* private mode */ }

function storeAuth() {
  try {
    localStorage.setItem(AUTH_KEY, JSON.stringify({ token: auth.token, expiresAt: auth.expiresAt }));
  } catch (e) { /* private mode */ }
}

function unlocked() {
  return !auth.required || (!!auth.token && auth.expiresAt > Date.now());
}

function onAuth(a) {
  if (a.required !== undefined) auth.required = a.required;
  if (a.token) {
    auth.token = a.token;
    auth.expiresAt = Date.now() + a.expiresIn * 1000;
    auth.buttonUntil = 0;
    toast(a.via === 'button' ? 'Unlocked by the button on the station' : 'Unlocked for 1 hour', 'success');
    closeAuth();
    send({ cmd: 'config' });
  } else if (a.valid === false) {
    auth.token = '';
    auth.expiresAt = 0;
  } else if (a.valid && a.expiresIn) {
    auth.expiresAt = Date.now() + a.expiresIn * 1000;
  }
  storeAuth();
  clearTimeout(authTimer);
  if (auth.token) authTimer = setTimeout(applyLock, Math.max(0, auth.expiresAt - Date.now()) + 500);
  applyLock();
}

function onAuthButton(seconds) {
  auth.buttonUntil = Date.now() + seconds * 1000;
  renderAuthModal();
}

// Everything that changes something, outside the settings form.
const LOCKED_CONTROLS = ['autoUpdate', 'updateBranch', 'checkUpdateBtn', 'fwPickBtn', 'rebootBtn', 'importBtn',
  'repeatReset', 'otaVerbose', 'flarmVerbose', 'adslVerbose'];

function applyLock() {
  const locked = !unlocked();
  const wasLocked = !$('lockedHint').hidden;
  $('lockedHint').hidden = !locked;
  document.querySelectorAll('#settingsForm input, #settingsForm select, #settingsForm button')
    .forEach((el) => {
      if (locked) el.disabled = true;
      else if (el.dataset.lockDisabled) el.disabled = false;
      el.dataset.lockDisabled = locked ? '1' : '';
    });
  LOCKED_CONTROLS.forEach((id) => { if ($(id)) $(id).disabled = locked; });
  if (locked) $('saveBar').classList.remove('visible');
  // Fields that depend on others get their own state back.
  if (wasLocked && !locked) updateFormUi();

  const btn = $('lockBtn');
  btn.hidden = !auth.required;
  btn.classList.toggle('locked', locked);
  const minutes = Math.max(1, Math.round((auth.expiresAt - Date.now()) / 60000));
  btn.textContent = locked ? '🔒 Locked' : `🔓 ${minutes} min`;
  btn.title = locked ? 'Read only - click to unlock' : `Unlocked for ${minutes} more minutes`;

  $('accessState').textContent = auth.required
    ? 'A password is set to protect settings changes. You can change or remove it here. ' +
      'Everyone in the network can still view everything.'
    : 'Currently anyone in the network can change the settings. Set a password to restrict changes - ' +
      'everyone in the network can still view everything.';
  $('setPasswordBtn').textContent = auth.required ? 'Change password' : 'Set password';
  $('removePasswordRow').hidden = !auth.required;
  // Without a password the form is the whole card; with one it folds away.
  const form = $('passwordForm');
  if (form.dataset.required !== String(auth.required)) {
    form.dataset.required = String(auth.required);
    form.open = !auth.required;
  }
  form.classList.toggle('always-open', !auth.required);
  renderAuthModal();
}

function renderAuthModal() {
  const locked = !unlocked();
  $('authLogin').hidden = !locked;
  $('authSession').hidden = locked;
  $('authTitle').textContent = locked ? 'Unlock' : 'Unlocked';
  const left = Math.max(0, Math.round((auth.expiresAt - Date.now()) / 60000));
  $('authSessionText').textContent = `Changes are allowed in this browser for ${left} more minutes.`;
  const buttonLeft = Math.round((auth.buttonUntil - Date.now()) / 1000);
  $('authButtonState').textContent = buttonLeft > 0
    ? `Now press the button on the station (${Math.floor(buttonLeft / 60)}:${String(buttonLeft % 60).padStart(2, '0')} left).`
    : '';
}

function openAuth() {
  $('authPassword').value = '';
  renderAuthModal();
  $('authModal').hidden = false;
  if (!unlocked()) $('authPassword').focus();
}

function closeAuth() {
  $('authModal').hidden = true;
}

setInterval(() => {
  if (!$('authModal').hidden && auth.buttonUntil) renderAuthModal();
}, 1000);
setInterval(() => { if (auth.token) applyLock(); }, 60000);

$('lockBtn').addEventListener('click', openAuth);
$('lockedUnlockBtn').addEventListener('click', openAuth);
$('authClose').addEventListener('click', closeAuth);
$('authModal').addEventListener('click', (e) => { if (e.target === $('authModal')) closeAuth(); });
$('authLoginBtn').addEventListener('click', () => send({ cmd: 'login', password: $('authPassword').value }));
$('authPassword').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') send({ cmd: 'login', password: $('authPassword').value });
});
$('authButtonBtn').addEventListener('click', () => send({ cmd: 'auth_button' }));
$('authLogoutBtn').addEventListener('click', () => {
  send({ cmd: 'logout' });
  closeAuth();
});

$('setPasswordBtn').addEventListener('click', () => {
  const password = $('newPassword').value;
  if (!password) return toast('Enter a password', 'error');
  if (password !== $('newPassword2').value) return toast('The two passwords differ', 'error');
  send({ cmd: 'set_password', password });
  $('newPassword').value = '';
  $('newPassword2').value = '';
});
$('removePasswordBtn').addEventListener('click', () => {
  if (confirm('Remove the password? Everyone in the network can then change the settings.')) {
    send({ cmd: 'set_password', password: '' });
  }
});

// ------------------------------------------------------------ location map --
// MapLibre GL and the label glyphs come from the Breezedude server, the vector
// tiles from OpenFreeMap and the terrain from the AWS Terrain Tiles. Nothing is
// requested before the user opens the map; without internet in this browser
// the button just reports that.
const MAP_LIB = 'http://fanet.breezedude.de/gs/lib';
const MAPLIBRE_JS = `${MAP_LIB}/maplibre-gl-5.24.0/maplibre-gl.js`;
const MAPLIBRE_JS_SRI = 'sha384-5+cfbwT0iiub6VsQAdn6yz16nr6sDiQoHx6tm4O8OVYXHYOxcffFmCJBL0dgdvGp';
const MAPLIBRE_CSS = `${MAP_LIB}/maplibre-gl-5.24.0/maplibre-gl.css`;
const MAPLIBRE_CSS_SRI = 'sha384-uTttxo/aOKbdE5RlD/SPzSDoDmNvGlUYPjONi2MN/b7c9HPSvW07OIuyP7uL6jxK';
const TERRAIN_TILES = 'https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png';
const SATELLITE_TILES = 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}';

let mapLibPromise = null;
let locMap = null;
let locMarker = null;
let mapBase = 'map';  // 'map' or 'sat'

function loadMapLibre() {
  if (window.maplibregl) return Promise.resolve();
  if (mapLibPromise) return mapLibPromise;
  mapLibPromise = new Promise((resolve, reject) => {
    const css = document.createElement('link');
    css.rel = 'stylesheet';
    css.href = MAPLIBRE_CSS;
    css.integrity = MAPLIBRE_CSS_SRI;
    css.crossOrigin = 'anonymous';
    document.head.appendChild(css);
    const script = document.createElement('script');
    script.src = MAPLIBRE_JS;
    script.integrity = MAPLIBRE_JS_SRI;
    script.crossOrigin = 'anonymous';
    const timer = setTimeout(() => reject(new Error('timeout')), 15000);
    script.onload = () => { clearTimeout(timer); resolve(); };
    script.onerror = () => { clearTimeout(timer); reject(new Error('load failed')); };
    document.head.appendChild(script);
  }).catch((err) => {
    mapLibPromise = null;
    throw err;
  });
  return mapLibPromise;
}

const nameExpr = ['coalesce', ['get', 'name:de'], ['get', 'name:latin'], ['get', 'name']];

function mapStyle() {
  const sat = mapBase === 'sat';
  const vis = (show) => ({ visibility: show ? 'visible' : 'none' });
  const dark = sat || document.documentElement.getAttribute('data-theme') === 'dark';
  const c = dark
    ? { bg: '#20272f', wood: '#26332b', grass: '#28302a', ice: '#2c3540', water: '#1d3447', road: '#56606c',
        major: '#8a7a55', text: '#d9e0e7', halo: '#1b2129', border: '#7d6a8a' }
    : { bg: '#f4f1ea', wood: '#d6e6c8', grass: '#e3edd2', ice: '#eef4f8', water: '#a9cbe6', road: '#ffffff',
        major: '#f2cf82', text: '#2a3440', halo: '#ffffff', border: '#9b7fae' };
  return {
    version: 8,
    glyphs: `${MAP_LIB}/fonts/{fontstack}/{range}.pbf`,
    sources: {
      omt: { type: 'vector', url: 'https://tiles.openfreemap.org/planet' },
      dem: {
        type: 'raster-dem', tiles: [TERRAIN_TILES], tileSize: 256, maxzoom: 15, encoding: 'terrarium',
        attribution: 'Terrain: <a href="https://github.com/tilezen/joerd" target="_blank" rel="noopener">AWS Terrain Tiles</a>',
      },
      sat: {
        type: 'raster', tiles: [SATELLITE_TILES], tileSize: 256, maxzoom: 19,
        attribution: 'Imagery © Esri, Maxar, Earthstar Geographics',
      },
    },
    layers: [
      { id: 'bg', type: 'background', paint: { 'background-color': c.bg } },
      { id: 'sat', type: 'raster', source: 'sat', layout: vis(sat) },
      { id: 'wood', type: 'fill', source: 'omt', 'source-layer': 'landcover',
        filter: ['==', ['get', 'class'], 'wood'], layout: vis(!sat), paint: { 'fill-color': c.wood } },
      { id: 'grass', type: 'fill', source: 'omt', 'source-layer': 'landcover',
        filter: ['in', ['get', 'class'], ['literal', ['grass', 'farmland']]], layout: vis(!sat),
        paint: { 'fill-color': c.grass } },
      { id: 'ice', type: 'fill', source: 'omt', 'source-layer': 'landcover',
        filter: ['in', ['get', 'class'], ['literal', ['ice', 'rock']]], layout: vis(!sat),
        paint: { 'fill-color': c.ice } },
      { id: 'hillshade', type: 'hillshade', source: 'dem', layout: vis(!sat),
        paint: { 'hillshade-exaggeration': 0.35, 'hillshade-shadow-color': dark ? '#000000' : '#5a5a5a' } },
      { id: 'water', type: 'fill', source: 'omt', 'source-layer': 'water', layout: vis(!sat),
        paint: { 'fill-color': c.water } },
      { id: 'river', type: 'line', source: 'omt', 'source-layer': 'waterway',
        paint: { 'line-color': c.water, 'line-width': ['interpolate', ['linear'], ['zoom'], 8, 0.5, 14, 2] } },
      { id: 'roads', type: 'line', source: 'omt', 'source-layer': 'transportation',
        filter: ['in', ['get', 'class'], ['literal', ['minor', 'tertiary', 'secondary', 'service', 'track']]],
        minzoom: 11, layout: vis(!sat), paint: { 'line-color': c.road, 'line-width': ['interpolate', ['linear'], ['zoom'], 11, 0.5, 16, 4] } },
      { id: 'roads-major', type: 'line', source: 'omt', 'source-layer': 'transportation',
        filter: ['in', ['get', 'class'], ['literal', ['motorway', 'trunk', 'primary']]],
        paint: { 'line-color': c.major, 'line-width': ['interpolate', ['linear'], ['zoom'], 6, 0.6, 16, 6] } },
      { id: 'border', type: 'line', source: 'omt', 'source-layer': 'boundary',
        filter: ['<=', ['get', 'admin_level'], 2], paint: { 'line-color': c.border, 'line-dasharray': [3, 2] } },
      { id: 'peaks', type: 'symbol', source: 'omt', 'source-layer': 'mountain_peak', minzoom: 11,
        layout: { 'text-field': ['concat', nameExpr, '\n', ['to-string', ['get', 'ele']], ' m'],
          'text-font': ['Noto Sans Regular'], 'text-size': 11 },
        paint: { 'text-color': c.text, 'text-halo-color': c.halo, 'text-halo-width': 1.2 } },
      { id: 'places', type: 'symbol', source: 'omt', 'source-layer': 'place',
        filter: ['in', ['get', 'class'], ['literal', ['city', 'town', 'village', 'hamlet']]],
        layout: { 'text-field': nameExpr, 'text-font': ['Noto Sans Regular'],
          'text-size': ['match', ['get', 'class'], 'city', 15, 'town', 13, 11] },
        paint: { 'text-color': c.text, 'text-halo-color': c.halo, 'text-halo-width': 1.5 } },
    ],
  };
}

// Elevation from one terrain tile, bilinear between the four nearest samples.
// Terrarium encoding: (R * 256 + G + B / 256) - 32768 metres.
const terrainTiles = new Map();
function terrainTile(z, x, y) {
  const key = `${z}/${x}/${y}`;
  if (!terrainTiles.has(key)) {
    terrainTiles.set(key, new Promise((resolve, reject) => {
      const img = new Image();
      img.crossOrigin = 'anonymous';
      img.onload = () => {
        const canvas = document.createElement('canvas');
        canvas.width = 256;
        canvas.height = 256;
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        ctx.drawImage(img, 0, 0);
        resolve(ctx.getImageData(0, 0, 256, 256).data);
      };
      img.onerror = () => {
        terrainTiles.delete(key);
        reject(new Error('terrain tile failed'));
      };
      img.src = TERRAIN_TILES.replace('{z}', z).replace('{x}', x).replace('{y}', y);
    }));
  }
  return terrainTiles.get(key);
}

async function terrainElevation(lat, lon) {
  const z = 14;
  const n = 2 ** z;
  const rad = (lat * Math.PI) / 180;
  const xf = ((lon + 180) / 360) * n;
  const yf = ((1 - Math.log(Math.tan(rad) + 1 / Math.cos(rad)) / Math.PI) / 2) * n;
  const tx = Math.floor(xf);
  const ty = Math.floor(yf);
  const data = await terrainTile(z, tx, ty);
  const px = Math.min(Math.max((xf - tx) * 256 - 0.5, 0), 254.999);
  const py = Math.min(Math.max((yf - ty) * 256 - 0.5, 0), 254.999);
  const x0 = Math.floor(px);
  const y0 = Math.floor(py);
  const at = (x, y) => {
    const i = (y * 256 + x) * 4;
    return data[i] * 256 + data[i + 1] + data[i + 2] / 256 - 32768;
  };
  const fx = px - x0;
  const fy = py - y0;
  const top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
  const bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1 - fy) + bottom * fy;
}

// The dialog edits a pending position; only "Apply" writes it to the form.
let mapPending = null;  // {lat, lon, ele|null}
let mapElevationSeq = 0;

function renderMapReadout() {
  const p = mapPending;
  $('mapReadout').textContent = p
    ? `${p.lat.toFixed(5)}, ${p.lon.toFixed(5)} · ${p.ele === null ? '… m' : Math.round(p.ele) + ' m'}`
    : 'Click the map to place the station';
  $('mapApply').disabled = !p || p.ele === null;
}

async function pickLocation(lngLat) {
  mapPending = { lat: lngLat.lat, lon: lngLat.lng, ele: null };
  renderMapReadout();
  const seq = ++mapElevationSeq;
  let ele = NaN;
  try {
    ele = await terrainElevation(lngLat.lat, lngLat.lng);
  } catch (e) {
    toast('Elevation not available - enter it by hand after applying', 'error');
  }
  if (seq !== mapElevationSeq) return;  // a newer pick is underway
  mapPending.ele = Number.isFinite(ele) ? ele : readField('elevation');
  renderMapReadout();
}

function closeLocationMap() {
  $('mapModal').hidden = true;
  document.removeEventListener('keydown', onMapKey);
}

function onMapKey(e) {
  if (e.key === 'Escape') closeLocationMap();
}

function setMapBase(base) {
  mapBase = base;
  document.querySelectorAll('#mapBase button').forEach((b) => b.classList.toggle('active', b.dataset.base === base));
  if (locMap) locMap.setStyle(mapStyle());
}

async function openLocationMap() {
  const btn = $('mapBtn');
  btn.disabled = true;
  btn.textContent = 'Loading map…';
  try {
    await loadMapLibre();
  } catch (e) {
    toast('The map needs internet access in this browser', 'error');
    return;
  } finally {
    btn.disabled = false;
    btn.textContent = 'Choose on map';
  }

  const lat = readField('lat');
  const lon = readField('lon');
  const known = setupComplete('valid', lat, lon);
  mapPending = known ? { lat, lon, ele: readField('elevation') } : null;
  $('mapModal').hidden = false;
  document.addEventListener('keydown', onMapKey);
  renderMapReadout();

  const center = known ? [lon, lat] : [11.5, 47.5];
  if (!locMap) {
    locMap = new maplibregl.Map({
      container: 'locMap',
      style: mapStyle(),
      center,
      zoom: known ? 14 : 7,
      attributionControl: { compact: true },
    });
    locMap.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');
    locMarker = new maplibregl.Marker({ draggable: true, color: cssVar('--accent') }).setLngLat(center);
    locMarker.on('dragend', () => pickLocation(locMarker.getLngLat()));
    locMap.on('click', (e) => {
      locMarker.setLngLat(e.lngLat).addTo(locMap);
      pickLocation(e.lngLat);
    });
  } else {
    locMap.resize();
    locMap.jumpTo({ center, zoom: known ? Math.max(locMap.getZoom(), 12) : 7 });
  }
  if (known) locMarker.setLngLat(center).addTo(locMap); else locMarker.remove();
}

$('mapBtn').addEventListener('click', openLocationMap);
$('mapCancel').addEventListener('click', closeLocationMap);
$('mapModal').addEventListener('click', (e) => { if (e.target === $('mapModal')) closeLocationMap(); });
$('mapApply').addEventListener('click', () => {
  if (!mapPending) return;
  $('lat').value = mapPending.lat.toFixed(5);
  $('lon').value = mapPending.lon.toFixed(5);
  if (Number.isFinite(mapPending.ele)) $('elevation').value = Math.round(mapPending.ele);
  updateFormUi();
  closeLocationMap();
});
document.querySelectorAll('#mapBase button').forEach((b) => b.addEventListener('click', () => setMapBase(b.dataset.base)));

// ---------------------------------------------------------- system: update --
// The channels come from the backend's manifest via the station. Until the
// station has fetched it once, stable and beta are offered. A selected channel
// the manifest no longer lists stays selectable so the setting is not lost.
const FALLBACK_CHANNELS = [{ name: 'stable' }, { name: 'beta' }];

function onUpdateChannels(list) {
  state.channels = list.filter((c) => c && typeof c.name === 'string');
  renderChannelOptions();
  updateBranchWarning();
}

function channelList() {
  const list = state.channels.length ? [...state.channels] : [...FALLBACK_CHANNELS];
  const current = state.config && state.config.updateBranch;
  if (current && !list.some((c) => c.name === current)) list.push({ name: current, missing: true });
  return list;
}

function renderChannelOptions() {
  const select = $('updateBranch');
  const value = (state.config && state.config.updateBranch) || select.value;
  const html = channelList().map((c) => {
    const label = c.name + (c.version ? ` · ${c.version}` : '') + (c.missing ? ' (not offered)' : '');
    return `<option value="${esc(c.name)}">${esc(label)}</option>`;
  }).join('');
  if (select.innerHTML !== html) select.innerHTML = html;
  select.value = value;
}

function updateBranchWarning() {
  const branch = $('updateBranch').value;
  const channel = channelList().find((c) => c.name === branch);
  const text = !channel ? ''
    : channel.missing ? 'No release is published on this channel at the moment.'
    : channel.description || '';
  const el = $('branchWarning');
  el.textContent = text;
  el.hidden = !text;
}

UPDATE_FIELDS.forEach((f) => $(f).addEventListener('change', () => {
  updateBranchWarning();
  send(settingsPayload({ [f]: readField(f) }));
  setTimeout(() => send({ cmd: 'config' }), 400);
}));

$('checkUpdateBtn').addEventListener('click', () => send({ cmd: 'check_update' }));

$('fwPickBtn').addEventListener('click', () => $('fwFile').click());
$('fwFile').addEventListener('change', () => {
  const file = $('fwFile').files[0];
  if (!file) return;
  $('fwFileName').textContent = file.name;
  const form = new FormData();
  form.append('file', file);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  if (auth.token) xhr.setRequestHeader('X-Auth', auth.token);
  $('fwProgressWrap').hidden = false;
  $('fwStatus').textContent = 'Uploading…';
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) $('fwProgress').style.width = `${(e.loaded / e.total) * 100}%`;
  };
  xhr.onload = () => {
    $('fwStatus').textContent = xhr.responseText || `Done (HTTP ${xhr.status})`;
    $('fwFile').value = '';
  };
  xhr.onerror = () => {
    $('fwStatus').textContent = 'Upload failed - the station may be restarting.';
  };
  xhr.send(form);
});

$('rebootBtn').addEventListener('click', () => {
  if (confirm('Restart the station now?')) send({ cmd: 'reboot' });
});

// ------------------------------------------------------- system: backup --
$('exportBtn').addEventListener('click', () => {
  const c = state.config;
  if (!c) return toast('No configuration loaded yet', 'error');
  const data = {};
  SETTINGS_FIELDS.concat(UPDATE_FIELDS, WIFI_FIELDS).forEach((f) => { data[f] = c[f]; });
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  const stamp = new Date().toISOString().slice(0, 16).replace('T', '_').replace(/:/g, '-');
  a.href = URL.createObjectURL(blob);
  a.download = `${(c.deviceName || 'groundstation').replace(/[^A-Za-z0-9_-]/g, '_')}_${stamp}.json`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(a.href);
});

$('importBtn').addEventListener('click', () => $('importFile').click());
$('importFile').addEventListener('change', (event) => {
  const file = event.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    let data;
    try { data = JSON.parse(reader.result); } catch (e) {
      $('importStatus').textContent = 'Not a valid configuration file.';
      return;
    }
    if (!confirm('Write this configuration to the station? WiFi settings are applied immediately.')) return;
    const settings = {};
    SETTINGS_FIELDS.concat(UPDATE_FIELDS).forEach((f) => { if (data[f] !== undefined) settings[f] = data[f]; });
    send(settingsPayload(settings));
    if (WIFI_FIELDS.some((f) => data[f] !== undefined)) {
      const wifi = { cmd: 'save_wifi' };
      WIFI_FIELDS.forEach((f) => { wifi[f] = data[f] !== undefined ? data[f] : (state.config || {})[f]; });
      send(wifi);
    }
    $('importStatus').textContent = 'Configuration written.';
    $('importFile').value = '';
    setTimeout(() => send({ cmd: 'config' }), 600);
  };
  reader.readAsText(file);
});

// --------------------------------------------------------------- consoles --
const MAX_CONSOLE_LINES = 1000;

function consoleTime(ts) {
  if (ts && ts > 1e9) return new Date(ts * 1000).toLocaleTimeString();
  if (ts) return '+' + new Date(ts * 1000).toISOString().slice(11, 19);
  return new Date().toLocaleTimeString();
}

function setConsole(el, lines) {
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
  el.value = lines.slice(-MAX_CONSOLE_LINES).join('\n') + (lines.length ? '\n' : '');
  if (atBottom) el.scrollTop = el.scrollHeight;
}

function appendConsole(el, text) {
  const incoming = String(text).replace(/\r/g, '').split('\n').filter((l) => l.trim());
  if (!incoming.length) return;
  setConsole(el, el.value.split('\n').filter((l) => l).concat(incoming));
}

$('clearSystemConsole').addEventListener('click', () => { $('systemConsole').value = ''; });
$('clearAprsConsole').addEventListener('click', () => { $('aprsConsole').value = ''; });

['ota', 'flarm', 'adsl'].forEach((p) => {
  $(p + 'Verbose').addEventListener('change', (e) => send({ cmd: `set_${p}_verbose`, verbose: e.target.checked }));
});

// ------------------------------------------------- sensor OTA live monitor --
// Per-chunk transfer statistics from the LoRa OTA gateway, drawn as they
// arrive. Upper half: SNR at the station for each chunk's ACK; lower half
// (mirrored): SNR at the sensor for the chunk itself; the line is the sensor's
// RSSI. The small chart shows retries (red bar = chunk that aborted) and the
// sensor's ACK resends.
const OTA_RETRY_ABORT = 15;
const otaLive = { active: false, total: 0, chunks: [], hideTimer: null };

function otaCanvas(id) {
  const canvas = $(id);
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0) return null;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(rect.width * dpr);
  canvas.height = Math.round(rect.height * dpr);
  return canvas;
}

function range(values) {
  const v = values.filter(Number.isFinite);
  if (!v.length) return null;
  const lo = Math.min(...v);
  const hi = Math.max(...v);
  return hi > lo ? [lo, hi] : [lo - 1, hi + 1];
}

function otaLiveDraw() {
  if ($('otaLive').hidden) return;
  const chunks = otaLive.chunks;
  const total = Math.max(otaLive.total, chunks.length, 1);

  const main = otaCanvas('otaLiveCanvas');
  if (main) {
    const ctx = main.getContext('2d');
    const w = main.width;
    const h = main.height;
    const mid = h / 2;
    const bw = Math.max(w / total, 1);
    const snr = range(chunks.map((c) => c && c.snr));
    const dsnr = range(chunks.map((c) => c && c.deviceSnr));
    const rssi = range(chunks.map((c) => c && c.deviceRssi));
    ctx.clearRect(0, 0, w, h);
    chunks.forEach((c, i) => {
      if (!c) return;
      const x = (i * w) / total;
      if (snr && Number.isFinite(c.snr)) {
        const bh = ((c.snr - snr[0]) / (snr[1] - snr[0])) * mid * 0.95 + mid * 0.05;
        ctx.fillStyle = cssVar('--c-fanet');
        ctx.fillRect(x, mid - bh, bw, bh);
      }
      if (dsnr && Number.isFinite(c.deviceSnr)) {
        const bh = ((c.deviceSnr - dsnr[0]) / (dsnr[1] - dsnr[0])) * mid * 0.95 + mid * 0.05;
        ctx.fillStyle = cssVar('--c-snr');
        ctx.fillRect(x, mid, bw, bh);
      }
    });
    if (rssi) {
      ctx.strokeStyle = cssVar('--warn');
      ctx.lineWidth = 1.5 * (window.devicePixelRatio || 1);
      ctx.beginPath();
      let pen = false;
      chunks.forEach((c, i) => {
        if (!c || !Number.isFinite(c.deviceRssi)) return;
        const x = ((i + 0.5) * w) / total;
        const y = h - ((c.deviceRssi - rssi[0]) / (rssi[1] - rssi[0])) * h;
        if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y);
        pen = true;
      });
      ctx.stroke();
    }
  }

  const retry = otaCanvas('otaLiveRetryCanvas');
  if (retry) {
    const ctx = retry.getContext('2d');
    const w = retry.width;
    const h = retry.height;
    const bw = Math.max(w / total, 1);
    const maxRetry = Math.max(1, ...chunks.map((c) => (c && c.retries < OTA_RETRY_ABORT ? c.retries : 0)));
    const maxAck = Math.max(1, ...chunks.map((c) => (c ? c.ackResends : 0)));
    ctx.clearRect(0, 0, w, h);
    chunks.forEach((c, i) => {
      if (!c) return;
      const x = (i * w) / total;
      if (c.retries >= OTA_RETRY_ABORT) {
        ctx.fillStyle = cssVar('--bad');
        ctx.fillRect(x, 0, bw, h);
      } else if (c.retries > 0) {
        ctx.fillStyle = cssVar('--c-flarm');
        const bh = (c.retries / maxRetry) * h;
        ctx.fillRect(x, h - bh, bw, bh);
      }
      if (c.ackResends > 0) {
        ctx.fillStyle = cssVar('--c-adsl');
        const bh = (c.ackResends / maxAck) * h;
        ctx.fillRect(x + bw * 0.6, h - bh, Math.max(bw * 0.4, 1), bh);
      }
    });
  }
}

function onOtaProgress(p) {
  if (p.phase === 'start') {
    clearTimeout(otaLive.hideTimer);
    otaLive.active = true;
    otaLive.total = Number(p.total) || 0;
    otaLive.chunks = [];
    $('otaLive').hidden = false;
    $('otaLiveIdle').textContent = 'Transfer running';
    $('otaLiveStatus').textContent = 'Transferring…';
    ['otaLiveSnr', 'otaLiveDeviceSnr', 'otaLiveDeviceRssi', 'otaLiveRetries', 'otaLiveAckResends']
      .forEach((id) => $(id).classList.remove('ok', 'bad'));
  } else if (p.phase === 'chunk') {
    if (!otaLive.active) {
      otaLive.active = true;
      $('otaLive').hidden = false;
    }
    const seq = Number(p.seq) || 0;
    const c = {
      retries: Number(p.retries) || 0,
      snr: Number(p.snr),
      ackResends: Number(p.ackResends) || 0,
      proactive: Number(p.proactiveAckResends) || 0,
      deviceSnr: Number(p.deviceSnr),
      deviceRssi: Number(p.deviceRssi),
    };
    otaLive.chunks[seq] = c;
    const total = otaLive.total || seq + 1;
    setText('otaLiveProgress', `${seq + 1} / ${total}`);
    $('otaLiveBar').style.width = `${((seq + 1) / total) * 100}%`;
    if (Number.isFinite(c.snr)) setText('otaLiveSnr', `SNR GS ${c.snr.toFixed(1)} dB`);
    if (Number.isFinite(c.deviceSnr)) setText('otaLiveDeviceSnr', `SNR sensor ${c.deviceSnr.toFixed(1)} dB`);
    if (Number.isFinite(c.deviceRssi)) setText('otaLiveDeviceRssi', `RSSI sensor ${c.deviceRssi.toFixed(0)} dBm`);
    setText('otaLiveRetries', c.retries >= OTA_RETRY_ABORT ? 'Retries: aborted' : `Retries ${c.retries}`);
    setText('otaLiveAckResends', `ACK resends ${c.ackResends}` + (c.proactive ? ` (${c.proactive} proactive)` : ''));
    if ($('tab-tools').classList.contains('active')) otaLiveDraw();
  } else if (p.phase === 'end') {
    otaLive.active = false;
    $('otaLiveIdle').textContent = p.success ? 'Last transfer completed' : 'Last transfer failed';
    $('otaLiveStatus').textContent = p.success ? 'Transfer completed - the sensor verifies and installs the image.'
      : 'Transfer failed.';
    otaLiveDraw();
  }
  renderBanners();
}

// ------------------------------------------------------------------- start --
showTab();
updateFormUi();
renderWeather();
renderTracking();
connect();
