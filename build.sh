#!/bin/bash
# Full build script for retro-cube
# Prerequisites: Emscripten SDK activated (source emsdk_env.sh)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Build products go here — keeps root clean
BUILD_OUT="$SCRIPT_DIR/build"
mkdir -p "$BUILD_OUT"

RENDERER_EXPORTS='["_main","_set_move_key","_add_mouse_delta","_get_game_tex_id","_set_frame_size","_upload_frame","_get_local_x","_get_local_y","_get_local_z","_get_local_yaw","_get_local_pitch","_set_remote_player","_remove_remote_player","_get_tv_x","_get_tv_y","_get_tv_z","_set_overscan","_set_room_xform","_set_lamp_pos","_set_lamp_intensity","_set_tv_light_intensity","_set_tv_quad_colors","_set_cone_yaw","_set_cone_pitch","_set_cone_power","_get_local_moving","_set_cat_eye_height","_set_local_y","_set_remote_player_model","_set_debug_cube_pos","_set_debug_cube_visible","_get_name_upload_buf","_set_remote_player_name_tex","_set_preview_mode","_exit_preview_mode","_set_preview_transform","_resize_canvas"]'

CORE_EXPORTS='["_main","_start_game","_set_button","_set_analog","_step_frame","_get_frame_ptr","_get_frame_w","_get_frame_h","_get_audio_buf_ptr","_get_audio_write_pos","_get_audio_buf_size","_get_audio_sample_rate"]'

# ── build_core: clone repo, run emmake, copy the resulting archive ────────────
# Usage: build_core <archive> <repo_url> <clone_dir> <build_subdir> <make_flags>
#   archive:     destination .a path relative to SCRIPT_DIR
#   repo_url:    git repository to clone
#   clone_dir:   directory to clone into (relative to SCRIPT_DIR)
#   build_subdir: directory to run emmake make in (relative to SCRIPT_DIR)
#   make_flags:  extra flags forwarded to emmake make
build_core() {
    local archive="$1" repo="$2" clone_dir="$3" build_dir="$4" make_flags="$5"
    if [ -f "$SCRIPT_DIR/$archive" ]; then
        echo "$(basename "$archive"): cached"
        return 0
    fi
    mkdir -p cores
    [ ! -d "$SCRIPT_DIR/$clone_dir" ] && \
        git clone --depth 1 "$repo" "$SCRIPT_DIR/$clone_dir"
    cd "$SCRIPT_DIR/$build_dir"
    # shellcheck disable=SC2086  # intentional word-splitting of make_flags
    emmake make $make_flags CC=emcc CXX=em++ AR=emar RANLIB=emranlib
    # Copy the first matching output archive (emscripten may produce .bc or .a)
    local stem; stem=$(basename "$archive" .a)
    for f in "${stem}_emscripten.bc" "${stem}_emscripten.a" "${stem}.bc" "${stem}.a"; do
        [ -f "$f" ] && { cp "$f" "$SCRIPT_DIR/$archive"; break; }
    done
    cd "$SCRIPT_DIR"
    [ ! -f "$SCRIPT_DIR/$archive" ] && { echo "$(basename "$archive") build failed"; exit 1; }
    echo "$(basename "$archive"): built"
}

# ── 1. Get libretro.h ─────────────────────────────────────────────────────────
if [ ! -f include/libretro.h ]; then
  mkdir -p include
  curl -Lo include/libretro.h \
    https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h
fi

# ── 2. Build gambatte core ────────────────────────────────────────────────────
GAMBATTE_A=cores/gambatte/gambatte_libretro.a
build_core "$GAMBATTE_A" \
    "https://github.com/libretro/gambatte-libretro" \
    "cores/gambatte" "cores/gambatte" \
    "platform=emscripten"

# ── 3. Build libretro-common ──────────────────────────────────────────────────
LIBRETRO_COMMON=cores/gambatte/libgambatte/libretro-common
COMMON_A=cores/gambatte/libretro-common.a

