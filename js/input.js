// Keyboard, mouse, and gamepad input handlers.
// WASD / left-stick / mouse / right-stick → renderer (player movement + camera)
// D-pad / face / shoulder buttons → libretro core (port 0 for host, relayed for guests)

import { state } from './state.js';
import { mpSendButton } from './multiplayer.js';

const MOVE_MAP = { 'KeyW': 0, 'KeyS': 1, 'KeyA': 2, 'KeyD': 3 };

// RETRO_DEVICE_ID_JOYPAD button IDs:
// B=0 Y=1 SELECT=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 A=8 X=9 L=10 R=11 L2=12 R2=13 L3=14 R3=15
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

// Standard Gamepad API button index → RETRO_DEVICE_ID_JOYPAD
const PAD_BUTTON_MAP = [
  [0,  8],  // A (south)  → RETRO A
  [1,  0],  // B (east)   → RETRO B
  [2,  9],  // X (west)   → RETRO X
  [3,  1],  // Y (north)  → RETRO Y
  [4,  10], // L1         → RETRO L
  [5,  11], // R1         → RETRO R
  [6,  12], // L2         → RETRO L2
  [7,  13], // R2         → RETRO R2
  [8,  2],  // Select     → RETRO SELECT
  [9,  3],  // Start      → RETRO START
  [10, 14], // L3         → RETRO L3
  [11, 15], // R3         → RETRO R3
  [12, 4],  // D-up       → RETRO UP
  [13, 5],  // D-down     → RETRO DOWN
  [14, 6],  // D-left     → RETRO LEFT
  [15, 7],  // D-right    → RETRO RIGHT
];

const _padPrev = {};   // { [gamepadIndex]: { buttons: bool[] } }

// Send a game button press to the core.
// Host always owns port 0; guests send to the host which assigns their port.
function sendGameButton(id, pressed) {
  if (state.mpIsHost) {
    if (state.coreWorker)
      state.coreWorker.postMessage({ type: 'button', port: 0, id, pressed });
  } else if (state.mpConnected) {
    mpSendButton(id, pressed);
  } else if (state.coreWorker) {
    state.coreWorker.postMessage({ type: 'button', port: 0, id, pressed });
  }
}

function pollGamepads() {
  const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (let gi = 0; gi < gamepads.length; gi++) {
    const gp = gamepads[gi];
    if (!gp) continue;

    if (!_padPrev[gi])
      _padPrev[gi] = { buttons: new Array(gp.buttons.length).fill(false) };
    const prev = _padPrev[gi];

    // ── Game buttons (face, dpad, shoulder) ──────────────────────────────────
    for (const [btnIdx, retroId] of PAD_BUTTON_MAP) {
      if (btnIdx >= gp.buttons.length) continue;
      const pressed = gp.buttons[btnIdx].pressed;
      if (pressed !== prev.buttons[btnIdx]) {
        prev.buttons[btnIdx] = pressed;
        sendGameButton(retroId, pressed);
      }
    }

  }
  requestAnimationFrame(pollGamepads);
}

export function initInput() {
  const canvas  = document.getElementById('canvas');
  const overlay = document.getElementById('overlay');

  canvas.addEventListener('click', () => {
    if (!document.getElementById('carousel-controls').classList.contains('hidden')) return;
    canvas.requestPointerLock();
  });

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
    const tag = document.activeElement && document.activeElement.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if (MOVE_MAP[e.code] !== undefined && state.rendererModule) {
      e.preventDefault();
      state.rendererModule.ccall('set_move_key', 'void',
        ['number','number'], [MOVE_MAP[e.code], 1]);
    }
    if (GAME_MAP[e.code] !== undefined) {
      e.preventDefault();
      sendGameButton(GAME_MAP[e.code], true);
    }
  });

  document.addEventListener('keyup', e => {
    const tag = document.activeElement && document.activeElement.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    if (MOVE_MAP[e.code] !== undefined && state.rendererModule)
      state.rendererModule.ccall('set_move_key', 'void',
        ['number','number'], [MOVE_MAP[e.code], 0]);
    if (GAME_MAP[e.code] !== undefined)
      sendGameButton(GAME_MAP[e.code], false);
  });

  // Start gamepad polling — cheap no-op when no controller is connected
  requestAnimationFrame(pollGamepads);
}
