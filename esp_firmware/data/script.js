var ESP_DAEMON_PLACEHOLDER_MAC = '00-00-00-00-00-00';

function normalizeMacDisplay(mac) {
  return String(mac).replace(/:/g, '-').toUpperCase();
}
var ESP_DAEMON_PLACEHOLDER_VERSION = '0.0.0';

/** Default when device has no valid controlPanelUrl (matches firmware default). */
var CONTROL_PANEL_DEFAULT_URL = 'https://scx.tw/links';

/** Target for Control Panel FAB short press; updated from GET /device. */
var controlPanelShortcutUrl = CONTROL_PANEL_DEFAULT_URL;

function sanitizeControlPanelUrlClient(raw) {
  var s = String(raw == null ? '' : raw).trim();
  if (!s) {
    return CONTROL_PANEL_DEFAULT_URL;
  }
  if (s.length > 192) {
    return CONTROL_PANEL_DEFAULT_URL;
  }
  if (s.indexOf('//') === 0) {
    return CONTROL_PANEL_DEFAULT_URL;
  }
  if (s.indexOf('http://') === 0 || s.indexOf('https://') === 0) {
    return s;
  }
  if (s.indexOf('/') === 0) {
    return s;
  }
  return CONTROL_PANEL_DEFAULT_URL;
}

function closeSpeedDial() {
  const root = document.getElementById('speedDial');
  const toggle = document.getElementById('speedDialToggle');
  if (!root) {
    return;
  }
  root.classList.remove('speed-dial--open');
  if (toggle) {
    toggle.setAttribute('aria-expanded', 'false');
    toggle.setAttribute('aria-label', 'Open menu');
  }
  root.querySelectorAll('.speed-dial__action').forEach(function(btn) {
    btn.setAttribute('tabindex', '-1');
  });
}

// Dark mode toggle functionality
document.addEventListener('DOMContentLoaded', function() {
  const modeToggle = document.getElementById('modeToggle');
  const settingsEntry = document.getElementById('settingsEntry');

  function initSpeedDial() {
    const root = document.getElementById('speedDial');
    const toggle = document.getElementById('speedDialToggle');
    const backdrop = document.getElementById('speedDialBackdrop');
    if (!root || !toggle) {
      return;
    }

    root.querySelectorAll('.speed-dial__action').forEach(function(btn) {
      btn.setAttribute('tabindex', '-1');
    });

    toggle.addEventListener('click', function(e) {
      e.stopPropagation();
      const open = !root.classList.contains('speed-dial--open');
      root.classList.toggle('speed-dial--open', open);
      toggle.setAttribute('aria-expanded', open);
      toggle.setAttribute('aria-label', open ? 'Close menu' : 'Open menu');
      root.querySelectorAll('.speed-dial__action').forEach(function(btn) {
        if (open) {
          btn.removeAttribute('tabindex');
        } else {
          btn.setAttribute('tabindex', '-1');
        }
      });
    });

    if (backdrop) {
      backdrop.addEventListener('click', closeSpeedDial);
    }

    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') {
        closeSpeedDial();
      }
    });
  }

  initSpeedDial();

  function updateThemeButtonMeta(isLightMode) {
    if (!modeToggle) {
      return;
    }
    const nextMode = isLightMode ? 'dark' : 'light';
    modeToggle.setAttribute('aria-label', 'Switch to ' + nextMode + ' mode');
  }
  
  // Check for saved theme preference or use default (dark)
  const currentTheme = localStorage.getItem('theme') || 'dark';
  
  // Apply saved theme on page load
  if (currentTheme === 'light') {
    document.documentElement.classList.add('light-mode');
    updateThemeButtonMeta(true);
    updateChartTheme();
  } else {
    updateThemeButtonMeta(false);
    updateChartTheme();
  }
  
  // Toggle between light/dark modes
  if (modeToggle) {
    modeToggle.addEventListener('click', function() {
      const isLightMode = document.documentElement.classList.toggle('light-mode');

      if (isLightMode) {
        updateThemeButtonMeta(true);
        localStorage.setItem('theme', 'light');
        updateChartTheme();
      } else {
        updateThemeButtonMeta(false);
        localStorage.setItem('theme', 'dark');
        updateChartTheme();
      }
      closeSpeedDial();
    });
  }

  if (settingsEntry) {
    settingsEntry.addEventListener('click', function() {
      closeSpeedDial();
      window.location.href = '/settings.html';
    });
  }

  var btnJson = document.getElementById('telemetryDownloadJson');
  var btnCsv = document.getElementById('telemetryDownloadCsv');
  if (btnJson) {
    btnJson.addEventListener('click', triggerTelemetryDownloadJson);
  }
  if (btnCsv) {
    btnCsv.addEventListener('click', triggerTelemetryDownloadCsv);
  }
});

