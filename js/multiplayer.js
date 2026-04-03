// WebRTC multiplayer via PeerJS.
// Host streams the game canvas as video; guests send position data via data channel.
// PeerJS is loaded from CDN as a non-module script, so it lives on window.Peer.

import { state, shareCanvas, shareCtx } from './state.js';
import { jsUpdateQuadColors } from './worker-bridge.js';
import { setStatus } from './utils.js';
import { getGameAudioTrack, startViewerAudio, setAudioSourcePos, setAudioPannerSettings } from './audio.js';

let peerInst  = null;
let mpConns   = [];
let gameStream = null;
let hostConn  = null;   // guest-side: connection back to the host
let remoteNames = new Array(8).fill('');  // last-seen name per remote slot

// Generate a nameplate RGBA texture and upload it to C++ for the given remote slot
function uploadNameplate(slotId, name) {
  if (!state.rendererModule || !name) return;
  const W = 256, H = 64;
  const c = document.createElement('canvas');
  c.width = W; c.height = H;
  const ctx = c.getContext('2d');
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(0,0,0,0.65)';
  ctx.fillRect(6, 6, W - 12, H - 12);
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold 28px sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(name, W / 2, H / 2, W - 24);
  const imgData = ctx.getImageData(0, 0, W, H);
  const M = state.rendererModule;
  const ptr = M.ccall('get_name_upload_buf', 'number', [], []);
  M.HEAPU8.set(imgData.data, ptr);
  M.ccall('set_remote_player_name_tex', null, ['number','number','number'], [slotId, W, H]);
}

// Send a button event to the host (called by input.js when this guest has control)
export function mpSendButton(id, pressed) {
  if (hostConn && hostConn.open)
    hostConn.send({ type: 'btn', id, pressed });
}

// ── Scene state ──────────────────────────────────────────────
function getSceneState() {
  const v = id => document.getElementById(id);
  return {
    type:         'scene',
    overscanX:    parseFloat(v('overscan-x').value),
    overscanY:    parseFloat(v('overscan-y').value),
    roomScale:    parseFloat(v('room-scale').value),
    roomRotY:     parseFloat(v('room-roty').value) || 0,
    roomTx:       parseFloat(v('room-tx').value)   || 0,
    roomTy:       parseFloat(v('room-ty').value)   || 0,
    roomTz:       parseFloat(v('room-tz').value)   || 0,
    lampX:        parseFloat(v('lamp-x').value)    || 0,
    lampY:        parseFloat(v('lamp-y').value)    || 0,
    lampZ:        parseFloat(v('lamp-z').value)    || 0,
    lampIntensity: parseFloat(v('lamp-intensity').value),
    tvIntensity:  parseFloat(v('tv-intensity').value),
    coneYaw:      parseFloat(v('cone-yaw').value),
    conePitch:    parseFloat(v('cone-pitch').value),
    conePower:    parseFloat(v('cone-power').value),
    audioX:       parseFloat(v('audio-src-x').value) || 0,
    audioY:       parseFloat(v('audio-src-y').value) || 0,
    audioZ:       parseFloat(v('audio-src-z').value) || 0,
    audioRefDist:    parseFloat(v('audio-ref-dist').value),
    audioMaxDist:    parseFloat(v('audio-max-dist').value),
    audioRolloff:    parseFloat(v('audio-rolloff').value),
    audioModel:      v('audio-model').value,
    audioCubeVisible: document.getElementById('audio-debug-cube').checked ? 1 : 0,
  };
}

