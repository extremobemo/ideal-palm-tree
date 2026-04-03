// Audio subsystem: AudioContext lifecycle, spatial PannerNode, ring buffer drain,
// and per-frame listener position updates driven by the renderer's player position.

import { state } from './state.js';

const AUDIO_RING  = 44100 * 4;  // 2 seconds stereo capacity at max sample rate
const audioRing   = new Float32Array(AUDIO_RING);
let audioRingWrite = 0;
let audioRingRead  = 0;

let audioCtx    = null;
let audioNode   = null;
let audioPanner = null;

// Feed Int16 stereo samples (from worker) into the JS ring buffer.
export function receiveAudio(buf) {
  const s = new Int16Array(buf);
  for (let i = 0; i < s.length; i++) {
    audioRing[audioRingWrite % AUDIO_RING] = s[i] / 32768.0;
    audioRingWrite++;
  }
}

// Start (or restart) the AudioContext for the given sample rate.
// Called once per core load when the worker sends its 'ready' message.
export function startAudio(sampleRate) {
  // Close previous context if sample rate changed (different core)
  if (audioCtx && audioCtx.sampleRate !== sampleRate) {
    audioCtx.close();
    audioCtx = null; audioNode = null; audioPanner = null;
  }
  if (!audioCtx) {
    try { audioCtx = new AudioContext({ sampleRate }); }
    catch(e) { audioCtx = new AudioContext(); }
  }
  if (audioCtx.state === 'suspended') audioCtx.resume();
  // Reset ring buffer so stale samples from the previous core don't play
  audioRingWrite = 0; audioRingRead = 0;

  if (audioNode) return; // ScriptProcessor already wired for this context

  // PannerNode: audio source fixed at the TV's world position
  audioPanner = audioCtx.createPanner();
  audioPanner.panningModel  = 'HRTF';
  audioPanner.distanceModel = 'inverse';
  audioPanner.refDistance   = 80;
  audioPanner.maxDistance   = 2000;
  audioPanner.rolloffFactor = 4;

  if (state.rendererModule) {
    const tvx = state.rendererModule._get_tv_x();
    const tvy = state.rendererModule._get_tv_y();
    const tvz = state.rendererModule._get_tv_z();
    if (audioPanner.positionX !== undefined) {
      audioPanner.positionX.value = tvx;
      audioPanner.positionY.value = tvy;
      audioPanner.positionZ.value = tvz;
    } else {
      audioPanner.setPosition(tvx, tvy, tvz);
    }
  }

  // 4096-sample ScriptProcessor, no inputs, 2 stereo output channels
  audioNode = audioCtx.createScriptProcessor(4096, 0, 2);
  audioNode.onaudioprocess = function(ev) {
    const L = ev.outputBuffer.getChannelData(0);
    const R = ev.outputBuffer.getChannelData(1);
    for (let i = 0; i < L.length; i++) {
      if (audioRingWrite - audioRingRead >= 2) {
        L[i] = audioRing[audioRingRead % AUDIO_RING]; audioRingRead++;
        R[i] = audioRing[audioRingRead % AUDIO_RING]; audioRingRead++;
      } else {
        L[i] = R[i] = 0;
      }
    }
  };

  // Chain: ScriptProcessor → PannerNode → speakers
  audioNode.connect(audioPanner);
  audioPanner.connect(audioCtx.destination);

  // Per-frame: update Web Audio listener to match the player's position and look direction
  (function updateListener() {
    if (audioCtx && state.rendererModule) {
      const lx  = state.rendererModule._get_local_x();
      const ly  = state.rendererModule._get_local_y();
      const lz  = state.rendererModule._get_local_z();
      const yaw = state.rendererModule._get_local_yaw();
      const pit = state.rendererModule._get_local_pitch();
      const cosPit = Math.cos(pit);
      const fx = cosPit * Math.sin(yaw);
      const fy = Math.sin(pit);
      const fz = cosPit * Math.cos(yaw);
      const listener = audioCtx.listener;
      if (listener.positionX !== undefined) {
        listener.positionX.value = lx;
        listener.positionY.value = ly;
        listener.positionZ.value = lz;
        listener.forwardX.value  = fx;
        listener.forwardY.value  = fy;
        listener.forwardZ.value  = fz;
        listener.upX.value = 0; listener.upY.value = 1; listener.upZ.value = 0;
      } else {
        listener.setPosition(lx, ly, lz);
        listener.setOrientation(fx, fy, fz, 0, 1, 0);
      }
    }
    requestAnimationFrame(updateListener);
  })();
}