// Battery discharge session voltage chart (canvas, SPIFFS). Data from GET /telemetry.
var lastTelemetryPayload = null;
var lastTelemetryFullPayload = null;
var telemetryRequestInFlight = false;
var readingsRequestInFlight = false;

function clampNumber(value, minValue, maxValue) {
  return Math.max(minValue, Math.min(maxValue, value));
}

function computeTelemetryMaxPoints() {
  var canvas = document.getElementById('techBatteryCanvas');
  var parent = canvas && canvas.parentElement;
  var width = parent ? Number(parent.clientWidth) : Number(window.innerWidth || 0);
  if (!isFinite(width) || width <= 0) {
    return 120;
  }
  // Keep roughly one point per 3.5px to avoid overdraw on small/mobile screens.
  var points = Math.round(width / 3.5);
  return clampNumber(points, 96, 220);
}

function fetchJsonWithTimeout(url, timeoutMs) {
  if (typeof AbortController === 'undefined') {
    return fetch(url, { cache: 'no-store' }).then(function(r) {
      return r.ok ? r.json() : null;
    });
  }
  var controller = new AbortController();
  var timeoutId = setTimeout(function() {
    controller.abort();
  }, timeoutMs);
  return fetch(url, { signal: controller.signal, cache: 'no-store' })
    .then(function(r) {
      clearTimeout(timeoutId);
      return r.ok ? r.json() : null;
    })
    .catch(function(err) {
      clearTimeout(timeoutId);
      if (err && err.name === 'AbortError') {
        return null;
      }
      throw err;
    });
}

function isLightTheme() {
  return document.documentElement.classList.contains('light-mode');
}

function syncTechCanvasSize(canvas) {
  const parent = canvas && canvas.parentElement;
  if (!parent) {
    return null;
  }
  const w = Math.max(1, parent.clientWidth);
  const h = Math.max(1, parent.clientHeight);
  const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
  canvas.width = Math.floor(w * dpr);
  canvas.height = Math.floor(h * dpr);
  canvas.style.width = w + 'px';
  canvas.style.height = h + 'px';
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { w: w, h: h, ctx: ctx };
}

function formatHMS(totalSec) {
  const s = Math.max(0, Number(totalSec) || 0);
  const hh = Math.floor(s / 3600);
  const mm = Math.floor((s % 3600) / 60);
  const ss = Math.floor(s % 60);
  const pad2 = (n) => (n < 10 ? '0' + n : '' + n);
  return pad2(hh) + ':' + pad2(mm) + ':' + pad2(ss);
}

function formatAxisUptimeShort(totalSec) {
  // For chart axis: keep it compact.
  const s = Math.max(0, Number(totalSec) || 0);
  const hh = Math.floor(s / 3600);
  const mm = Math.floor((s % 3600) / 60);
  const ss = Math.floor(s % 60);
  const pad2 = (n) => (n < 10 ? '0' + n : '' + n);
  if (hh > 0) {
    return hh + ':' + pad2(mm);
  }
  return pad2(mm) + ':' + pad2(ss);
}

function buildSmoothedVoltageSeries(rawValues) {
  var out = new Array(rawValues.length);
  var radius = rawValues.length > 300 ? 3 : 2;
  var i;
  for (i = 0; i < rawValues.length; i++) {
    var num = 0;
    var den = 0;
    var j;
    for (j = -radius; j <= radius; j++) {
      var k = i + j;
      if (k < 0 || k >= rawValues.length) {
        continue;
      }
      var v = Number(rawValues[k]);
      if (!isFinite(v)) {
        continue;
      }
      var w = (radius + 1) - Math.abs(j);
      num += v * w;
      den += w;
    }
    out[i] = den > 0 ? (num / den) : Number(rawValues[i]);
  }
  return out;
}

function traceSmoothPath(ctx, points) {
  if (!points || points.length === 0) {
    return false;
  }
  ctx.moveTo(points[0].x, points[0].y);
  if (points.length === 1) {
    return true;
  }
  if (points.length === 2) {
    ctx.lineTo(points[1].x, points[1].y);
    return true;
  }

  var i;
  for (i = 1; i < points.length - 1; i++) {
    var xc = (points[i].x + points[i + 1].x) / 2;
    var yc = (points[i].y + points[i + 1].y) / 2;
    ctx.quadraticCurveTo(points[i].x, points[i].y, xc, yc);
  }

  var penultimate = points[points.length - 2];
  var last = points[points.length - 1];
  ctx.quadraticCurveTo(penultimate.x, penultimate.y, last.x, last.y);
  return true;
}

