const ESP_DAEMON_PLACEHOLDER_MAC = '00-00-00-00-00-00';
const ESP_DAEMON_PLACEHOLDER_VERSION = '0.0.0';
const SAVE_STATUS_BASE_CLASS = 'actions__status';
const SAVE_STATUS_FADE_CLASS = 'actions__status--fade-out';
const SAVE_STATUS_AUTO_HIDE_MS = 3000;
const SAVE_STATUS_FADE_MS = 240;
const ESTOP_ROUTE_MAX = 16;
const ESTOP_SETTINGS_READ_ENDPOINT = '/estop/settings/read';
const ESTOP_SETTINGS_WRITE_ENDPOINT = '/estop/settings';
const ESTOP_SETTINGS_EXPORT_ENDPOINT = '/estop/settings/export';
const ESTOP_SETTINGS_IMPORT_ENDPOINT = '/estop/settings/import';
const ESTOP_SETTINGS_RESET_ENDPOINT = '/estop/settings/reset';
const ESTOP_SETTINGS_FACTORY_RESET_ENDPOINT = '/estop/settings/factory-reset';
const STATUS_POLL_INTERVAL_VISIBLE_MS = 1000;
const STATUS_POLL_INTERVAL_HIDDEN_MS = 3000;

let estopStatusPollTimer = null;
let estopStatusPollInFlight = false;
let estopSaveStatusHideTimer = null;
let estopSaveStatusClearTimer = null;

function normalizeMacDisplay(mac) {
  return String(mac || '').replace(/:/g, '-').toUpperCase();
}

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
  return String(macColon || '').replace(/:/g, '-').toUpperCase();
}

function sanitizeFilenamePart(raw, fallback) {
  const s = String(raw == null ? '' : raw).trim();
  const cleaned = s
    .replace(/[^a-zA-Z0-9._-]+/g, '_')
    .replace(/_+/g, '_')
    .replace(/^[_\.-]+|[_\.-]+$/g, '');
  return cleaned || fallback;
}

function stripRedundantEstopPrefix(rawName) {
  const s = String(rawName || '');
  const stripped = s
    .replace(/^(?:esp[-_]?estop|estop)[-_]*/i, '')
    .replace(/^[_\.-]+|[_\.-]+$/g, '');
  return stripped || 'device';
}

