const ESP_DAEMON_PLACEHOLDER_MAC = '00-00-00-00-00-00';

function normalizeMacDisplay(mac) {
  return String(mac).replace(/:/g, '-').toUpperCase();
}
const ESP_DAEMON_PLACEHOLDER_VERSION = '0.0.0';

const SETTINGS_AUTH_STORAGE_KEY = 'esp_daemon_settings_auth_pin';

/** Must match firmware kMaxEmergencyMacs */
const EMERGENCY_MAC_MAX = 16;

/** Go Live / Live Server / file preview: no ESP at this origin. */
function isLocalUiPreview() {
  if (window.location.protocol === 'file:') {
    return true;
  }
  const h = window.location.hostname;
  if (h === '127.0.0.1' || h === 'localhost' || h === '[::1]' || h === '::1' || h === '') {
    return true;
  }
  return false;
}

/** Only trust PIN/device flags if /device looks like ESP-Daemon firmware (not Live Server fallback JSON). */
function looksLikeEspDevicePayload(d) {
  return (
    d != null &&
    typeof d === 'object' &&
    typeof d.version === 'string' &&
    d.version.length > 0 &&
    typeof d.mac === 'string' &&
    d.mac.length > 0
  );
}

const SAVE_STATUS_BASE_CLASS = 'actions__status';
const SAVE_STATUS_FADE_CLASS = 'actions__status--fade-out';
const SAVE_STATUS_AUTO_HIDE_MS = 3000;
const SAVE_STATUS_FADE_MS = 240;
let saveStatusHideTimer = null;
let saveStatusClearTimer = null;

function setPreviewHint(msg) {
  const el = document.getElementById('saveStatus');
  if (!el) {
    return;
  }
  el.textContent = msg;
  el.className = SAVE_STATUS_BASE_CLASS + ' preview-hint';
}

const FIELD_IDS = [
  'deviceName', 'controlPanelUrl', 'pinProtectionEnabled', 'pinCode',
  'runtimeEspNowEnabled', 'runtimeMicroRosEnabled', 'runtimeLedEnabled', 'runtimeSensorEnabled',
  'rosNodeName', 'rosDomainId', 'rosTimerMs', 'mrosTimeoutMs', 'mrosPingIntervalMs', 'espNowChannel',
  'chassisSwitchPin', 'missionSwitchPin', 'negPressureSwitchPin',
  'chassisPowerPin', 'missionPower12vPin', 'missionPower7v4Pin', 'negPressurePowerPin',
  'switchActiveHigh', 'powerActiveHigh',
  'ledPin', 'ledCount', 'ledBrightness', 'ledOverrideDurationMs',
  'voltmeterPin', 'voltageDividerR1', 'voltageDividerR2', 'voltmeterCalibration', 'voltmeterOffset',
  'slidingWindowSize', 'timerPeriodUs', 'batteryDisconnectThreshold', 'batteryLowThreshold'
];