function drawTechBatteryChart(telem) {
  const canvas = document.getElementById('techBatteryCanvas');
  if (!canvas) {
    return;
  }
  const size = syncTechCanvasSize(canvas);
  if (!size) {
    return;
  }
  const ctx = size.ctx;
  const W = size.w;
  const H = size.h;
  const light = isLightTheme();
  const padL = 44;
  const padR = 10;
  const padT = 10;
  const padB = 26;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;

  ctx.fillStyle = light ? '#faf6f6' : '#060404';
  ctx.fillRect(0, 0, W, H);

  const vArr = telem && telem.v && telem.v.length ? telem.v : [];
  if (vArr.length === 0) {
    ctx.fillStyle = light ? 'rgba(80,0,0,0.45)' : 'rgba(255, 99, 90, 0.55)';
    ctx.font = '600 12px ui-sans-serif, system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('Collecting (1 Hz, waiting for full discharge session)', W / 2, H / 2);
    return;
  }
  const plotArr = buildSmoothedVoltageSeries(vArr);

  var minV = Infinity;
  var maxV = -Infinity;
  var i;
  var val;
  for (i = 0; i < plotArr.length; i++) {
    val = Number(plotArr[i]);
    if (!isFinite(val)) {
      continue;
    }
    if (val < minV) {
      minV = val;
    }
    if (val > maxV) {
      maxV = val;
    }
  }
  if (!isFinite(minV) || !isFinite(maxV)) {
    minV = 0;
    maxV = 1;
  }
  if (maxV - minV < 0.05) {
    maxV = minV + 0.2;
    minV -= 0.05;
  } else {
    var padV = (maxV - minV) * 0.12;
    minV -= padV;
    maxV += padV;
  }

  const periodMs = telem.periodMs && Number(telem.periodMs) > 0 ? Number(telem.periodMs) : 1000;
  const n = vArr.length;
  const uptimeSec = telem.uptimeSec != null ? Number(telem.uptimeSec) : ((n - 1) * periodMs) / 1000;

  ctx.strokeStyle = light ? 'rgba(0,0,0,0.08)' : 'rgba(255, 69, 58, 0.12)';
  ctx.lineWidth = 1;
  var gridY;
  for (gridY = 0; gridY <= 4; gridY++) {
    var y = padT + (plotH * gridY) / 4;
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + plotW, y);
    ctx.stroke();
  }
  var gridX;
  for (gridX = 0; gridX <= 5; gridX++) {
    var xg = padL + (plotW * gridX) / 5;
    ctx.beginPath();
    ctx.moveTo(xg, padT);
    ctx.lineTo(xg, padT + plotH);
    ctx.stroke();
  }

  ctx.fillStyle = light ? '#616161' : 'rgba(255, 190, 185, 0.5)';
  ctx.font = '10px ui-monospace, monospace';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (gridY = 0; gridY <= 4; gridY++) {
    var vy = maxV - ((maxV - minV) * gridY) / 4;
    var yy = padT + (plotH * gridY) / 4;
    ctx.fillText(vy.toFixed(2), padL - 5, yy);
  }

  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (gridX = 0; gridX <= 5; gridX++) {
    var frac = gridX / 5;
    var tSec = frac * uptimeSec;
    var lx = padL + plotW * frac;
    // Clamp label so the rightmost timestamp is not cut off.
    var label = formatAxisUptimeShort(tSec);
    var txtW = ctx.measureText(label).width;
    var minX = 1 + txtW / 2;
    var maxX = W - 1 - txtW / 2;
    lx = Math.max(minX, Math.min(lx, maxX));
    ctx.fillText(label, lx, padT + plotH + 5);
  }

  function xAt(idx) {
    return padL + (n <= 1 ? plotW / 2 : (plotW * idx) / (n - 1));
  }
  function yAt(volt) {
    var t = (volt - minV) / (maxV - minV);
    return padT + plotH * (1 - t);
  }

  ctx.lineJoin = 'round';
  ctx.beginPath();
  var px;
  var py;
  var points = [];
  for (i = 0; i < n; i++) {
    val = Number(plotArr[i]);
    if (!isFinite(val)) {
      continue;
    }
    px = xAt(i);
    py = yAt(val);
    points.push({ x: px, y: py });
  }
  if (points.length === 0) {
    return;
  }
  traceSmoothPath(ctx, points);

  ctx.lineTo(points[points.length - 1].x, padT + plotH);
  ctx.lineTo(points[0].x, padT + plotH);
  ctx.closePath();
  var grad = ctx.createLinearGradient(0, padT, 0, padT + plotH);
  if (light) {
    grad.addColorStop(0, 'rgba(255, 59, 48, 0.2)');
    grad.addColorStop(1, 'rgba(255, 59, 48, 0.02)');
  } else {
    grad.addColorStop(0, 'rgba(255, 69, 58, 0.32)');
    grad.addColorStop(1, 'rgba(255, 40, 40, 0.02)');
  }
  ctx.fillStyle = grad;
  ctx.fill();

  if (!light) {
    ctx.strokeStyle = 'rgba(255, 69, 58, 0.22)';
    ctx.lineWidth = 6;
    ctx.beginPath();
    if (traceSmoothPath(ctx, points)) {
      ctx.stroke();
    }
  }

  ctx.strokeStyle = light ? '#ff3b30' : '#ff453a';
  ctx.lineWidth = 2;
  ctx.shadowColor = light ? 'transparent' : 'rgba(255, 69, 58, 0.45)';
  ctx.shadowBlur = light ? 0 : 10;
  ctx.beginPath();
  if (traceSmoothPath(ctx, points)) {
    ctx.stroke();
  }
  ctx.shadowBlur = 0;

  // Chart title intentionally omitted (single discharge uptime session).
}

