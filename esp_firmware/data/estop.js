const ESP_DAEMON_PLACEHOLDER_MAC = '00-00-00-00-00-00';
const ESP_DAEMON_PLACEHOLDER_VERSION = '0.0.0';
const SAVE_STATUS_BASE_CLASS = 'actions__status';

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

function setStatus(msg, ok) {
  const el = document.getElementById('saveStatus');
  if (!el) {
    return;
  }
  if (!msg) {
    el.textContent = '';
    el.className = SAVE_STATUS_BASE_CLASS;
    return;
  }
  el.textContent = msg;
  el.className = SAVE_STATUS_BASE_CLASS + ' ' + (ok ? 'ok' : 'err');
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

function initNumericStepperControls() {
  document.querySelectorAll('.settings-card input[type="number"]:not([readonly])').forEach(function(input) {
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

function loadSettings() {
  return fetch('/settings/read', {
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

      const targetInput = document.getElementById('estopTargetMac');
      const pinInput = document.getElementById('estopSwitchPin');
      const activeHighInput = document.getElementById('estopSwitchActiveHigh');
      const logicInvertedInput = document.getElementById('estopSwitchLogicInverted');
      const wledEnabledInput = document.getElementById('estopWledEnabled');
      const wledBaseUrlInput = document.getElementById('estopWledBaseUrl');
      const wledPresetInput = document.getElementById('estopWledPreset');

      if (targetInput) {
        const normalized = normalizeMacColonFromAny(result.data.estopTargetMac || '');
        targetInput.value = normalized ? macColonToDisplayHyphen(normalized) : '';
      }
      if (pinInput) {
        pinInput.value = result.data.estopSwitchPin != null ? String(result.data.estopSwitchPin) : '';
      }
      if (activeHighInput) {
        activeHighInput.checked = !!result.data.estopSwitchActiveHigh;
      }
      if (logicInvertedInput) {
        logicInvertedInput.checked = !!result.data.estopSwitchLogicInverted;
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
      const packetEl = document.getElementById('packetCount');
      const wledEl = document.getElementById('wledStatus');

      const pressed = !!data.pressed;
      setPillState(switchState, pressed ? 'PRESSED (STOP)' : 'RELEASED', !pressed);

      if (rawEl) {
        rawEl.textContent = data.rawLevel != null ? String(data.rawLevel) : '-';
      }
      if (channelEl) {
        channelEl.textContent = data.espNowChannel != null ? String(data.espNowChannel) : '-';
      }
      if (packetEl) {
        packetEl.textContent = data.packetsSent != null ? String(data.packetsSent) : '0';
      }

      const peerConfigured = !!data.targetPeerConfigured;
      const effectiveMac = data.effectiveTargetMac ? macColonToDisplayHyphen(data.effectiveTargetMac) : '';
      const configuredMac = normalizeMacColonFromAny(data.targetMac || '');

      if (peerConfigured) {
        setPillState(peerStatusEl, effectiveMac || 'configured', true);
      } else if (configuredMac) {
        setPillState(peerStatusEl, 'waiting (' + macColonToDisplayHyphen(configuredMac) + ')', false);
      } else {
        setPillState(peerStatusEl, 'not configured', false);
      }

      const snapshotReady = !!data.wledSnapshotReady;
      const wledText = mapWledStatusText(data.wledStatus, snapshotReady);
      const wledOk = wledText === 'ready' || wledText === 'ready (snapshot)' || wledText === 'restored';
      setPillState(wledEl, wledText, wledOk);
    })
    .catch(function() {
      setPillState(document.getElementById('switchState'), 'offline', false);
      setPillState(document.getElementById('peerStatus'), 'offline', false);
      setPillState(document.getElementById('wledStatus'), 'offline', false);
    });
}

function collectSavePayload() {
  const macInput = document.getElementById('estopTargetMac');
  const pinInput = document.getElementById('estopSwitchPin');
  const activeHighInput = document.getElementById('estopSwitchActiveHigh');
  const logicInvertedInput = document.getElementById('estopSwitchLogicInverted');
  const wledEnabledInput = document.getElementById('estopWledEnabled');
  const wledBaseUrlInput = document.getElementById('estopWledBaseUrl');
  const wledPresetInput = document.getElementById('estopWledPreset');

  const macRaw = macInput ? macInput.value : '';
  const macTrimmed = String(macRaw || '').trim();
  const normalizedMac = macTrimmed.length ? normalizeMacColonFromAny(macTrimmed) : '';

  if (macTrimmed.length > 0 && !normalizedMac) {
    throw new Error('Invalid MAC format. Use AA-BB-CC-DD-EE-FF.');
  }

  const pinValue = pinInput ? Number(pinInput.value) : NaN;
  if (!Number.isFinite(pinValue) || pinValue < 0 || pinValue > 48) {
    throw new Error('E-Stop GPIO must be between 0 and 48.');
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

  return {
    authPin: '',
    estopTargetMac: normalizedMac,
    estopSwitchPin: Math.round(pinValue),
    estopSwitchActiveHigh: !!(activeHighInput && activeHighInput.checked),
    estopSwitchLogicInverted: !!(logicInvertedInput && logicInvertedInput.checked),
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

  fetch('/settings', {
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

document.addEventListener('DOMContentLoaded', function() {
  initNumericStepperControls();
  initThemeToggle();

  const saveBtn = document.getElementById('saveBtn');
  const reloadBtn = document.getElementById('reloadBtn');

  if (saveBtn) {
    saveBtn.addEventListener('click', saveSettings);
  }
  if (reloadBtn) {
    reloadBtn.addEventListener('click', function() {
      Promise.all([loadSettings(), refreshEStopStatus()])
        .then(function() {
          setStatus('Reloaded', true);
        })
        .catch(function(err) {
          setStatus(err.message || 'Reload failed', false);
        });
    });
  }
  loadDeviceInfo();
  Promise.all([loadSettings(), refreshEStopStatus()])
    .then(function() {
      setStatus('Ready', true);
    })
    .catch(function(err) {
      setStatus(err.message || 'Initial load failed', false);
    });

  setInterval(function() {
    refreshEStopStatus();
  }, 250);
});