if [ ! -f "$COMMON_A" ]; then
  echo "Building libretro-common..."
  CINCLUDES="-I include -I $LIBRETRO_COMMON/include"
  COBJS=""
  for src in \
      "$LIBRETRO_COMMON/compat/compat_strl.c" \
      "$LIBRETRO_COMMON/compat/compat_snprintf.c" \
      "$LIBRETRO_COMMON/compat/compat_posix_string.c" \
      "$LIBRETRO_COMMON/compat/compat_strcasestr.c" \
      "$LIBRETRO_COMMON/encodings/encoding_utf.c" \
      "$LIBRETRO_COMMON/file/file_path.c" \
      "$LIBRETRO_COMMON/file/file_path_io.c" \
      "$LIBRETRO_COMMON/streams/file_stream.c" \
      "$LIBRETRO_COMMON/streams/file_stream_transforms.c" \
      "$LIBRETRO_COMMON/string/stdstring.c" \
      "$LIBRETRO_COMMON/vfs/vfs_implementation.c"; do
    obj="${src%.c}.o"
    emcc -O2 $CINCLUDES -c "$src" -o "$obj"
    COBJS="$COBJS $obj"
  done
  emar rcs "$COMMON_A" $COBJS
fi

# ── 4. Compile + link renderer bundle (main thread, loads once on page open) ──
# INITIAL_MEMORY=64 MB: 3D scene geometry, textures, and avatar models
RENDERER_SRCS="renderer.cpp renderer_math.cpp renderer_gl_utils.cpp renderer_scene.cpp renderer_crt.cpp renderer_anim.cpp renderer_render.cpp"
echo "Compiling renderer sources..."
RENDERER_OBJS=""
for src in $RENDERER_SRCS; do
  obj="src/${src%.cpp}.o"
  em++ -O2 -std=c++17 -I include -c "src/$src" -o "$obj"
  RENDERER_OBJS="$RENDERER_OBJS $obj"
done
echo "Linking game_renderer.js..."
em++ -O2 -std=c++17 \
  $RENDERER_OBJS \
  -s FULL_ES3=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8"]' \
  -s EXPORTED_FUNCTIONS="$RENDERER_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=67108864 \
  --preload-file tv/ \
  --extern-pre-js src/renderer_pre.js \
  -o "$BUILD_OUT/game_renderer.js"