function updateTelemetryMeta(telem) {
  const el = document.getElementById('batteryUptimeLine');
  if (!el) {
    return;
  }
  const uptimeSec = telem && telem.uptimeSec != null ? Number(telem.uptimeSec) : 0;
  const truncated = telem && telem.truncated === true;
  const total = telem && telem.count != null ? Number(telem.count) : 0;
  const shown = telem && telem.points != null ? Number(telem.points) : total;
  const sampled = telem && telem.downsampled === true;
  const pointsText = sampled && total > 0 ? (' · ' + shown + '/' + total + ' pts') : '';
  el.textContent = 'Battery uptime ' + formatHMS(uptimeSec) + pointsText + (truncated ? ' (truncated)' : '');
}

function fetchTelemetry() {
  if (telemetryRequestInFlight) {
    return;
  }
  telemetryRequestInFlight = true;
  var maxPoints = computeTelemetryMaxPoints();
  fetchJsonWithTimeout('/telemetry?maxPoints=' + encodeURIComponent(String(maxPoints)), 3500)
    .then(function(r) {
      if (r) {
        lastTelemetryPayload = r;
        drawTechBatteryChart(r);
        updateTelemetryMeta(r);
      }
    })
    .catch(function() {})
    .then(function() {
      telemetryRequestInFlight = false;
    });
}

function hasTelemetrySamples(payload) {
  return !!(payload && payload.v && payload.v.length && Number(payload.count || payload.v.length) > 0);
}

function isSameTelemetrySession(a, b) {
  if (!a || !b) {
    return false;
  }
  var sa = Number(a.sessionStartMs);
  var sb = Number(b.sessionStartMs);
  if (isFinite(sa) && isFinite(sb) && sa > 0 && sb > 0) {
    return sa === sb;
  }
  return false;
}

function fetchTelemetryForDownload() {
  return fetch('/telemetry?full=1')
    .then(function(r) {
      return r.ok ? r.json() : null;
    })
    .then(function(data) {
      if (hasTelemetrySamples(data)) {
        lastTelemetryFullPayload = data;
        return data;
      }
      if (hasTelemetrySamples(lastTelemetryFullPayload) && isSameTelemetrySession(lastTelemetryPayload, lastTelemetryFullPayload)) {
        return lastTelemetryFullPayload;
      }
      if (hasTelemetrySamples(lastTelemetryPayload) && lastTelemetryPayload.downsampled !== true) {
        return lastTelemetryPayload;
      }
      return null;
    })
    .catch(function() {
      if (hasTelemetrySamples(lastTelemetryFullPayload) && isSameTelemetrySession(lastTelemetryPayload, lastTelemetryFullPayload)) {
        return lastTelemetryFullPayload;
      }
      if (hasTelemetrySamples(lastTelemetryPayload) && lastTelemetryPayload.downsampled !== true) {
        return lastTelemetryPayload;
      }
      return null;
    });
}

