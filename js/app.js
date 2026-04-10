// Entry point: canvas setup, renderer loading, and module initialization.
// All domain logic has been extracted into focused modules:
//   screens.js     — landing / carousel / room state machine
//   settings-ui.js — slider wiring for scene, lighting, audio
//   controls-ui.js — key bindings panel
//   rom-loader.js  — ROM dispatch, disc picker, BIOS inputs

import { state, setState } from './state.js';
import { setStatus } from './utils.js';
import { initInput } from './input.js';
import { initControlsPanel } from './controls-ui.js';
import { initSettingsUI, applyAllSettings } from './settings-ui.js';
import { initRomLoader } from './rom-loader.js';
import { initScreens, showScreen, getCarouselIdx } from './screens.js';
import * as renderer from './renderer.js';
import { CANVAS_ASPECT, CANVAS_MAX_WIDTH_RATIO, CANVAS_PADDING, BUILD_DIR } from './config.js';

// ── Canvas sizing ────────────────────────────────────────────
const canvas = document.getElementById('canvas');

const _nativeWidthDesc  = Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype, 'width');
const _nativeHeightDesc = Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype, 'height');

function _lockCanvas() {
  Object.defineProperty(canvas, 'width', {
    get()  { return _nativeWidthDesc.get.call(this); },
    set()  {},
    configurable: true
  });
  Object.defineProperty(canvas, 'height', {
    get()  { return _nativeHeightDesc.get.call(this); },
    set()  {},
    configurable: true
  });
}

function _setCanvasSize(w, h) {
  // Remove own property overrides so the native prototype setter is reachable
  delete canvas.width;
  delete canvas.height;
  canvas.width  = w;
  canvas.height = h;
  _lockCanvas();
}

(function sizeCanvas() {
  const maxW = Math.round(window.innerWidth * CANVAS_MAX_WIDTH_RATIO);
  const maxH = window.innerHeight - CANVAS_PADDING;
  let w = maxW, h = Math.round(maxW / CANVAS_ASPECT);
  if (h > maxH) { h = maxH; w = Math.round(h * CANVAS_ASPECT); }
  canvas.width  = w;
  canvas.height = h;
  // Lock dimensions — n64wasm's SDL fires resize events that would otherwise
  // shrink this canvas to N64 native resolution.
  _lockCanvas();
})();

// ── Fullscreen ───────────────────────────────────────────────
let _windowedW, _windowedH;

document.getElementById('fullscreen-btn').addEventListener('click', function() {
  if (document.fullscreenElement) {
    document.exitFullscreen();
  } else {
    canvas.requestFullscreen().catch(function(err) {
      console.error('Fullscreen request failed:', err);
    });
  }
});

document.addEventListener('fullscreenchange', function() {
  if (document.fullscreenElement === canvas) {
    _windowedW = canvas.width;
    _windowedH = canvas.height;
    _setCanvasSize(screen.width * devicePixelRatio, screen.height * devicePixelRatio);
  } else {
    _setCanvasSize(_windowedW, _windowedH);
  }
  renderer.resizeCanvas();
});

// ── Global error display ─────────────────────────────────────
window.onerror = function(msg, src, line) {
  setStatus('JS error: ' + msg + ' (' + (src || '').split('/').pop() + ':' + line + ')');
};

// ── Renderer loader ──────────────────────────────────────────

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

let _rendererLoading = false;

function onRendererProgress(msg) {
  const fill = document.getElementById('load-progress-fill');
  if (!fill) return;
  const m = msg.match(/\((\d+)\/(\d+)\)/);
  if (m) {
    const loaded = parseInt(m[1]), total = parseInt(m[2]);
    fill.style.width = Math.round(loaded / total * 100) + '%';
  } else if (msg === '' || msg === 'Running...') {
    fill.style.width = '100%';
    setTimeout(function() {
      const wrap = document.getElementById('load-progress-wrap');
      if (wrap) wrap.style.display = 'none';
      document.getElementById('landing-btn-row').classList.remove('hidden');
    }, 400);
  }
}

export function loadRenderer() {
  if (_rendererLoading || state.rendererModule) return;
  _rendererLoading = true;
  window.Module = window.Module || {};
  window.Module.setStatus = onRendererProgress;
  injectAndWait(BUILD_DIR + 'game_renderer.js', function(mod) {
    setState('rendererModule', mod);
    setState('frontendGL', window.GL);
    setState('frontendCtx', document.getElementById('canvas').getContext('webgl2'));
    applyAllSettings();
    setStatus('Ready — drop a ROM to play');
    // If carousel is already showing when renderer finishes, activate preview
    if (!document.getElementById('carousel-controls').classList.contains('hidden'))
      renderer.setPreviewMode(getCarouselIdx());
    // Fade in canvas now that the first frame is ready
    requestAnimationFrame(() => {
      document.getElementById('canvas').style.opacity = '1';
    });
  });
}

// ── Init all modules ─────────────────────────────────────────
initInput();
initControlsPanel();
initSettingsUI();
initRomLoader();
initScreens(loadRenderer);

// Start the 74 MB .data download immediately and show the landing screen
loadRenderer();
showScreen('landing');
