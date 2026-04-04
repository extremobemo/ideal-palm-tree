// Shared mutable state accessed across modules.
// All fields are set by the module that owns them; other modules read via import.

export const state = {
  rendererModule: null,  // game_renderer.js Emscripten module — set on page load
  coreWorker:     null,  // active libretro Web Worker (null when idle)
  frontendGL:     null,  // Emscripten GL table, captured at renderer load
  frontendCtx:    null,  // WebGL2 context of #canvas
  n64Running:     false, // true while N64Wasm copyLoop is active
  n64Module:      null,  // N64Wasm module handle
  mpIsHost:       false, // true when this client is hosting a multiplayer session
  mpConnected:    false, // true when this client is connected as a guest
  controller:    'host', // 'host' or a conn index — who currently controls the game
  isController:  false,  // guest-side: true when this guest has been given control
  localModel:     0,     // avatar model index (0=cat, 1=incidental_70, 2=mech), sent in position packets
  localName:      '',    // display name shown above this player's head to others
};

// Off-screen canvas used to capture game frames for WebRTC streaming.
// Written to by worker-bridge.js (libretro frames) and n64.js (N64 copyLoop).
// Read by multiplayer.js via shareCanvas.captureStream().
export const shareCanvas = document.createElement('canvas');
shareCanvas.width = 640;
shareCanvas.height = 480;
export const shareCtx = shareCanvas.getContext('2d');