function getDeviceNameForExportFilename() {
  const hostWithoutLocal = String(window.location.hostname || '')
    .trim()
    .replace(/\.local$/i, '');
  const hostName = sanitizeFilenamePart(hostWithoutLocal, 'device');
  return stripRedundantEstopPrefix(hostName);
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

function setStatus(msg, ok) {
  const el = document.getElementById('saveStatus');
  if (!el) {
    return;
  }
  if (estopSaveStatusHideTimer) {
    clearTimeout(estopSaveStatusHideTimer);
    estopSaveStatusHideTimer = null;
  }
  if (estopSaveStatusClearTimer) {
    clearTimeout(estopSaveStatusClearTimer);
    estopSaveStatusClearTimer = null;
  }
  el.classList.remove(SAVE_STATUS_FADE_CLASS);
  if (!msg) {
    el.textContent = '';
    el.className = SAVE_STATUS_BASE_CLASS;
    return;
  }
  el.textContent = msg;
  el.className = SAVE_STATUS_BASE_CLASS + ' ' + (ok ? 'ok' : 'err');
  estopSaveStatusHideTimer = setTimeout(function() {
    const statusEl = document.getElementById('saveStatus');
    if (!statusEl) {
      return;
    }
    statusEl.classList.add(SAVE_STATUS_FADE_CLASS);
    estopSaveStatusClearTimer = setTimeout(function() {
      const clearEl = document.getElementById('saveStatus');
      if (!clearEl) {
        return;
      }
      clearEl.textContent = '';
      clearEl.className = SAVE_STATUS_BASE_CLASS;
      clearEl.classList.remove(SAVE_STATUS_FADE_CLASS);
      estopSaveStatusClearTimer = null;
    }, SAVE_STATUS_FADE_MS);
    estopSaveStatusHideTimer = null;
  }, SAVE_STATUS_AUTO_HIDE_MS);
}

function updateThemeButtonMeta(isLightMode) {
  const modeToggle = document.getElementById('modeToggle');
  if (!modeToggle) {
    return;
  }
  const nextMode = isLightMode ? 'dark' : 'light';
  modeToggle.setAttribute('aria-label', 'Switch to ' + nextMode + ' mode');
}

function applyTheme(theme) {
  const isLight = theme === 'light';
  document.documentElement.classList.toggle('light-mode', isLight);
  updateThemeButtonMeta(isLight);
}

function initThemeToggle() {
  const modeToggle = document.getElementById('modeToggle');
  const savedTheme = localStorage.getItem('theme') || 'dark';
  applyTheme(savedTheme);

  if (!modeToggle) {
    return;
  }

  modeToggle.addEventListener('click', function() {
    const isLight = !document.documentElement.classList.contains('light-mode');
    applyTheme(isLight ? 'light' : 'dark');
    localStorage.setItem('theme', isLight ? 'light' : 'dark');
  });
}

function initNumericStepperControls(root) {
  const scope = root || document;
  scope.querySelectorAll('.settings-card input[type="number"]:not([readonly])').forEach(function(input) {
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

    dec.addEventListener('click', function() {
      input.focus();
      try {
        input.stepDown();
      } catch (e) {
        // Keep behavior stable when value is temporarily invalid or empty.
      }
    });

    inc.addEventListener('click', function() {
      input.focus();
      try {
        input.stepUp();
      } catch (e) {
        // Keep behavior stable when value is temporarily invalid or empty.
      }
    });
  });
}

function setPillState(el, text, okState) {
  if (!el) {
    return;
  }
  el.textContent = text;
  el.classList.remove('power-on', 'power-off');
  if (okState === true) {
    el.classList.add('power-on');
  } else if (okState === false) {
    el.classList.add('power-off');
  }
}

function normalizeUrlNoTrailingSlash(raw) {
  const value = String(raw || '').trim();
  if (!value) {
    return '';
  }
  return value.replace(/\/+$/, '');
}

function mapWledStatusText(statusRaw, snapshotReady) {
  const status = String(statusRaw || '').trim().toLowerCase();
  const hasSnapshot = !!snapshotReady;
  switch (status) {
    case 'ready':
      return hasSnapshot ? 'ready (snapshot)' : 'ready';
    case 'disabled':
      return 'disabled';
    case 'wifi_disconnected':
      return 'wifi disconnected';
    case 'url_empty':
    case 'restore_url_empty':
      return 'url empty';
    case 'snapshot_get_failed':
      return 'snapshot failed';
    case 'snapshot_invalid':
      return 'snapshot invalid';
    case 'estop_preset_applied':
      return 'estop preset active';
    case 'preset_apply_failed':
      return 'preset apply failed';
    case 'restore_wait_wifi':
      return 'restore waiting wifi';
    case 'restore_failed_retrying':
      return 'restore retrying';
    case 'restored':
      return 'restored';
    case 'no_snapshot':
      return 'no snapshot';
    default:
      return status || '-';
  }
}

function parseJsonSafe(res) {
  return res.text().then(function(t) {
    if (!t) {
      return {};
    }
    try {
      return JSON.parse(t);
    } catch (e) {
      return {};
    }
  });
}

function updateDeviceBanner(data) {
  const macEl = document.getElementById('deviceMac');
  const verEl = document.getElementById('deviceVersion');

  if (macEl) {
    macEl.textContent = data && data.mac ? normalizeMacDisplay(data.mac) : ESP_DAEMON_PLACEHOLDER_MAC;
  }
  if (verEl) {
    verEl.textContent = data && data.version ? data.version : ESP_DAEMON_PLACEHOLDER_VERSION;
  }
}

function loadDeviceInfo() {
  return fetch('/device')
    .then(function(res) {
      if (!res.ok) {
        throw new Error('device fetch failed');
      }
      return parseJsonSafe(res);
    })
    .then(function(data) {
      updateDeviceBanner(data);
    })
    .catch(function() {
      updateDeviceBanner(null);
    });
}

function getRouteListEl() {
  return document.getElementById('estopRouteList');
}

function setRouteError(msg) {
  const el = document.getElementById('estopRouteError');
  if (!el) {
    return;
  }
  el.textContent = msg || '';
}

function clearRouteError() {
  setRouteError('');
}

function defaultRouteConfig() {
  return {
    targetMac: '',
    switchPin: 4,
    switchActiveHigh: false,
    switchLogicInverted: false
  };
}

function getRouteItems() {
  const list = getRouteListEl();
  if (!list) {
    return [];
  }
  return Array.from(list.querySelectorAll('.estop-route-item'));
}

function updateRouteLabels() {
  getRouteItems().forEach(function(item, idx) {
    const title = item.querySelector('.estop-route-item__title');
    if (title) {
      title.textContent = 'Route ' + String(idx + 1);
    }
  });
}

function buildRouteItem(routeRaw) {
  const route = Object.assign(defaultRouteConfig(), routeRaw || {});
  const uid = 'route_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 8);

  const li = document.createElement('li');
  li.className = 'estop-route-item';
  li.innerHTML = [
    '<div class="estop-route-item__head">',
    '  <div class="estop-route-item__meta">',
    '    <span class="estop-route-item__title">Route</span>',
    '    <span class="status-pill estop-route-live">pending</span>',
    '  </div>',
    '  <button type="button" class="btn estop-route-remove">Remove</button>',
    '</div>',
    '<div class="estop-route-item__fields">',
    '  <div class="field">',
    '    <label for="' + uid + '_target">ESP-Daemon MAC (target)</label>',
    '    <input id="' + uid + '_target" class="estop-route-target" type="text" inputmode="text" autocomplete="off" placeholder="AA-BB-CC-DD-EE-FF" maxlength="17" spellcheck="false">',
    '  </div>',
    '  <div class="field">',
    '    <label for="' + uid + '_pin">E-Stop GPIO</label>',
    '    <input id="' + uid + '_pin" class="estop-route-pin" type="number" min="0" max="48">',
    '  </div>',
    '  <div class="settings-toggle-stack settings-toggle-stack--compact">',
    '    <div class="settings-switch-row">',
    '      <label class="settings-switch-label" for="' + uid + '_active">Switch active when HIGH</label>',
    '      <label class="switch-control">',
    '        <input id="' + uid + '_active" class="estop-route-active-high" type="checkbox">',
    '        <span class="switch-slider"></span>',
    '      </label>',
    '    </div>',
    '    <div class="settings-switch-row">',
    '      <label class="settings-switch-label" for="' + uid + '_invert">Invert pressed/released logic</label>',
    '      <label class="switch-control">',
    '        <input id="' + uid + '_invert" class="estop-route-logic-inverted" type="checkbox">',
    '        <span class="switch-slider"></span>',
    '      </label>',
    '    </div>',
    '  </div>',
    '</div>'
  ].join('');

  const normalizedMac = normalizeMacColonFromAny(route.targetMac || '');
  const macInput = li.querySelector('.estop-route-target');
  const pinInput = li.querySelector('.estop-route-pin');
  const activeHighInput = li.querySelector('.estop-route-active-high');
  const logicInvertedInput = li.querySelector('.estop-route-logic-inverted');

  if (macInput) {
    macInput.value = normalizedMac ? macColonToDisplayHyphen(normalizedMac) : String(route.targetMac || '').toUpperCase();
  }
  if (pinInput) {
    pinInput.value = route.switchPin != null ? String(route.switchPin) : '4';
  }
  if (activeHighInput) {
    activeHighInput.checked = !!route.switchActiveHigh;
  }
  if (logicInvertedInput) {
    logicInvertedInput.checked = !!route.switchLogicInverted;
  }

  const removeBtn = li.querySelector('.estop-route-remove');
  if (removeBtn) {
    removeBtn.addEventListener('click', function() {
      const items = getRouteItems();
      if (items.length <= 1) {
        setRouteError('At least 1 route is required.');
        return;
      }
      li.remove();
      updateRouteLabels();
      clearRouteError();
    });
  }

  return li;
}

function addRoute(routeRaw) {
  const list = getRouteListEl();
  if (!list) {
    return;
  }
  const items = getRouteItems();
  if (items.length >= ESTOP_ROUTE_MAX) {
    setRouteError('Maximum ' + ESTOP_ROUTE_MAX + ' routes.');
    return;
  }

  const item = buildRouteItem(routeRaw);
  list.appendChild(item);
  initNumericStepperControls(item);
  updateRouteLabels();
  clearRouteError();
}

function clearRouteList() {
  const list = getRouteListEl();
  if (!list) {
    return;
  }
  list.innerHTML = '';
}

function applyRoutesFromSettings(data) {
  const routes = Array.isArray(data.estopRoutes) && data.estopRoutes.length > 0
    ? data.estopRoutes
    : [{
      targetMac: data.estopTargetMac || '',
      switchPin: data.estopSwitchPin != null ? data.estopSwitchPin : 4,
      switchActiveHigh: !!data.estopSwitchActiveHigh,
      switchLogicInverted: !!data.estopSwitchLogicInverted
    }];

  clearRouteList();
  routes.slice(0, ESTOP_ROUTE_MAX).forEach(function(route) {
    addRoute(route);
  });

  if (getRouteItems().length === 0) {
    addRoute(defaultRouteConfig());
  }
}

function normalizeRouteFromDom(item, idx) {
  const macInput = item.querySelector('.estop-route-target');
  const pinInput = item.querySelector('.estop-route-pin');
  const activeHighInput = item.querySelector('.estop-route-active-high');
  const logicInvertedInput = item.querySelector('.estop-route-logic-inverted');

  const routeId = 'Route ' + String(idx + 1);

  const macRaw = macInput ? String(macInput.value || '').trim() : '';
  const normalizedMac = macRaw.length > 0 ? normalizeMacColonFromAny(macRaw) : '';
  if (macRaw.length > 0 && !normalizedMac) {
    throw new Error(routeId + ': invalid MAC format. Use AA-BB-CC-DD-EE-FF.');
  }

  const pinValue = pinInput ? Number(pinInput.value) : NaN;
  if (!Number.isFinite(pinValue) || pinValue < 0 || pinValue > 48) {
    throw new Error(routeId + ': GPIO must be between 0 and 48.');
  }

  return {
    targetMac: normalizedMac || '',
    switchPin: Math.round(pinValue),
    switchActiveHigh: !!(activeHighInput && activeHighInput.checked),
    switchLogicInverted: !!(logicInvertedInput && logicInvertedInput.checked)
  };
}

function collectRoutesPayload() {
  const items = getRouteItems();
  if (items.length === 0) {
    throw new Error('At least 1 route is required.');
  }
  if (items.length > ESTOP_ROUTE_MAX) {
    throw new Error('Maximum ' + ESTOP_ROUTE_MAX + ' routes.');
  }

  const routes = items.map(function(item, idx) {
    return normalizeRouteFromDom(item, idx);
  });

  for (let i = 0; i < routes.length; i++) {
    for (let j = i + 1; j < routes.length; j++) {
      const a = routes[i];
      const b = routes[j];
      if (a.targetMac === b.targetMac &&
          a.switchPin === b.switchPin &&
          a.switchActiveHigh === b.switchActiveHigh &&
          a.switchLogicInverted === b.switchLogicInverted) {
        throw new Error('Route ' + String(i + 1) + ' and Route ' + String(j + 1) + ' are duplicated.');
      }
    }
  }

  return routes;
}

function applyRouteRuntimeStatus(statusRoutes) {
  const rows = getRouteItems();
  rows.forEach(function(item, idx) {
    const pill = item.querySelector('.estop-route-live');
    if (!pill) {
      return;
    }

    const st = Array.isArray(statusRoutes) ? statusRoutes[idx] : null;
    if (!st) {
      setPillState(pill, 'pending', false);
      return;
    }

    if (st.peerConfigured) {
      setPillState(pill, 'online', true);
      return;
    }

    const targetMac = normalizeMacColonFromAny(st.targetMac || '');
    if (targetMac) {
      setPillState(pill, 'waiting', false);
    } else {
      setPillState(pill, 'no target', false);
    }
  });
}

function loadSettings() {
  return fetch(ESTOP_SETTINGS_READ_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok) {
        throw new Error(result.data.message || 'Failed to load settings');
      }

      applyRoutesFromSettings(result.data);

      const buzzerEnabledInput = document.getElementById('estopBuzzerEnabled');
      const buzzerPinInput = document.getElementById('estopBuzzerPin');
      const wledEnabledInput = document.getElementById('estopWledEnabled');
      const wledBaseUrlInput = document.getElementById('estopWledBaseUrl');
      const wledPresetInput = document.getElementById('estopWledPreset');

      if (buzzerEnabledInput) {
        buzzerEnabledInput.checked = !!result.data.estopBuzzerEnabled;
      }
      if (buzzerPinInput) {
        buzzerPinInput.value = result.data.estopBuzzerPin != null ? String(result.data.estopBuzzerPin) : '5';
      }
      if (wledEnabledInput) {
        wledEnabledInput.checked = !!result.data.estopWledEnabled;
      }
      if (wledBaseUrlInput) {
        wledBaseUrlInput.value = normalizeUrlNoTrailingSlash(result.data.estopWledBaseUrl || '');
      }
      if (wledPresetInput) {
        wledPresetInput.value = result.data.estopWledPreset != null ? String(result.data.estopWledPreset) : '1';
      }
    });
}

