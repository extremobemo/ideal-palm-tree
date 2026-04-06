// Entry point: canvas setup, renderer loading, ROM dispatch, slider wiring,
// BIOS/save file loading, and core readiness status checks.

import { state } from './state.js';
import { setStatus } from './utils.js';
import { spawnCoreWorker, setBiosFile, ps1BiosLoaded, setSaturnBiosFile, saturnBiosLoaded } from './worker-bridge.js';
import { loadN64 } from './n64.js';
import { mpHost, mpJoin, broadcastScene } from './multiplayer.js';
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
  'iso': 'core_saturn.js',
  'ccd': 'core_saturn.js',
};
// Extensions shared between PS1 and Saturn — trigger a system picker
const DISC_EXTS = new Set(['bin', 'cue', 'chd', 'img']);
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

document.getElementById('local-name').addEventListener('input', function() {
  state.localName = this.value.trim().slice(0, 20);
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
  // Simple cores — just check file presence
  ['gbc', 'gba', 'snes'].forEach(function(id) {
    fetch('core_' + id + '.js', { method: 'HEAD' })
      .then(function(r) { setCoreIcon(id, r.ok); })
      .catch(function()  { setCoreIcon(id, false); });
  });

  // PS1 — requires BIOS; track availability for the BIOS upload handler
  fetch('core_ps1.js', { method: 'HEAD' })
    .then(function(r) {
      _ps1CoreAvailable = r.ok;
      setCoreIcon('ps1', false, r.ok ? '(needs BIOS)' : '(file missing)');
    })
    .catch(function() { _ps1CoreAvailable = false; setCoreIcon('ps1', false, '(file missing)'); });

  // Saturn — requires BIOS
  fetch('core_saturn.js', { method: 'HEAD' })
    .then(function(r) { setCoreIcon('saturn', false, r.ok ? '(needs BIOS)' : '(file missing)'); })
    .catch(function() { setCoreIcon('saturn', false, '(file missing)'); });

  // N64 needs both the JS bundle and assets.zip
  Promise.all([
    fetch('n64wasm.js', { method: 'HEAD' }).then(r => r.ok).catch(() => false),
    fetch('assets.zip', { method: 'HEAD' }).then(r => r.ok).catch(() => false),
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

document.getElementById('saturn-bios-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = function(ev) {
    setSaturnBiosFile(new Uint8Array(ev.target.result));
    setCoreIcon('saturn', true, '');
    setStatus('Saturn BIOS ready: ' + file.name + ' — now load a Saturn disc');
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

  if (DISC_EXTS.has(ext)) {
    _discPickerFile = file;
    _discPickerExt  = ext;
    document.getElementById('disc-prompt').classList.remove('hidden');
    return;
  }

  const bundle = CORE_MAP[ext];
  if (!bundle) {
    setStatus('Unsupported format: .' + ext);
    return;
  }

  if (bundle === 'core_saturn.js' && !saturnBiosLoaded) {
    setStatus('Load a Saturn BIOS (.bin) first via Settings, then reload your disc');
    return;
  }

  spawnCoreWorker(bundle, file, ext);
});

// ── Disc system picker ────────────────────────────────────────
let _discPickerFile = null;
let _discPickerExt  = null;

function _launchDisc(bundle) {
  document.getElementById('disc-prompt').classList.add('hidden');
  const file = _discPickerFile;
  const ext  = _discPickerExt;
  _discPickerFile = null;
  _discPickerExt  = null;
  spawnCoreWorker(bundle, file, ext);
}

const _discSystems = [
  { id: 'disc-pick-ps1',    bundle: 'core_ps1.js',    biosCheck: () => ps1BiosLoaded,    biosMsg: 'Load a PS1 BIOS (.bin) first, then reload your disc' },
  { id: 'disc-pick-saturn', bundle: 'core_saturn.js', biosCheck: () => saturnBiosLoaded, biosMsg: 'Load a Saturn BIOS (.bin) first via Settings, then reload your disc' },
];
_discSystems.forEach(function({ id, bundle, biosCheck, biosMsg }) {
  document.getElementById(id).addEventListener('click', function() {
    if (!biosCheck()) {
      document.getElementById('disc-prompt').classList.add('hidden');
      setStatus(biosMsg);
      return;
    }
    _launchDisc(bundle);
  });
});

// ── Screen state machine ──────────────────────────────────────
const CHAR_NAMES = ['Cat', 'Incidental 70', 'Mech'];
let _carouselIdx   = 0;
let _pendingIsHost = false;
let _pendingHostId = null;


function showScreen(name) {
  ['landing-screen', 'carousel-screen', 'carousel-controls', 'join-prompt',
   'ui', 'mp-bar', 'settings-btn'].forEach(function(id) {
    const el = document.getElementById(id);
    if (el) el.classList.add('hidden');
  });
  if (name === 'landing') {
    document.getElementById('landing-screen').classList.remove('hidden');
  } else if (name === 'carousel') {
    document.getElementById('carousel-screen').classList.remove('hidden');
    document.getElementById('carousel-controls').classList.remove('hidden');
  } else if (name === 'room') {
    ['ui', 'mp-bar', 'settings-btn', 'overlay'].forEach(function(id) {
      document.getElementById(id).classList.remove('hidden');
    });
  }
}

function updateCarouselDisplay() {
  document.getElementById('carousel-char-name').textContent = CHAR_NAMES[_carouselIdx];
  if (state.rendererModule)
    state.rendererModule.ccall('set_preview_mode', null, ['number'], [_carouselIdx]);
}

function openCarousel() {
  _carouselIdx = 0;
  loadRenderer();
  updateCarouselDisplay();
  showScreen('carousel');
  if (state.rendererModule)
    state.rendererModule.ccall('set_preview_mode', null, ['number'], [_carouselIdx]);
}

function showJoinPrompt() {
  document.getElementById('join-prompt').classList.remove('hidden');
  document.getElementById('join-hostid').value = '';
  document.getElementById('join-prompt-status').textContent = '';
  document.getElementById('join-hostid').focus();
}

// Landing buttons
document.getElementById('btn-landing-host').addEventListener('click', function() {
  _pendingIsHost = true;
  openCarousel();
});
document.getElementById('btn-landing-join').addEventListener('click', function() {
  _pendingIsHost = false;
  showJoinPrompt();
});

// Carousel arrows
document.getElementById('carousel-prev').addEventListener('click', function() {
  _carouselIdx = (_carouselIdx + 2) % 3;
  updateCarouselDisplay();
});
document.getElementById('carousel-next').addEventListener('click', function() {
  _carouselIdx = (_carouselIdx + 1) % 3;
  updateCarouselDisplay();
});

// Carousel confirm
document.getElementById('carousel-confirm').addEventListener('click', function() {
  state.localModel = _carouselIdx;
  state.localName  = document.getElementById('carousel-name').value.trim().slice(0, 20) || 'Player';
  document.getElementById('player-model').value = _carouselIdx;
  document.getElementById('local-name').value   = state.localName;
  _previewNameplate.classList.add('hidden');
  if (state.rendererModule)
    state.rendererModule.ccall('exit_preview_mode', null, [], []);
  if (_pendingIsHost) {
    showScreen('room');
    mpHost();
  } else {
    showScreen('room');
    document.getElementById('load-rom-label').style.display = 'none';
    setStatus('');
    document.getElementById('mp-status').textContent = 'Room ID: ' + _pendingHostId;
    mpJoin(_pendingHostId);
  }
});

// Join prompt — store host ID then proceed to character select
document.getElementById('join-confirm').addEventListener('click', function() {
  const hostId = document.getElementById('join-hostid').value.trim();
  if (!hostId) {
    document.getElementById('join-prompt-status').textContent = 'Enter a peer ID first';
    return;
  }
  _pendingHostId = hostId;
  document.getElementById('join-prompt').classList.add('hidden');
  openCarousel();
});
document.getElementById('join-hostid').addEventListener('keydown', function(e) {
  if (e.key === 'Enter') document.getElementById('join-confirm').click();
});

// ── Preview nameplate ─────────────────────────────────────────
const _previewNameplate = document.getElementById('preview-nameplate');
document.getElementById('carousel-name').addEventListener('input', function() {
  const name = this.value.trim();
  if (name) {
    _previewNameplate.textContent = name;
    _previewNameplate.classList.remove('hidden');
  } else {
    _previewNameplate.classList.add('hidden');
  }
});

// ── Scene broadcast ───────────────────────────────────────────
// Any change to a scene control is forwarded to connected guests.
document.querySelector('.scene-section').addEventListener('input', broadcastScene);

// ── Input setup ───────────────────────────────────────────────
initInput();

// ── Renderer load (deferred until character select) ───────────
let _rendererLoading = false;
function loadRenderer() {
  if (_rendererLoading || state.rendererModule) return;
  _rendererLoading = true;
  injectAndWait('game_renderer.js', function(mod) {
    state.rendererModule = mod;
    state.frontendGL     = window.GL;
    state.frontendCtx    = document.getElementById('canvas').getContext('webgl2');
    applyRoomXform();
    applyOverscan();
    applyLampPos();
    applyLampIntensity();
    applyTvIntensity();
    applyConeParams();
    setStatus('Ready — drop a ROM to play');
    // If carousel is already showing when renderer finishes, activate preview
    if (!document.getElementById('carousel-controls').classList.contains('hidden'))
      mod.ccall('set_preview_mode', null, ['number'], [_carouselIdx]);
    // Fade in canvas now that the first frame is ready — avoids black flash on init
    requestAnimationFrame(() => {
      document.getElementById('canvas').style.opacity = '1';
    });
  });
}

// ── Initial screen ────────────────────────────────────────────
showScreen('landing');
