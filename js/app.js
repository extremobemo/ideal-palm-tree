// Entry point: canvas setup, renderer loading, ROM dispatch, slider wiring,
// BIOS/save file loading, and core readiness status checks.

import { state } from './state.js';
import { setStatus } from './utils.js';
import { spawnCoreWorker, setBiosFile, setGbaSave, ps1BiosLoaded, PS1_EXTS } from './worker-bridge.js';
import { loadN64 } from './n64.js';
import { mpHost, mpJoin } from './multiplayer.js';
import { initInput } from './input.js';

// ── Canvas sizing ─────────────────────────────────────────────
const canvas = document.getElementById('canvas');

(function sizeCanvas() {
  const pad = 80;
  const maxW = Math.round(window.innerWidth * 0.75);
  const maxH = window.innerHeight - pad;
  const aspect = 16 / 9;
  let w = maxW, h = Math.round(maxW / aspect);
  if (h > maxH) { h = maxH; w = Math.round(h * aspect); }
  canvas.width  = w;
  canvas.height = h;
  // Lock dimensions — n64wasm's SDL fires resize events that would otherwise
  // shrink this canvas to N64 native resolution.
  Object.defineProperty(canvas, 'width',  { get: () => w, set: () => {} });
  Object.defineProperty(canvas, 'height', { get: () => h, set: () => {} });
})();

// ── Global error display ──────────────────────────────────────
window.onerror = function(msg, src, line) {
  setStatus('JS error: ' + msg + ' (' + (src || '').split('/').pop() + ':' + line + ')');
};

// ── Core routing ──────────────────────────────────────────────
const CORE_MAP = {
  'gb':  'core_gbc.js',
  'gbc': 'core_gbc.js',
  'gba': 'core_gba.js',
  'sfc': 'core_snes.js',
  'smc': 'core_snes.js',
  'fig': 'core_snes.js',
  'swc': 'core_snes.js',
  'bs':  'core_snes.js',
  'bin': 'core_ps1.js',
  'cue': 'core_ps1.js',
  'chd': 'core_ps1.js',
  'img': 'core_ps1.js',
};
const N64_EXTS = new Set(['z64', 'n64', 'v64']);

// ── Renderer loader ───────────────────────────────────────────
// Inject a <script> tag and poll until window.Module.calledRun is true.
// Used for game_renderer.js (main thread Emscripten bundle).
function injectAndWait(src, onReady, onError) {
  const s = document.createElement('script');
  s.src = src;
  s.onerror = function() {
    setStatus('Failed to load ' + src);
    if (onError) onError();
  };
  document.body.appendChild(s);
  function check() {
    if (window.Module && window.Module.calledRun) {
      onReady(window.Module);
    } else {
      setTimeout(check, 100);
    }
  }
  setTimeout(check, 200);
}

// ── Slider wiring ─────────────────────────────────────────────
function applyRoomXform() {
  if (!state.rendererModule) return;
  const scale = parseFloat(document.getElementById('room-scale').value);
  const rotY  = parseFloat(document.getElementById('room-roty').value) || 0;
  const tx    = parseFloat(document.getElementById('room-tx').value)   || 0;
  const ty    = parseFloat(document.getElementById('room-ty').value)   || 0;
  const tz    = parseFloat(document.getElementById('room-tz').value)   || 0;
  document.getElementById('room-scale-val').textContent = scale;
  state.rendererModule.ccall('set_room_xform', null,
    ['number','number','number','number','number'], [scale, rotY, tx, ty, tz]);
}
['room-scale','room-roty','room-tx','room-ty','room-tz'].forEach(function(id) {
  document.getElementById(id).addEventListener('input', applyRoomXform);
});

function applyOverscan() {
  if (!state.rendererModule) return;
  const x = parseFloat(document.getElementById('overscan-x').value);
  const y = parseFloat(document.getElementById('overscan-y').value);
  state.rendererModule.ccall('set_overscan', null, ['number','number'], [x, y]);
}
document.getElementById('overscan-x').addEventListener('input', applyOverscan);
document.getElementById('overscan-y').addEventListener('input', applyOverscan);

function applyLampPos() {
  if (!state.rendererModule) return;
  const x = parseFloat(document.getElementById('lamp-x').value) || 0;
  const y = parseFloat(document.getElementById('lamp-y').value) || 0;
  const z = parseFloat(document.getElementById('lamp-z').value) || 0;
  state.rendererModule.ccall('set_lamp_pos', null, ['number','number','number'], [x, y, z]);
}
['lamp-x','lamp-y','lamp-z'].forEach(function(id) {
  document.getElementById(id).addEventListener('input', applyLampPos);
});

function applyLampIntensity() {
  if (!state.rendererModule) return;
  const v = parseFloat(document.getElementById('lamp-intensity').value);
  document.getElementById('lamp-intensity-val').textContent = v;
  state.rendererModule.ccall('set_lamp_intensity', null, ['number'], [v]);
}
document.getElementById('lamp-intensity').addEventListener('input', applyLampIntensity);