function refreshEStopStatus() {
  return fetch('/estop/status')
    .then(function(res) {
      if (!res.ok) {
        throw new Error('status fetch failed');
      }
      return parseJsonSafe(res);
    })
    .then(function(data) {
      const switchState = document.getElementById('switchState');
      const rawEl = document.getElementById('switchRaw');
      const channelEl = document.getElementById('espNowChannelRuntime');
      const peerStatusEl = document.getElementById('peerStatus');
      const routeStatusEl = document.getElementById('routeStatus');
      const packetEl = document.getElementById('packetCount');
      const wledEl = document.getElementById('wledStatus');

      const pressed = !!data.pressed;
      const routeCount = Number(data.routeCount || 0);
      const pressedRouteCount = Number(data.pressedRouteCount || 0);
      const configuredPeerCount = Number(data.configuredPeerCount || 0);

      setPillState(switchState, pressed ? 'PRESSED (STOP)' : 'RELEASED', !pressed);

      if (rawEl) {
        if (routeCount > 1) {
          rawEl.textContent = String(pressedRouteCount) + '/' + String(routeCount) + ' pressed';
        } else {
          rawEl.textContent = data.rawLevel != null ? String(data.rawLevel) : '-';
        }
      }
      if (channelEl) {
        channelEl.textContent = data.espNowChannel != null ? String(data.espNowChannel) : '-';
      }
      if (packetEl) {
        packetEl.textContent = data.packetsSent != null ? String(data.packetsSent) : '0';
      }

      if (routeStatusEl) {
        const routeOk = routeCount > 0 && pressedRouteCount === 0;
        setPillState(routeStatusEl, String(pressedRouteCount) + '/' + String(routeCount) + ' active', routeOk);
      }

      if (routeCount === 0) {
        setPillState(peerStatusEl, 'not configured', false);
      } else if (configuredPeerCount === routeCount) {
        setPillState(peerStatusEl, String(configuredPeerCount) + '/' + String(routeCount) + ' online', true);
      } else if (configuredPeerCount > 0) {
        setPillState(peerStatusEl, String(configuredPeerCount) + '/' + String(routeCount) + ' online', false);
      } else {
        setPillState(peerStatusEl, 'waiting', false);
      }

      const snapshotReady = !!data.wledSnapshotReady;
      const wledText = mapWledStatusText(data.wledStatus, snapshotReady);
      const wledOk = wledText === 'ready' || wledText === 'ready (snapshot)' || wledText === 'restored';
      setPillState(wledEl, wledText, wledOk);

      applyRouteRuntimeStatus(data.routes);
    })
    .catch(function() {
      setPillState(document.getElementById('switchState'), 'offline', false);
      setPillState(document.getElementById('peerStatus'), 'offline', false);
      setPillState(document.getElementById('routeStatus'), 'offline', false);
      setPillState(document.getElementById('wledStatus'), 'offline', false);
    });
}