function applyTheme(theme) {
  const root = document.documentElement;
  const modeToggle = document.getElementById('modeToggle');
  const light = theme === 'light';
  root.classList.toggle('light-mode', light);
  if (modeToggle) {
    const nextMode = light ? 'dark' : 'light';
    modeToggle.setAttribute('aria-label', 'Switch to ' + nextMode + ' mode');
  }
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

function recalcVoltmeterCalibration() {
  const r1 = Number(document.getElementById('voltageDividerR1').value);
  const r2 = Number(document.getElementById('voltageDividerR2').value);
  const calibrationEl = document.getElementById('voltmeterCalibration');
  if (!Number.isFinite(r1) || !Number.isFinite(r2) || r2 <= 0) {
    calibrationEl.value = '';
    return;
  }
  calibrationEl.value = ((r1 + r2) / r2).toFixed(6);
}

function setStatus(msg, ok) {
  const el = document.getElementById('saveStatus');
  if (!el) {
    return;
  }
  if (saveStatusHideTimer) {
    clearTimeout(saveStatusHideTimer);
    saveStatusHideTimer = null;
  }
  if (saveStatusClearTimer) {
    clearTimeout(saveStatusClearTimer);
    saveStatusClearTimer = null;
  }
  el.classList.remove(SAVE_STATUS_FADE_CLASS);
  if (!msg) {
    el.textContent = '';
    el.className = SAVE_STATUS_BASE_CLASS;
    return;
  }
  el.textContent = msg;
  el.className = SAVE_STATUS_BASE_CLASS + ' ' + (ok ? 'ok' : 'err');
  saveStatusHideTimer = setTimeout(function() {
    const statusEl = document.getElementById('saveStatus');
    if (!statusEl) {
      return;
    }
    statusEl.classList.add(SAVE_STATUS_FADE_CLASS);
    saveStatusClearTimer = setTimeout(function() {
      const clearEl = document.getElementById('saveStatus');
      if (!clearEl) {
        return;
      }
      clearEl.textContent = '';
      clearEl.className = SAVE_STATUS_BASE_CLASS;
      clearEl.classList.remove(SAVE_STATUS_FADE_CLASS);
      saveStatusClearTimer = null;
    }, SAVE_STATUS_FADE_MS);
    saveStatusHideTimer = null;
  }, SAVE_STATUS_AUTO_HIDE_MS);
}

function setFieldValue(id, value) {
  const el = document.getElementById(id);
  if (!el) return;
  if (el.type === 'checkbox') {
    el.checked = !!value;
  } else {
    el.value = value !== undefined && value !== null ? value : '';
  }
}

function sanitizeFilenamePart(raw, fallback) {
  const s = String(raw == null ? '' : raw).trim();
  const cleaned = s
    .replace(/[^a-zA-Z0-9._-]+/g, '_')
    .replace(/_+/g, '_')
    .replace(/^[_\.-]+|[_\.-]+$/g, '');
  return cleaned || fallback;
}

function getDeviceNameForExportFilename() {
  const el = document.getElementById('deviceName');
  if (el && String(el.value || '').trim()) {
    return sanitizeFilenamePart(el.value, 'device');
  }
  return sanitizeFilenamePart(window.location.hostname || '', 'device');
}

function getExportTimestampUtcCompact() {
  const d = new Date();
  const y = String(d.getUTCFullYear());
  const mo = String(d.getUTCMonth() + 1).padStart(2, '0');
  const da = String(d.getUTCDate()).padStart(2, '0');
  const h = String(d.getUTCHours()).padStart(2, '0');
  const mi = String(d.getUTCMinutes()).padStart(2, '0');
  const s = String(d.getUTCSeconds()).padStart(2, '0');
  return y + mo + da + 'T' + h + mi + s + 'Z';
}

/** Normalize to aa:bb:cc:dd:ee:ff; input may use : or - between pairs. */
function normalizeMacColonFromAny(raw) {
  const cleaned = String(raw || '')
    .trim()
    .toLowerCase()
    .replace(/[^0-9a-f]/g, '');
  if (cleaned.length !== 12) {
    return null;
  }
  return cleaned.match(/.{2}/g).join(':');
}

function macColonToDisplayHyphen(macColon) {
  return String(macColon || '')
    .replace(/:/g, '-')
    .toUpperCase();
}

function clearEmergencyMacError() {
  const el = document.getElementById('emergencyMacError');
  if (el) {
    el.textContent = '';
  }
}

function setEmergencyMacError(msg) {
  const el = document.getElementById('emergencyMacError');
  if (el) {
    el.textContent = msg || '';
  }
}

function getEmergencyMacListUl() {
  return document.getElementById('emergencyMacList');
}

function getEmergencyMacPayloadList() {
  const ul = getEmergencyMacListUl();
  if (!ul) {
    return [];
  }
  return Array.from(ul.querySelectorAll('li[data-mac]'))
    .map((li) => li.getAttribute('data-mac'))
    .filter(Boolean);
}

function emergencyMacListHas(macColon) {
  const t = macColon.toLowerCase();
  return getEmergencyMacPayloadList().some((m) => m.toLowerCase() === t);
}

function addEmergencyMacToDom(macColon) {
  const ul = getEmergencyMacListUl();
  if (!ul) {
    return;
  }
  const li = document.createElement('li');
  li.setAttribute('data-mac', macColon);
  const addr = document.createElement('span');
  addr.className = 'mac-whitelist-list__addr';
  addr.textContent = macColonToDisplayHyphen(macColon);
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'btn mac-whitelist-remove';
  btn.textContent = 'Remove';
  btn.setAttribute('aria-label', 'Remove ' + macColonToDisplayHyphen(macColon));
  btn.addEventListener('click', function () {
    li.remove();
    clearEmergencyMacError();
  });
  li.appendChild(addr);
  li.appendChild(btn);
  ul.appendChild(li);
}

function clearEmergencyMacListDom() {
  const ul = getEmergencyMacListUl();
  if (ul) {
    ul.innerHTML = '';
  }
}

function applyEmergencyMacListFromServer(arr) {
  clearEmergencyMacListDom();
  if (!Array.isArray(arr)) {
    return;
  }
  arr.forEach((raw) => {
    const m = normalizeMacColonFromAny(raw);
    if (m) {
      addEmergencyMacToDom(m);
    }
  });
}

function tryAddEmergencyMacFromInput() {
  const input = document.getElementById('emergencyMacInput');
  if (!input) {
    return;
  }
  clearEmergencyMacError();
  const parsed = normalizeMacColonFromAny(input.value);
  if (!parsed) {
    setEmergencyMacError('Invalid MAC. Use 12 hex digits with : or - between pairs (e.g. AA-BB-CC-DD-EE-FF).');
    return;
  }
  if (emergencyMacListHas(parsed)) {
    setEmergencyMacError('This MAC is already listed.');
    return;
  }
  if (getEmergencyMacPayloadList().length >= EMERGENCY_MAC_MAX) {
    setEmergencyMacError('Maximum ' + EMERGENCY_MAC_MAX + ' addresses.');
    return;
  }
  addEmergencyMacToDom(parsed);
  input.value = '';
  input.focus();
}

/** Wrap settings numeric inputs with themed minus/plus controls (uses native stepDown/stepUp). */
function initNumericStepperControls() {
  document.querySelectorAll('.settings-card input[type="number"]:not([readonly])').forEach((input) => {
    if (input.closest('.num-stepper')) {
      return;
    }
    const wrap = document.createElement('div');
    wrap.className = 'num-stepper';

    const dec = document.createElement('button');
    dec.type = 'button';
    dec.className = 'num-stepper__btn num-stepper__btn--dec';
    dec.setAttribute('aria-label', 'Decrease value');
    dec.textContent = '\u2212';

    const inc = document.createElement('button');
    inc.type = 'button';
    inc.className = 'num-stepper__btn num-stepper__btn--inc';
    inc.setAttribute('aria-label', 'Increase value');
    inc.textContent = '+';

    input.parentNode.insertBefore(wrap, input);
    wrap.appendChild(dec);
    wrap.appendChild(input);
    wrap.appendChild(inc);

    dec.addEventListener('click', () => {
      input.focus();
      try {
        input.stepDown();
      } catch (e) {
        /* invalid state on some browsers when empty */
      }
    });
    inc.addEventListener('click', () => {
      input.focus();
      try {
        input.stepUp();
      } catch (e) {
        /* ignore */
      }
    });
  });
}

function initEmergencyMacWhitelistUi() {
  const addBtn = document.getElementById('emergencyMacAddBtn');
  const input = document.getElementById('emergencyMacInput');
  if (addBtn) {
    addBtn.addEventListener('click', tryAddEmergencyMacFromInput);
  }
  if (input) {
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') {
        e.preventDefault();
        tryAddEmergencyMacFromInput();
      }
    });
  }
}

