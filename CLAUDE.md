# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

Retro Cube is a browser-based retro gaming emulator that renders classic games on a virtual CRT TV inside a 3D living room environment. Players move around in first-person. Emulator cores (GBC, GBA, SNES, PS1, N64) are compiled to WebAssembly via Emscripten. Multiplayer shows other players as animated cat avatars via WebRTC (PeerJS).

## Build

**Prerequisites:** Emscripten SDK must be installed and activated.

```bash
source /path/to/emsdk/emsdk_env.sh   # activate Emscripten
./build.sh                            # build all cores (GBC, GBA, SNES, PS1 + download N64)
```

`build.sh` clones emulator core repos into `/cores/`, builds each to a static `.a` library, then links with `renderer.cpp` or `core.cpp` to produce per-system bundles: `game_renderer.js`, `core_gbc.js/.wasm`, `core_snes.js/.wasm`, `core_ps1.js/.wasm`, `core_gba.js/.wasm`. N64 (`n64wasm.js/.wasm`) is downloaded prebuilt — see N64 note below.

**Run locally:**
```bash
python3 -m http.server 8080
# open http://localhost:8080/index.html
```

There is no test suite.

## Architecture

The project has three layers:

### 1. C++ compiled to WebAssembly — two files

**`renderer.cpp`** — 3D room renderer (~900 lines + `renderer_shaders.h` for GLSL strings). Responsibilities:
- 3D scene rendering with OpenGL ES 3 / WebGL 2: parses glTF models (`cgltf`), manages a list of `TvPrim` draw calls, applies PBR-style lighting
- Lighting model: a warm ceiling lamp + 4 TV quadrant lights whose colors are sampled live from the game frame edges to simulate CRT glow
- CRT post-process: renders game frame to FBO with scanline/warp shader, composites onto TV screen geometry
- Skinned animation for the cat avatar (29-bone skeleton, keyframe sampling)
- First-person camera: `Player` struct with position + yaw/pitch; `RemotePlayer` structs for multiplayer avatars (up to 8)

**`core.cpp`** — libretro core driver (~150 lines). Responsibilities:
- Loads and drives libretro emulator cores (`retro_init`, `retro_load_game`, `retro_run`)
- Converts video frames to RGBA and exposes them via `get_frame_ptr()`
- Audio: 16384-frame stereo ring buffer exposed to JS

**~40 exported functions** (`EMSCRIPTEN_KEEPALIVE`) are the JS↔C++ interface:
- `start_game(path)` — load ROM
- `set_button(id, pressed)` — gamepad input (16 buttons)
- `set_move_key(key, pressed)` / `add_mouse_delta(dx, dy)` — player movement
- `get_frame_ptr/w/h()` — video output
- `get_audio_buf_ptr()` — audio ring buffer
- `set_lamp_pos/intensity()`, `set_tv_quad_colors()`, `set_room_xform()`, `set_overscan()` — environment tuning
- `set_remote_player()`, `remove_remote_player()` — multiplayer

### 2. Browser JS

The JS layer is split across several files with distinct responsibilities:

- **`js/app.js`** — Entry point: canvas sizing, renderer module loading, ROM dispatch, slider wiring, core status checks
- **`js/audio.js`** — AudioContext lifecycle, spatial PannerNode, ring buffer drain, per-frame listener position updates
- **`js/worker-bridge.js`** — Worker lifecycle (`spawnCoreWorker`, `terminateCoreWorker`), frame/audio receive from worker, BIOS/save file state
- **`js/input.js`** — Keyboard/mouse event listeners, WASD and gamepad button mappings
- **`js/multiplayer.js`** — PeerJS WebRTC: host/join, position sync, video stream receive
- **`js/n64.js`** — N64-specific loading path (see N64 note below)
- **`core_worker.js`** — Generic Web Worker shell that hosts any libretro core bundle; receives `load`/`button`/`terminate` messages, posts `ready`/`frame`/`audio` messages

#### Renderer / Core Split

The C++ layer uses two separate source files:
- **`renderer.cpp`** → **`game_renderer.js`**: loads on page open, owns the 3D scene, WebGL context, and player movement. No emulation logic.
- **`core.cpp`** → **`core_*.js`**: one bundle per system, loaded inside `core_worker.js` as a Web Worker when a ROM is dropped. Owns all libretro emulation. Sends frames + audio back to the main thread via transferable `ArrayBuffer`s.

#### N64 — Separate Code Path

**N64 does not use the libretro/Worker architecture.** It uses a completely different emulator: [nbarkhina/N64Wasm](https://github.com/nbarkhina/N64Wasm), which is downloaded prebuilt. Key differences from the other cores:

- Runs on the **main thread** (not a Worker) via an IIFE, with its own offscreen `<canvas id="n64canvas">`
- Uses SDL internally, fires its own `requestAnimationFrame` loop, and writes directly to a WebGL context
- Requires `assets.zip` and `config.txt` written to its virtual filesystem before `callMain()`
- Frames are copied from `n64canvas` → a 2D blit canvas → the main WebGL texture each frame

This is why `js/n64.js` exists as a standalone module rather than routing through `core_worker.js`.

### 3. `tv/` — 3D Assets
glTF models and PNG textures: `CRT_TV.gltf`, `crt_room_full.gltf`, `room.gltf`, `cat/scene.gltf`. Loaded by `cgltf` inside `renderer.cpp`; the `--preload-file tv` Emscripten flag bundles them into the `.data` file at build time.

## Key Build Flags (from `build.sh`)

- `-s FULL_ES3=1` — WebGL 2
- `-s EXPORTED_FUNCTIONS` — the ~40 C++ exports
- `-s INITIAL_MEMORY=67108864` (64 MB, PS1 uses 256 MB)
- `--preload-file tv` — bundles 3D assets into `.data` file
- `-O2 -std=c++17`

## Adding a New Emulator Core

Follow the pattern in `build.sh`: clone the libretro core repo into `cores/`, build it to a static `.a` with `emmake make`, then add an `em++` link step that compiles `core.cpp` with the new `.a` to produce a new `core_<system>.js/.wasm` pair (use `-s ENVIRONMENT=worker`). Then in `js/app.js`, add the new file extensions to `CORE_MAP` mapping them to the new bundle name. `core_worker.js` handles all libretro cores generically — no changes needed there.