function getStatusPollIntervalMs() {
  return document.hidden ? STATUS_POLL_INTERVAL_HIDDEN_MS : STATUS_POLL_INTERVAL_VISIBLE_MS;
}

function scheduleNextStatusPoll(delayMs) {
  if (estopStatusPollTimer) {
    clearTimeout(estopStatusPollTimer);
  }
  const delay = Number.isFinite(delayMs) ? Math.max(0, delayMs) : getStatusPollIntervalMs();
  estopStatusPollTimer = setTimeout(runStatusPollTick, delay);
}

function runStatusPollTick() {
  if (estopStatusPollInFlight) {
    scheduleNextStatusPoll(getStatusPollIntervalMs());
    return;
  }

  estopStatusPollInFlight = true;
  refreshEStopStatus()
    .then(function() {
      estopStatusPollInFlight = false;
      scheduleNextStatusPoll(getStatusPollIntervalMs());
    })
    .catch(function() {
      estopStatusPollInFlight = false;
      scheduleNextStatusPoll(getStatusPollIntervalMs());
    });
}

function collectSavePayload() {
  const routes = collectRoutesPayload();
  const buzzerEnabledInput = document.getElementById('estopBuzzerEnabled');
  const buzzerPinInput = document.getElementById('estopBuzzerPin');
  const wledEnabledInput = document.getElementById('estopWledEnabled');
  const wledBaseUrlInput = document.getElementById('estopWledBaseUrl');
  const wledPresetInput = document.getElementById('estopWledPreset');

  const buzzerEnabled = !!(buzzerEnabledInput && buzzerEnabledInput.checked);
  const buzzerPinValue = buzzerPinInput ? Number(buzzerPinInput.value) : NaN;
  if (!Number.isFinite(buzzerPinValue) || buzzerPinValue < 0 || buzzerPinValue > 48) {
    throw new Error('Buzzer GPIO must be between 0 and 48.');
  }

  const wledEnabled = !!(wledEnabledInput && wledEnabledInput.checked);
  const wledBaseUrl = normalizeUrlNoTrailingSlash(wledBaseUrlInput ? wledBaseUrlInput.value : '');
  if (wledEnabled) {
    if (!wledBaseUrl) {
      throw new Error('WLED Base URL is required when WLED Sync is enabled.');
    }
    if (!/^https?:\/\//i.test(wledBaseUrl)) {
      throw new Error('WLED Base URL must start with http:// or https://');
    }
  }

  const wledPresetValue = wledPresetInput ? Number(wledPresetInput.value) : NaN;
  if (!Number.isFinite(wledPresetValue) || wledPresetValue < 1 || wledPresetValue > 250) {
    throw new Error('WLED preset must be between 1 and 250.');
  }

  const firstRoute = routes[0] || defaultRouteConfig();

  return {
    authPin: '',
    estopRoutes: routes,
    estopTargetMac: firstRoute.targetMac,
    estopSwitchPin: firstRoute.switchPin,
    estopSwitchActiveHigh: firstRoute.switchActiveHigh,
    estopSwitchLogicInverted: firstRoute.switchLogicInverted,
    estopBuzzerEnabled: buzzerEnabled,
    estopBuzzerPin: Math.round(buzzerPinValue),
    estopWledEnabled: wledEnabled,
    estopWledBaseUrl: wledBaseUrl,
    estopWledPreset: Math.round(wledPresetValue)
  };
}

