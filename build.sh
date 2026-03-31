#!/bin/bash
# Full build script for retro-cube
# Prerequisites: Emscripten SDK activated (source emsdk_env.sh)

set -e

# ── 1. Get libretro.h ─────────────────────────────────────────────────────────
if [ ! -f include/libretro.h ]; then
  mkdir -p include
  curl -Lo include/libretro.h \
    https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h
fi

# ── 2. Build gambatte core ────────────────────────────────────────────────────
# Output is a .bc (LLVM bitcode archive), not .a
CORE_A=cores/gambatte/gambatte_libretro.a

if [ ! -f "$CORE_A" ]; then
  mkdir -p cores
  [ ! -d cores/gambatte ] && \
    git clone --depth 1 https://github.com/libretro/gambatte-libretro cores/gambatte

  cd cores/gambatte
  emmake make platform=emscripten CC=emcc CXX=em++ AR=emar RANLIB=emranlib
  # The core outputs a .bc that is actually an ar archive — copy it with .a extension
  cp gambatte_libretro_emscripten.bc gambatte_libretro.a
  cd ../..
fi

[ ! -f "$CORE_A" ] && { echo "Build failed: core archive not found"; exit 1; }
echo "Core: $CORE_A"

# ── 3. Compile libretro-common C files → libretro-common.a ───────────────────
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

# ── 4. Compile frontend and link everything ───────────────────────────────────
em++ -O2 -std=c++17 \
  -I include \
  -I "$LIBRETRO_COMMON/include" \
  frontend.cpp \
  "$COMMON_A" \
  "$CORE_A" \
  -s FULL_ES2=1 \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS"]' \
  -s EXPORTED_FUNCTIONS='["_main","_start_game","_set_button","_set_move_key","_add_mouse_delta"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=134217728 \
  --preload-file tv/ \
  -o game.js

echo ""
echo "Build complete!  Serve with:"
echo "  python3 -m http.server 8080"
echo "Then open http://localhost:8080/index.html"