function updateChartTheme() {
  drawTechBatteryChart(lastTelemetryPayload || { v: [] });
}

function handleResize() {
  drawTechBatteryChart(lastTelemetryPayload || { v: [] });
}

function updateChartFontSizes() {
  handleResize();
}

function getExportTimestampUtcCompact(dateValue) {
  const d = dateValue || new Date();
  const y = String(d.getUTCFullYear());
  const mo = String(d.getUTCMonth() + 1).padStart(2, '0');
  const da = String(d.getUTCDate()).padStart(2, '0');
  const h = String(d.getUTCHours()).padStart(2, '0');
  const mi = String(d.getUTCMinutes()).padStart(2, '0');
  const s = String(d.getUTCSeconds()).padStart(2, '0');
  return y + mo + da + 'T' + h + mi + s + 'Z';
}

function getExportTimestampUtcIso(dateValue) {
  const d = dateValue || new Date();
  return d.toISOString();
}

function triggerTelemetryDownloadJson() {
  fetchTelemetryForDownload().then(function(payload) {
    if (!payload || !payload.count) {
      return;
    }
    const now = new Date();
    const exported = Object.assign({}, payload, {
      exportedAt: getExportTimestampUtcIso(now)
    });
    const body = JSON.stringify(exported, null, 2);
    const blob = new Blob([body], { type: 'application/json' });
    const a = document.createElement('a');
    const stamp = getExportTimestampUtcCompact(now);
    a.href = URL.createObjectURL(blob);
    a.download = 'battery_log_' + stamp + '.json';
    a.click();
    URL.revokeObjectURL(a.href);
  });
}

function triggerTelemetryDownloadCsv() {
  fetchTelemetryForDownload().then(function(payload) {
    if (!payload || !payload.v || !payload.v.length) {
      return;
    }
    const periodSec = (payload.periodMs != null ? Number(payload.periodMs) : 1000) / 1000;
    const lines = ['uptime_s,voltage_V'];
    const arr = payload.v;
    for (var j = 0; j < arr.length; j++) {
      lines.push(String(j * periodSec) + ',' + String(arr[j]));
    }
    const blob = new Blob([lines.join('\n')], { type: 'text/csv;charset=utf-8' });
    const a = document.createElement('a');
    const stamp = getExportTimestampUtcCompact(new Date());
    a.href = URL.createObjectURL(blob);
    a.download = 'battery_log_' + stamp + '.csv';
    a.click();
    URL.revokeObjectURL(a.href);
  });
}

// Add event listener for window resize with debouncing
let resizeTimer;
window.addEventListener('resize', function() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(handleResize, 100);
});

// Initial chart layout after load; telemetry polled separately from /telemetry.
window.addEventListener('load', function() {
  getReadings();
  fetchTelemetry();
  setTimeout(handleResize, 200);
  setTimeout(handleResize, 500);
});

setInterval(fetchTelemetry, 2000);

const CONTROL_GROUP_DEFAULTS = {
  controlGroup1: 'Chassis Power',
  controlGroup2: 'Actuators Power',
  controlGroup3: 'Others Power'
};

const CONTROL_GROUP_FIELDS = {
  controlGroup1: 'controlGroup1Name',
  controlGroup2: 'controlGroup2Name',
  controlGroup3: 'controlGroup3Name'
};

function normalizeGroupName(value, fallback) {
  const text = String(value == null ? '' : value).trim();
  return text.length ? text : fallback;
}

function normalizePowerLabel(name) {
  const text = String(name == null ? '' : name).trim();
  if (!text.length) {
    return '';
  }
  return /\bpower$/i.test(text) ? text : (text + ' Power');
}

function normalizeSwitchLabelBase(name) {
  const text = String(name == null ? '' : name).trim();
  if (!text.length) {
    return '';
  }
  return text.replace(/\s+power$/i, '').trim() || text;
}

const POWER_SWITCH_CONFIG = [
  { key: 'controlGroup1Power', switchId: 'controlGroup1PowerSwitch', statusId: 'controlGroup1PowerStatus', labelId: 'controlGroup1PowerLabel', groupKey: 'controlGroup1', label: '' },
  { key: 'controlGroup2Power', switchId: 'controlGroup2PowerSwitch', statusId: 'controlGroup2PowerStatus', labelId: 'controlGroup2PowerLabel', groupKey: 'controlGroup2', label: '' },
  { key: 'controlGroup3Power', switchId: 'controlGroup3PowerSwitch', statusId: 'controlGroup3PowerStatus', labelId: 'controlGroup3PowerLabel', groupKey: 'controlGroup3', label: '' }
];