function saveSettings() {
  let payload;
  try {
    payload = collectSavePayload();
  } catch (err) {
    setStatus(err.message || 'Invalid input', false);
    return;
  }

  fetch(ESTOP_SETTINGS_WRITE_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok || result.data.success !== true) {
        throw new Error(result.data.message || 'Save failed');
      }
      setStatus(result.data.message || 'Settings saved', true);
      return Promise.all([loadSettings(), refreshEStopStatus()]);
    })
    .catch(function(err) {
      setStatus(err.message || 'Save failed', false);
    });
}

function exportSettings() {
  fetch(ESTOP_SETTINGS_EXPORT_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then(function(res) {
      if (!res.ok) {
        return parseJsonSafe(res).then(function(data) {
          throw new Error(data.message || 'Backup failed');
        });
      }
      return res.text();
    })
    .then(function(rawText) {
      let exported = {};
      try {
        exported = JSON.parse(rawText || '{}');
      } catch (err) {
        throw new Error('Backup payload is not valid JSON');
      }

      const now = new Date();
      exported.exportedAt = getExportTimestampUtcIso(now);

      const blob = new Blob([JSON.stringify(exported, null, 2)], { type: 'application/json' });
      const link = document.createElement('a');
      link.href = URL.createObjectURL(blob);
      link.download = 'esp-estop_' + getDeviceNameForExportFilename() + '_settings_' + getExportTimestampUtcCompact(now) + '.json';
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(link.href);
      setStatus('Backup exported', true);
    })
    .catch(function(err) {
      setStatus(err.message || 'Backup failed', false);
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

  fetch(ESTOP_SETTINGS_IMPORT_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      authPin: '',
      settings: imported
    })
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok || result.data.success !== true) {
        throw new Error(result.data.message || 'Import failed');
      }
      setStatus(result.data.message || 'Backup restored', true);
      return Promise.all([loadSettings(), refreshEStopStatus()]);
    })
    .catch(function(err) {
      setStatus(err.message || 'Import failed', false);
    });
}

