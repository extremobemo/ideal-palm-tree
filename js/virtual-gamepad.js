// Virtual gamepad layer for N64 multi-player.
// Monkey-patches navigator.getGamepads() so Emscripten/SDL sees guest button
// inputs as physical gamepads at indices 1-N without any changes to n64.js.

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