function collectPayload() {
  recalcVoltmeterCalibration();
  const payload = {};

  FIELD_IDS.forEach((id) => {
    const el = document.getElementById(id);
    if (!el) return;
    if (el.type === 'checkbox') {
      payload[id] = el.checked;
      return;
    }

    const numTypes = ['number'];
    if (numTypes.includes(el.type)) {
      payload[id] = el.value === '' ? null : Number(el.value);
    } else {
      payload[id] = el.value;
    }
  });

  payload.authPin = getAuthPin();

  payload.emergencySwitchMacs = getEmergencyMacPayloadList();

  return payload;
}

function getAuthPin() {
  return sessionStorage.getItem(SETTINGS_AUTH_STORAGE_KEY) || '';
}

function setStoredAuthPin(pin) {
  const s = pin != null ? String(pin).trim() : '';
  if (s.length > 0) {
    sessionStorage.setItem(SETTINGS_AUTH_STORAGE_KEY, s);
  } else {
    sessionStorage.removeItem(SETTINGS_AUTH_STORAGE_KEY);
  }
}

function tryUnlockWithPin(pin) {
  return fetch('/settings/unlock', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ pin: pin })
  }).then(async (res) => {
    let data = {};
    try {
      data = await res.json();
    } catch (e) {
      /* ignore */
    }
    return { ok: res.ok && data.success === true, data };
  });
}