var emergencyPowerPanelLocked = false;

function setEmergencyPowerPanelLocked(locked) {
  emergencyPowerPanelLocked = locked;
  var stopBtn = document.getElementById('emergencyStopAllBtn');
  var hint = document.getElementById('emergencyStopHint');
  POWER_SWITCH_CONFIG.forEach(function(cfg) {
    var el = document.getElementById(cfg.switchId);
    if (el) {
      el.disabled = locked;
      var row = el.closest('.power-switch-row');
      if (row) {
        row.classList.toggle('power-switch-row--disabled', locked);
      }
    }
  });
  if (stopBtn) {
    stopBtn.disabled = locked;
  }
  if (hint) {
    hint.hidden = !locked;
  }
}

function sendEmergencyStopAll() {
  var stopBtn = document.getElementById('emergencyStopAllBtn');
  if (stopBtn) {
    stopBtn.disabled = true;
  }
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/power', true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.onreadystatechange = function() {
    if (xhr.readyState !== 4) {
      return;
    }
    if (xhr.status === 200) {
      try {
        updatePowerStateUI(JSON.parse(xhr.responseText));
      } catch (e) {
        console.error('Emergency stop response parse error', e);
      }
      setEmergencyPowerPanelLocked(true);
    } else {
      if (stopBtn) {
        stopBtn.disabled = false;
      }
      console.error('Emergency stop failed', xhr.status);
    }
  };
  xhr.send(
    JSON.stringify({
      controlGroup1Power: false,
      controlGroup2Power: false,
      controlGroup3Power: false
    })
  );
}

const PHYSICAL_SWITCH_CONFIG = [
  { key: 'controlGroup1Switch', rawKey: 'controlGroup1SwitchRaw', statusId: 'controlGroup1SwitchStatus', groupKey: 'controlGroup1', label: '', pinField: 'controlGroup1SwitchPin' },
  { key: 'controlGroup2Switch', rawKey: 'controlGroup2SwitchRaw', statusId: 'controlGroup2SwitchStatus', groupKey: 'controlGroup2', label: '', pinField: 'controlGroup2SwitchPin' },
  { key: 'controlGroup3Switch', rawKey: 'controlGroup3SwitchRaw', statusId: 'controlGroup3SwitchStatus', groupKey: 'controlGroup3', label: '', pinField: 'controlGroup3SwitchPin' }
];

function applyControlGroupLabels(settings) {
  const names = {
    controlGroup1: CONTROL_GROUP_DEFAULTS.controlGroup1,
    controlGroup2: CONTROL_GROUP_DEFAULTS.controlGroup2,
    controlGroup3: CONTROL_GROUP_DEFAULTS.controlGroup3
  };

  if (settings && typeof settings === 'object') {
    names.controlGroup1 = normalizeGroupName(settings[CONTROL_GROUP_FIELDS.controlGroup1], CONTROL_GROUP_DEFAULTS.controlGroup1);
    names.controlGroup2 = normalizeGroupName(settings[CONTROL_GROUP_FIELDS.controlGroup2], CONTROL_GROUP_DEFAULTS.controlGroup2);
    names.controlGroup3 = normalizeGroupName(settings[CONTROL_GROUP_FIELDS.controlGroup3], CONTROL_GROUP_DEFAULTS.controlGroup3);
  }

  POWER_SWITCH_CONFIG.forEach((config) => {
    const groupName = names[config.groupKey] || CONTROL_GROUP_DEFAULTS.controlGroup1;
    config.label = normalizePowerLabel(groupName);
    if (config.labelId) {
      const labelElement = document.getElementById(config.labelId);
      if (labelElement) {
        labelElement.textContent = config.label;
      }
    }
  });

  PHYSICAL_SWITCH_CONFIG.forEach((config) => {
    const groupName = names[config.groupKey] || CONTROL_GROUP_DEFAULTS.controlGroup1;
    let label = normalizeSwitchLabelBase(groupName) + ' Switch';
    if (settings) {
      const pin = settings[config.pinField];
      if (pin !== undefined && pin !== null && Number.isFinite(Number(pin))) {
        label += ' (GPIO' + Number(pin) + ')';
      }
    }
    config.label = label;
  });
}

function loadControlLabelsFromReadings() {
  fetch('/readings')
    .then((res) => (res.ok ? res.json() : null))
    .then((readings) => {
      if (!readings) {
        applyControlGroupLabels(null);
        return;
      }
      applyControlGroupLabels(readings);
    })
    .catch(() => {
      applyControlGroupLabels(null);
    });
}

