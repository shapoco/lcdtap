'use strict';

// LcdTap monitor application: renders the UI markup into #app and drives the
// JSON protocol over an injected connection object.
//
// Shared verbatim by the GitHub-Pages monitor (SerialConnection) and the
// page served by pico2w_remote itself (HttpConnection): the connection only
// needs { connected, connect(arg), reopen(), disconnect(),
// sendCommand(obj, timeoutMs) -> Promise<rawLine>, onLost }.

// ─── Toast ──────────────────────────────────────────────────────────────────

export function showToast(msg, type = '', duration = 3500) {
  const area = document.getElementById('toast-area');
  const el = document.createElement('div');
  el.className = 'toast' + (type ? ' ' + type : '');
  el.textContent = msg;
  area.appendChild(el);
  setTimeout(() => { el.style.opacity = '0'; el.style.transition = 'opacity 0.3s'; setTimeout(() => el.remove(), 300); }, duration);
}

// ─── Base64 decode ──────────────────────────────────────────────────────────

export function base64Decode(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

// ─── RGB565-RLE decode ──────────────────────────────────────────────────────

// Packet stream: control byte c, then
//   c & 0x80 set   -> run:     next 2 bytes (RGB565 LE) repeat (c & 0x7F)+1 times
//   c & 0x80 clear -> literal: next ((c & 0x7F)+1)*2 bytes are raw pixels
export function rleDecodeRGB565(bytes, expectedLen) {
  const out = new Uint8Array(expectedLen);
  let i = 0, o = 0;
  while (i < bytes.length) {
    const c = bytes[i++];
    const count = (c & 0x7F) + 1;
    if (o + count * 2 > expectedLen) throw new Error('RLE overflow');
    if (c & 0x80) {
      if (i + 2 > bytes.length) throw new Error('RLE truncated');
      const lo = bytes[i++], hi = bytes[i++];
      for (let k = 0; k < count; k++) { out[o++] = lo; out[o++] = hi; }
    } else {
      if (i + count * 2 > bytes.length) throw new Error('RLE truncated');
      out.set(bytes.subarray(i, i + count * 2), o);
      i += count * 2;
      o += count * 2;
    }
  }
  if (o !== expectedLen) throw new Error(`RLE underflow (${o} of ${expectedLen} bytes)`);
  return out;
}

// ─── Text buffer decode ─────────────────────────────────────────────────────

// Convert a character-LCD text buffer (row-major, cols*rows bytes of the
// controller's native codes) to a display string, one line per row.
// 0x20-0x7E map to ASCII, 0xA1-0xDF to half-width katakana (U+FF61-FF9F);
// everything else (CGRAM chars, CGROM extras) has no Unicode equivalent and
// shows as U+FFFD.
export function textBufferToString(bytes, cols, rows) {
  const lines = [];
  for (let r = 0; r < rows; r++) {
    let line = '';
    for (let c = 0; c < cols; c++) {
      const code = bytes[r * cols + c];
      if (code === undefined) break;
      if (code >= 0x20 && code <= 0x7E) line += String.fromCharCode(code);
      else if (code >= 0xA1 && code <= 0xDF) line += String.fromCharCode(0xFEC0 + code);
      else line += '�';
    }
    lines.push(line);
  }
  return lines.join('\n');
}

// ─── App markup ─────────────────────────────────────────────────────────────

function appMarkup(title, transports) {
  const transportSelect = transports.length >= 2 ? `
    <select id="transport-select">
      ${transports.map(t => `<option value="${t.id}">${t.label}</option>`).join('\n      ')}
    </select>` : '';

  return `
<!-- ── Header ──────────────────────────────────────────── -->
<header>
  <h1>${title}</h1>
  <div class="conn-status">
    <div class="conn-dot" id="conn-dot"></div>
    <span id="conn-label">Disconnected</span>
  </div>
  <div class="header-spacer"></div>
  <div class="conn-controls">${transportSelect}
    <button id="btn-connect" class="primary">Connect</button>
    <button id="btn-disconnect" disabled>Disconnect</button>
  </div>
</header>

<!-- ── Tabs ────────────────────────────────────────────── -->
<div class="tab-bar">
  <button class="tab-btn active" data-tab="config" disabled>Config</button>
  <button class="tab-btn" data-tab="framebuffer" disabled>Frame Buffer</button>
  <button class="tab-btn" data-tab="dump" disabled>Command Dump</button>
  <button class="tab-btn" data-tab="stats" disabled>Statistics</button>
</div>

<!-- ── Config tab ──────────────────────────────────────── -->
<div class="tab-pane active" id="tab-config">
  <div class="section-title">Presets</div>
  <div class="preset-controls">
    <select id="preset-select" class="preset-select">
      <option value="">-- Select a preset --</option>
    </select>
    <button id="btn-load-preset" disabled>Load</button>
  </div>
  <div class="section-title" style="margin-top: 20px;">Parameters</div>
  <div class="param-list" id="param-list">
    <div style="color:var(--text-muted)">Connect to a device to load parameters.</div>
  </div>
  <div class="config-actions">
    <button id="btn-apply" class="primary" disabled>Apply</button>
    <button id="btn-refresh-params" disabled>Refresh</button>
    <div class="dropdown">
      <button id="btn-export" disabled>Export <span class="caret">▼</span></button>
      <div class="dropdown-menu">
        <button id="menu-export-file">to File</button>
        <button id="menu-export-clip">to Clipboard</button>
      </div>
    </div>
    <div class="dropdown">
      <button id="btn-import" disabled>Import <span class="caret">▼</span></button>
      <div class="dropdown-menu">
        <button id="menu-import-file">from File</button>
        <button id="menu-import-clip">from Clipboard</button>
      </div>
    </div>
  </div>
</div>

<!-- ── Frame Buffer tab ────────────────────────────────── -->
<div class="tab-pane" id="tab-framebuffer">
  <div class="fb-controls">
    <button id="btn-capture" disabled>Capture</button>
    <button id="btn-copy" disabled>Copy</button>
    <button id="btn-save" disabled>Save</button>
    <div class="auto-reload-ctrl">
      <label>
        <div class="toggle">
          <input type="checkbox" id="auto-reload">
          <div class="toggle-track"></div>
        </div>
        Auto Reload
      </label>
      <select id="auto-reload-interval">
        <option value="200">0.2s</option>
        <option value="500">0.5s</option>
        <option value="1000">1s</option>
        <option value="2000">2s</option>
        <option value="5000" selected>5s</option>
      </select>
    </div>
    <div class="auto-reload-ctrl">
      <label>
        <div class="toggle">
          <input type="checkbox" id="write-protected">
          <div class="toggle-track"></div>
        </div>
        Write Protected
      </label>
    </div>
  </div>
  <div class="fb-container">
    <canvas id="fb-canvas" width="1" height="1"></canvas>
    <div class="fb-info" id="fb-info">No image captured.</div>
    <textarea id="fb-text" class="fb-text" readonly spellcheck="false" wrap="off" style="display:none"></textarea>
  </div>
</div>

<!-- ── Command Dump tab ────────────────────────────────── -->
<div class="tab-pane" id="tab-dump">
  <div class="dump-controls">
    <button id="btn-force-trigger" disabled>Force Trigger</button>
    <button id="btn-abort" class="danger" disabled>Abort</button>
  </div>
  <div class="dump-status-bar">
    <span>Status:</span>
    <span class="status-badge WAIT" id="dump-status-badge">WAIT</span>
    <span id="dump-byte-count">– bytes</span>
  </div>
  <div class="dump-view-wrapper">
    <div id="dump-view" style="color:var(--text-muted)">No data.</div>
  </div>
</div>

<!-- ── Statistics tab ──────────────────────────────────── -->
<div class="tab-pane" id="tab-stats">
  <div class="stats-controls">
    <button id="btn-stats-reset" disabled>Reset</button>
  </div>
  <table class="stats-table"><tbody id="stats-body">
    <tr><td class="stats-name" style="color:var(--text-muted)">No data.</td></tr>
  </tbody></table>
</div>

<!-- ── Toast area ──────────────────────────────────────── -->
<div id="toast-area"></div>
`;
}

// ─── App ────────────────────────────────────────────────────────────────────

export function initApp({
  conn,
  title = 'LcdTap Monitor',
  transports = [],          // [{ id, label }]; a select is shown when >= 2
  defaultTransport = null,
  getConnectArg = null,     // (transportId) => argument for conn.connect()
  autoConnect = false,
} = {}) {
  document.getElementById('app').innerHTML = appMarkup(title, transports);

  // ─── App State ────────────────────────────────────────────────────────────

  let params = [];           // current param descriptors
  let dumpPollTimer = null;
  let dumpLastStatus = null;
  let dumpLastBytes = -1;
  let statsPollTimer = null;
  let autoReloadTimer = null;
  let activeTab = 'config';
  // Text buffer probing: 'unknown' until the first capture on this visit to
  // the Frame Buffer tab; 'unsupported' stops further gettextbuffer commands
  // until the tab is left, so pixel-only devices see no extra traffic.
  let fbTextState = 'unknown';
  // Set by Apply: capture automatically once the Frame Buffer tab is live
  // again (immediately, or after the post-reboot auto-reconnect).
  let pendingAutoCapture = false;

  // ─── Connection UI ────────────────────────────────────────────────────────

  const elConnDot   = document.getElementById('conn-dot');
  const elConnLabel = document.getElementById('conn-label');
  const btnConnect    = document.getElementById('btn-connect');
  const btnDisconnect = document.getElementById('btn-disconnect');
  const transportSelect = document.getElementById('transport-select');  // may be null

  function setConnected(yes) {
    elConnDot.className = 'conn-dot' + (yes ? ' connected' : '');
    elConnLabel.textContent = yes ? 'Connected' : 'Disconnected';
    btnConnect.disabled    =  yes;
    btnDisconnect.disabled = !yes;
    if (transportSelect) transportSelect.disabled = yes;
    document.querySelectorAll('.tab-btn').forEach(b => { b.disabled = !yes; });
    document.querySelectorAll('#btn-apply,#btn-refresh-params,#btn-load-preset,#btn-export,#btn-import,#btn-capture,#btn-force-trigger,#btn-abort,#btn-stats-reset')
      .forEach(b => { b.disabled = !yes; });
    // Copy/Save stay disabled until a frame is captured
    if (!yes) {
      document.getElementById('btn-copy').disabled = true;
      document.getElementById('btn-save').disabled = true;
      // The device (and possibly its controller family) may change across a
      // reconnect: re-probe text buffer support on the next capture.
      fbTextState = 'unknown';
    }
  }

  if (transportSelect) {
    transportSelect.value = defaultTransport || transports[0].id;
  }

  // The clipboard API needs a secure context; the device page is plain
  // http://, so hide the clipboard entries there rather than offering
  // controls that fail.
  if (!window.isSecureContext) {
    document.getElementById('btn-copy').style.display = 'none';
    document.getElementById('menu-export-clip').style.display = 'none';
    document.getElementById('menu-import-clip').style.display = 'none';
  }

  const sleep = (ms) => new Promise(r => setTimeout(r, ms));

  // ─── Auto-reconnect ───────────────────────────────────────────────────────
  // Applying certain settings makes the device self-reset, dropping the link
  // for a few seconds. Detect the drop and re-open automatically, retrying
  // until it comes back.

  let reconnecting = false;
  let reconnectCancel = false;
  const RECONNECT_RETRIES = 30;      // ~15 s total
  const RECONNECT_INTERVAL_MS = 500;

  conn.onLost = () => {
    // Ignore drops we caused ourselves (user pressed Disconnect).
    if (reconnecting) return;
    attemptReconnect();
  };

  async function attemptReconnect() {
    reconnecting = true;
    reconnectCancel = false;
    stopDumpPolling();
    stopStatsPolling();
    stopAutoReload();
    setConnected(false);
    // Keep Disconnect live so the user can abort the retry loop.
    btnDisconnect.disabled = false;
    elConnDot.className = 'conn-dot reconnecting';
    showToast('Connection lost — reconnecting…', '', 4000);

    for (let i = 1; i <= RECONNECT_RETRIES && !reconnectCancel; i++) {
      elConnLabel.textContent = `Reconnecting… (${i}/${RECONNECT_RETRIES})`;
      await sleep(RECONNECT_INTERVAL_MS);
      if (reconnectCancel) break;
      try {
        await conn.reopen();
        const resp = await conn.sendCommand({ command: 'hello' }, 3000);
        const obj = JSON.parse(resp);
        if (obj.response !== 'welcome lcdtap') throw new Error('bad hello');
        reconnecting = false;
        setConnected(true);
        showToast('Reconnected', 'success');
        await loadPresets();
        await loadParams();
        if (pendingAutoCapture && activeTab === 'framebuffer') {
          pendingAutoCapture = false;
          await captureFramebuffer(true);
        }
        return;
      } catch (_) {
        // Device is still rebooting (or mid-enumeration); clean up and retry.
        try { await conn.disconnect(); } catch (_) {}
      }
    }

    reconnecting = false;
    pendingAutoCapture = false;
    try { await conn.disconnect(); } catch (_) {}
    setConnected(false);
    if (!reconnectCancel) {
      showToast('Auto-reconnect failed. Please reconnect manually.', 'error');
    }
  }

  btnConnect.addEventListener('click', async () => {
    try {
      btnConnect.disabled = true;
      const arg = getConnectArg
        ? getConnectArg(transportSelect ? transportSelect.value : null)
        : undefined;
      await conn.connect(arg);
      // Handshake
      const resp = await conn.sendCommand({ command: 'hello' }, 5000);
      const obj = JSON.parse(resp);
      if (obj.response !== 'welcome lcdtap') throw new Error('Unexpected hello response: ' + resp);
      setConnected(true);
      showToast('Connected to LcdTap', 'success');
      // Auto-load config tab
      await loadPresets();
      await loadParams();
    } catch (e) {
      try { await conn.disconnect(); } catch (_) {}
      setConnected(false);
      showToast('Connection failed: ' + e.message, 'error');
      btnConnect.disabled = false;
    }
  });

  btnDisconnect.addEventListener('click', async () => {
    reconnectCancel = true;   // abort an in-progress auto-reconnect, if any
    reconnecting = false;
    pendingAutoCapture = false;
    stopDumpPolling();
    stopStatsPolling();
    stopAutoReload();
    await conn.disconnect();
    setConnected(false);
    showToast('Disconnected');
  });

  // ─── Tab switching ────────────────────────────────────────────────────────

  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      if (btn.disabled) return;
      const tab = btn.dataset.tab;
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b === btn));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.toggle('active', p.id === 'tab-' + tab));
      const prev = activeTab;
      activeTab = tab;
      if (tab === 'dump') startDumpPolling();
      else if (prev === 'dump') stopDumpPolling();
      if (tab === 'stats') startStatsPolling();
      else if (prev === 'stats') stopStatsPolling();
      if (prev === 'framebuffer' && tab !== 'framebuffer') fbTextState = 'unknown';
    });
  });

  // ─── Config Tab ───────────────────────────────────────────────────────────

  async function loadPresets() {
    try {
      const resp = await conn.sendCommand({ command: 'getpresets' }, 5000);
      const obj = JSON.parse(resp);
      const select = document.getElementById('preset-select');
      select.innerHTML = '<option value="">-- Select a preset --</option>';
      for (const name of obj.presets) {
        const opt = document.createElement('option');
        opt.value = name;
        opt.textContent = name;
        select.appendChild(opt);
      }
    } catch (e) {
      showToast('getpresets failed: ' + e.message, 'error');
    }
  }

  async function loadParams() {
    try {
      const resp = await conn.sendCommand({ command: 'getparams' }, 8000);
      const obj = JSON.parse(resp);
      params = obj.params;
      buildParamUI(params);
    } catch (e) {
      showToast('getparams failed: ' + e.message, 'error');
    }
  }

  let paramElements = {};  // id → input/select element

  // A parameter is enabled when its enable key is in range AND the enable key
  // itself is enabled. That second condition is a cascade: it is what makes
  // "Video DAC Type" grey out on the parallel bus, where it is "Output
  // Interface" that the bus disables, not the DAC directly. Mirrors
  // Osd::updateItemEnables() in lib/src/osd.cpp.
  function updateParamEnables() {
    const byId = {};
    for (const p of params) byId[p.id] = p;

    const state = {};     // id -> resolved enabled flag
    const visiting = {};  // ids on the current resolution path

    function isEnabled(p) {
      if (state[p.id] !== undefined) return state[p.id];
      if (visiting[p.id]) return true;  // guard against a dependency cycle
      visiting[p.id] = true;

      let enabled = true;
      if (p.enableKeyId != null) {
        const keyEl = paramElements[p.enableKeyId];
        const keyParam = byId[p.enableKeyId];
        if (keyEl && keyParam) {
          const keyVal = keyEl.type === 'checkbox'
            ? (keyEl.checked ? 1 : 0)
            : parseInt(keyEl.value, 10);
          enabled = isEnabled(keyParam) &&
                    keyVal >= p.enableKeyValueMin &&
                    keyVal <= p.enableKeyValueMax;
        }
        // An unresolvable enable key leaves the row enabled rather than
        // hiding a control the user might need.
      }

      delete visiting[p.id];
      state[p.id] = enabled;
      return enabled;
    }

    for (const p of params) {
      const row = paramElements[p.id]?.closest('.param-row');
      if (row) row.classList.toggle('param-disabled', !isEnabled(p));
    }
  }

  function buildParamUI(params) {
    paramElements = {};
    const list = document.getElementById('param-list');
    list.innerHTML = '';

    for (const p of params) {
      const row = document.createElement('div');
      row.className = 'param-row';

      const labelDiv = document.createElement('div');
      labelDiv.className = 'param-label';
      labelDiv.textContent = p.name;

      const ctrlDiv = document.createElement('div');
      ctrlDiv.className = 'param-ctrl';

      if (p.type === 'INTEGER') {
        ctrlDiv.appendChild(buildIntCtrl(p));
      } else if (p.type === 'HEX') {
        ctrlDiv.appendChild(buildHexCtrl(p));
      } else if (p.type === 'BOOLEAN') {
        ctrlDiv.appendChild(buildBoolCtrl(p));
      } else if (p.type === 'ENUM') {
        ctrlDiv.appendChild(buildEnumCtrl(p));
      }

      const el = ctrlDiv.querySelector('input[data-param-id], select[data-param-id]');
      if (el) {
        paramElements[p.id] = el;
        el.addEventListener('change', updateParamEnables);
      }

      row.appendChild(labelDiv);
      row.appendChild(ctrlDiv);
      list.appendChild(row);
    }

    updateParamEnables();
  }

  function buildIntCtrl(p) {
    const wrap = document.createElement('div');
    wrap.className = 'int-ctrl';

    const btnMinus = document.createElement('button');
    btnMinus.textContent = '−';
    btnMinus.title = 'Decrease';

    const input = document.createElement('input');
    input.type = 'number';
    input.min = p.min;
    input.max = p.max;
    input.step = p.step;
    input.value = p.value;
    input.dataset.paramId = p.id;
    input.dataset.paramType = 'INTEGER';

    const btnPlus = document.createElement('button');
    btnPlus.textContent = '+';
    btnPlus.title = 'Increase';

    btnMinus.addEventListener('click', () => {
      let v = parseInt(input.value, 10) - p.step;
      if (v < p.min) v = p.min;
      input.value = v;
      input.dispatchEvent(new Event('change'));
    });
    btnPlus.addEventListener('click', () => {
      let v = parseInt(input.value, 10) + p.step;
      if (v > p.max) v = p.max;
      input.value = v;
      input.dispatchEvent(new Event('change'));
    });

    wrap.appendChild(btnMinus);
    wrap.appendChild(input);
    wrap.appendChild(btnPlus);

    if (p.unit) {
      const unit = document.createElement('span');
      unit.className = 'unit-label';
      unit.textContent = p.unit;
      wrap.appendChild(unit);
    }
    return wrap;
  }

  function buildHexCtrl(p) {
    const wrap = document.createElement('div');
    wrap.className = 'int-ctrl';

    const toHex = v => '0x' + v.toString(16).toUpperCase().padStart(2, '0');
    const clamp = v => {
      if (isNaN(v)) v = p.min;
      return Math.min(p.max, Math.max(p.min, v));
    };

    const btnMinus = document.createElement('button');
    btnMinus.textContent = '−';
    btnMinus.title = 'Decrease';

    const input = document.createElement('input');
    input.type = 'text';
    input.value = toHex(p.value);
    input.dataset.paramId = p.id;
    input.dataset.paramType = 'HEX';
    input.addEventListener('blur', () => {
      input.value = toHex(clamp(parseInt(input.value, 16)));
    });

    const btnPlus = document.createElement('button');
    btnPlus.textContent = '+';
    btnPlus.title = 'Increase';

    btnMinus.addEventListener('click', () => {
      input.value = toHex(clamp(parseInt(input.value, 16) - p.step));
      input.dispatchEvent(new Event('change'));
    });
    btnPlus.addEventListener('click', () => {
      input.value = toHex(clamp(parseInt(input.value, 16) + p.step));
      input.dispatchEvent(new Event('change'));
    });

    wrap.appendChild(btnMinus);
    wrap.appendChild(input);
    wrap.appendChild(btnPlus);
    return wrap;
  }

  function buildBoolCtrl(p) {
    const label = document.createElement('label');
    label.className = 'toggle';

    const input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!p.value;
    input.dataset.paramId = p.id;
    input.dataset.paramType = 'BOOLEAN';

    const track = document.createElement('div');
    track.className = 'toggle-track';

    label.appendChild(input);
    label.appendChild(track);
    return label;
  }

  function buildEnumCtrl(p) {
    const select = document.createElement('select');
    select.dataset.paramId = p.id;
    select.dataset.paramType = 'ENUM';

    for (const [label, val] of Object.entries(p.options)) {
      const opt = document.createElement('option');
      opt.value = val;
      opt.textContent = label;
      if (val === p.value) opt.selected = true;
      select.appendChild(opt);
    }
    return select;
  }

  document.getElementById('btn-apply').addEventListener('click', async () => {
    const paramMap = {};
    document.querySelectorAll('[data-param-id]').forEach(el => {
      const id = el.dataset.paramId;
      const type = el.dataset.paramType;
      if (type === 'BOOLEAN') {
        paramMap[id] = el.checked;
      } else if (type === 'HEX') {
        paramMap[id] = parseInt(el.value, 16);
      } else {
        paramMap[id] = parseInt(el.value, 10);
      }
    });

    try {
      document.getElementById('btn-apply').disabled = true;
      const resp = await conn.sendCommand({ command: 'setparams', params: paramMap }, 8000);
      const obj = JSON.parse(resp);
      if (obj.response === 'ok') {
        showToast('Parameters applied', 'success');
        // Switch to Frame Buffer tab and capture. A reboot-inducing apply
        // drops the link right after the response, in which case this
        // immediate attempt fails silently and the auto-reconnect success
        // path retries the capture via pendingAutoCapture.
        document.querySelector('.tab-btn[data-tab="framebuffer"]').click();
        pendingAutoCapture = true;
        captureFramebuffer(true);
      } else {
        const detail = obj.state ? ` (state: ${obj.state})` : '';
        showToast('setparams error: ' + (obj.error || JSON.stringify(obj)) + detail, 'error');
      }
    } catch (e) {
      showToast('setparams failed: ' + e.message, 'error');
    } finally {
      document.getElementById('btn-apply').disabled = false;
    }
  });

  document.getElementById('btn-refresh-params').addEventListener('click', loadParams);

  document.getElementById('btn-load-preset').addEventListener('click', async () => {
    const presetName = document.getElementById('preset-select').value;
    if (!presetName) return;
    const btn = document.getElementById('btn-load-preset');
    btn.disabled = true;
    try {
      const resp = await conn.sendCommand({ command: 'getparams', preset: presetName }, 8000);
      const obj = JSON.parse(resp);
      params = obj.params;
      buildParamUI(params);
      showToast(`Preset "${presetName}" loaded`, 'success');
    } catch (e) {
      showToast('Load preset failed: ' + e.message, 'error');
    } finally {
      btn.disabled = !conn.connected;
    }
  });

  // ─── Config Export / Import ───────────────────────────────────────────────

  // cfgN index -> stable identifier, frozen at the cfgN->identifier
  // changeover. Keeps exported JSON keys stable even against firmware that
  // still reports the retired "cfgN" ids, and maps them back on import.
  const LEGACY_CFG_IDS = [
    'ctrlFamily', 'busInterface', 'i2cAddr', 'buffWidth', 'buffHeight',
    'textCols', 'textRows', 'textCgramArea', 'trimMode', 'trimX', 'trimY',
    'trimWidth', 'trimHeight', 'flipMode', 'inverted', 'swapRB', 'forcePwrOn',
    'intfFmtOvr', 'outputRot', 'scaleMode',
  ];

  function toStableId(uiId) {
    const m = /^cfg(\d+)$/.exec(uiId);
    return m ? (LEGACY_CFG_IDS[parseInt(m[1], 10)] ?? uiId) : uiId;
  }

  // Stable identifier -> the id the connected firmware uses (identity on
  // current firmware; cfgN on firmware predating stable ids).
  function toUiId(stableId) {
    if (paramElements[stableId]) return stableId;
    const idx = LEGACY_CFG_IDS.indexOf(stableId);
    return idx >= 0 ? 'cfg' + idx : stableId;
  }

  // Collect the current UI values under stable keys — the same set Apply
  // sends, so the export mirrors what setparams would receive.
  function collectConfig() {
    const config = {};
    document.querySelectorAll('[data-param-id]').forEach(el => {
      const key = toStableId(el.dataset.paramId);
      if (el.dataset.paramType === 'BOOLEAN') config[key] = el.checked;
      else if (el.dataset.paramType === 'HEX') config[key] = parseInt(el.value, 16);
      else config[key] = parseInt(el.value, 10);
    });
    return config;
  }

  // Reverse-lookup the display label of an ENUM param's current value.
  function enumLabel(uiId) {
    const el = paramElements[uiId];
    const p = params.find(q => q.id === uiId);
    if (!el || !p || !p.options) return null;
    const v = parseInt(el.value, 10);
    for (const [label, val] of Object.entries(p.options)) {
      if (val === v) return label;
    }
    return null;
  }

  function exportFileName() {
    const family = enumLabel(toUiId('ctrlFamily'));
    const iface = enumLabel(toUiId('busInterface'));
    const w = paramElements[toUiId('buffWidth')]?.value;
    const h = paramElements[toUiId('buffHeight')]?.value;
    if (!family || !iface || !w || !h) return 'lcdtap_cfg.json';
    return `lcdtap_cfg_${family}_${iface}_${w}x${h}.json`.replace(/ /g, '-');
  }

  // Set one UI control from an imported value, clamping to the control's
  // range. Returns false when the param or value cannot be applied.
  function setParamValue(uiId, value) {
    const el = paramElements[uiId];
    const p = params.find(q => q.id === uiId);
    if (!el || !p) return false;
    if (p.type === 'BOOLEAN') {
      el.checked = !!value;
      return true;
    }
    let v = typeof value === 'boolean' ? (value ? 1 : 0) : parseInt(value, 10);
    if (isNaN(v)) return false;
    if (p.type === 'ENUM') {
      if (!Object.values(p.options).includes(v)) return false;
      el.value = v;
      return true;
    }
    v = Math.min(p.max, Math.max(p.min, v));
    el.value = p.type === 'HEX'
      ? '0x' + v.toString(16).toUpperCase().padStart(2, '0')
      : v;
    return true;
  }

  function applyImportedConfig(obj) {
    if (!obj || typeof obj.config !== 'object' || obj.config === null) {
      showToast('Invalid config JSON: missing "config" object', 'error');
      return;
    }
    let applied = 0, skipped = 0;
    for (const [key, value] of Object.entries(obj.config)) {
      if (setParamValue(toUiId(key), value)) applied++;
      else skipped++;
    }
    updateParamEnables();
    const skipMsg = skipped ? ` (${skipped} skipped)` : '';
    showToast(`Loaded ${applied} params${skipMsg} — press Apply to send`, 'success');
  }

  function closeDropdowns() {
    document.querySelectorAll('.dropdown.open').forEach(d => d.classList.remove('open'));
  }
  document.addEventListener('click', closeDropdowns);

  for (const btnId of ['btn-export', 'btn-import']) {
    const btn = document.getElementById(btnId);
    btn.addEventListener('click', e => {
      e.stopPropagation();
      const dd = btn.closest('.dropdown');
      const wasOpen = dd.classList.contains('open');
      closeDropdowns();
      if (!wasOpen) {
        dd.classList.remove('drop-up');
        dd.classList.add('open');
        // Flip upward when the menu would spill past the viewport bottom.
        const menu = dd.querySelector('.dropdown-menu');
        if (menu.getBoundingClientRect().bottom > window.innerHeight) {
          dd.classList.add('drop-up');
        }
      }
    });
  }

  document.getElementById('menu-export-file').addEventListener('click', () => {
    closeDropdowns();
    const json = JSON.stringify({ config: collectConfig() }, null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = exportFileName();
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  });

  document.getElementById('menu-export-clip').addEventListener('click', async () => {
    closeDropdowns();
    const json = JSON.stringify({ config: collectConfig() }, null, 2);
    try {
      await navigator.clipboard.writeText(json);
      showToast('Config copied to clipboard', 'success');
    } catch (e) {
      showToast('Copy failed: ' + e.message, 'error');
    }
  });

  document.getElementById('menu-import-file').addEventListener('click', () => {
    closeDropdowns();
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json,application/json';
    input.addEventListener('change', () => {
      const file = input.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = () => {
        try { applyImportedConfig(JSON.parse(reader.result)); }
        catch (e) { showToast('Import failed: ' + e.message, 'error'); }
      };
      reader.readAsText(file);
    });
    input.click();
  });

  document.getElementById('menu-import-clip').addEventListener('click', async () => {
    closeDropdowns();
    try {
      applyImportedConfig(JSON.parse(await navigator.clipboard.readText()));
    } catch (e) {
      showToast('Import failed: ' + e.message, 'error');
    }
  });

  // ─── Frame Buffer Tab ─────────────────────────────────────────────────────

  const canvas = document.getElementById('fb-canvas');
  const ctx = canvas.getContext('2d');

  // Fetch and render the character-LCD text buffer, or decide the device has
  // none. Failures are silent by design: an old firmware answers "unknown
  // command" and a pixel device answers cols=0, both meaning "stop asking".
  async function fetchTextBuffer() {
    const textEl = document.getElementById('fb-text');
    try {
      const resp = await conn.sendCommand({ command: 'gettextbuffer' }, 8000);
      const obj = JSON.parse(resp);
      if (!(obj.cols > 0) || !(obj.rows > 0)) {
        fbTextState = 'unsupported';
        textEl.style.display = 'none';
        return;
      }
      textEl.value = textBufferToString(base64Decode(obj.data), obj.cols, obj.rows);
      // U+FFFD replacement chars render wider than one monospace cell; give
      // the box some slack instead of letting it scroll (overflow: hidden).
      textEl.cols = Math.ceil(obj.cols * 2);
      textEl.rows = obj.rows;
      textEl.style.display = '';
      fbTextState = 'supported';
    } catch (e) {
      fbTextState = 'unsupported';
      textEl.style.display = 'none';
      console.warn('gettextbuffer failed:', e.message);
    }
  }

  async function captureFramebuffer(silent = false) {
    const btnCapture = document.getElementById('btn-capture');
    btnCapture.disabled = true;
    try {
      const writeProtected = document.getElementById('write-protected').checked;
      const resp = await conn.sendCommand({ command: 'getframebuffer', writeProtected }, 60000);
      const obj = JSON.parse(resp);
      const { width, height, data: b64 } = obj;

      const decoded = base64Decode(b64);
      const expectedBytes = width * height * 2;
      const raw = obj.format === 'RGB565-RLE'
        ? rleDecodeRGB565(decoded, expectedBytes)
        : decoded;
      console.log(`[FB] width=${width} height=${height} format=${obj.format} expected=${expectedBytes} wire=${decoded.length} decoded=${raw.length} b64_prefix="${b64.slice(0,16)}..."`);

      canvas.width = width;
      canvas.height = height;

      const imgData = ctx.createImageData(width, height);
      const px = imgData.data;
      for (let i = 0, p = 0; i < raw.length - 1; i += 2, p += 4) {
        const word = raw[i] | (raw[i + 1] << 8);
        const r5 = (word >> 11) & 0x1F;
        const g6 = (word >> 5) & 0x3F;
        const b5 = word & 0x1F;
        px[p    ] = (r5 << 3) | (r5 >> 2);
        px[p + 1] = (g6 << 2) | (g6 >> 4);
        px[p + 2] = (b5 << 3) | (b5 >> 2);
        px[p + 3] = 255;
      }
      ctx.putImageData(imgData, 0, 0);

      // Scale canvas display size (max 640px wide, nearest-neighbor)
      const scale = Math.max(1, Math.floor(Math.min(640 / width, 480 / height)));
      canvas.style.width  = (width  * scale) + 'px';
      canvas.style.height = (height * scale) + 'px';

      const bytesMismatch = raw.length !== expectedBytes ? ` ⚠ decoded ${raw.length} (expected ${expectedBytes})` : '';
      document.getElementById('fb-info').textContent = `${width} × ${height} px · scale ×${scale} · decoded ${raw.length} bytes${bytesMismatch}`;
      document.getElementById('btn-copy').disabled = false;
      document.getElementById('btn-save').disabled = false;

      if (fbTextState !== 'unsupported') await fetchTextBuffer();
      pendingAutoCapture = false;
    } catch (e) {
      if (silent) console.warn('Capture failed:', e.message);
      else showToast('Capture failed: ' + e.message, 'error');
    } finally {
      btnCapture.disabled = !conn.connected;
    }
  }

  document.getElementById('btn-capture').addEventListener('click', () => captureFramebuffer());

  document.getElementById('btn-copy').addEventListener('click', () => {
    canvas.toBlob(async blob => {
      try {
        await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
        showToast('Image copied to clipboard', 'success');
      } catch (e) {
        showToast('Copy failed: ' + e.message, 'error');
      }
    });
  });

  document.getElementById('btn-save').addEventListener('click', () => {
    canvas.toBlob(blob => {
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = 'lcdtap_' + Date.now() + '.png';
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    });
  });

  const autoReloadCheck = document.getElementById('auto-reload');
  const autoReloadIntervalSel = document.getElementById('auto-reload-interval');

  function stopAutoReload() {
    if (autoReloadTimer) { clearInterval(autoReloadTimer); autoReloadTimer = null; }
    autoReloadCheck.checked = false;
  }

  autoReloadCheck.addEventListener('change', () => {
    if (autoReloadCheck.checked) {
      autoReloadTimer = setInterval(captureFramebuffer, parseInt(autoReloadIntervalSel.value));
    } else {
      stopAutoReload();
    }
  });

  autoReloadIntervalSel.addEventListener('change', () => {
    if (autoReloadCheck.checked) {
      clearInterval(autoReloadTimer);
      autoReloadTimer = setInterval(captureFramebuffer, parseInt(autoReloadIntervalSel.value));
    }
  });

  // ─── Command Dump Tab ─────────────────────────────────────────────────────

  document.getElementById('btn-force-trigger').addEventListener('click', async () => {
    try {
      await conn.sendCommand({ command: 'cmddump_start' }, 5000);
      await conn.sendCommand({ command: 'cmddump_forcetrigger' }, 5000);
      dumpLastStatus = null;
      dumpLastBytes = -1;
      showToast('Dump triggered', 'success');
    } catch (e) {
      showToast('Force trigger failed: ' + e.message, 'error');
    }
  });

  document.getElementById('btn-abort').addEventListener('click', async () => {
    try {
      await conn.sendCommand({ command: 'cmddump_abort' }, 5000);
      showToast('Dump aborted');
    } catch (e) {
      showToast('Abort failed: ' + e.message, 'error');
    }
  });

  function startDumpPolling() {
    if (dumpPollTimer) return;
    dumpPollTimer = setInterval(pollDumpStatus, 500);
  }

  function stopDumpPolling() {
    if (dumpPollTimer) { clearInterval(dumpPollTimer); dumpPollTimer = null; }
  }

  async function pollDumpStatus() {
    if (!conn.connected) return;
    try {
      const resp = await conn.sendCommand({ command: 'cmddump_getstatus' }, 3000);
      const obj = JSON.parse(resp);
      const { status, bytes } = obj;

      // Update status badge
      const badge = document.getElementById('dump-status-badge');
      badge.textContent = status;
      badge.className = 'status-badge ' + status;
      document.getElementById('dump-byte-count').textContent = bytes + ' bytes';

      if (status !== dumpLastStatus || bytes !== dumpLastBytes) {
        dumpLastStatus = status;
        dumpLastBytes = bytes;
        await fetchAndRenderDump();
      }
    } catch (e) {
      // Ignore timeout during polling; might be busy sending framebuffer
    }
  }

  async function fetchAndRenderDump() {
    try {
      const resp = await conn.sendCommand({ command: 'cmddump_read' }, 8000);
      const obj = JSON.parse(resp);
      const raw = base64Decode(obj.data);
      console.log(`[DUMP] length=${obj.length} words, decoded=${raw.length} bytes (expected=${obj.length * 2})`);
      const el = document.getElementById('dump-byte-count');
      const mismatch = raw.length !== obj.length * 2 ? ` ⚠ decoded ${raw.length}` : '';
      el.textContent = (dumpLastBytes >= 0 ? dumpLastBytes : raw.length) + ' bytes' + mismatch;
      renderDump(raw);
    } catch (e) {
      console.warn('cmddump_read failed:', e.message);
    }
  }

  const DUMP_COLS = 16;

  function renderDump(raw) {
    const view = document.getElementById('dump-view');

    if (!raw || raw.length < 2) {
      view.innerHTML = '<span style="color:var(--text-muted)">No data.</span>';
      return;
    }

    // Decode uint16_t words (little-endian)
    const words = [];
    for (let i = 0; i + 1 < raw.length; i += 2) {
      words.push(raw[i] | (raw[i + 1] << 8));
    }

    const frag = document.createDocumentFragment();
    let colParity = 0;  // alternates for data byte background

    for (let i = 0; i < words.length; i += DUMP_COLS) {
      const rowDiv = document.createElement('div');
      rowDiv.className = 'dump-row';

      const addr = document.createElement('span');
      addr.className = 'dump-row-addr';
      addr.textContent = i.toString(16).padStart(3, '0') + ':';
      rowDiv.appendChild(addr);

      colParity = 0;
      for (let j = 0; j < DUMP_COLS && i + j < words.length; j++) {
        const word = words[i + j];
        const cell = document.createElement('span');
        cell.className = 'dump-cell';

        if (word & 0x8000) {
          // Special event
          if (word === 0x8001) {
            cell.classList.add('evt-rst');
            cell.textContent = 'RST';
          } else {
            cell.classList.add('evt-unk');
            cell.textContent = '??';
          }
        } else if (word & 0x100) {
          // Data byte
          cell.classList.add(colParity ? 'dat1' : 'dat0');
          cell.textContent = (word & 0xFF).toString(16).padStart(2, '0').toUpperCase();
          colParity ^= 1;
        } else {
          // Command byte
          cell.classList.add('cmd');
          cell.textContent = (word & 0xFF).toString(16).padStart(2, '0').toUpperCase();
          colParity = 0;
        }

        rowDiv.appendChild(cell);
      }
      frag.appendChild(rowDiv);
    }

    view.innerHTML = '';
    view.appendChild(frag);
  }

  // ─── Statistics Tab ───────────────────────────────────────────────────────

  function startStatsPolling() {
    if (statsPollTimer) return;
    pollStats();
    statsPollTimer = setInterval(pollStats, 1000);
  }

  function stopStatsPolling() {
    if (statsPollTimer) { clearInterval(statsPollTimer); statsPollTimer = null; }
  }

  function formatStatValue(entry) {
    const v = entry.value >>> 0;
    if (entry.fmt === 'rate') {
      if (v >= 1e6) return (v / 1e6).toFixed(2) + ' MB/s';
      if (v >= 1e3) return (v / 1e3).toFixed(2) + ' KB/s';
      return v + ' B/s';
    }
    if (entry.fmt === 'hex') {
      // Values above 0xFF mean "none yet"
      if (v > 0xFF) return '--';
      return '0x' + v.toString(16).padStart(2, '0').toUpperCase();
    }
    let s = v.toLocaleString('en-US');
    if (entry.unit) s += ' ' + entry.unit;
    return s;
  }

  async function pollStats() {
    if (!conn.connected) return;
    try {
      const resp = await conn.sendCommand({ command: 'getstats' }, 3000);
      const obj = JSON.parse(resp);
      renderStats(obj.stats || []);
    } catch (e) {
      // Ignore timeout during polling; might be busy sending framebuffer
    }
  }

  function renderStats(stats) {
    const body = document.getElementById('stats-body');
    const frag = document.createDocumentFragment();
    for (const entry of stats) {
      const tr = document.createElement('tr');
      const name = document.createElement('td');
      name.className = 'stats-name';
      name.textContent = entry.name;
      const val = document.createElement('td');
      val.className = 'stats-value';
      val.textContent = formatStatValue(entry);
      tr.appendChild(name);
      tr.appendChild(val);
      frag.appendChild(tr);
    }
    body.innerHTML = '';
    body.appendChild(frag);
  }

  document.getElementById('btn-stats-reset').addEventListener('click', async () => {
    try {
      await conn.sendCommand({ command: 'statsreset' }, 5000);
      showToast('Statistics reset', 'success');
      pollStats();
    } catch (e) {
      showToast('Reset failed: ' + e.message, 'error');
    }
  });

  if (autoConnect) btnConnect.click();
}