function applyDeviceBanner(data) {
  if (!data) {
    return;
  }
  const macEl = document.getElementById('deviceMac');
  const verEl = document.getElementById('deviceVersion');
  if (macEl) {
    macEl.textContent = data.mac && String(data.mac).length
      ? normalizeMacDisplay(data.mac)
      : ESP_DAEMON_PLACEHOLDER_MAC;
  }
  if (verEl) {
    verEl.textContent = data.version && String(data.version).length ? data.version : ESP_DAEMON_PLACEHOLDER_VERSION;
  }
}

function showSettingsApp(skipLoad) {
  const gate = document.getElementById('settingsPinGate');
  const main = document.getElementById('settingsMain');
  const previewNote = document.getElementById('settingsGatePreviewNote');
  if (previewNote) {
    previewNote.hidden = true;
  }
  if (gate) {
    gate.hidden = true;
  }
  if (main) {
    main.hidden = false;
  }
  if (!skipLoad) {
    loadSettings();
  }
}

function showPinGate() {
  const gate = document.getElementById('settingsPinGate');
  const main = document.getElementById('settingsMain');
  if (gate) {
    gate.hidden = false;
  }
  if (main) {
    main.hidden = true;
  }
  const errEl = document.getElementById('settingsGateError');
  if (errEl) {
    errEl.textContent = '';
  }
  const input = document.getElementById('settingsGatePinInput');
  if (input) {
    input.value = '';
    input.focus();
  }
}

function unlockSettingsGate() {
  const input = document.getElementById('settingsGatePinInput');
  const errEl = document.getElementById('settingsGateError');
  if (!input || !errEl) {
    return;
  }
  errEl.textContent = '';
  const pin = String(input.value).trim();
  tryUnlockWithPin(pin).then(({ ok, data }) => {
    if (ok) {
      setStoredAuthPin(pin);
      input.value = '';
      showSettingsApp();
    } else {
      errEl.textContent = (data && data.message) || 'Invalid PIN';
    }
  }).catch(() => {
    errEl.textContent = 'Request failed';
  });
}

