// core.cpp
// libretro core driver — runs inside a Web Worker.
// Compiled by build.sh into core_gbc.js, core_gba.js, core_snes.js, core_ps1.js.
//
// Responsibilities:
//   - Initialises and drives a libretro emulator core (retro_init / retro_run)
//   - Converts video frames to RGBA and exposes them via get_frame_ptr()
//   - Buffers audio into a 16384-frame stereo ring buffer
//   - Exposes start_game() and step_frame() for core_worker.js to call via ccall

#include <emscripten.h>
#include <libretro.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <algorithm>

// ============================================================
//  State
// ============================================================
static const int AUDIO_BUF = 16384 * 2;  // int16 units (16384 stereo frames)
static int16_t   g_audio_buf[AUDIO_BUF];
static int       g_audio_write = 0;
static unsigned  g_audio_sample_rate = 44100;

static std::vector<uint8_t> g_frame_rgba;
static unsigned g_frame_w  = 160, g_frame_h = 144; // updated by retro_video_refresh

static retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static bool     g_running  = false;
static bool     g_buttons[4][16] = {};  // [port][button], up to 4 simultaneous players

// ============================================================
//  Libretro callbacks
// ============================================================
static void retro_log_cb(retro_log_level, const char* fmt, ...) {
    va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a);
}

void retro_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    g_frame_w = w; g_frame_h = h;
    g_frame_rgba.resize(w*h*4);
    if (g_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (unsigned y=0;y<h;y++) {
            const uint8_t* s=(const uint8_t*)data+y*pitch;
            uint8_t* d=g_frame_rgba.data()+y*w*4;
            for (unsigned x=0;x<w;x++) {
                d[x*4+0]=s[x*4+2]; d[x*4+1]=s[x*4+1];
                d[x*4+2]=s[x*4+0]; d[x*4+3]=255;
            }
        }
    } else {
        for (unsigned y=0;y<h;y++) {
            const uint16_t* s=(const uint16_t*)((const uint8_t*)data+y*pitch);
            uint8_t* d=g_frame_rgba.data()+y*w*4;
            for (unsigned x=0;x<w;x++) {
                uint16_t px=s[x]; uint8_t r,g,b;
                if (g_pixfmt==RETRO_PIXEL_FORMAT_RGB565) {
                    r=(px>>11)&0x1F; r=(r<<3)|(r>>2);
                    g=(px>>5) &0x3F; g=(g<<2)|(g>>4);
                    b=(px>>0) &0x1F; b=(b<<3)|(b>>2);
                } else {
                    r=(px>>10)&0x1F; r=(r<<3)|(r>>2);
                    g=(px>>5) &0x1F; g=(g<<3)|(g>>2);
                    b=(px>>0) &0x1F; b=(b<<3)|(b>>2);
                }
                d[x*4+0]=r; d[x*4+1]=g; d[x*4+2]=b; d[x*4+3]=255;
            }
        }
    }
    // No GL calls here — frame is shipped to main thread by core_worker.js
}

