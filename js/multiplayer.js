// WebRTC multiplayer via PeerJS.
// Host streams the game canvas as video; guests send position data via data channel.
// PeerJS is loaded from CDN as a non-module script, so it lives on window.Peer.

import { state, shareCanvas, shareCtx } from './state.js';
import { jsUpdateQuadColors } from './worker-bridge.js';
import { setStatus } from './utils.js';

let peerInst = null;
let mpConns  = [];
let gameStream = null;

function mpSetStatus(msg) {
  document.getElementById('mp-status').textContent = msg;
}

// Broadcast local player position + model to all connected peers at 20 Hz
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
    });
  }, 50);
}

// Start capturing game frames into shareCanvas for WebRTC streaming.
// The actual blit happens inside receiveFrame() (libretro) or the N64 copyLoop.
function startFrameCapture() {
  gameStream = shareCanvas.captureStream(30);
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
  startFrameCapture();
  peerInst = new window.Peer();
  peerInst.on('error', e => mpSetStatus('Peer error: ' + e.type));
  peerInst.on('open', function(id) {
    mpSetStatus('Your ID: ' + id + '  (share with guest)');
  });
  peerInst.on('connection', function(conn) {
    mpConns.push(conn);
    const remoteId = mpConns.length - 1;
    conn.on('open', function() {
      mpSetStatus('Guest connected');
      startPositionSync(conn);
    });
    conn.on('data', function(data) {
      if (data.type === 'pos' && state.rendererModule) {
        state.rendererModule.ccall('set_remote_player', 'void',
          ['number','number','number','number','number','number'],
          [remoteId, data.x, data.y, data.z, data.yaw, data.moving || 0]);
        state.rendererModule.ccall('set_remote_player_model', 'void',
          ['number','number'], [remoteId, data.model || 0]);
      }
    });
    conn.on('close', function() {
      if (state.rendererModule)
        state.rendererModule.ccall('remove_remote_player', 'void', ['number'], [remoteId]);
      mpConns.splice(mpConns.indexOf(conn), 1);
      mpSetStatus('Guest disconnected');
    });
  });
  peerInst.on('call', function(call) {
    call.answer(gameStream || new MediaStream());
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
    const conn = peerInst.connect(hostId, { reliable: false });
    conn.on('open', function() {
      mpSetStatus('In room with ' + hostId);
      startPositionSync(conn);
    });
    conn.on('data', function(data) {
      if (data.type === 'pos' && state.rendererModule) {
        state.rendererModule.ccall('set_remote_player', 'void',
          ['number','number','number','number','number','number'],
          [0, data.x, data.y, data.z, data.yaw, data.moving || 0]);
        state.rendererModule.ccall('set_remote_player_model', 'void',
          ['number','number'], [0, data.model || 0]);
      }
    });
    conn.on('close', function() {
      if (state.rendererModule)
        state.rendererModule.ccall('remove_remote_player', 'void', ['number'], [0]);
      mpSetStatus('Host disconnected');
    });

    // Video — caller must include a video track in the offer so the SDP has a
    // video section; otherwise the answerer (host) can't send their stream back.
    const dummyCanvas = document.createElement('canvas');
    dummyCanvas.width = 2; dummyCanvas.height = 2;
    const callStream = dummyCanvas.captureStream
      ? dummyCanvas.captureStream(1)
      : new MediaStream();
    const call = peerInst.call(hostId, callStream);
    call.on('stream', function(stream) {
      const vTracks = stream.getVideoTracks();
      if (!vTracks.length) { mpSetStatus('No video track received'); return; }
      mpSetStatus('Receiving game from ' + hostId);
      setupVideoReceive(new MediaStream(vTracks));
    });
    call.on('error', e => mpSetStatus('Call error: ' + e));
  });
}