function applySceneState(s) {
  const set = (id, val) => { const el = document.getElementById(id); if (el) el.value = val; };
  const txt = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
  set('overscan-x', s.overscanX);        set('overscan-y', s.overscanY);
  set('room-scale', s.roomScale);        txt('room-scale-val', s.roomScale);
  set('room-roty', s.roomRotY);
  set('room-tx', s.roomTx);             set('room-ty', s.roomTy);          set('room-tz', s.roomTz);
  set('lamp-x', s.lampX);               set('lamp-y', s.lampY);            set('lamp-z', s.lampZ);
  set('lamp-intensity', s.lampIntensity); txt('lamp-intensity-val', s.lampIntensity);
  set('tv-intensity', s.tvIntensity);   txt('tv-intensity-val', s.tvIntensity);
  set('cone-yaw', s.coneYaw);           set('cone-pitch', s.conePitch);    set('cone-power', s.conePower);
  set('audio-src-x', s.audioX);         set('audio-src-y', s.audioY);      set('audio-src-z', s.audioZ);
  set('audio-ref-dist', s.audioRefDist); txt('audio-ref-dist-val', s.audioRefDist);
  set('audio-max-dist', s.audioMaxDist); txt('audio-max-dist-val', s.audioMaxDist);
  set('audio-rolloff', s.audioRolloff);  txt('audio-rolloff-val', s.audioRolloff);
  const mdl = document.getElementById('audio-model'); if (mdl) mdl.value = s.audioModel;

  if (state.rendererModule) {
    const M = state.rendererModule;
    M.ccall('set_overscan',          null, ['number','number'],                         [s.overscanX, s.overscanY]);
    M.ccall('set_room_xform',        null, ['number','number','number','number','number'],[s.roomScale, s.roomRotY, s.roomTx, s.roomTy, s.roomTz]);
    M.ccall('set_lamp_pos',          null, ['number','number','number'],                [s.lampX, s.lampY, s.lampZ]);
    M.ccall('set_lamp_intensity',    null, ['number'],                                  [s.lampIntensity]);
    M.ccall('set_tv_light_intensity',null, ['number'],                                  [s.tvIntensity]);
    M.ccall('set_cone_yaw',          null, ['number'],                                  [s.coneYaw]);
    M.ccall('set_cone_pitch',        null, ['number'],                                  [s.conePitch]);
    M.ccall('set_cone_power',        null, ['number'],                                  [s.conePower]);
  }
  setAudioSourcePos(s.audioX, s.audioY, s.audioZ);
  setAudioPannerSettings(s.audioRefDist, s.audioMaxDist, s.audioRolloff, s.audioModel);
  const cubeEl = document.getElementById('audio-debug-cube');
  if (cubeEl) cubeEl.checked = !!s.audioCubeVisible;
  if (state.rendererModule && state.rendererModule._set_debug_cube_visible)
    state.rendererModule.ccall('set_debug_cube_visible', null, ['number'], [s.audioCubeVisible ? 1 : 0]);
}

export function broadcastScene() {
  if (!state.mpIsHost) return;
  const s = getSceneState();
  mpConns.forEach(function(c) { if (c.open) c.send(s); });
}

function mpSetStatus(msg) {
  document.getElementById('mp-status').textContent = msg;
}

function ctrlSend(connIdx, isController) {
  const c = mpConns[connIdx];
  if (c && c.open) c.send({ type: 'ctrl', isController });
}

function ctrlSetHost() {
  if (state.controller !== 'host') {
    ctrlSend(state.controller, false);
  }
  state.controller = 'host';
  const sel = document.getElementById('controller-select');
  if (sel) sel.value = 'host';
}

document.getElementById('controller-select').addEventListener('change', function() {
  const prev = state.controller;
  const next = this.value === 'host' ? 'host' : parseInt(this.value);
  if (prev !== 'host') ctrlSend(prev, false);
  state.controller = next;
  if (next !== 'host') ctrlSend(next, true);
});

// Broadcast local player position + model to all connected peers at 60 Hz
function startPositionSync(conn) {
  setInterval(function() {
    if (!conn.open || !state.rendererModule) return;
    conn.send({
      type:   'pos',
      x:      state.rendererModule.ccall('get_local_x',      'number', [], []),
      y:      state.rendererModule.ccall('get_local_y',      'number', [], []),
      z:      state.rendererModule.ccall('get_local_z',      'number', [], []),
      yaw:    state.rendererModule.ccall('get_local_yaw',    'number', [], []),
      moving: state.rendererModule.ccall('get_local_moving', 'number', [], []),
      model:  state.localModel,
      name:   state.localName,
    });
  }, 16);
}

// Start capturing game frames into shareCanvas for WebRTC streaming.
// The actual blit happens inside receiveFrame() (libretro) or the N64 copyLoop.
function startFrameCapture() {
  gameStream = shareCanvas.captureStream(60);
}

