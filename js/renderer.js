// Typed wrapper over the Emscripten game_renderer.js module.
// Every other JS module should call these functions instead of using
// state.rendererModule.ccall() or state.rendererModule._fn() directly.

import { state } from './state.js';

// ── Internal helpers ─────────────────────────────────────────

function _ccall(name, ret, argTypes, args) {
  if (!state.rendererModule) return ret === 'number' ? 0 : undefined;
  return state.rendererModule.ccall(name, ret, argTypes, args);
}

function _direct(name) {
  const fn = state.rendererModule && state.rendererModule['_' + name];
  return fn ? fn() : 0;
}

function _hasExport(name) {
  return !!(state.rendererModule && state.rendererModule['_' + name]);
}

// ── Ready check ──────────────────────────────────────────────

export function isReady() { return !!state.rendererModule; }

// ── Scene / room ─────────────────────────────────────────────

export function setRoomXform(scale, rotY, tx, ty, tz) {
  _ccall('set_room_xform', null,
    ['number','number','number','number','number'], [scale, rotY, tx, ty, tz]);
}

export function setOverscan(x, y) {
  _ccall('set_overscan', null, ['number','number'], [x, y]);
}

// ── Lighting ─────────────────────────────────────────────────

export function setLampPos(x, y, z) {
  _ccall('set_lamp_pos', null, ['number','number','number'], [x, y, z]);
}

export function setLampIntensity(v) {
  _ccall('set_lamp_intensity', null, ['number'], [v]);
}

export function setTvLightIntensity(v) {
  _ccall('set_tv_light_intensity', null, ['number'], [v]);
}

export function setConeYaw(v) {
  _ccall('set_cone_yaw', null, ['number'], [v]);
}

export function setConePitch(v) {
  _ccall('set_cone_pitch', null, ['number'], [v]);
}

export function setConePower(v) {
  _ccall('set_cone_power', null, ['number'], [v]);
}

export function setTvQuadColors(
  r0,g0,b0, r1,g1,b1, r2,g2,b2, r3,g3,b3
) {
  _ccall('set_tv_quad_colors', null, Array(12).fill('number'),
    [r0,g0,b0, r1,g1,b1, r2,g2,b2, r3,g3,b3]);
}

// ── Player movement ──────────────────────────────────────────

export function addMouseDelta(dx, dy) {
  _ccall('add_mouse_delta', 'void', ['number','number'], [dx, dy]);
}

export function setMoveKey(key, pressed) {
  _ccall('set_move_key', 'void', ['number','number'], [key, pressed]);
}

export function setLocalY(v) {
  _ccall('set_local_y', null, ['number'], [v]);
}

export function setCatEyeHeight(v) {
  _ccall('set_cat_eye_height', null, ['number'], [v]);
}

// ── Player position queries ──────────────────────────────────
// These use direct function calls (_get_local_x) for maximum
// performance in per-frame audio listener updates.

export function getLocalX()      { return _direct('get_local_x'); }
export function getLocalY()      { return _direct('get_local_y'); }
export function getLocalZ()      { return _direct('get_local_z'); }
export function getLocalYaw()    { return _direct('get_local_yaw'); }
export function getLocalPitch()  { return _direct('get_local_pitch'); }
export function getLocalMoving() { return _direct('get_local_moving'); }

// ccall versions (used in multiplayer position sync where overhead is irrelevant)
export function getLocalXCcall()      { return _ccall('get_local_x',      'number', [], []); }
export function getLocalYCcall()      { return _ccall('get_local_y',      'number', [], []); }
export function getLocalZCcall()      { return _ccall('get_local_z',      'number', [], []); }
export function getLocalYawCcall()    { return _ccall('get_local_yaw',    'number', [], []); }
export function getLocalMovingCcall() { return _ccall('get_local_moving', 'number', [], []); }

// ── TV position (for seeding audio source) ───────────────────

export function getTvX() { return _direct('get_tv_x'); }
export function getTvY() { return _direct('get_tv_y'); }
export function getTvZ() { return _direct('get_tv_z'); }

// ── Texture / frame ──────────────────────────────────────────

export function getGameTexId() {
  return _ccall('get_game_tex_id', 'number', [], []);
}

export function setFrameSize(w, h) {
  _ccall('set_frame_size', 'void', ['number','number'], [w, h]);
}

// ── Debug cube ───────────────────────────────────────────────

export function setDebugCubePos(x, y, z) {
  if (_hasExport('set_debug_cube_pos'))
    _ccall('set_debug_cube_pos', null, ['number','number','number'], [x, y, z]);
}

export function setDebugCubeVisible(v) {
  if (_hasExport('set_debug_cube_visible'))
    _ccall('set_debug_cube_visible', null, ['number'], [v]);
}

// ── Remote players ───────────────────────────────────────────

export function setRemotePlayer(slot, x, y, z, yaw, moving) {
  _ccall('set_remote_player', 'void',
    ['number','number','number','number','number','number'],
    [slot, x, y, z, yaw, moving]);
}

export function setRemotePlayerModel(slot, model) {
  _ccall('set_remote_player_model', 'void', ['number','number'], [slot, model]);
}

export function removeRemotePlayer(slot) {
  _ccall('remove_remote_player', 'void', ['number'], [slot]);
}

// Nameplate texture upload: JS writes RGBA pixels into a shared C++ buffer,
// then tells the renderer to create/update the GL texture from it.
export function getNameUploadBuf() {
  return _ccall('get_name_upload_buf', 'number', [], []);
}

export function setRemotePlayerNameTex(slot, w, h) {
  _ccall('set_remote_player_name_tex', null, ['number','number','number'], [slot, w, h]);
}

// Write pixel data into the shared upload buffer
export function writeNameBuf(data) {
  if (!state.rendererModule) return;
  const ptr = getNameUploadBuf();
  state.rendererModule.HEAPU8.set(data, ptr);
}

// ── Character preview ────────────────────────────────────────

export function setPreviewMode(idx) {
  _ccall('set_preview_mode', null, ['number'], [idx]);
}

export function exitPreviewMode() {
  _ccall('exit_preview_mode', null, [], []);
}

// ── GL context access (for texture upload paths) ─────────────

export function getGLTextures() {
  return state.frontendGL ? state.frontendGL.textures : null;
}

export function getGLContext() {
  return state.frontendCtx;
}

// ── Canvas resize (fullscreen) ──────────────────────────────

export function resizeCanvas() {
  if (_hasExport('resize_canvas'))
    _ccall('resize_canvas', null, [], []);
}
