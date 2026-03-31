# Plan: Add PS1 Support Alongside GBC

## Context
The project currently builds a single WASM binary with gambatte (GBC) statically linked. Adding PS1 requires a second WASM build with a PS1 libretro core. Since `dlopen` is unreliable in WASM, cores must be linked at compile time — so we need two separate `game.js` outputs and a way to switch between them at runtime in the browser. The user is willing to supply their own PS1 BIOS file.

## Recommended Core: PCSX-ReARMed
- Beetle PSX has documented Emscripten failures (GitHub issues #824, #874: needs 517MB+, WebGL API mismatches)
- PCSX-ReARMed is the community-recommended PS1 core for WASM environments
- ARM dynarec won't run in WASM, but PCSX-ReARMed falls back to an interpreter automatically
- Builds with `emmake make platform=emscripten` (same pattern as gambatte)

## Architecture: Two WASM Builds

```
game_gbc.js   ← gambatte core (current output, renamed)
game_ps1.js   ← pcsx_rearmed core (new)
```

The HTML page loads the correct JS bundle based on which ROM the user picks (detected by file extension).

## Changes Required

### 1. `build.sh` — add PS1 build target
- Clone PCSX-ReARMed: `git clone --depth 1 https://github.com/libretro/pcsx_rearmed cores/pcsx_rearmed`
- Build: `emmake make -C cores/pcsx_rearmed platform=emscripten DYNAREC=none`
- The `.bc`/`.a` output is named `pcsx_rearmed_libretro_emscripten.bc` → copy to `pcsx_rearmed_libretro.a`
- Add second `em++` link step outputting `game_ps1.js` with:
  - `INITIAL_MEMORY=268435456` (256MB — PS1 needs more headroom than GBC's 128MB)
  - Same exported functions as GBC build
  - `--preload-file tv/` same as GBC

- Rename current output from `game.js` → `game_gbc.js`

### 2. `frontend.cpp` — change system directory path
Only one line change needed:

```cpp
// Before:
case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char**)data="/"; return true;

// After:
case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *(const char**)data="/system/"; return true;
case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char**)data="/saves/"; return true;
```

This same `frontend.cpp` is compiled into both WASM builds — no per-core branching needed.

### 3. `index.html` — core switching + BIOS upload UI

**Core switching logic:**
```javascript
// Extension → bundle map
const CORE_MAP = {
  gb: 'game_gbc.js', gbc: 'game_gbc.js',
  bin: 'game_ps1.js', cue: 'game_ps1.js', chd: 'game_ps1.js',
};

let loadedCore = null;

document.getElementById('rom-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  const ext = file.name.split('.').pop().toLowerCase();
  const bundle = CORE_MAP[ext];
  if (!bundle) { status.textContent = 'Unknown format'; return; }

  if (loadedCore !== bundle) {
    // Dynamically swap the script tag
    const old = document.getElementById('core-script');
    if (old) old.remove();
    const s = document.createElement('script');
    s.id = 'core-script'; s.src = bundle;
    s.onload = () => { loadedCore = bundle; loadRom(file); };
    document.body.appendChild(s);
  } else {
    loadRom(file);
  }
});

function loadRom(file) {
  FS.writeFile('/rom.bin', new Uint8Array(/* file bytes */));
  Module.ccall('start_game', 'void', ['string'], ['/rom.bin']);
}
```

**BIOS upload UI** (add below the Load ROM button):
```html
<label>Load PS1 BIOS
  <input type="file" id="bios-input" accept=".bin" hidden>
</label>
```

```javascript
document.getElementById('bios-input').addEventListener('change', function(e) {
  const file = e.target.files[0];
  const reader = new FileReader();
  reader.onload = ev => {
    // Create /system/ dir if needed (Emscripten FS)
    try { FS.mkdir('/system'); } catch(e) {}
    // PCSX-ReARMed looks for scph1001.bin (US BIOS) or scph5500.bin (JP)
    FS.writeFile('/system/' + file.name, new Uint8Array(ev.target.result));
    document.getElementById('status').textContent = 'BIOS loaded: ' + file.name;
  };
  reader.readAsArrayBuffer(file);
});
```

**Important**: BIOS must be loaded before `start_game()` is called. The UI should show a warning if the user tries to load a PS1 ROM without a BIOS.

**Disc format notes:**
- `.chd` — single-file format, most convenient; supported by PCSX-ReARMed
- `.bin/.cue` — user must upload the `.cue` file (point start_game at the `.cue`); the `.bin` must also be written to FS at the same relative path
- Recommend `.chd` in the UI hint text

### 4. File accept list update on ROM input
```html
<input type="file" id="rom-input" accept=".gb,.gbc,.bin,.cue,.chd" hidden>
```

## Critical Files to Modify
- `build.sh` — add PS1 core build + second link step, rename GBC output
- `frontend.cpp` — fix system/save directory paths (line ~168)
- `index.html` — dynamic script loading, BIOS upload UI, extension-based core selection

## Build Order
1. `build.sh` runs GBC build first (existing logic, output renamed to `game_gbc.js`)
2. Then clones/builds PCSX-ReARMed, links to `game_ps1.js`
3. Both builds use the same `frontend.cpp` and same `tv/` preload

## Verification
1. `bash build.sh` — both `game_gbc.js` and `game_ps1.js` must be produced
2. `python3 -m http.server 8080` → open browser
3. Load a `.gb` or `.gbc` ROM → GBC core loads, game plays on TV
4. Upload a valid PS1 BIOS `.bin` file → status shows "BIOS loaded"
5. Load a `.chd` PS1 disc image → PS1 core loads, game plays on TV
6. Switching between a GBC and PS1 ROM should hot-swap the WASM bundle
