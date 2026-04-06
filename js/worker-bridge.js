// Libretro core Worker lifecycle: spawn, terminate, and message handling.
// Also owns BIOS/save file state and the frame-upload + quad-color utilities
// shared by n64.js and multiplayer.js.

import { state, shareCanvas, shareCtx } from './state.js';
import { startAudio, receiveAudio } from './audio.js';
import { setStatus } from './utils.js';

// BIOS/save state — kept module-local, sent to worker on spawn
let _ps1BiosName  = null;
let _ps1BiosBytes = null;
export let ps1BiosLoaded = false;

export function setBiosFile(name, bytes) {
  _ps1BiosName  = name;
  _ps1BiosBytes = bytes;
  ps1BiosLoaded = true;
}

let _saturnBiosBytes = null;
export let saturnBiosLoaded = false;

export function setSaturnBiosFile(bytes) {
  _saturnBiosBytes = bytes;
  saturnBiosLoaded = true;
}


// Scratch canvas for frame mirroring to shareCanvas (WebRTC host streaming)
const frameTmp = document.createElement('canvas');
let   frameTmpCtx = null;

// 2×2 sample canvas for jsUpdateQuadColors (canvas/video source path)
const _quadSampleCanvas = document.createElement('canvas');
_quadSampleCanvas.width = 2; _quadSampleCanvas.height = 2;
const _quadSampleCtx = _quadSampleCanvas.getContext('2d', { willReadFrequently: true });

// Sample a video source (canvas or video element) into 4 quadrant average colours.
// Used by n64.js (copyLoop) and multiplayer.js (guest video stream).
export function jsUpdateQuadColors(source) {
  if (!state.rendererModule) return;
  try {
    _quadSampleCtx.drawImage(source, 0, 0, 2, 2);
    const d = _quadSampleCtx.getImageData(0, 0, 2, 2).data;
    // 2×2 layout (y=0 top): d[0..3]=TL d[4..7]=TR d[8..11]=BL d[12..15]=BR
    // Must match C++ quad order: q0=left-bottom, q1=right-bottom, q2=left-top, q3=right-top
    state.rendererModule.ccall('set_tv_quad_colors', null,
      ['number','number','number','number','number','number',
       'number','number','number','number','number','number'],
      [d[8]/255,  d[9]/255,  d[10]/255,   // q0 BL
       d[12]/255, d[13]/255, d[14]/255,   // q1 BR
       d[0]/255,  d[1]/255,  d[2]/255,    // q2 TL
       d[4]/255,  d[5]/255,  d[6]/255]);  // q3 TR
  } catch(e) { console.warn('jsUpdateQuadColors:', e); }
}

// Sample 4 quadrant average colors from a raw RGBA buffer and push to renderer.
// Used by libretro core frames received from the worker (avoids a canvas round-trip).
function jsUpdateQuadColors_fromBuffer(rgba, fw, fh) {
  if (!state.rendererModule) return;
  const hw = fw >> 1, hh = fh >> 1;
  // q0=left-bottom, q1=right-bottom, q2=left-top, q3=right-top
  const regions = [
    [0,  hw, hh, fh],
    [hw, fw, hh, fh],
    [0,  hw, 0,  hh],
    [hw, fw, 0,  hh],
  ];
  const cols = [];
  for (const [x0, x1, y0, y1] of regions) {
    let r = 0, g = 0, b = 0, cnt = 0;
    for (let y = y0; y < y1; y += 4)
      for (let x = x0; x < x1; x += 4) {
        const i = (y * fw + x) * 4;
        r += rgba[i]; g += rgba[i+1]; b += rgba[i+2]; cnt++;
      }
    cols.push(r/(cnt*255), g/(cnt*255), b/(cnt*255));
  }
  state.rendererModule.ccall('set_tv_quad_colors', null, Array(12).fill('number'), cols);
}

// Upload a game frame (received from the worker) into the TV screen texture.
export function receiveFrame(buf, fw, fh) {
  if (!state.rendererModule || !state.frontendGL || !state.frontendCtx) return;
  state.rendererModule.ccall('set_frame_size', 'void', ['number','number'], [fw, fh]);
  const texId = state.rendererModule.ccall('get_game_tex_id', 'number', [], []);
  const tex = state.frontendGL.textures[texId];
  if (!tex) return;

  const rgba = new Uint8ClampedArray(buf, 0, fw * fh * 4);
  state.frontendCtx.bindTexture(state.frontendCtx.TEXTURE_2D, tex);
  state.frontendCtx.texImage2D(state.frontendCtx.TEXTURE_2D, 0,
    state.frontendCtx.RGBA, fw, fh, 0, state.frontendCtx.RGBA,
    state.frontendCtx.UNSIGNED_BYTE, rgba);
  state.frontendCtx.texParameteri(state.frontendCtx.TEXTURE_2D,
    state.frontendCtx.TEXTURE_MIN_FILTER, state.frontendCtx.LINEAR);
  state.frontendCtx.texParameteri(state.frontendCtx.TEXTURE_2D,
    state.frontendCtx.TEXTURE_MAG_FILTER, state.frontendCtx.LINEAR);

  jsUpdateQuadColors_fromBuffer(rgba, fw, fh);

  // Mirror to shareCanvas for WebRTC host streaming
  if (state.mpIsHost) {
    if (frameTmp.width !== fw || frameTmp.height !== fh) {
      frameTmp.width = fw; frameTmp.height = fh;
      frameTmpCtx = frameTmp.getContext('2d');
    }
    frameTmpCtx.putImageData(new ImageData(rgba, fw, fh), 0, 0);
    shareCtx.drawImage(frameTmp, 0, 0, 640, 480);
  }
}

// Handle messages from the active libretro core Worker.
function onWorkerMessage(e) {
  const msg = e.data;
  if (msg.type === 'ready') {
    startAudio(msg.sampleRate);
    setStatus(state.nowPlaying || 'Playing');
  } else if (msg.type === 'frame') {
    receiveFrame(msg.buf, msg.w, msg.h);
  } else if (msg.type === 'audio') {
    receiveAudio(msg.samples);
  }
}

// Terminate any running core Worker and spin up a new one for the given bundle.
export function spawnCoreWorker(bundle, file, ext) {
  if (state.coreWorker) { state.coreWorker.terminate(); state.coreWorker = null; }
  state.n64Running = false;  // stop N64 copyLoop from blitting over the new core's frames
  setStatus('Loading core...');
  const reader = new FileReader();
  reader.onload = function(ev) {
    state.coreWorker = new Worker('core_worker.js?v=' + Date.now());
    state.coreWorker.onmessage = onWorkerMessage;
    state.coreWorker.onerror = function(e) {
      setStatus('Worker error: ' + (e.message || e));
    };
    state.nowPlaying = file.name;
    const msg = {
      type:     'load',
      bundle:   bundle,
      romName:  file.name,
      romBytes: ev.target.result,  // transferred (zero-copy)
      version:  Date.now(),
    };
    if (bundle === 'core_ps1.js' && _ps1BiosBytes) {
      msg.biosName  = _ps1BiosName;
      msg.biosBytes = _ps1BiosBytes.buffer.slice(0);  // copy — keep original
    }
    if (bundle === 'core_saturn.js' && _saturnBiosBytes) {
      msg.biosBytes   = _saturnBiosBytes.buffer.slice(0);
      msg.biosType    = 'saturn';
    }

    state.coreWorker.postMessage(msg, [msg.romBytes]);
  };
  reader.readAsArrayBuffer(file);
}
