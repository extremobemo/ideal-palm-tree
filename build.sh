#!/bin/bash
# Full build script for retro-cube
# Prerequisites: Emscripten SDK activated (source emsdk_env.sh)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

EXPORTS='["_main","_start_game","_set_button","_set_move_key","_add_mouse_delta","_get_game_tex_id","_set_frame_size"]'

# ── 1. Get libretro.h ─────────────────────────────────────────────────────────
if [ ! -f include/libretro.h ]; then
  mkdir -p include
  curl -Lo include/libretro.h \
    https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h
fi

# ── 2. Build gambatte core ────────────────────────────────────────────────────
GAMBATTE_A=cores/gambatte/gambatte_libretro.a

if [ ! -f "$GAMBATTE_A" ]; then
  mkdir -p cores
  [ ! -d cores/gambatte ] && \
    git clone --depth 1 https://github.com/libretro/gambatte-libretro cores/gambatte

  cd cores/gambatte
  emmake make platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib
  cp gambatte_libretro_emscripten.bc gambatte_libretro.a
  cd "$SCRIPT_DIR"
fi

[ ! -f "$GAMBATTE_A" ] && { echo "gambatte build failed"; exit 1; }
echo "Gambatte: $GAMBATTE_A"

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

# ── 4. Link GBC bundle ────────────────────────────────────────────────────────
echo "Linking game_gbc.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  frontend.cpp \
  "$COMMON_A" \
  "$GAMBATTE_A" \
  -s FULL_ES2=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS"]' \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  --preload-file tv/ \
  -o game_gbc.js

# ── 5. Build snes9x core ──────────────────────────────────────────────────────
SNES9X_A=cores/snes9x/snes9x_libretro.a

if [ ! -f "$SNES9X_A" ]; then
  mkdir -p cores
  [ ! -d cores/snes9x ] && \
    git clone --depth 1 https://github.com/libretro/snes9x cores/snes9x

  cd cores/snes9x/libretro
  emmake make platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib \
    HAVE_THREADS=0 HAVE_NEON=0
  if [ -f snes9x_libretro_emscripten.bc ]; then
    cp snes9x_libretro_emscripten.bc "$SCRIPT_DIR/$SNES9X_A"
  elif [ -f snes9x_libretro.bc ]; then
    cp snes9x_libretro.bc "$SCRIPT_DIR/$SNES9X_A"
  fi
  cd "$SCRIPT_DIR"
fi

[ ! -f "$SNES9X_A" ] && { echo "snes9x build failed: archive not found"; exit 1; }
echo "snes9x: $SNES9X_A"

# ── 6. Link SNES bundle ───────────────────────────────────────────────────────
echo "Linking game_snes.js..."
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  frontend.cpp \
  "$COMMON_A" \
  "$SNES9X_A" \
  -s FULL_ES2=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS"]' \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  --preload-file tv/ \
  -o game_snes.js

# ── 7. Download N64Wasm prebuilt files ────────────────────────────────────────
if [ ! -f n64wasm.js ]; then
  echo "Downloading N64Wasm prebuilt..."
  curl -Lo n64wasm.js \
    https://raw.githubusercontent.com/nbarkhina/N64Wasm/master/dist/n64wasm.js
  curl -Lo n64wasm.wasm \
    https://raw.githubusercontent.com/nbarkhina/N64Wasm/master/dist/n64wasm.wasm
  curl -Lo assets.zip \
    https://github.com/nbarkhina/N64Wasm/raw/master/dist/assets.zip
fi

echo ""
echo "Build complete!  Serve with:"
echo "  python3 -m http.server 8080"
echo "Then open http://localhost:8080/index.html"