function restoreDefaultsSettings() {
  const confirmed = window.confirm('Restore E-STOP settings to defaults?');
  if (!confirmed) {
    return;
  }

  fetch(ESTOP_SETTINGS_RESET_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok || result.data.success !== true) {
        throw new Error(result.data.message || 'Restore defaults failed');
      }
      setStatus(result.data.message || 'Defaults restored', true);
      return Promise.all([loadSettings(), refreshEStopStatus()]);
    })
    .catch(function(err) {
      setStatus(err.message || 'Restore defaults failed', false);
    });
}

function factoryResetSettings() {
  const confirmed = window.confirm('Factory reset will erase full NVS (including Wi-Fi credentials). Continue?');
  if (!confirmed) {
    return;
  }

  fetch(ESTOP_SETTINGS_FACTORY_RESET_ENDPOINT, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok || result.data.success !== true) {
        throw new Error(result.data.message || 'Factory reset failed');
      }
      setStatus(result.data.message || 'Factory reset complete. Device rebooting...', true);
      setTimeout(function() {
        window.location.reload();
      }, 1500);
    })
    .catch(function(err) {
      setStatus(err.message || 'Factory reset failed', false);
    });
}

function rebootDevice() {
  const confirmed = window.confirm('Reboot this device now?');
  if (!confirmed) {
    return;
  }

  fetch('/esp/reboot', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ authPin: '' })
  })
    .then(function(res) {
      return parseJsonSafe(res).then(function(data) {
        return { ok: res.ok, data: data };
      });
    })
    .then(function(result) {
      if (!result.ok || result.data.success !== true) {
        throw new Error(result.data.message || 'Reboot failed');
      }
      setStatus(result.data.message || 'Rebooting...', true);
    })
    .catch(function(err) {
      setStatus(err.message || 'Reboot failed', false);
    });
}

