// Fuse VW Gantry palette — JS bridge + UI logic.
//
// Bridge functions exposed by the C++ JS provider (see ExtMcpPalette.cpp):
//   window.requestStatus()     — request a fresh updateStatus() push
//   window.startBridge()       — start the SocketBridge
//   window.stopBridge()        — stop the SocketBridge
//   window.setAutoStart(bool)  — persist auto_start preference
//
// C++ pushes status to JS via:
//   window.updateStatus({ running, port, endpoint, sdk_version, vw_year,
//                          auto_start, settings_path })

// VW theme callback — must exist or else uncaught TypeError kills subsequent
// ExecuteJavaScript calls. (See VWX_PALETTE_DEMO/CLAUDE.md.)
window.onVectorworksThemeChanged = function () {};

// ── Element refs ────────────────────────────────────────────────────────
const $ = (id) => document.getElementById(id);
const els = {
  dot:          $('dot'),
  title:        $('title'),
  endpoint:     $('endpoint'),
  statusText:   $('status-text'),
  vwVersion:    $('vw-version'),
  toggleBtn:    $('toggle-btn'),
  autostartCb:  $('autostart-cb'),
  settingsPath: $('settings-path'),
};

// ── Bridge availability poll ───────────────────────────────────────────
// VW injects window.startBridge etc. AFTER OnInit returns. Poll until ready,
// then enable the UI and request initial status.
let bridgeReady = false;

function bridgeAvailable() {
  return typeof window.requestStatus === 'function'
      && typeof window.startBridge === 'function'
      && typeof window.stopBridge === 'function'
      && typeof window.setAutoStart === 'function';
}

function awaitBridge() {
  if (bridgeAvailable()) {
    bridgeReady = true;
    onBridgeReady();
    return;
  }
  setTimeout(awaitBridge, 100);
}

function onBridgeReady() {
  els.toggleBtn.disabled = false;
  els.autostartCb.disabled = false;
  // First status push happens from C++ OnInit, but request again for safety
  // in case OnInit ran before the JS finished loading.
  try { window.requestStatus(); } catch (_) {}
}

// ── UI state machine ───────────────────────────────────────────────────
let lastState = null;

function applyState(s) {
  lastState = s;

  // Endpoint text
  els.endpoint.textContent = s.endpoint || '—';

  // VW version line
  els.vwVersion.textContent = s.vw_year ? `VW${s.vw_year} (SDK ${s.sdk_version})` : '—';

  // Status dot + text + button
  if (s.running) {
    els.dot.className = 'dot on';
    els.statusText.textContent = `Running on port ${s.port}`;
    els.toggleBtn.textContent = 'Stop bridge';
    els.toggleBtn.classList.add('stop');
  } else {
    els.dot.className = 'dot';
    els.statusText.textContent = 'Stopped';
    els.toggleBtn.textContent = 'Start bridge';
    els.toggleBtn.classList.remove('stop');
  }

  // Auto-start
  els.autostartCb.checked = !!s.auto_start;

  // Settings path
  els.settingsPath.textContent = s.settings_path || '';
}

// ── C++ → JS entry point ───────────────────────────────────────────────
window.updateStatus = function (state) {
  // Defensive: state may arrive as object or pre-serialized string.
  if (typeof state === 'string') {
    try { state = JSON.parse(state); } catch (_) { return; }
  }
  applyState(state || {});
};

// ── User actions ───────────────────────────────────────────────────────
els.toggleBtn.addEventListener('click', () => {
  if (!bridgeReady || !lastState) return;
  els.toggleBtn.disabled = true;
  if (lastState.running) {
    try { window.stopBridge(); } catch (e) { console.error(e); }
  } else {
    try { window.startBridge(); } catch (e) { console.error(e); }
  }
  // Re-enable after a short delay; C++ will push fresh status which the
  // applyState call also re-syncs button state.
  setTimeout(() => { els.toggleBtn.disabled = false; }, 350);
});

els.autostartCb.addEventListener('change', () => {
  if (!bridgeReady) return;
  try { window.setAutoStart(els.autostartCb.checked); } catch (e) { console.error(e); }
});

// ── Periodic refresh (in case C++ side restarts the bridge externally) ──
setInterval(() => {
  if (bridgeReady) {
    try { window.requestStatus(); } catch (_) {}
  }
}, 2000);

// ── Boot ───────────────────────────────────────────────────────────────
awaitBridge();