// Rewrite SDP to prefer H.264 Baseline (profile-level-id 42e*/4200*).
// Baseline has no B-frames so every frame decodes immediately — eliminates
// the 2-3 frame lookahead latency introduced by High profile or VP8 B-frames.
function preferH264Baseline(sdp) {
  const lines = sdp.split('\r\n');
  let basePt = null;
  for (const line of lines) {
    const m = line.match(/^a=fmtp:(\d+) .*profile-level-id=42[e8][01f]/i);
    if (m) { basePt = m[1]; break; }
  }
  if (!basePt) return sdp;
  return lines.map(function(line) {
    if (!line.startsWith('m=video')) return line;
    const parts = line.split(' ');
    const pts   = parts.slice(3);
    return parts.slice(0, 3).concat([basePt], pts.filter(p => p !== basePt)).join(' ');
  }).join('\r\n');
}

// Ask the browser to encode at high priority, 60 fps max, 2.5 Mbps.
// Called after the peer connection is established so getSenders() is populated.
function applyLowLatencyEncoding(pc) {
  pc.getSenders().forEach(function(sender) {
    if (!sender.track || sender.track.kind !== 'video') return;
    const p = sender.getParameters();
    if (!p.encodings || !p.encodings.length) p.encodings = [{}];
    p.encodings[0].maxBitrate      = 2_500_000;
    p.encodings[0].maxFramerate    = 60;
    p.encodings[0].priority        = 'high';
    p.encodings[0].networkPriority = 'high';
    sender.setParameters(p).catch(e => console.warn('setParameters:', e));
  });
}

// Blit a received video stream into the TV texture (guest-side WebRTC receive)
function setupVideoReceive(stream) {
  const videoEl = document.createElement('video');
  videoEl.autoplay = true; videoEl.playsInline = true; videoEl.muted = true;
  videoEl.srcObject = stream;
  videoEl.play().catch(e => console.warn('mp video:', e));

  const texId = state.rendererModule.ccall('get_game_tex_id', 'number', [], []);
  state.rendererModule.ccall('set_frame_size', 'void', ['number','number'], [640, 480]);

  (function blitLoop() {
    if (videoEl.readyState >= 2 && state.frontendGL && state.frontendCtx) {
      const tex = state.frontendGL.textures[texId];
      if (tex) {
        state.frontendCtx.bindTexture(state.frontendCtx.TEXTURE_2D, tex);
        state.frontendCtx.texImage2D(state.frontendCtx.TEXTURE_2D, 0,
          state.frontendCtx.RGBA, state.frontendCtx.RGBA,
          state.frontendCtx.UNSIGNED_BYTE, videoEl);
        state.frontendCtx.texParameteri(state.frontendCtx.TEXTURE_2D,
          state.frontendCtx.TEXTURE_MIN_FILTER, state.frontendCtx.LINEAR);
        jsUpdateQuadColors(videoEl);
      }
    }
    requestAnimationFrame(blitLoop);
  })();
}

export function mpHost() {
  state.mpIsHost = true;
  state.controller = 'host';
  startFrameCapture();
  document.getElementById('controller-wrap').style.display = '';
  peerInst = new window.Peer();
  peerInst.on('error', e => mpSetStatus('Peer error: ' + e.type));
  peerInst.on('open', function(id) {
    mpSetStatus('Your ID: ' + id + '  (share with guest)');
  });
  peerInst.on('connection', function(conn) {
    mpConns.push(conn);
    const remoteId = mpConns.length - 1;
    conn.on('open', function() {
      conn.send(getSceneState());
      mpSetStatus('Guest connected');
      startPositionSync(conn);
      // Add this guest to the controller select
      const sel = document.getElementById('controller-select');
      if (sel) {
        const opt = document.createElement('option');
        opt.value = remoteId;
        opt.textContent = 'Guest ' + (remoteId + 1);
        sel.appendChild(opt);
      }
    });
    conn.on('data', function(data) {
      if (data.type === 'pos' && state.rendererModule) {
        state.rendererModule.ccall('set_remote_player', 'void',
          ['number','number','number','number','number','number'],
          [remoteId, data.x, data.y, data.z, data.yaw, data.moving || 0]);
        state.rendererModule.ccall('set_remote_player_model', 'void',
          ['number','number'], [remoteId, data.model || 0]);
        if (data.name !== remoteNames[remoteId]) {
          remoteNames[remoteId] = data.name || '';
          uploadNameplate(remoteId, remoteNames[remoteId]);
        }
      } else if (data.type === 'scene') {
        applySceneState(data);
      } else if (data.type === 'btn' && state.controller === remoteId && state.coreWorker) {
        state.coreWorker.postMessage({ type: 'button', id: data.id, pressed: data.pressed });
      }
    });
    conn.on('close', function() {
      // If this guest had control, reset to host
      if (state.controller === remoteId) ctrlSetHost();
      remoteNames[remoteId] = '';
      if (state.rendererModule)
        state.rendererModule.ccall('remove_remote_player', 'void', ['number'], [remoteId]);
      // Remove their option from the controller select
      const sel = document.getElementById('controller-select');
      if (sel) {
        const opt = sel.querySelector('option[value="' + remoteId + '"]');
        if (opt) opt.remove();
      }
      mpConns.splice(mpConns.indexOf(conn), 1);
      mpSetStatus('Guest disconnected');
    });
  });
  peerInst.on('call', function(call) {
    const tracks = gameStream ? [...gameStream.getTracks()] : [];
    const audioTrack = getGameAudioTrack();
    if (audioTrack) tracks.push(audioTrack);
    call.answer(new MediaStream(tracks), { sdpTransform: preferH264Baseline });
    call.on('stream', function() {
      applyLowLatencyEncoding(call.peerConnection);
    });
  });
}