applyControlGroupLabels(null);

function normalizeBool(value) {
  return value === true || value === 1 || value === '1' || value === 'true';
}

function updateSystemStatus(jsonValue) {
  if (jsonValue["batteryStatus"] !== undefined) {
    var batteryPill = document.getElementById("batteryStatusValue");
    if (batteryPill) {
      batteryPill.textContent = jsonValue["batteryStatus"];
    }
  }
  if (jsonValue["microROS"] !== undefined) {
    var microPill = document.getElementById("microRosValue");
    if (microPill) {
      microPill.textContent = jsonValue["microROS"];
    }
  }
}

function updatePowerStateUI(jsonValue) {
  POWER_SWITCH_CONFIG.forEach((config) => {
    if (jsonValue[config.key] === undefined) {
      return;
    }

    const enabled = normalizeBool(jsonValue[config.key]);
    const statusElement = document.getElementById(config.statusId);
    const switchElement = document.getElementById(config.switchId);

    if (statusElement) {
      statusElement.textContent = config.label + ': ' + (enabled ? 'ON' : 'OFF');
      statusElement.classList.remove('power-on', 'power-off');
      statusElement.classList.add(enabled ? 'power-on' : 'power-off');
    }

    if (switchElement && switchElement.checked !== enabled) {
      switchElement.checked = enabled;
    }
    if (switchElement && emergencyPowerPanelLocked) {
      switchElement.disabled = true;
    }
  });
}

function updatePhysicalSwitchUI(jsonValue) {
  PHYSICAL_SWITCH_CONFIG.forEach((config) => {
    if (jsonValue[config.key] === undefined) {
      return;
    }

    const active = normalizeBool(jsonValue[config.key]);
    const raw = jsonValue[config.rawKey];
    const statusElement = document.getElementById(config.statusId);
    if (!statusElement) {
      return;
    }

    let text = config.label + ': ' + (active ? 'ACTIVE' : 'INACTIVE');
    if (raw !== undefined) {
      text += ' (raw=' + raw + ')';
    }
    statusElement.textContent = text;
    statusElement.classList.remove('power-on', 'power-off');
    statusElement.classList.add(active ? 'power-on' : 'power-off');
  });
}

// Plot data from JSON response (/readings): status + live voltage readout (rolling history from /telemetry).
function plotData(jsonValue) {
  applyControlGroupLabels(jsonValue);
  updateSystemStatus(jsonValue);
  updatePowerStateUI(jsonValue);
  updatePhysicalSwitchUI(jsonValue);

  if (jsonValue.sensor !== undefined) {
    var liveEl = document.getElementById('liveVoltageValue');
    if (liveEl) {
      var v = Number(jsonValue.sensor);
      liveEl.textContent = isFinite(v) ? v.toFixed(2) + ' V' : '--';
    }
  }

  var estLine = document.getElementById('batteryEstimateLine');
  if (estLine) {
    var pct = jsonValue.estBattPct;
    var mins = jsonValue.estBattMin;
    var pctNum = pct !== undefined && pct !== null ? Number(pct) : NaN;
    var pctOk = isFinite(pctNum) && pctNum >= 0;
    var pctRounded = pctOk ? Math.max(0, Math.min(100, Math.round(pctNum))) : null;

    var iconFill = document.getElementById('batteryIconFill');
    if (iconFill) {
      var scale = pctOk ? (pctRounded / 100) : 0;
      iconFill.style.transform = 'scaleX(' + scale + ')';
      iconFill.style.opacity = pctOk ? '1' : '0.35';
    }

    var remOk = mins !== undefined && mins !== null && isFinite(Number(mins)) && Number(mins) >= 0;
    var remText = remOk ? formatHMS(Number(mins) * 60) : '--:--:--';

    if (!pctOk) {
      estLine.textContent = '--% · estimating...';
    } else {
      estLine.textContent = pctRounded + '% · ' + (remOk ? (remText + ' left') : 'estimating...');
    }
  }
}

// Function to get current readings when the page loads
function getReadings() {
  if (readingsRequestInFlight) {
    return;
  }
  readingsRequestInFlight = true;
  fetchJsonWithTimeout('/readings', 2500)
    .then(function(data) {
      if (!data) {
        return;
      }
      plotData(data);
    })
    .catch(function() {})
    .then(function() {
      readingsRequestInFlight = false;
    });
}

// Poll sensor and power data periodically (sync WebServer mode)
setInterval(getReadings, 1000);

