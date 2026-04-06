// Virtual gamepad layer for N64 multi-player.
// Monkey-patches navigator.getGamepads() so Emscripten/SDL sees guest button
// inputs as physical gamepads without any changes to n64.js.
//
// SDL scans for joysticks during SDL_Init (inside callMain).  Virtual pads must
// exist BEFORE callMain so SDL opens their slots and polls them each frame.
// Call initN64VirtualPads() just before state.n64Module.callMain() to pre-register
// a neutral dummy at slot 0 (player 1 gamepad — keyboard still takes precedence)
// and an empty guest pad at slot 1 (player 2).  setVirtualButton() then updates
// slot 1 as guest button events arrive.

const _virtualPads = [];

function makeVirtualPad(index) {
  return {
    id: 'Virtual Gamepad ' + index,
    index,
    connected: true,
    timestamp: performance.now(),
    mapping: 'standard',
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
    axes: [0, 0, 0, 0],
  };
}

// Call this BEFORE n64Module.callMain() so SDL sees the pads during SDL_Init.
export function initN64VirtualPads() {
  for (let i = 0; i <= 1; i++) {
    while (_virtualPads.length <= i) _virtualPads.push(null);
    if (!_virtualPads[i]) _virtualPads[i] = makeVirtualPad(i);
  }
}

export function setVirtualButton(padIndex, buttonIndex, pressed) {
  while (_virtualPads.length <= padIndex) _virtualPads.push(null);
  if (!_virtualPads[padIndex]) _virtualPads[padIndex] = makeVirtualPad(padIndex);
  const btn = _virtualPads[padIndex].buttons[buttonIndex];
  if (!btn) return;
  btn.pressed = pressed;
  btn.touched = pressed;
  btn.value   = pressed ? 1 : 0;
  _virtualPads[padIndex].timestamp = performance.now();
}

export function setVirtualAxis(padIndex, axisIndex, value) {
  while (_virtualPads.length <= padIndex) _virtualPads.push(null);
  if (!_virtualPads[padIndex]) _virtualPads[padIndex] = makeVirtualPad(padIndex);
  _virtualPads[padIndex].axes[axisIndex] = value;
  _virtualPads[padIndex].timestamp = performance.now();
}

// Install the patch once at module load time.
// Real gamepads are returned as-is; virtual pads fill any empty slots after them.
if (navigator.getGamepads) {
  const _real = navigator.getGamepads.bind(navigator);
  navigator.getGamepads = function() {
    const real   = Array.from(_real());
    const result = [...real];
    for (let i = 0; i < _virtualPads.length; i++) {
      if (_virtualPads[i] && !result[i]) result[i] = _virtualPads[i];
    }
    return result;
  };
}