export function mpJoin() {
  const hostId = document.getElementById('join-id').value.trim();
  if (!hostId) { mpSetStatus('Enter a peer ID first'); return; }
  state.mpIsHost = false;
  mpSetStatus('Connecting...');
  peerInst = new window.Peer();
  peerInst.on('error', e => mpSetStatus('Peer error: ' + e.type));
  peerInst.on('open', function() {
    // Data channel for position sync
    hostConn = peerInst.connect(hostId, { reliable: false });
    hostConn.on('open', function() {
      state.mpConnected = true;
      document.querySelector('.scene-section').style.display = 'none';
      mpSetStatus('In room with ' + hostId);
      startPositionSync(hostConn);
    });
    hostConn.on('data', function(data) {
      if (data.type === 'pos' && state.rendererModule) {
        state.rendererModule.ccall('set_remote_player', 'void',
          ['number','number','number','number','number','number'],
          [0, data.x, data.y, data.z, data.yaw, data.moving || 0]);
        state.rendererModule.ccall('set_remote_player_model', 'void',
          ['number','number'], [0, data.model || 0]);
        if (data.name !== remoteNames[0]) {
          remoteNames[0] = data.name || '';
          uploadNameplate(0, remoteNames[0]);
        }
      } else if (data.type === 'scene') {
        applySceneState(data);
      } else if (data.type === 'ctrl') {
        state.isController = !!data.isController;
      }
    });
    hostConn.on('close', function() {
      state.mpConnected = false;
      state.isController = false;
      remoteNames[0] = '';
      document.querySelector('.scene-section').style.display = '';
      if (state.rendererModule)
        state.rendererModule.ccall('remove_remote_player', 'void', ['number'], [0]);
      mpSetStatus('Host disconnected');
    });

    // Caller must include tracks for every media type it wants to receive back.
    // A video-only offer means the host can't send audio, so add a silent audio
    // track too so the SDP gets an m=audio section.
    const dummyCanvas = document.createElement('canvas');
    dummyCanvas.width = 2; dummyCanvas.height = 2;
    const callTracks = dummyCanvas.captureStream ? [...dummyCanvas.captureStream(1).getTracks()] : [];
    try {
      const silentCtx = new AudioContext();
      const silentDest = silentCtx.createMediaStreamDestination();
      const silentTrack = silentDest.stream.getAudioTracks()[0];
      if (silentTrack) callTracks.push(silentTrack);
    } catch(e) {}
    const callStream = new MediaStream(callTracks);
    const call = peerInst.call(hostId, callStream, { sdpTransform: preferH264Baseline });
    call.on('stream', function(stream) {
      const vTracks = stream.getVideoTracks();
      if (!vTracks.length) { mpSetStatus('No video track received'); return; }
      mpSetStatus('Receiving game from ' + hostId);
      applyLowLatencyEncoding(call.peerConnection);
      const aTracks = stream.getAudioTracks();
      if (aTracks.length) startViewerAudio(aTracks[0]);
      setupVideoReceive(new MediaStream(vTracks));
    });
    call.on('error', e => mpSetStatus('Call error: ' + e));
  });
}
