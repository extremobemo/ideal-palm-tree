// Keyboard and mouse input handlers.
// WASD/mouse → renderer (player movement via ccall)
// Arrow keys / game buttons → libretro core Worker (via postMessage)

import { state } from './state.js';

const MOVE_MAP = { 'KeyW': 0, 'KeyS': 1, 'KeyA': 2, 'KeyD': 3 };

// RETRO_DEVICE_ID_JOYPAD button IDs:
// B=0 Y=1 SELECT=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 A=8 L=10 R=11
const GAME_MAP = {
  'ArrowUp':    4,
  'ArrowDown':  5,
  'ArrowLeft':  6,
  'ArrowRight': 7,
  'Enter':      3,
  'ShiftLeft':  2,
  'ShiftRight': 2,
  'KeyZ':       8,   // A button
  'KeyX':       0,   // B button
  'KeyQ':      10,   // L
  'KeyE':      11,   // R
};

export function initInput() {
  const canvas  = document.getElementById('canvas');
  const overlay = document.getElementById('overlay');

  canvas.addEventListener('click', () => canvas.requestPointerLock());

  document.addEventListener('pointerlockchange', () => {
    overlay.classList.add('hidden');
  });

  document.addEventListener('mousemove', e => {
    if (document.pointerLockElement === canvas && state.rendererModule) {
      state.rendererModule.ccall('add_mouse_delta', 'void',
        ['number','number'], [e.movementX, e.movementY]);
    }
  });

  document.addEventListener('keydown', e => {
    if (MOVE_MAP[e.code] !== undefined && state.rendererModule) {
      e.preventDefault();
      state.rendererModule.ccall('set_move_key', 'void',
        ['number','number'], [MOVE_MAP[e.code], 1]);
    }
    if (GAME_MAP[e.code] !== undefined) {
      e.preventDefault();
      if (state.coreWorker)
        state.coreWorker.postMessage({ type: 'button', id: GAME_MAP[e.code], pressed: true });
    }
  });

  document.addEventListener('keyup', e => {
    if (MOVE_MAP[e.code] !== undefined && state.rendererModule)
      state.rendererModule.ccall('set_move_key', 'void',
        ['number','number'], [MOVE_MAP[e.code], 0]);
    if (GAME_MAP[e.code] !== undefined && state.coreWorker)
      state.coreWorker.postMessage({ type: 'button', id: GAME_MAP[e.code], pressed: false });
  });
}