function initRouteEditorUi() {
  const addBtn = document.getElementById('estopRouteAddBtn');
  if (addBtn) {
    addBtn.addEventListener('click', function() {
      addRoute(defaultRouteConfig());
    });
  }
}

document.addEventListener('DOMContentLoaded', function() {
  initNumericStepperControls(document);
  initThemeToggle();
  initRouteEditorUi();

  const saveBtn = document.getElementById('saveBtn');
  const restoreDefaultsBtn = document.getElementById('restoreDefaultsBtn');
  const factoryResetBtn = document.getElementById('factoryResetBtn');
  const exportBtn = document.getElementById('exportBtn');
  const importBtn = document.getElementById('importBtn');
  const importFile = document.getElementById('importFile');
  const rebootBtn = document.getElementById('rebootBtn');

  if (saveBtn) {
    saveBtn.addEventListener('click', saveSettings);
  }
  if (restoreDefaultsBtn) {
    restoreDefaultsBtn.addEventListener('click', restoreDefaultsSettings);
  }
  if (factoryResetBtn) {
    factoryResetBtn.addEventListener('click', factoryResetSettings);
  }
  if (exportBtn) {
    exportBtn.addEventListener('click', exportSettings);
  }
  if (importBtn && importFile) {
    importBtn.addEventListener('click', function() {
      importFile.click();
    });
  }
  if (importFile) {
    importFile.addEventListener('change', function(event) {
      const file = event.target.files && event.target.files[0];
      if (!file) {
        return;
      }
      const reader = new FileReader();
      reader.onload = function() {
        importSettingsFromText(String(reader.result || ''));
      };
      reader.onerror = function() {
        setStatus('Failed to read import file', false);
      };
      reader.readAsText(file);
      event.target.value = '';
    });
  }
  if (rebootBtn) {
    rebootBtn.addEventListener('click', rebootDevice);
  }

  loadDeviceInfo();
  Promise.all([loadSettings(), refreshEStopStatus()])
    .then(function() {
      setStatus('Ready', true);
      scheduleNextStatusPoll(getStatusPollIntervalMs());
    })
    .catch(function(err) {
      setStatus(err.message || 'Initial load failed', false);
      scheduleNextStatusPoll(getStatusPollIntervalMs());
    });

  document.addEventListener('visibilitychange', function() {
    scheduleNextStatusPoll(document.hidden ? STATUS_POLL_INTERVAL_HIDDEN_MS : 100);
  });
});