// Ensure chart redraws properly on orientation change
window.addEventListener('orientationchange', function() {
  setTimeout(handleResize, 300);
});

// Emergency/power switch functionality
document.addEventListener('DOMContentLoaded', function() {
  const actionButton = document.getElementById('actionButton');
  loadControlLabelsFromReadings();

  POWER_SWITCH_CONFIG.forEach((config) => {
    const switchElement = document.getElementById(config.switchId);
    if (!switchElement) {
      return;
    }
    const row = switchElement.closest('.power-switch-row');

    if (row) {
      row.addEventListener('click', function(e) {
        if (emergencyPowerPanelLocked || switchElement.disabled) {
          return;
        }
        if (e.target && e.target.closest('.switch-control')) {
          return;
        }
        switchElement.click();
      });
    }

    switchElement.addEventListener('change', function() {
      sendPowerCommand(config.key, this.checked, this);
    });
  });

  var emergencyStopBtn = document.getElementById('emergencyStopAllBtn');
  if (emergencyStopBtn) {
    emergencyStopBtn.addEventListener('click', sendEmergencyStopAll);
  }

  // Modern action button with short and long press functionality
  if (actionButton) {
    let pressTimer;
    let longPress = false;
    
    // Mouse/touch down event - start timer
    actionButton.addEventListener('mousedown', startPressTimer);
    actionButton.addEventListener('touchstart', function(e) {
      e.preventDefault();
      startPressTimer();
    });
    
    // Mouse/touch up event - if not long press, navigate
    actionButton.addEventListener('mouseup', handlePressEnd);
    actionButton.addEventListener('touchend', function(e) {
      e.preventDefault();
      handlePressEnd();
    });
    
    // If the user moves away while pressing, cancel the long press
    actionButton.addEventListener('mouseleave', clearPressTimer);
    actionButton.addEventListener('touchcancel', clearPressTimer);
    
    function startPressTimer() {
      clearTimeout(pressTimer);
      longPress = false;
      
      // If button is held for 800ms, it's a long press for refresh
      pressTimer = setTimeout(() => {
        longPress = true;
        actionButton.classList.add('loading');
        
        // Add a small delay to show the animation
        setTimeout(() => {
          window.location.reload();
        }, 500);
      }, 800);
    }
    
    function clearPressTimer() {
      clearTimeout(pressTimer);
    }
    
    function handlePressEnd() {
      clearTimeout(pressTimer);
      
      // If it was a short press (not long press), navigate to the specified URL
      if (!longPress) {
        closeSpeedDial();
        window.location.href = controlPanelShortcutUrl;
      }
    }
  }
});

// Send power command to the server
function sendPowerCommand(powerKey, enabled, switchElement) {
  if (emergencyPowerPanelLocked) {
    return;
  }
  var xhr = new XMLHttpRequest();
  xhr.open("POST", "/power", true);
  xhr.setRequestHeader("Content-Type", "application/json");

  if (switchElement) {
    switchElement.disabled = true;
  }

  xhr.onreadystatechange = function() {
    if (xhr.readyState === 4) {
      if (switchElement) {
        switchElement.disabled = false;
      }

      if (xhr.status === 200) {
        try {
          updatePowerStateUI(JSON.parse(xhr.responseText));
        } catch (e) {
          console.error('Error parsing response:', e);
        }
      } else {
        if (switchElement) {
          switchElement.checked = !enabled;
        }
        console.error('Error sending power command: Network response was not ok');
      }
    }
  };

  var payload = {};
  payload[powerKey] = enabled;
  xhr.send(JSON.stringify(payload));
}

function loadDeviceBanner() {
  fetch('/device')
    .then(function(res) { return res.ok ? res.json() : null; })
    .then(function(data) {
      if (!data) {
        return;
      }
      var macEl = document.getElementById('deviceMac');
      var verEl = document.getElementById('deviceVersion');
      if (macEl) {
        macEl.textContent = data.mac && String(data.mac).length
          ? normalizeMacDisplay(data.mac)
          : ESP_DAEMON_PLACEHOLDER_MAC;
      }
      if (verEl) {
        verEl.textContent = data.version && String(data.version).length ? data.version : ESP_DAEMON_PLACEHOLDER_VERSION;
      }
      controlPanelShortcutUrl = sanitizeControlPanelUrlClient(data.controlPanelUrl);
    })
    .catch(function() {});
}

document.addEventListener('DOMContentLoaded', function() {
  loadDeviceBanner();
});