function applyTvIntensity() {
  if (!state.rendererModule) return;
  const v = parseFloat(document.getElementById('tv-intensity').value);
  document.getElementById('tv-intensity-val').textContent = v;
  state.rendererModule.ccall('set_tv_light_intensity', null, ['number'], [v]);
}
document.getElementById('tv-intensity').addEventListener('input', applyTvIntensity);

function applyConeParams() {
  if (!state.rendererModule) return;
  state.rendererModule.ccall('set_cone_yaw',   null, ['number'],
    [parseFloat(document.getElementById('cone-yaw').value)]);
  state.rendererModule.ccall('set_cone_pitch', null, ['number'],
    [parseFloat(document.getElementById('cone-pitch').value)]);
  state.rendererModule.ccall('set_cone_power', null, ['number'],
    [parseFloat(document.getElementById('cone-power').value)]);
}
['cone-yaw','cone-pitch','cone-power'].forEach(id =>
  document.getElementById(id).addEventListener('input', applyConeParams));

document.getElementById('my-y').addEventListener('input', function() {
  if (!state.rendererModule) return;
  state.rendererModule.ccall('set_local_y', null, ['number'], [parseFloat(this.value)]);
});

document.getElementById('cat-eye-height').addEventListener('input', function() {
  if (!state.rendererModule) return;
  state.rendererModule.ccall('set_cat_eye_height', null, ['number'], [parseFloat(this.value)]);
});

document.getElementById('player-model').addEventListener('change', function() {
  state.localModel = parseInt(this.value);
});

// ── Core readiness checks ─────────────────────────────────────
let _ps1CoreAvailable = false;

function setCoreIcon(id, ok, note) {
  const icon = document.getElementById('core-icon-' + id);
  if (icon) icon.textContent = ok ? '✅' : '❌';
  if (note !== undefined) {
    const noteEl = document.getElementById('core-note-' + id);
    if (noteEl) noteEl.textContent = note;
  }
}

function checkCores() {
  const coreFiles = { gbc: 'core_gbc.js', gba: 'core_gba.js', snes: 'core_snes.js' };
  Object.entries(coreFiles).forEach(function([id, file]) {
    fetch(file, { method: 'HEAD' })
      .then(function(r) { setCoreIcon(id, r.ok); })
      .catch(function()  { setCoreIcon(id, false); });
  });

  // PS1 also requires a BIOS file from the user
  fetch('core_ps1.js', { method: 'HEAD' })
    .then(function(r) {
      _ps1CoreAvailable = r.ok;
      if (!r.ok) { setCoreIcon('ps1', false, '(file missing)'); return; }
      setCoreIcon('ps1', false, '(needs BIOS)');
    })
    .catch(function() {
      _ps1CoreAvailable = false;
      setCoreIcon('ps1', false, '(file missing)');
    });

  // N64 needs both the JS bundle and assets.zip
  Promise.all([
    fetch('n64wasm.js',  { method: 'HEAD' }).then(r => r.ok).catch(() => false),
    fetch('assets.zip',  { method: 'HEAD' }).then(r => r.ok).catch(() => false),
  ]).then(function([wasm, assets]) {
    if (wasm && assets) { setCoreIcon('n64', true); }
    else { setCoreIcon('n64', false, wasm ? '(missing assets.zip)' : '(file missing)'); }
  });
}

checkCores();

// ── File inputs ───────────────────────────────────────────────
document.getElementById('bios-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = function(ev) {
    setBiosFile(file.name, new Uint8Array(ev.target.result));
    if (_ps1CoreAvailable) setCoreIcon('ps1', true, '');
    setStatus('BIOS ready: ' + file.name + ' — now load a PS1 disc');
  };
  reader.readAsArrayBuffer(file);
});

document.getElementById('gba-save-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = ev => {
    setGbaSave(new Uint8Array(ev.target.result));
    setStatus('GBA save loaded: ' + file.name);
  };
  reader.readAsArrayBuffer(file);
});

document.getElementById('rom-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  if (!file) return;
  const ext = file.name.split('.').pop().toLowerCase();

  if (N64_EXTS.has(ext)) {
    loadN64(file);
    return;
  }

  const bundle = CORE_MAP[ext];
  if (!bundle) {
    setStatus('Unsupported format: .' + ext);
    return;
  }

  if (PS1_EXTS.has(ext) && !ps1BiosLoaded) {
    setStatus('Load a PS1 BIOS (.bin) first, then reload your disc');
    return;
  }

  spawnCoreWorker(bundle, file, ext);
});

// ── Multiplayer button wiring ─────────────────────────────────
document.getElementById('btn-host').addEventListener('click', mpHost);
document.getElementById('btn-join').addEventListener('click', mpJoin);

// ── Input setup ───────────────────────────────────────────────
initInput();

// ── Eager renderer load ───────────────────────────────────────
// Load the 3D room immediately on page open — no ROM needed.
injectAndWait('game_renderer.js', function(mod) {
  state.rendererModule = mod;
  state.frontendGL     = window.GL;
  state.frontendCtx    = document.getElementById('canvas').getContext('webgl2');
  // Apply initial slider values now that the module is live
  applyRoomXform();
  applyOverscan();
  applyLampPos();
  applyLampIntensity();
  applyTvIntensity();
  applyConeParams();
  setStatus('Ready — drop a ROM to play');
});