void retro_audio_sample(int16_t l, int16_t r) {
    g_audio_buf[g_audio_write] = l;
    g_audio_write = (g_audio_write + 1) % AUDIO_BUF;
    g_audio_buf[g_audio_write] = r;
    g_audio_write = (g_audio_write + 1) % AUDIO_BUF;
}
size_t retro_audio_sample_batch(const int16_t* data, size_t frames) {
    for (size_t i = 0; i < frames * 2; i++) {
        g_audio_buf[g_audio_write] = data[i];
        g_audio_write = (g_audio_write + 1) % AUDIO_BUF;
    }
    return frames;
}
void   retro_input_poll() {}
int16_t retro_input_state(unsigned port,unsigned device,unsigned,unsigned id) {
    if (port>=4||device!=RETRO_DEVICE_JOYPAD||id>=16) return 0;
    return g_buttons[port][id]?1:0;
}
bool retro_environment(unsigned cmd, void* data) {
    switch(cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixfmt=*(retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data=true; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data="/system"; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data="/saves"; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((retro_log_callback*)data)->log=retro_log_cb; return true;
        default: return false;
    }
}

// ============================================================
//  Exported functions (called by core_worker.js via ccall)
// ============================================================

extern "C" EMSCRIPTEN_KEEPALIVE void set_button(int port, int id, int pressed) {
    if (port>=0&&port<4&&id>=0&&id<16) g_buttons[port][id]=pressed;
}

extern "C" EMSCRIPTEN_KEEPALIVE void start_game(const char* path) {
    // retro_init is called here (not in main) so that BIOS/system files written
    // by core_worker.js onReady are already in the FS before the core initialises.
    retro_init();
    FILE* f=fopen(path,"rb");
    if (!f) { printf("Cannot open %s\n",path); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(),1,sz,f); fclose(f);
    retro_game_info info={}; info.path=path; info.data=buf.data(); info.size=(size_t)sz;
    if (retro_load_game(&info)) {
        g_running = true;
        retro_system_av_info av = {};
        retro_get_system_av_info(&av);
        if (av.timing.sample_rate > 0)
            g_audio_sample_rate = (unsigned)av.timing.sample_rate;
        printf("ROM loaded (%ld bytes), audio %u Hz\n", sz, g_audio_sample_rate);

        // Load save RAM from /saves/<stem>.srm into the core's SRAM buffer
        void* sram_ptr  = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (sram_ptr && sram_size > 0) {
            std::string rp(path);
            size_t sl = rp.rfind('/');
            std::string base = (sl != std::string::npos) ? rp.substr(sl+1) : rp;
            size_t dt = base.rfind('.');
            std::string stem = (dt != std::string::npos) ? base.substr(0, dt) : base;
            std::string savePath = std::string("/saves/") + stem + ".srm";
            FILE* sf = fopen(savePath.c_str(), "rb");
            if (sf) {
                fseek(sf, 0, SEEK_END); long ssz = ftell(sf); rewind(sf);
                if (ssz > 0 && (size_t)ssz <= sram_size)
                    fread(sram_ptr, 1, ssz, sf);
                fclose(sf);
                printf("Save loaded: %s (%ld bytes)\n", savePath.c_str(), ssz);
            }
        }
    } else printf("retro_load_game failed\n");
}

// Run one emulator frame — called by the worker's setInterval loop
extern "C" EMSCRIPTEN_KEEPALIVE void step_frame() {
    if (g_running) retro_run();
}

extern "C" EMSCRIPTEN_KEEPALIVE uint8_t* get_frame_ptr() {
    return g_frame_rgba.empty() ? nullptr : g_frame_rgba.data();
}
extern "C" EMSCRIPTEN_KEEPALIVE int get_frame_w() { return (int)g_frame_w; }
extern "C" EMSCRIPTEN_KEEPALIVE int get_frame_h() { return (int)g_frame_h; }

extern "C" EMSCRIPTEN_KEEPALIVE int16_t* get_audio_buf_ptr()    { return g_audio_buf; }
extern "C" EMSCRIPTEN_KEEPALIVE int      get_audio_write_pos()  { return g_audio_write; }
extern "C" EMSCRIPTEN_KEEPALIVE int      get_audio_buf_size()   { return AUDIO_BUF; }
extern "C" EMSCRIPTEN_KEEPALIVE int      get_audio_sample_rate(){ return (int)g_audio_sample_rate; }

// ============================================================
//  Main
// ============================================================
int main() {
    retro_set_environment      (retro_environment);
    retro_set_video_refresh    (retro_video_refresh);
    retro_set_audio_sample     (retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll       (retro_input_poll);
    retro_set_input_state      (retro_input_state);
    // retro_init is deferred to start_game() so BIOS files can be written first
    emscripten_exit_with_live_runtime();
    return 0;
}
