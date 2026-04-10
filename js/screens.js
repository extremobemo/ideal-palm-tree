// Screen state machine: landing → carousel (character select) → room.
// Extracted from app.js to keep navigation logic in one place.

import { state, setState } from './state.js';
import * as renderer from './renderer.js';
import { mpHost, mpJoin } from './multiplayer.js';
import { setStatus } from './utils.js';
import { CHAR_NAMES } from './config.js';

let _carouselIdx   = 0;
let _pendingIsHost = false;
let _pendingHostId = null;

// ── Screen transitions ───────────────────────────────────────

export function showScreen(name) {
  ['landing-screen', 'carousel-screen', 'carousel-controls', 'join-prompt',
   'ui', 'mp-bar', 'settings-btn', 'controls-btn', 'fullscreen-btn'].forEach(function(id) {
    const el = document.getElementById(id);
    if (el) el.classList.add('hidden');
  });
  if (name === 'landing') {
    document.getElementById('landing-screen').classList.remove('hidden');
  } else if (name === 'carousel') {
    document.getElementById('carousel-screen').classList.remove('hidden');
    document.getElementById('carousel-controls').classList.remove('hidden');
  } else if (name === 'room') {
    ['ui', 'mp-bar', 'settings-btn', 'controls-btn', 'fullscreen-btn', 'overlay'].forEach(function(id) {
      document.getElementById(id).classList.remove('hidden');
    });
  }
}

// ── Carousel helpers ─────────────────────────────────────────

function updateCarouselDisplay() {
  document.getElementById('carousel-char-name').textContent = CHAR_NAMES[_carouselIdx];
  renderer.setPreviewMode(_carouselIdx);
}

// openCarousel needs a reference to loadRenderer from app.js; it's passed via init.
let _loadRenderer = null;

function openCarousel() {
  _carouselIdx = 0;
  if (_loadRenderer) _loadRenderer();
  updateCarouselDisplay();
  showScreen('carousel');
  renderer.setPreviewMode(_carouselIdx);
}

function showJoinPrompt() {
  document.getElementById('join-prompt').classList.remove('hidden');
  document.getElementById('join-hostid').value = '';
  document.getElementById('join-prompt-status').textContent = '';
  document.getElementById('join-hostid').focus();
}

// ── Init — wire all screen navigation ────────────────────────

export function initScreens(loadRenderer) {
  _loadRenderer = loadRenderer;

  const previewNameplate = document.getElementById('preview-nameplate');

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
    _carouselIdx = (_carouselIdx + CHAR_NAMES.length - 1) % CHAR_NAMES.length;
    updateCarouselDisplay();
  });
  document.getElementById('carousel-next').addEventListener('click', function() {
    _carouselIdx = (_carouselIdx + 1) % CHAR_NAMES.length;
    updateCarouselDisplay();
  });

  // Carousel confirm
  document.getElementById('carousel-confirm').addEventListener('click', function() {
    setState('localModel', _carouselIdx);
    setState('localName', document.getElementById('carousel-name').value.trim().slice(0, 20) || 'Player');
    document.getElementById('player-model').value = _carouselIdx;
    document.getElementById('local-name').value   = state.localName;
    previewNameplate.classList.add('hidden');
    renderer.exitPreviewMode();
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

  // Preview nameplate
  document.getElementById('carousel-name').addEventListener('input', function() {
    const name = this.value.trim();
    if (name) {
      previewNameplate.textContent = name;
      previewNameplate.classList.remove('hidden');
    } else {
      previewNameplate.classList.add('hidden');
    }
  });
}

// Used by app.js when the renderer loads after the carousel is already showing.
export function getCarouselIdx() { return _carouselIdx; }
