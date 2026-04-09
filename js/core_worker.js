// core_worker.js — Web Worker wrapper for libretro core bundles
// Loaded via: new Worker('core_worker.js')
// Receives:  { type:'load', bundle, romName, romBytes, biosName?, biosBytes?, saveBytes? }
//            { type:'button', id, pressed }
//            { type:'terminate' }
// Posts:     { type:'ready', sampleRate }
//            { type:'frame', buf, w, h }   (buf is a Transferable ArrayBuffer)
//            { type:'audio', samples }      (samples is a Transferable ArrayBuffer)

'use strict';

var coreM = null;        // our handle to the loaded Emscripten module
var frameInterval = null;
var audioReadPos = 0;
var audioBase = 0;
var audioBufSize = 0;

// Ping-pong buffers for zero-copy frame transfer.
// After postMessage with transfer, the sent buffer is detached.
// We alternate between two buffers so the worker always owns one.
var pingBuf = null;
var pongBuf = null;
var usesPing = true;

var audioBuf = null;   // pre-allocated Int16Array for audio accumulation; sized once in onReady

// Max PS1 frame = 640×480×4 = 1,228,800 bytes
var MAX_FRAME_BYTES = 1228800;

self.onmessage = function(e) {
  var msg = e.data;
  if (msg.type === 'load') {
    loadCore(msg);
  } else if (msg.type === 'button') {
    if (coreM) coreM._set_button(msg.port || 0, msg.id, msg.pressed ? 1 : 0);
  } else if (msg.type === 'axis') {
    if (coreM) coreM._set_analog(msg.port || 0, msg.stick, msg.axis, msg.value);
  } else if (msg.type === 'terminate') {
    if (frameInterval) { clearInterval(frameInterval); frameInterval = null; }
  }
};

function loadCore(msg) {
  // Tell Emscripten to resolve .wasm relative to the bundle path, not the worker location.
  var bundleDir = msg.bundle.substring(0, msg.bundle.lastIndexOf('/') + 1);
  self.Module = { locateFile: function(path) { return '/' + bundleDir + path; } };

  self.importScripts('/' + msg.bundle + '?v=' + msg.version);

  function check() {
    if (self.Module && self.Module.calledRun) {
      coreM = self.Module;
      onReady(msg);
    } else {
      setTimeout(check, 50);
    }
  }
  setTimeout(check, 100);
}

function onReady(msg) {
  // Write filesystem files before calling start_game.
  // retro_init() is now called inside start_game() (not in main()), so BIOS files
  // written here are available when the core initialises.
  try { coreM.FS.mkdir('/system'); } catch(e) {}
  if (msg.biosType === 'saturn') {
    var satBios = new Uint8Array(msg.biosBytes);
    coreM.FS.writeFile('/system/saturn_bios.bin', satBios);
    coreM.FS.writeFile('/system/sega_101.bin',    satBios);
    coreM.FS.writeFile('/system/mpr-17933.bin',   satBios);
  } else if (msg.biosBytes && msg.biosName) {
    coreM.FS.writeFile('/system/' + msg.biosName, new Uint8Array(msg.biosBytes));
  }
  if (msg.saveBytes) {
    try { coreM.FS.mkdir('/saves'); } catch(e) {}
    coreM.FS.writeFile('/saves/rom.srm', new Uint8Array(msg.saveBytes));
  }
  coreM.FS.writeFile('/' + msg.romName, new Uint8Array(msg.romBytes));
  coreM.ccall('start_game', 'void', ['string'], ['/' + msg.romName]);

  // Notify main thread: core is ready, send sample rate for AudioContext setup
  self.postMessage({ type: 'ready', sampleRate: coreM._get_audio_sample_rate() });

  // Sync audio read cursor to wherever the write cursor is now
  audioReadPos = coreM._get_audio_write_pos();
  audioBase    = coreM._get_audio_buf_ptr() >> 1;  // HEAP16 index
  audioBufSize = coreM._get_audio_buf_size();

  // Pre-allocate ping-pong frame buffers
  pingBuf = new ArrayBuffer(MAX_FRAME_BYTES);
  pongBuf = new ArrayBuffer(MAX_FRAME_BYTES);
  // Pre-allocate audio accumulation buffer — reused every tick, avoids per-frame allocation
  audioBuf = new Int16Array(audioBufSize);

  // Run one emulator frame every ~16ms (~60fps)
  frameInterval = setInterval(tickFrame, 16);
}

function tickFrame() {
  // Run one libretro frame — fires retro_video_refresh + retro_audio_*
  coreM._step_frame();

  // ── Frame transfer ────────────────────────────────────────────────────────
  var ptr = coreM._get_frame_ptr();
  var fw  = coreM._get_frame_w();
  var fh  = coreM._get_frame_h();

  if (ptr && fw > 0 && fh > 0) {
    var byteCount = fw * fh * 4;

    // Pick the buffer we own this tick
    var buf = usesPing ? pingBuf : pongBuf;

    // Grow the buffer if the resolution increased (e.g. PS1 widescreen modes)
    if (!buf || buf.byteLength < byteCount) {
      buf = new ArrayBuffer(byteCount);
      if (usesPing) pingBuf = buf;
      else          pongBuf = buf;
    }

    // Copy WASM heap → local ArrayBuffer
    var src  = new Uint8Array(coreM.HEAPU8.buffer, ptr, byteCount);
    var dest = new Uint8Array(buf, 0, byteCount);
    dest.set(src);

    // Transfer ownership to main thread (zero-copy — buf is detached here after this call)
    self.postMessage({ type: 'frame', buf: buf, w: fw, h: fh }, [buf]);

    // Flip — next tick we use the other buffer
    usesPing = !usesPing;
  }

  // ── Audio transfer ────────────────────────────────────────────────────────
  var wp    = coreM._get_audio_write_pos();
  var avail = (wp - audioReadPos + audioBufSize) % audioBufSize;

  if (avail > 0) {
    var h16 = coreM.HEAP16;
    for (var i = 0; i < avail; i++) {
      audioBuf[i] = h16[audioBase + ((audioReadPos + i) % audioBufSize)];
    }
    audioReadPos = (audioReadPos + avail) % audioBufSize;
    // Slice into a fresh transferable buffer (audioBuf itself must remain reusable)
    var out = audioBuf.buffer.slice(0, avail * 2);
    self.postMessage({ type: 'audio', samples: out }, [out]);
  }
}
