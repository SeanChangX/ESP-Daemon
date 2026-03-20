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

  var minV = Infinity;
  var maxV = -Infinity;
  var i;
  var val;
  for (i = 0; i < vArr.length; i++) {
    val = Number(vArr[i]);
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
  var started = false;
  var px;
  var py;
  for (i = 0; i < n; i++) {
    val = Number(vArr[i]);
    if (!isFinite(val)) {
      continue;
    }
    px = xAt(i);
    py = yAt(val);
    if (!started) {
      ctx.moveTo(px, py);
      started = true;
    } else {
      ctx.lineTo(px, py);
    }
  }
  if (!started) {
    return;
  }

  ctx.lineTo(xAt(n - 1), padT + plotH);
  ctx.lineTo(xAt(0), padT + plotH);
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
    started = false;
    for (i = 0; i < n; i++) {
      val = Number(vArr[i]);
      if (!isFinite(val)) {
        continue;
      }
      px = xAt(i);
      py = yAt(val);
      if (!started) {
        ctx.moveTo(px, py);
        started = true;
      } else {
        ctx.lineTo(px, py);
      }
    }
    ctx.stroke();
  }

  ctx.strokeStyle = light ? '#ff3b30' : '#ff453a';
  ctx.lineWidth = 2;
  ctx.shadowColor = light ? 'transparent' : 'rgba(255, 69, 58, 0.45)';
  ctx.shadowBlur = light ? 0 : 10;
  ctx.beginPath();
  started = false;
  for (i = 0; i < n; i++) {
    val = Number(vArr[i]);
    if (!isFinite(val)) {
      continue;
    }
    px = xAt(i);
    py = yAt(val);
    if (!started) {
      ctx.moveTo(px, py);
      started = true;
    } else {
      ctx.lineTo(px, py);
    }
  }
  ctx.stroke();
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
  el.textContent = 'Battery uptime ' + formatHMS(uptimeSec) + (truncated ? ' (truncated)' : '');
}

function fetchTelemetry() {
  fetch('/telemetry')
    .then(function(r) {
      return r.ok ? r.json() : null;
    })
    .then(function(data) {
      if (!data) {
        return;
      }
      lastTelemetryPayload = data;
      drawTechBatteryChart(data);
      updateTelemetryMeta(data);
    })
    .catch(function() {});
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

function triggerTelemetryDownloadJson() {
  if (!lastTelemetryPayload || !lastTelemetryPayload.count) {
    return;
  }
  const body = JSON.stringify(lastTelemetryPayload, null, 2);
  const blob = new Blob([body], { type: 'application/json' });
  const a = document.createElement('a');
  const ms = lastTelemetryPayload.deviceMs != null ? lastTelemetryPayload.deviceMs : Date.now();
  a.href = URL.createObjectURL(blob);
  a.download = 'battery_log_' + ms + '.json';
  a.click();
  URL.revokeObjectURL(a.href);
}

function triggerTelemetryDownloadCsv() {
  if (!lastTelemetryPayload || !lastTelemetryPayload.v || !lastTelemetryPayload.v.length) {
    return;
  }
  const periodSec = (lastTelemetryPayload.periodMs != null ? Number(lastTelemetryPayload.periodMs) : 1000) / 1000;
  const lines = ['uptime_s,voltage_V'];
  const arr = lastTelemetryPayload.v;
  for (var j = 0; j < arr.length; j++) {
    lines.push(String(j * periodSec) + ',' + String(arr[j]));
  }
  const blob = new Blob([lines.join('\n')], { type: 'text/csv;charset=utf-8' });
  const a = document.createElement('a');
  const ms = lastTelemetryPayload.deviceMs != null ? lastTelemetryPayload.deviceMs : Date.now();
  a.href = URL.createObjectURL(blob);
  a.download = 'battery_log_' + ms + '.csv';
  a.click();
  URL.revokeObjectURL(a.href);
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

const POWER_SWITCH_CONFIG = [
  { key: 'chassisPower', switchId: 'chassisPowerSwitch', statusId: 'chassisPowerStatus', label: 'Chassis Power' },
  { key: 'missionPower', switchId: 'missionPowerSwitch', statusId: 'missionPowerStatus', label: 'Mission Mechanism Power' },
  { key: 'negativePressurePower', switchId: 'negativePressurePowerSwitch', statusId: 'negativePressurePowerStatus', label: 'Negative Pressure Chassis' }
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
      chassisPower: false,
      missionPower: false,
      negativePressurePower: false
    })
  );
}

const PHYSICAL_SWITCH_CONFIG = [
  { key: 'chassisSwitch', rawKey: 'chassisSwitchRaw', statusId: 'chassisSwitchStatus', label: 'Chassis Switch', pinField: 'chassisSwitchPin' },
  { key: 'missionSwitch', rawKey: 'missionSwitchRaw', statusId: 'missionSwitchStatus', label: 'Mission Switch', pinField: 'missionSwitchPin' },
  { key: 'negativePressureSwitch', rawKey: 'negativePressureSwitchRaw', statusId: 'negativePressureSwitchStatus', label: 'Negative Pressure Switch', pinField: 'negPressureSwitchPin' }
];

function loadSwitchLabelsFromSettings() {
  fetch('/settings/read', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then((res) => (res.ok ? res.json() : null))
    .then((settings) => {
      if (!settings) return;
      PHYSICAL_SWITCH_CONFIG.forEach((config) => {
        const pin = settings[config.pinField];
        if (pin !== undefined && pin !== null && Number.isFinite(Number(pin))) {
          config.label = config.label.split(' (GPIO')[0] + ' (GPIO' + Number(pin) + ')';
        }
      });
    })
    .catch(() => {
      // Keep default labels when settings endpoint is unavailable.
    });
}

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
      estLine.textContent = '--% · -- left';
    } else {
      estLine.textContent = pctRounded + '% · ' + (remOk ? (remText + ' left') : '-- left');
    }
  }
}

// Function to get current readings when the page loads
function getReadings() {
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var myObj = JSON.parse(this.responseText);
      plotData(myObj);
    }
  };
  xhr.open("GET", "/readings", true);
  xhr.send();
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
  loadSwitchLabelsFromSettings();

  POWER_SWITCH_CONFIG.forEach((config) => {
    const switchElement = document.getElementById(config.switchId);
    if (!switchElement) {
      return;
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