async function bootstrapSettingsAccess() {
  if (new URLSearchParams(window.location.search).has('previewpin')) {
    setStoredAuthPin('');
    showPinGate();
    const note = document.getElementById('settingsGatePreviewNote');
    if (note) {
      note.hidden = false;
    }
    return;
  }

  // HTML inline may set this so Go Live work even when settings.js is browser-cached
  if (window.__ESP_SETTINGS_SKIP_PIN_GATE__ === true) {
    setStoredAuthPin('');
    showSettingsApp(true);
    setPreviewHint('Go Live / local preview (no ESP). Open this page from the device IP to load or save.');
    return;
  }

  if (isLocalUiPreview()) {
    setStoredAuthPin('');
    showSettingsApp(true);
    setPreviewHint('Go Live / local preview (no ESP). Open this page from the device IP to load or save.');
    return;
  }

  let raw = null;
  try {
    const res = await fetch('/device');
    if (res.ok) {
      const text = await res.text();
      if (text) {
        try {
          raw = JSON.parse(text);
        } catch (parseErr) {
          raw = null;
        }
      }
    }
  } catch (e) {
    showSettingsApp(true);
    return;
  }

  const data = looksLikeEspDevicePayload(raw) ? raw : null;

  applyDeviceBanner(data);

  if (!data) {
    setStoredAuthPin('');
    showSettingsApp(true);
    setPreviewHint('No ESP-Daemon on this host (/device missing or invalid). PIN skipped for UI preview.');
    return;
  }

  const pinRequired = !!data.settingsPinRequired;
  if (!pinRequired) {
    setStoredAuthPin('');
    showSettingsApp(false);
    return;
  }

  const cached = getAuthPin();
  if (cached) {
    const { ok } = await tryUnlockWithPin(cached);
    if (ok) {
      showSettingsApp(false);
      return;
    }
    setStoredAuthPin('');
  }
  showPinGate();
}

function loadSettings() {
  fetch('/settings/read', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: getAuthPin() })
  })
    .then((res) => {
      return res.json().then((data) => ({ ok: res.ok, data }));
    })
    .then(({ ok, data }) => {
      if (!ok) {
        throw new Error(data.message || 'Failed to fetch settings');
      }
      FIELD_IDS.forEach((id) => setFieldValue(id, data[id]));
      recalcVoltmeterCalibration();
      applyEmergencyMacListFromServer(data.emergencySwitchMacs);
      setStatus('Settings loaded', true);
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

function saveSettings() {
  const pinProt = document.getElementById('pinProtectionEnabled').checked;
  const pc = String(document.getElementById('pinCode').value || '').trim();
  if (pinProt && pc.length > 0) {
    if (!/^\d+$/.test(pc)) {
      setStatus('PIN code must contain only digits (0-9)', false);
      return;
    }
    if (pc.length < 4 || pc.length > 32) {
      setStatus('PIN code must be 4-32 digits', false);
      return;
    }
  }

  const payload = collectPayload();
  fetch('/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  })
    .then(async (res) => {
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.success) {
        throw new Error(data.message || 'Save failed');
      }
      return data;
    })
    .then((data) => {
      setStatus(data.message || 'Saved', true);
      if (!pinProt) {
        setStoredAuthPin('');
      } else if (pc.length >= 4) {
        // PIN changed in this save, promote new PIN for follow-up requests.
        setStoredAuthPin(pc);
      } else {
        // Keep current unlocked PIN when pinCode field is blank (masked/not edited).
        setStoredAuthPin(getAuthPin());
      }
      loadSettings();
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

function exportSettings() {
  fetch('/settings/export', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: getAuthPin() })
  })
    .then(async (res) => {
      if (!res.ok) {
        const data = await res.json().catch(() => ({}));
        throw new Error(data.message || 'Export failed');
      }
      return res.blob();
    })
    .then((blob) => {
      const url = URL.createObjectURL(blob);
      const stamp = getExportTimestampUtcCompact();
      const deviceName = getDeviceNameForExportFilename();
      const link = document.createElement('a');
      link.href = url;
      link.download = 'esp-daemon_settings_' + deviceName + '_' + stamp + '.json';
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
      setStatus('Settings exported', true);
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

function resetDefaultsSettings() {
  fetch('/settings/reset', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: getAuthPin() })
  })
    .then(async (res) => {
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.success) {
        throw new Error(data.message || 'Factory reset failed');
      }
      return data;
    })
    .then(() => {
      setStatus('Factory reset complete', true);
      // Re-bootstrap so PIN gate reflects new defaults.
      setTimeout(() => window.location.reload(), 300);
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

function rebootESP() {
  fetch('/esp/reboot', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: getAuthPin() })
  })
    .then(async (res) => {
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.success) {
        throw new Error(data.message || 'Reboot failed');
      }
      setStatus(data.message || 'Rebooting...', true);
      // ESP will reboot soon; keep UI feedback short.
      setTimeout(() => window.location.reload(), 1500);
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

function importSettingsFromText(rawText) {
  let imported;
  try {
    imported = JSON.parse(rawText);
  } catch (err) {
    setStatus('Import JSON parse failed', false);
    return;
  }

  fetch('/settings/import', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      authPin: getAuthPin(),
      settings: imported
    })
  })
    .then(async (res) => {
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.success) {
        throw new Error(data.message || 'Import failed');
      }
      return data;
    })
    .then((data) => {
      setStatus(data.message || 'Imported', true);
      loadSettings();
    })
    .catch((err) => {
      setStatus(err.message, false);
    });
}