rm -f $RENDERER_OBJS src/*.o

# ── 5. Link GBC core bundle (Web Worker) ─────────────────────────────────────
# INITIAL_MEMORY=128 MB: Gambatte needs extra headroom for VRAM + save-state buffers
echo "Linking core_gbc.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  src/core.cpp \
  "$COMMON_A" \
  "$GAMBATTE_A" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8","HEAP16"]' \
  -s EXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  -s ENVIRONMENT=worker \
  -o "$BUILD_OUT/core_gbc.js"

# ── 6. Build snes9x core ──────────────────────────────────────────────────────
SNES9X_A=cores/snes9x/snes9x_libretro.a
build_core "$SNES9X_A" \
    "https://github.com/libretro/snes9x" \
    "cores/snes9x" "cores/snes9x/libretro" \
    "platform=emscripten HAVE_THREADS=0 HAVE_NEON=0"

# ── 7. Link SNES core bundle (Web Worker) ────────────────────────────────────
# INITIAL_MEMORY=128 MB: snes9x requires substantial RAM for texture cache + SA-1 coprocessor emulation
echo "Linking core_snes.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  src/core.cpp \
  "$COMMON_A" \
  "$SNES9X_A" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8","HEAP16"]' \
  -s EXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  -s ENVIRONMENT=worker \
  -o "$BUILD_OUT/core_snes.js"

# ── 8. Build PCSX-ReARMed core ────────────────────────────────────────────────
PCSX_A=cores/pcsx_rearmed/pcsx_rearmed_libretro.a
build_core "$PCSX_A" \
    "https://github.com/libretro/pcsx_rearmed" \
    "cores/pcsx_rearmed" "cores/pcsx_rearmed" \
    "-f Makefile.libretro platform=emscripten CFLAGS_OPT=-O2"

# ── 9. Link PS1 core bundle (Web Worker) ─────────────────────────────────────
# INITIAL_MEMORY=256 MB: PS1 emulation needs full system RAM (2 MB) + GPU VRAM + disc image buffering
echo "Linking core_ps1.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  src/core.cpp \
  "$COMMON_A" \
  "$PCSX_A" \
  -sUSE_ZLIB=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8","HEAP16"]' \
  -s EXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=268435456 \
  -s ENVIRONMENT=worker \
  -o "$BUILD_OUT/core_ps1.js"

# ── 10. Build mGBA core ───────────────────────────────────────────────────────
MGBA_A=cores/mgba/mgba_libretro.a
build_core "$MGBA_A" \
    "https://github.com/libretro/mgba" \
    "cores/mgba" "cores/mgba" \
    "-f Makefile.libretro platform=emscripten"

# ── 11. Link GBA core bundle (Web Worker) ────────────────────────────────────
# INITIAL_MEMORY=128 MB: mGBA requires ROM/BIOS space + IWRAM/EWRAM emulation
echo "Linking core_gba.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  src/core.cpp \
  "$COMMON_A" \
  "$MGBA_A" \
  -sUSE_ZLIB=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8","HEAP16"]' \
  -s EXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  -s ENVIRONMENT=worker \
  -o "$BUILD_OUT/core_gba.js"

# ── 12. Build Yabause core (Sega Saturn) ─────────────────────────────────────
YABAUSE_A=cores/yabause/yabause_libretro.a
if [ ! -f "$SCRIPT_DIR/$YABAUSE_A" ]; then
    mkdir -p cores
    [ ! -d "$SCRIPT_DIR/cores/yabause" ] && \
        git clone --depth 1 "https://github.com/libretro/yabause" "$SCRIPT_DIR/cores/yabause"
    # Remove x86-only flag that Emscripten/clang doesn't support
    sed -i 's/-mfpmath=sse//g' "$SCRIPT_DIR/cores/yabause/yabause/src/libretro/Makefile"
    cd "$SCRIPT_DIR/cores/yabause/yabause/src/libretro"
    # HAVE_THREADS=0: use thr-dummy.c instead of thr-rthreads.c (avoids missing slock/sthread symbols)
    emmake make platform=emscripten HAVE_THREADS=0 CC=emcc CXX=em++ AR=emar RANLIB=emranlib
    for f in "yabause_libretro_emscripten.bc" "yabause_libretro_emscripten.a" \
             "yabause_libretro.bc" "yabause_libretro.a"; do
        [ -f "$f" ] && { cp "$f" "$SCRIPT_DIR/$YABAUSE_A"; break; }
    done
    cd "$SCRIPT_DIR"
    [ ! -f "$SCRIPT_DIR/$YABAUSE_A" ] && { echo "yabause_libretro.a build failed"; exit 1; }
    # Remove thr-rthreads.c.o: emar rcs appends into existing archives so the old
    # rthreads object may linger; strip it to avoid missing-symbol link errors.
    emar d "$SCRIPT_DIR/$YABAUSE_A" thr-rthreads.c.o 2>/dev/null || true
    echo "yabause_libretro.a: built"
else
    echo "yabause_libretro.a: cached"
fi

# ── 13. Link Saturn core bundle (Web Worker) ─────────────────────────────────
# INITIAL_MEMORY=256 MB: Saturn has dual SH-2 CPUs + VDP1/VDP2 graphics chips
echo "Linking core_saturn.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  src/core.cpp \
  "$COMMON_A" \
  "$YABAUSE_A" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS","HEAPU8","HEAP16"]' \
  -s EXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=268435456 \
  -s ENVIRONMENT=worker \
  -o "$BUILD_OUT/core_saturn.js"

# ── 14. Download N64Wasm prebuilt files ───────────────────────────────────────
if [ ! -f "$BUILD_OUT/n64wasm.js" ]; then
  echo "Downloading N64Wasm prebuilt..."
  curl -Lo "$BUILD_OUT/n64wasm.js" \
    https://raw.githubusercontent.com/nbarkhina/N64Wasm/master/dist/n64wasm.js
  curl -Lo "$BUILD_OUT/n64wasm.wasm" \
    https://raw.githubusercontent.com/nbarkhina/N64Wasm/master/dist/n64wasm.wasm
  curl -Lo "$BUILD_OUT/assets.zip" \
    https://github.com/nbarkhina/N64Wasm/raw/master/dist/assets.zip
fi

echo ""
echo "Build complete!  Serve with:"
echo "  python3 -m http.server 8080"
echo "Then open http://localhost:8080/index.html"
