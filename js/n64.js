// N64 emulation — SEPARATE CODE PATH from the libretro/Worker architecture.
//
// All other cores (GBC, GBA, SNES, PS1) use frontend.cpp compiled with -DCORE_ONLY,
// run inside core_worker.js as a Web Worker, and communicate via postMessage.
//
// N64 uses nbarkhina/N64Wasm — a completely different emulator that predates this
// Worker architecture and does not implement the libretro interface. Key differences:
//   - Runs on the main thread (not a Worker) via an IIFE
//   - Uses SDL internally; fires its own requestAnimationFrame loop
//   - Writes directly to a WebGL context on <canvas id="n64canvas"> (off-screen)
//   - Requires assets.zip + config.txt written to its FS before callMain()
//   - Frames are copied: n64canvas → 2D blit canvas → main WebGL TV texture each frame
//     (Firefox refuses texImage2D from a WebGL2 canvas into a WebGL1 context, hence
//      the intermediate 2D canvas step)
//
// This is why loadN64() exists as a standalone function rather than routing through
// spawnCoreWorker() in worker-bridge.js.

import { state, shareCtx } from './state.js';
import { jsUpdateQuadColors } from './worker-bridge.js';
import { setStatus } from './utils.js';

// Wrap n64wasm.js in an IIFE before injecting.
// n64wasm.js (Emscripten 2.0.7) uses var/function declarations that conflict with
// let/class in the newer Emscripten output used by the renderer bundle.
// Fix: fetch as text, wrap in an IIFE, expose only Module via window._n64M.
function loadN64Wasm(callback) {
  window._n64ModuleInit = {
    canvas: document.getElementById('n64canvas'),
    noInitialRun: true,
    locateFile: function(path) { return path; },
  };
  fetch('n64wasm.js')
    .then(function(r) { return r.text(); })
    .then(function(code) {
      const wrapped = '(function(){\nvar Module=window._n64ModuleInit||{};\n'
                    + code
                    + '\nif(typeof FS!=="undefined")Module.FS=FS;\n'
                    + 'if(typeof callMain!=="undefined")Module.callMain=callMain;\n'
                    + 'window._n64M=Module;\n})();';
      const blob = new Blob([wrapped], { type: 'application/javascript' });
      const url  = URL.createObjectURL(blob);
      const s = document.createElement('script');
      s.src = url;
      s.onerror = function() { setStatus('Failed to execute n64wasm.js'); };
      document.body.appendChild(s);
      (function check() {
        if (window._n64M && window._n64M.calledRun) {
          URL.revokeObjectURL(url);
          callback(window._n64M);
        } else {
          setTimeout(check, 100);
        }
      })();
    })
    .catch(function() { setStatus('Failed to fetch n64wasm.js'); });
}

export function loadN64(file) {
  if (state.n64Running) {
    if (!confirm('Loading another N64 ROM requires a page reload. Continue?')) return;
    location.reload();
    return;
  }
  // Stop any running libretro worker
  if (state.coreWorker) { state.coreWorker.terminate(); state.coreWorker = null; }

  setStatus('Loading N64 emulator...');

  // Renderer is already loaded — set up game texture for N64 resolution
  const texId = state.rendererModule.ccall('get_game_tex_id', 'number', [], []);
  state.rendererModule.ccall('set_frame_size', 'void', ['number','number'], [640, 480]);

  loadN64Wasm(function(n64Mod) {
    state.n64Module = n64Mod;

    setStatus('Loading ' + file.name + '...');
    const reader = new FileReader();
    reader.onload = function(ev) {
      const romBytes = new Uint8Array(ev.target.result);

      // N64Wasm requires assets.zip, config.txt, and cheat.txt before callMain
      fetch('assets.zip')
        .then(function(r) { return r.arrayBuffer(); })
        .then(function(assetsBuf) {
          const fs = state.n64Module.FS;
          fs.writeFile('assets.zip', new Uint8Array(assetsBuf));

          const cfg = [
            '12','13','14','15','0','2','9','4','6','5','11','-1','-1','-1','-1',
            'b','n','y','h','Enter','i','k','j','l','a','q','e','s','d','`',
            'Up','Down','Left','Right',
            '0','0','0',   // save files
            '0','0','1',   // fps, swap sticks, disable audio sync
            '0','0','0',   // invert Y axis 2/3/4P
            '0','0','0','0',
          ].join('\r\n') + '\r\n';
          fs.writeFile('config.txt', cfg);
          fs.writeFile('cheat.txt', '');
          fs.writeFile('custom.v64', romBytes);

          try { new AudioContext().resume(); } catch(e) {}
          state.n64Module.callMain(['custom.v64']);
          state.n64Running = true;
          setStatus('Playing: ' + file.name);

          // Copy n64canvas → TV texture every frame via an intermediate 2D canvas.
          // Firefox refuses texImage2D(webgl2Canvas) into a WebGL1 context, so we
          // go through a 2D canvas first. frontendGL/frontendCtx were captured at
          // renderer load time, before n64wasm overwrote window.GL.
          const n64canvas   = document.getElementById('n64canvas');
          const blitCanvas  = document.createElement('canvas');
          blitCanvas.width  = n64canvas.width;
          blitCanvas.height = n64canvas.height;
          const blit2d = blitCanvas.getContext('2d');

          (function copyLoop() {
            if (state.n64Running) {
              const tex = state.frontendGL.textures[texId];
              if (tex) {
                blit2d.drawImage(n64canvas, 0, 0);
                jsUpdateQuadColors(blitCanvas);
                state.frontendCtx.bindTexture(state.frontendCtx.TEXTURE_2D, tex);
                state.frontendCtx.texImage2D(state.frontendCtx.TEXTURE_2D, 0,
                  state.frontendCtx.RGBA, state.frontendCtx.RGBA,
                  state.frontendCtx.UNSIGNED_BYTE, blitCanvas);
                state.frontendCtx.texParameteri(state.frontendCtx.TEXTURE_2D,
                  state.frontendCtx.TEXTURE_MIN_FILTER, state.frontendCtx.LINEAR);
                state.frontendCtx.texParameteri(state.frontendCtx.TEXTURE_2D,
                  state.frontendCtx.TEXTURE_MAG_FILTER, state.frontendCtx.LINEAR);
                // Mirror to shareCanvas for WebRTC streaming
                shareCtx.drawImage(blitCanvas, 0, 0, 640, 480);
              }
            }
            requestAnimationFrame(copyLoop);
          })();
        })
        .catch(function() { setStatus('Failed to load assets.zip'); });
    };
    reader.readAsArrayBuffer(file);
  });
}