document.addEventListener('DOMContentLoaded', () => {
  const modeToggle = document.getElementById('modeToggle');
  const savedTheme = localStorage.getItem('theme') || 'dark';
  applyTheme(savedTheme);

  initSpeedDial();

  initNumericStepperControls();

  if (modeToggle) {
    modeToggle.addEventListener('click', () => {
      const isLight = document.documentElement.classList.toggle('light-mode');
      localStorage.setItem('theme', isLight ? 'light' : 'dark');
      applyTheme(isLight ? 'light' : 'dark');
      closeSpeedDial();
    });
  }

  document.getElementById('backBtn').addEventListener('click', () => {
    closeSpeedDial();
    window.location.href = '/';
  });

  document.getElementById('saveBtn').addEventListener('click', saveSettings);
  const resetDefaultsBtn = document.getElementById('resetDefaultsBtn');
  if (resetDefaultsBtn) {
    resetDefaultsBtn.addEventListener('click', resetDefaultsSettings);
  }
  document.getElementById('exportBtn').addEventListener('click', exportSettings);
  document.getElementById('importBtn').addEventListener('click', () => {
    document.getElementById('importFile').click();
  });
  const espRebootBtn = document.getElementById('espRebootBtn');
  if (espRebootBtn) {
    espRebootBtn.addEventListener('click', rebootESP);
  }
  document.getElementById('importFile').addEventListener('change', (event) => {
    const file = event.target.files && event.target.files[0];
    if (!file) {
      return;
    }
    const reader = new FileReader();
    reader.onload = () => importSettingsFromText(String(reader.result || ''));
    reader.onerror = () => setStatus('Failed to read import file', false);
    reader.readAsText(file);
    event.target.value = '';
  });
  document.getElementById('voltageDividerR1').addEventListener('input', recalcVoltmeterCalibration);
  document.getElementById('voltageDividerR2').addEventListener('input', recalcVoltmeterCalibration);

  initEmergencyMacWhitelistUi();

  const gateUnlockBtn = document.getElementById('settingsGateUnlockBtn');
  const gatePinInput = document.getElementById('settingsGatePinInput');
  if (gateUnlockBtn) {
    gateUnlockBtn.addEventListener('click', unlockSettingsGate);
  }
  if (gatePinInput) {
    gatePinInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        unlockSettingsGate();
      }
    });
  }

  bootstrapSettingsAccess().catch(() => {
    showSettingsApp(true);
  });
});
