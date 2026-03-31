# Adjust CORE_LIB to point to your compiled core .a file
CORE_LIB := cores/gambatte/libgambatte_libretro.a

EMCC     := emcc
CXXFLAGS := -O2 -std=c++17
INCLUDES := -I include   # put libretro.h here

LDFLAGS  := \
  -s USE_WEBGL2=0        \
  -s FULL_ES2=1          \
  -s EXPORTED_RUNTIME_METHODS='["ccall","FS"]' \
  -s EXPORTED_FUNCTIONS='["_main","_start_game","_set_button"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=67108864 \
  -s ASYNCIFY=0          \
  --shell-file shell.html \
  -o game.js

all: game.js

game.js: frontend.cpp $(CORE_LIB)
	$(EMCC) $(CXXFLAGS) $(INCLUDES) $^ $(LDFLAGS)

clean:
	rm -f game.js game.wasm

.PHONY: all clean
