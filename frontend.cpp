// ============================================================
//  frontend.cpp
//  Compile with -DRENDERER_ONLY for the main-thread 3D renderer
//  Compile with -DCORE_ONLY    for a libretro core Web Worker
// ============================================================

#ifdef RENDERER_ONLY
#  define CGLTF_IMPLEMENTATION
#  define STB_IMAGE_IMPLEMENTATION
#  include <emscripten.h>
#  include <emscripten/html5.h>
#  include <GLES2/gl2.h>
#  include <cgltf.h>
#  include <stb_image.h>
#endif

#ifdef CORE_ONLY
#  include <emscripten.h>
#  include <libretro.h>
#endif

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>

// ============================================================
//  Matrix math  (column-major, matches GLSL)  — renderer only
// ============================================================
#ifdef RENDERER_ONLY
typedef float M4[16];

static void m4_identity(M4 m) { memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.f; }

static void m4_mul(M4 out, const M4 a, const M4 b) {
    M4 tmp;
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) {
        tmp[c*4+r]=0;
        for(int k=0;k<4;k++) tmp[c*4+r]+=a[k*4+r]*b[c*4+k];
    }
    memcpy(out,tmp,64);
}

static void m4_persp(M4 m, float fov, float asp, float n, float f) {
    float t=tanf(fov*.5f); memset(m,0,64);
    m[0]=1.f/(asp*t); m[5]=1.f/t;
    m[10]=(f+n)/(n-f); m[11]=-1.f; m[14]=(2.f*f*n)/(n-f);
}

// Look-at (eye toward target, up=+Y)
static void m4_lookat(M4 m, float ex,float ey,float ez,
                              float tx,float ty,float tz) {
    float fx=tx-ex, fy=ty-ey, fz=tz-ez;
    float il=1.f/sqrtf(fx*fx+fy*fy+fz*fz);
    fx*=il; fy*=il; fz*=il;
    // right = forward × up(0,1,0)
    float rx=fz, ry=0.f, rz=-fx;
    float rl=1.f/sqrtf(rx*rx+ry*ry+rz*rz);
    rx*=rl; ry*=rl; rz*=rl;
    // up = right × forward
    float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
    m4_identity(m);
    m[0]=rx; m[4]=ry; m[8] =rz;
    m[1]=ux; m[5]=uy; m[9] =uz;
    m[2]=-fx;m[6]=-fy;m[10]=-fz;
    m[12]=-(rx*ex+ry*ey+rz*ez);
    m[13]=-(ux*ex+uy*ey+uz*ez);
    m[14]= (fx*ex+fy*ey+fz*ez);
}

static void m4_from_trs(M4 out, const float t[3], const float r[4], const float s[3]) {
    float x=r[0],y=r[1],z=r[2],w=r[3];
    float x2=x+x,y2=y+y,z2=z+z;
    float xx=x*x2,xy=x*y2,xz=x*z2;
    float yy=y*y2,yz=y*z2,zz=z*z2;
    float wx=w*x2,wy=w*y2,wz=w*z2;
    // column-major
    out[0]=(1-(yy+zz))*s[0]; out[1]=(xy+wz)*s[0];  out[2]=(xz-wy)*s[0];  out[3]=0.f;
    out[4]=(xy-wz)*s[1];     out[5]=(1-(xx+zz))*s[1]; out[6]=(yz+wx)*s[1]; out[7]=0.f;
    out[8]=(xz+wy)*s[2];     out[9]=(yz-wx)*s[2];  out[10]=(1-(xx+yy))*s[2]; out[11]=0.f;
    out[12]=t[0]; out[13]=t[1]; out[14]=t[2]; out[15]=1.f;
}

static void q_slerp(float* out, const float* a, const float* b, float t) {
    float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    float b2[4]={b[0],b[1],b[2],b[3]};
    if (dot < 0.f) { dot=-dot; b2[0]=-b2[0];b2[1]=-b2[1];b2[2]=-b2[2];b2[3]=-b2[3]; }
    if (dot > 0.9995f) {
        for(int i=0;i<4;i++) out[i]=a[i]+(b2[i]-a[i])*t;
        float l=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
        for(int i=0;i<4;i++) out[i]/=l;
        return;
    }
    float th0=acosf(dot), th=th0*t;
    float s0=sinf(th)/sinf(th0), sa=cosf(th)-dot*s0;
    for(int i=0;i<4;i++) out[i]=sa*a[i]+s0*b2[i];
}

// ============================================================
//  GL state  — renderer only
// ============================================================
struct TvPrim {
    GLuint vbo;
    int    vcount;
    GLuint base_tex;    // 0 if none
    bool   is_screen;   // use game texture
    bool   double_sided;
    bool   is_room;     // part of room model — apply g_room_xform
    M4     world;
};
static std::vector<TvPrim> g_prims;
static GLuint g_game_tex   = 0;
static GLuint g_white_tex  = 0;  // fallback for room prims with no texture

// ── Room transform (tuned at runtime, hardcoded once locked in) ──────────────
static float g_room_scale = 32.f;
static float g_room_rot_y = -3.14159265f;   // radians (-180 deg)
static float g_room_tx = 4.f, g_room_ty = 21.f, g_room_tz = 15.f;
static GLuint g_prog     = 0;
static int    g_a_pos=-1, g_a_uv=-1, g_a_norm=-1;
static int    g_u_mvp=-1, g_u_model=-1, g_u_tex=-1, g_u_screen=-1, g_u_overscan=-1, g_u_tv_quad_pos=-1, g_u_tv_quad_col=-1, g_u_tv_normal=-1, g_u_lamp_pos=-1, g_u_lamp_intensity=-1;
static float  g_tv_screen_normal[3] = {0.f, 0.f, -1.f};  // base outward normal
static float  g_cone_yaw   =    0.f;   // degrees, rotates cone left/right
static float  g_cone_pitch = -362.f;   // degrees, tilts cone up/down
static float  g_cone_power =  18.2f;   // exponent — higher = tighter cone
static int    g_u_cone_power = -1;
static int    g_skin_u_cone_power = -1;
static float  g_overscan_x = 0.04f, g_overscan_y = 0.04f;
static float  g_tv_screen_pos[3] = {0.f, 0.f, 0.f};
static float  g_tv_quad_pos[4][3] = {};
static float  g_tv_quad_col[4][3] = {{0.3f,0.4f,0.8f},{0.3f,0.4f,0.8f},{0.3f,0.4f,0.8f},{0.3f,0.4f,0.8f}};
static float  g_tv_light_intensity = 1.7f;
static float  g_tv_half_x = 0.5f, g_tv_half_y = 0.5f;
static float  g_lamp_pos[3] = {0.f, -45.f, -260.f};
static float  g_lamp_intensity = 1.0f;
static float  g_cat_eye_height  = -35.f; // camera is at eye level; cat rendered this far below

// ============================================================
//  Player  (structured for future multiplayer)
// ============================================================
struct Player {
    float x = 0.f, y = 20.f, z = -80.f;  // world position (feet)
    float yaw = 0.f, pitch = 0.f;         // look angles (radians)
};

static Player g_local;

struct RemotePlayer {
    float x = 0.f, y = -40.f, z = 0.f;
    float yaw = 0.f;
    bool  active = false;
    bool  moving = false;
    float anim_time = 0.f;
    int   anim_idx  = 0;
    int   model_idx = 0; // 0=cat, 1=incidental_70
};
static RemotePlayer g_remote[8];
// ── Cat skinned model ────────────────────────────────────────────────────────
struct CatChannel {
    int node, path; // path: 0=T, 1=R, 2=S
    std::vector<float> times, values;
};
struct CatAnim {
    float duration;
    std::vector<CatChannel> channels;
};
static const int CAT_JOINTS = 29;
static const int CAT_NODES  = 64; // large enough for all supported models
struct AvatarMesh { int start, count; GLuint tex; };
struct AvatarModel {
    GLuint vbo = 0;
    std::vector<AvatarMesh> meshes;
    int joint_count = 0;
    int joint_nodes[CAT_JOINTS] = {};
    float inv_bind[CAT_JOINTS][16] = {};
    int node_count = 0;
    int node_parent[CAT_NODES] = {};
    float node_def_t[CAT_NODES][3] = {};
    float node_def_r[CAT_NODES][4] = {};
    float node_def_s[CAT_NODES][3] = {};
    std::vector<CatAnim> anims;
    int idle_anim = 0;
    int walk_anim = 0;
    bool loaded = false;
};
static AvatarModel g_models[2]; // 0=cat, 1=incidental_70
static GLuint g_skin_prog  = 0;
// Shared scratch buffers for animation evaluation (overwritten each frame per player)
static float  g_cat_bone_mats [CAT_JOINTS][16];
static float  g_cat_node_t    [CAT_NODES][3];
static float  g_cat_node_r    [CAT_NODES][4];
static float  g_cat_node_s    [CAT_NODES][3];
static float  g_cat_node_global[CAT_NODES][16];
static int    g_skin_u_vp=-1, g_skin_u_world=-1, g_skin_u_bones=-1;
static int    g_skin_u_tex=-1, g_skin_u_tv_quad_pos=-1, g_skin_u_tv_quad_col=-1, g_skin_u_tv_normal=-1;
static int    g_skin_u_lamp_pos=-1, g_skin_u_lamp_intensity=-1;
static int    g_skin_a_pos=-1, g_skin_a_uv=-1, g_skin_a_norm=-1;
static int    g_skin_a_joints=-1, g_skin_a_weights=-1;

static bool g_move[4] = {};  // W S A D
static bool g_js_colors = false; // true = JS drives quad colors (always true in new arch)

// Frame dimensions — set by set_frame_size() and used by CRT shader uniforms
static unsigned g_frame_w = 160, g_frame_h = 144;

// ============================================================
//  CRT pass state  — renderer only
// ============================================================
static GLuint g_crt_fbo  = 0;
static GLuint g_crt_tex  = 0;
static GLuint g_crt_prog = 0;
static GLuint g_crt_vbo  = 0;
static int    g_crt_a_vert=-1, g_crt_a_uv=-1;
static int    g_crt_u_mvp=-1, g_crt_u_fcount=-1;
static int    g_crt_u_out=-1, g_crt_u_texsz=-1, g_crt_u_insz=-1, g_crt_u_tex=-1;
static const int CRT_W = 640, CRT_H = 480;
static int    g_frame_count = 0;

#endif // RENDERER_ONLY

// ============================================================
//  Core globals  — core only
// ============================================================
#ifdef CORE_ONLY
static const int AUDIO_BUF = 16384 * 2;  // int16 units (16384 stereo frames)
static int16_t   g_audio_buf[AUDIO_BUF];
static int       g_audio_write = 0;
static unsigned  g_audio_sample_rate = 44100;

static std::vector<uint8_t> g_frame_rgba;
static unsigned g_frame_w  = 160, g_frame_h = 144; // updated by retro_video_refresh

static retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static bool     g_running  = false;
static bool     g_buttons[16] = {};
#endif // CORE_ONLY

// ============================================================
//  Libretro callbacks  — core only
// ============================================================
#ifdef CORE_ONLY
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
    if (port||device!=RETRO_DEVICE_JOYPAD||id>=16) return 0;
    return g_buttons[id]?1:0;
}
bool retro_environment(unsigned cmd, void* data) {
    switch(cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixfmt=*(retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data=true; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data="/system/"; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data="/saves/"; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((retro_log_callback*)data)->log=retro_log_cb; return true;
        default: return false;
    }
}
#endif // CORE_ONLY

// ============================================================
//  Shaders  — renderer only
// ============================================================
#ifdef RENDERER_ONLY
static const char* VS = R"(
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec3 a_norm;
uniform mat4 u_mvp;
uniform mat4 u_model;
varying vec2 v_uv;
varying vec3 v_wpos;
varying vec3 v_wnorm;
void main() {
    vec4 wp = u_model * vec4(a_pos, 1.0);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv   = a_uv;
    v_wpos = wp.xyz;
    v_wnorm = normalize(vec3(u_model * vec4(a_norm, 0.0)));
}
)";

static const char* FS = R"(
precision mediump float;
varying vec2 v_uv;
varying vec3 v_wpos;
varying vec3 v_wnorm;
uniform sampler2D u_tex;
uniform float u_screen;       // 1.0 = screen (emissive), 0.0 = normal
uniform vec2  u_overscan;     // per-axis overscan factor
uniform vec3  u_tv_quad_pos[4]; // world-space centre of each screen quadrant
uniform vec3  u_tv_quad_col[4]; // averaged colour of each screen quadrant
uniform vec3  u_tv_normal;      // cone direction (unit)
uniform float u_cone_power;     // cone falloff exponent
uniform vec3  u_lamp_pos;
uniform float u_lamp_intensity;

void main() {
    if (u_screen > 0.5) {
        vec2 uv = vec2(1.0 - v_uv.x, v_uv.y);
        uv = uv * (1.0 - 2.0 * u_overscan) + u_overscan;
        gl_FragColor = texture2D(u_tex, uv);
    } else {
        vec3 n = normalize(v_wnorm);
        vec3 albedo = texture2D(u_tex, v_uv).rgb;

        // ── Interior ceiling lamp ───────────────────────────────────────────
        vec3 to_lamp  = u_lamp_pos - v_wpos;
        float lamp_dist  = length(to_lamp);
        float lamp_ndotl = max(dot(n, to_lamp / lamp_dist), 0.0);
        float lamp_atten = 1.0 / (1.0 + lamp_dist * lamp_dist * 0.000022);
        vec3 lamp = vec3(1.0, 0.84, 0.55) * lamp_ndotl * lamp_atten * u_lamp_intensity;

        // ── TV screen glow: 4 directional quadrant colour-sampled lights ───
        vec3 tv_glow = vec3(0.0);
        for (int i = 0; i < 4; i++) {
            vec3 to_q   = u_tv_quad_pos[i] - v_wpos;
            float qd    = length(to_q);
            float ndotl = max(dot(n, to_q / qd), 0.0);
            float atten = 1.0 / (1.0 + qd * qd * 0.00028);
            float cone  = max(dot(u_tv_normal, -(to_q / qd)), 0.0);
            cone        = pow(cone, u_cone_power);
            tv_glow    += u_tv_quad_col[i] * ndotl * atten * cone * 2.2;
        }

        // ── Very low warm ambient ───────────────────────────────────────────
        vec3 ambient = vec3(0.07, 0.052, 0.034);

        gl_FragColor = vec4(albedo * (ambient + lamp + tv_glow), 1.0);
    }
}
)";

// ============================================================
//  Skinned-mesh shaders (GLSL ES 3.00 / WebGL2)
// ============================================================
static const char* SKIN_VS = R"(#version 300 es
in vec3 a_pos;
in vec2 a_uv;
in vec3 a_norm;
in vec4 a_joints;
in vec4 a_weights;
uniform mat4 u_vp;
uniform mat4 u_world;
uniform mat4 u_bones[29];
out vec2 v_uv;
out vec3 v_wpos;
out vec3 v_wnorm;
void main() {
    mat4 skin = a_weights.x * u_bones[int(a_joints.x)]
              + a_weights.y * u_bones[int(a_joints.y)]
              + a_weights.z * u_bones[int(a_joints.z)]
              + a_weights.w * u_bones[int(a_joints.w)];
    vec4 wp = u_world * skin * vec4(a_pos, 1.0);
    gl_Position = u_vp * wp;
    v_uv    = a_uv;
    v_wpos  = wp.xyz;
    v_wnorm = normalize(mat3(u_world) * mat3(skin) * a_norm);
}
)";

static const char* SKIN_FS = R"(#version 300 es
precision mediump float;
in vec2  v_uv;
in vec3  v_wpos;
in vec3  v_wnorm;
uniform sampler2D u_tex;
uniform vec3  u_tv_quad_pos[4];
uniform vec3  u_tv_quad_col[4];
uniform vec3  u_tv_normal;
uniform float u_cone_power;
uniform vec3  u_lamp_pos;
uniform float u_lamp_intensity;
out vec4 fragColor;
void main() {
    vec3 n = normalize(v_wnorm);
    vec3 albedo = texture(u_tex, v_uv).rgb;
    vec3 to_lamp  = u_lamp_pos - v_wpos;
    float ld = length(to_lamp);
    float lamp_ndotl = max(dot(n, to_lamp/ld), 0.0);
    float lamp_atten = 1.0/(1.0+ld*ld*0.000022);
    vec3 lamp = vec3(1.0,0.84,0.55)*lamp_ndotl*lamp_atten*u_lamp_intensity;
    vec3 tv_glow = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec3 to_q   = u_tv_quad_pos[i] - v_wpos;
        float qd    = length(to_q);
        float ndotl = max(dot(n, to_q/qd), 0.0);
        float atten = 1.0/(1.0+qd*qd*0.00028);
        float cone  = max(dot(u_tv_normal, -(to_q/qd)), 0.0);
        cone        = pow(cone, u_cone_power);
        tv_glow    += u_tv_quad_col[i]*ndotl*atten*cone*2.2;
    }
    vec3 ambient = vec3(0.07,0.052,0.034);
    fragColor = vec4(albedo*(ambient+lamp+tv_glow), 1.0);
}
)";

// ============================================================
//  CRT-Geom shaders (adapted for GLSL ES 1.0)
//  Original: Copyright (C) 2010-2012 cgwg, Themaister and DOLLS (GPL v2+)
// ============================================================
static const char* CRT_VS = R"(
#define CRTgamma 2.4
#define d 1.6
#define R 2.0
#define x_tilt 0.0
#define y_tilt 0.0
#define SHARPER 1.0

attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
uniform mediump vec2 OutputSize;
uniform mediump vec2 TextureSize;
uniform mediump vec2 InputSize;

varying vec4 TEX0;
varying vec2 aspect;
varying vec3 stretch;
varying vec2 sinangle;
varying vec2 cosangle;
varying vec2 one;
varying float mod_factor;
varying vec2 ilfac;

#define FIX(c) max(abs(c), 1e-5);

float intersect(vec2 xy) {
    float A = dot(xy,xy)+d*d;
    float B = 2.0*(R*(dot(xy,sinangle)-d*cosangle.x*cosangle.y)-d*d);
    float C = d*d + 2.0*R*d*cosangle.x*cosangle.y;
    return (-B-sqrt(B*B-4.0*A*C))/(2.0*A);
}
vec2 bkwtrans(vec2 xy) {
    float c = intersect(xy);
    vec2 pt = vec2(c)*xy;
    pt -= vec2(-R)*sinangle;
    pt /= vec2(R);
    vec2 tang = sinangle/cosangle;
    vec2 poc = pt/cosangle;
    float A = dot(tang,tang)+1.0;
    float B = -2.0*dot(poc,tang);
    float C = dot(poc,poc)-1.0;
    float a = (-B+sqrt(B*B-4.0*A*C))/(2.0*A);
    vec2 uv = (pt-a*sinangle)/cosangle;
    float r = R*acos(a);
    return uv*r/sin(r/R);
}
vec2 fwtrans(vec2 uv) {
    float r = FIX(sqrt(dot(uv,uv)));
    uv *= sin(r/R)/r;
    float x = 1.0-cos(r/R);
    float D = d/R + x*cosangle.x*cosangle.y+dot(uv,sinangle);
    return d*(uv*cosangle-x*sinangle)/D;
}
vec3 maxscale() {
    vec2 c = bkwtrans(-R * sinangle / (1.0 + R/d*cosangle.x*cosangle.y));
    vec2 a = vec2(0.5)*aspect;
    vec2 lo = vec2(fwtrans(vec2(-a.x,c.y)).x, fwtrans(vec2(c.x,-a.y)).y)/aspect;
    vec2 hi = vec2(fwtrans(vec2(+a.x,c.y)).x, fwtrans(vec2(c.x,+a.y)).y)/aspect;
    return vec3((hi+lo)*aspect*0.5, max(hi.x-lo.x, hi.y-lo.y));
}

void main() {
    aspect = vec2(1.0, 0.75);
    gl_Position = VertexCoord.x*MVPMatrix[0] + VertexCoord.y*MVPMatrix[1]
                + VertexCoord.z*MVPMatrix[2] + VertexCoord.w*MVPMatrix[3];
    TEX0.xy = TexCoord.xy * 1.0001;
    sinangle = sin(vec2(x_tilt, y_tilt)) + vec2(0.001);
    cosangle = cos(vec2(x_tilt, y_tilt)) + vec2(0.001);
    stretch = maxscale();
    ilfac = vec2(1.0, clamp(floor(InputSize.y/200.0), 1.0, 2.0));
    one = ilfac / vec2(SHARPER * TextureSize.x, TextureSize.y);
    mod_factor = TexCoord.x * TextureSize.x * OutputSize.x / InputSize.x;
}
)";

static const char* CRT_FS = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

#define CRTgamma      2.4
#define monitorgamma  2.2
#define d             1.6
#define R             2.0
#define CURVATURE     1.0
#define cornersize    0.03
#define cornersmooth  1000.0
#define overscan_x    100.0
#define overscan_y    100.0
#define DOTMASK       0.3
#define scanline_weight 0.3
#define lum           0.0
#define interlace_detect 1.0
#define SATURATION    1.0

uniform int FrameCount;
uniform mediump vec2 OutputSize;
uniform mediump vec2 TextureSize;
uniform mediump vec2 InputSize;
uniform sampler2D Texture;

varying vec4 TEX0;
varying vec2 aspect;
varying vec3 stretch;
varying vec2 sinangle;
varying vec2 cosangle;
varying vec2 one;
varying float mod_factor;
varying vec2 ilfac;

#define FIX(c) max(abs(c), 1e-5);
#define PI 3.141592653589
#define TEX2D(c) pow(texture2D(Texture,(c)), vec4(CRTgamma))

float intersect(vec2 xy) {
    float A = dot(xy,xy)+d*d;
    float B = 2.0*(R*(dot(xy,sinangle)-d*cosangle.x*cosangle.y)-d*d);
    float C = d*d + 2.0*R*d*cosangle.x*cosangle.y;
    return (-B-sqrt(B*B-4.0*A*C))/(2.0*A);
}
vec2 bkwtrans(vec2 xy) {
    float c = intersect(xy);
    vec2 pt = vec2(c)*xy;
    pt -= vec2(-R)*sinangle;
    pt /= vec2(R);
    vec2 tang = sinangle/cosangle;
    vec2 poc = pt/cosangle;
    float A = dot(tang,tang)+1.0;
    float B = -2.0*dot(poc,tang);
    float C = dot(poc,poc)-1.0;
    float a = (-B+sqrt(B*B-4.0*A*C))/(2.0*A);
    vec2 uv = (pt-a*sinangle)/cosangle;
    float r = FIX(R*acos(a));
    return uv*r/sin(r/R);
}
vec2 transform(vec2 coord) {
    coord *= TextureSize / InputSize;
    coord = (coord-vec2(0.5))*aspect*stretch.z + stretch.xy;
    return (bkwtrans(coord)/vec2(overscan_x/100.0, overscan_y/100.0)/aspect+vec2(0.5))*InputSize/TextureSize;
}
float corner(vec2 coord) {
    coord *= TextureSize / InputSize;
    coord = (coord-vec2(0.5))*vec2(overscan_x/100.0, overscan_y/100.0)+vec2(0.5);
    coord = min(coord, vec2(1.0)-coord)*aspect;
    vec2 cdist = vec2(cornersize);
    coord = cdist - min(coord, cdist);
    float dist = sqrt(dot(coord,coord));
    return clamp((cdist.x-dist)*cornersmooth, 0.0, 1.0)*1.0001;
}
vec4 scanlineWeights(float distance, vec4 color) {
    vec4 wid = 2.0 + 2.0*pow(color, vec4(4.0));
    vec4 weights = vec4(distance / scanline_weight);
    return (lum+1.4)*exp(-pow(weights*inversesqrt(0.5*wid),wid))/(0.6+0.2*wid);
}
vec3 inv_gamma(vec3 col, vec3 power) {
    vec3 cir = col-1.0; cir *= cir;
    return mix(sqrt(col), sqrt(1.0-cir), power);
}

void main() {
    vec2 xy = transform(TEX0.xy);
    float cval = corner(xy);

    vec2 ilvec = vec2(0.0, ilfac.y*interlace_detect > 1.5 ? mod(float(FrameCount),2.0) : 0.0);
    vec2 ratio_scale = (xy*TextureSize - vec2(0.5) + ilvec) / ilfac;
    float filter_ = InputSize.y / OutputSize.y;
    vec2 uv_ratio = fract(ratio_scale);
    xy = (floor(ratio_scale)*ilfac + vec2(0.5) - ilvec) / TextureSize;

    vec4 coeffs = PI * vec4(1.0+uv_ratio.x, uv_ratio.x, 1.0-uv_ratio.x, 2.0-uv_ratio.x);
    coeffs = FIX(coeffs);
    coeffs = 2.0*sin(coeffs)*sin(coeffs/2.0)/(coeffs*coeffs);
    coeffs /= dot(coeffs, vec4(1.0));

    vec4 col  = clamp(mat4(TEX2D(xy+vec2(-one.x,0.0)), TEX2D(xy),
                           TEX2D(xy+vec2(one.x,0.0)),  TEX2D(xy+vec2(2.0*one.x,0.0)))*coeffs, 0.0,1.0);
    vec4 col2 = clamp(mat4(TEX2D(xy+vec2(-one.x,one.y)), TEX2D(xy+vec2(0.0,one.y)),
                           TEX2D(xy+one),                TEX2D(xy+vec2(2.0*one.x,one.y)))*coeffs, 0.0,1.0);

    vec4 weights  = scanlineWeights(uv_ratio.y, col);
    vec4 weights2 = scanlineWeights(1.0-uv_ratio.y, col2);
    uv_ratio.y += 1.0/3.0*filter_;
    weights  = (weights  + scanlineWeights(uv_ratio.y, col))  / 3.0;
    weights2 = (weights2 + scanlineWeights(abs(1.0-uv_ratio.y), col2)) / 3.0;
    uv_ratio.y -= 2.0/3.0*filter_;
    weights  += scanlineWeights(abs(uv_ratio.y), col)  / 3.0;
    weights2 += scanlineWeights(abs(1.0-uv_ratio.y), col2) / 3.0;

    vec3 mul_res = (col*weights + col2*weights2).rgb * vec3(cval);

    mul_res *= mix(vec3(1.0,1.0-DOTMASK,1.0), vec3(1.0-DOTMASK,1.0,1.0-DOTMASK),
                   floor(mod(mod_factor, 2.0)));

    vec3 pwr = vec3(1.0/((-0.7*(1.0-scanline_weight)+1.0)*(-0.5*DOTMASK+1.0))-1.25);
    mul_res = inv_gamma(mul_res, pwr);

    float luma = dot(mul_res, vec3(0.3,0.6,0.1));
    mul_res = mix(vec3(luma), mul_res, SATURATION);

    gl_FragColor = vec4(mul_res, 1.0);
}
)";

static GLuint make_shader(GLenum type, const char* src) {
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,nullptr); glCompileShader(s);
    GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) { char buf[512]; glGetShaderInfoLog(s,512,nullptr,buf); printf("Shader error: %s\n",buf); }
    return s;
}

// ============================================================
//  Texture loading (stb_image)
// ============================================================
static GLuint load_tex(const char* path) {
    int w,h,ch;
    uint8_t* px=stbi_load(path,&w,&h,&ch,4);
    if (!px) { printf("Cannot load texture: %s\n",path); return 0; }
    GLuint t; glGenTextures(1,&t);
    glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glGenerateMipmap(GL_TEXTURE_2D);  // WebGL2: NPOT mipmaps are fine
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    stbi_image_free(px);
    return t;
}

// Load texture from an already-decoded buffer view (embedded GLTF images)
static GLuint load_tex_bv(cgltf_buffer_view* bv) {
    if (!bv || !bv->buffer->data) return 0;
    const uint8_t* d = (const uint8_t*)bv->buffer->data + bv->offset;
    int w,h,ch;
    uint8_t* px = stbi_load_from_memory(d,(int)bv->size,&w,&h,&ch,4);
    if (!px) {
        printf("load_tex_bv: stbi failed (size=%zu, reason=%s)\n", bv->size, stbi_failure_reason());
        return 0;
    }
    GLuint t; glGenTextures(1,&t);
    glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glGenerateMipmap(GL_TEXTURE_2D);  // WebGL2: NPOT mipmaps are fine
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    stbi_image_free(px);
    return t;
}

// ============================================================
//  GLTF loader
// ============================================================
static void load_tv() {
    cgltf_options opts = {};
    cgltf_data* gltf = nullptr;
    cgltf_result r = cgltf_parse_file(&opts, "/tv/CRT_TV.gltf", &gltf);
    if (r != cgltf_result_success) { printf("cgltf parse failed: %d\n",r); return; }
    r = cgltf_load_buffers(&opts, gltf, "/tv/CRT_TV.gltf");
    if (r != cgltf_result_success) { printf("cgltf load_buffers failed: %d\n",r); return; }

    for (cgltf_size ni=0; ni<gltf->nodes_count; ni++) {
        cgltf_node* node=&gltf->nodes[ni];
        if (!node->mesh) continue;

        M4 world;
        cgltf_node_transform_world(node, world);
        // USDZ conversion leaves model upside-down — apply Y-flip correction
        world[1] = -world[1]; world[5] = -world[5];
        world[9] = -world[9]; world[13] = -world[13];

        for (cgltf_size pi=0; pi<node->mesh->primitives_count; pi++) {
            cgltf_primitive* prim=&node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            cgltf_accessor *pos_acc=nullptr, *uv_acc=nullptr, *norm_acc=nullptr;
            for (cgltf_size ai=0; ai<prim->attributes_count; ai++) {
                auto& attr=prim->attributes[ai];
                if (attr.type==cgltf_attribute_type_position)  pos_acc=attr.data;
                if (attr.type==cgltf_attribute_type_texcoord && attr.index==0) uv_acc=attr.data;
                if (attr.type==cgltf_attribute_type_normal)    norm_acc=attr.data;
            }
            if (!pos_acc) continue;

            // All attributes share the same interleaved buffer view
            cgltf_buffer_view* bv = pos_acc->buffer_view;
            const uint8_t* bv_data = (const uint8_t*)bv->buffer->data + bv->offset;

            GLuint vbo; glGenBuffers(1,&vbo);
            glBindBuffer(GL_ARRAY_BUFFER,vbo);
            glBufferData(GL_ARRAY_BUFFER, bv->size, bv_data, GL_STATIC_DRAW);

            // Material → is it the screen?
            bool is_screen = false;
            GLuint base_tex = 0;
            if (prim->material && prim->material->name) {
                is_screen = (strcmp(prim->material->name,"TVScreen")==0);
                if (!is_screen) {
                    auto& pbr = prim->material->pbr_metallic_roughness;
                    if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
                        const char* uri = pbr.base_color_texture.texture->image->uri;
                        char path[256];
                        snprintf(path,sizeof(path),"/tv/%s",uri);
                        base_tex = load_tex(path);
                    }
                }
            }

            if (is_screen && pos_acc) {
                float mn_x=1e9f,mx_x=-1e9f,mn_y=1e9f,mx_y=-1e9f;
                for (size_t vi=0; vi<pos_acc->count; vi++) {
                    float p[3]; cgltf_accessor_read_float(pos_acc,vi,p,3);
                    if(p[0]<mn_x)mn_x=p[0]; if(p[0]>mx_x)mx_x=p[0];
                    if(p[1]<mn_y)mn_y=p[1]; if(p[1]>mx_y)mx_y=p[1];
                }
                g_tv_half_x = (mx_x-mn_x)*0.5f;
                g_tv_half_y = (mx_y-mn_y)*0.5f;
                printf("Screen local half-extents: %.3f x %.3f\n", g_tv_half_x, g_tv_half_y);
            }

            TvPrim tp;
            tp.vbo          = vbo;
            tp.vcount       = (int)pos_acc->count;
            tp.base_tex     = base_tex;
            tp.is_screen    = is_screen;
            tp.double_sided = false;
            tp.is_room      = false;
            memcpy(tp.world, world, sizeof(M4));
            g_prims.push_back(tp);
        }
    }
    printf("Loaded %zu TV primitives\n", g_prims.size());
    cgltf_free(gltf);
}

static void load_room() {
    cgltf_options opts = {};
    cgltf_data* gltf = nullptr;
    cgltf_result r = cgltf_parse_file(&opts, "/tv/crt_room_full.gltf", &gltf);
    if (r != cgltf_result_success) { printf("crt_room_full.gltf parse failed: %d\n",r); return; }
    r = cgltf_load_buffers(&opts, gltf, "/tv/crt_room_full.gltf");
    if (r != cgltf_result_success) { printf("room.gltf load_buffers failed: %d\n",r); return; }

    // Cache textures by image index — many primitives share the same images
    std::vector<GLuint> tex_cache(gltf->images_count, 0);
    auto get_tex = [&](cgltf_image* img) -> GLuint {
        int idx = (int)(img - gltf->images);
        if (tex_cache[idx]) return tex_cache[idx];
        GLuint t = img->buffer_view ? load_tex_bv(img->buffer_view) : 0;
        if (!t && img->uri) {
            char path[256]; snprintf(path,sizeof(path),"/tv/%s",img->uri);
            t = load_tex(path);
        }
        tex_cache[idx] = t;
        return t;
    };

    for (cgltf_size ni = 0; ni < gltf->nodes_count; ni++) {
        cgltf_node* node = &gltf->nodes[ni];
        if (!node->mesh) continue;

        M4 world;
        cgltf_node_transform_world(node, world);
        // SketchUp export is upside-down relative to our scene — same Y-flip as TV
        world[1]=-world[1]; world[5]=-world[5]; world[9]=-world[9]; world[13]=-world[13];

        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
            cgltf_primitive* prim = &node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles || !prim->indices) continue;

            cgltf_accessor *pos_acc=nullptr, *uv_acc=nullptr, *norm_acc=nullptr;
            for (cgltf_size ai=0; ai<prim->attributes_count; ai++) {
                auto& a = prim->attributes[ai];
                if (a.type==cgltf_attribute_type_position) pos_acc=a.data;
                if (a.type==cgltf_attribute_type_texcoord && a.index==0) uv_acc=a.data;
                if (a.type==cgltf_attribute_type_normal)   norm_acc=a.data;
            }
            if (!pos_acc) continue;

            // Expand indexed geometry into interleaved flat list (pos+uv+norm = 8 floats)
            size_t idx_count = prim->indices->count;
            std::vector<float> vdata;
            vdata.reserve(idx_count * 8);
            for (size_t i = 0; i < idx_count; i++) {
                unsigned int idx = 0;
                cgltf_accessor_read_uint(prim->indices, i, &idx, 1);
                float p[3]={0,0,0}, u[2]={0,0}, n[3]={0,1,0};
                cgltf_accessor_read_float(pos_acc, idx, p, 3);
                if (uv_acc)   cgltf_accessor_read_float(uv_acc,   idx, u, 2);
                if (norm_acc) cgltf_accessor_read_float(norm_acc, idx, n, 3);
                vdata.insert(vdata.end(), {p[0],p[1],p[2], u[0],u[1], n[0],n[1],n[2]});
            }

            GLuint vbo; glGenBuffers(1,&vbo);
            glBindBuffer(GL_ARRAY_BUFFER,vbo);
            glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vdata.size()*4),vdata.data(),GL_STATIC_DRAW);

            // Resolve texture from material
            GLuint base_tex = 0;
            if (prim->material) {
                auto& pbr = prim->material->pbr_metallic_roughness;
                if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
                    base_tex = get_tex(pbr.base_color_texture.texture->image);
                } else {
                    // Factor-only material: bake into a 1×1 colour texture
                    float* cf = pbr.base_color_factor;
                    uint8_t col[4]={(uint8_t)(cf[0]*255),(uint8_t)(cf[1]*255),
                                    (uint8_t)(cf[2]*255),255};
                    glGenTextures(1,&base_tex);
                    glBindTexture(GL_TEXTURE_2D,base_tex);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,col);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                }
            }

            TvPrim tp;
            tp.vbo         = vbo;
            tp.vcount      = (int)idx_count;
            tp.base_tex    = base_tex;
            tp.is_screen   = false;
            tp.double_sided = prim->material && prim->material->double_sided;
            tp.is_room     = true;
            memcpy(tp.world, world, sizeof(M4));
            g_prims.push_back(tp);
        }
    }
    // Print room accessor bounds to help align with TV coordinate space
    if (gltf->accessors_count > 0) {
        for (cgltf_size i = 0; i < gltf->accessors_count; i++) {
            cgltf_accessor* a = &gltf->accessors[i];
            if (a->type == cgltf_type_vec3 && a->has_min && a->has_max) {
                printf("Room VEC3 bounds: min(%.2f %.2f %.2f) max(%.2f %.2f %.2f)\n",
                    a->min[0],a->min[1],a->min[2], a->max[0],a->max[1],a->max[2]);
                break;
            }
        }
    }
    printf("Room loaded, total prims: %zu\n", g_prims.size());
    cgltf_free(gltf);
}

static void load_avatar(AvatarModel* dest, const char* gltf_path) {
    // ── Free previous data ────────────────────────────────────────────────────
    if (dest->vbo) { glDeleteBuffers(1, &dest->vbo); dest->vbo = 0; }
    for (auto& m : dest->meshes) if (m.tex) glDeleteTextures(1, &m.tex);
    dest->meshes.clear();
    dest->anims.clear();
    dest->loaded = false;

    cgltf_options opts = {};
    cgltf_data* gltf = nullptr;
    cgltf_result r = cgltf_parse_file(&opts, gltf_path, &gltf);
    if (r != cgltf_result_success) { printf("avatar: cgltf_parse_file failed %d\n", r); return; }
    r = cgltf_load_buffers(&opts, gltf, gltf_path);
    if (r != cgltf_result_success) { printf("avatar: cgltf_load_buffers failed %d\n", r); cgltf_free(gltf); return; }

    dest->node_count = (int)gltf->nodes_count;

    // ── Node hierarchy + default TRS ─────────────────────────────────────────
    for (int ni = 0; ni < dest->node_count; ni++) dest->node_parent[ni] = -1;
    for (int ni = 0; ni < dest->node_count; ni++) {
        cgltf_node* n = &gltf->nodes[ni];
        for (cgltf_size ci = 0; ci < n->children_count; ci++)
            dest->node_parent[(int)(n->children[ci]-gltf->nodes)] = ni;
    }
    for (int ni = 0; ni < dest->node_count; ni++) {
        cgltf_node* n = &gltf->nodes[ni];
        if (n->has_translation) memcpy(dest->node_def_t[ni], n->translation, 12);
        else { dest->node_def_t[ni][0]=0; dest->node_def_t[ni][1]=0; dest->node_def_t[ni][2]=0; }
        if (n->has_rotation) memcpy(dest->node_def_r[ni], n->rotation, 16);
        else { dest->node_def_r[ni][0]=0; dest->node_def_r[ni][1]=0; dest->node_def_r[ni][2]=0; dest->node_def_r[ni][3]=1; }
        if (n->has_scale) memcpy(dest->node_def_s[ni], n->scale, 12);
        else { dest->node_def_s[ni][0]=1; dest->node_def_s[ni][1]=1; dest->node_def_s[ni][2]=1; }
    }

    // ── Skin ─────────────────────────────────────────────────────────────────
    cgltf_skin* skin = &gltf->skins[0];
    dest->joint_count = (int)skin->joints_count;
    if (dest->joint_count > CAT_JOINTS) dest->joint_count = CAT_JOINTS;
    for (int j = 0; j < dest->joint_count; j++)
        dest->joint_nodes[j] = (int)(skin->joints[j] - gltf->nodes);
    if (skin->inverse_bind_matrices) {
        for (int j = 0; j < dest->joint_count; j++)
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, dest->inv_bind[j], 16);
    } else {
        for (int j = 0; j < dest->joint_count; j++) { m4_identity(dest->inv_bind[j]); }
    }

    // ── Mesh VBO (all meshes concatenated) + per-mesh textures ───────────────
    char base_dir[256] = {};
    const char* last_slash = strrchr(gltf_path, '/');
    if (last_slash) {
        int len = (int)(last_slash - gltf_path) + 1;
        if (len < 256) { memcpy(base_dir, gltf_path, len); }
    }

    std::vector<float> vdata;
    for (cgltf_size mi = 0; mi < gltf->meshes_count; mi++) {
        cgltf_primitive* prim = &gltf->meshes[mi].primitives[0];
        cgltf_accessor *pos_acc=nullptr,*uv_acc=nullptr,*norm_acc=nullptr,*joints_acc=nullptr,*weights_acc=nullptr;
        for (cgltf_size ai = 0; ai < prim->attributes_count; ai++) {
            cgltf_attribute& at = prim->attributes[ai];
            if (at.type==cgltf_attribute_type_position)                        pos_acc=at.data;
            if (at.type==cgltf_attribute_type_texcoord && at.index==0)         uv_acc=at.data;
            if (at.type==cgltf_attribute_type_normal)                          norm_acc=at.data;
            if (at.type==cgltf_attribute_type_joints  && at.index==0)         joints_acc=at.data;
            if (at.type==cgltf_attribute_type_weights && at.index==0)         weights_acc=at.data;
        }
        int icount = (int)prim->indices->count;
        int vstart = (int)(vdata.size() / 16);
        vdata.resize(vdata.size() + icount * 16);
        for (int ii = 0; ii < icount; ii++) {
            unsigned idx = 0; cgltf_accessor_read_uint(prim->indices, ii, &idx, 1);
            float* dst = &vdata[(vstart + ii)*16];
            if (pos_acc)     cgltf_accessor_read_float(pos_acc,    idx, dst+0,  3); else {dst[0]=dst[1]=dst[2]=0;}
            if (uv_acc)      cgltf_accessor_read_float(uv_acc,     idx, dst+3,  2); else {dst[3]=dst[4]=0;}
            if (norm_acc)    cgltf_accessor_read_float(norm_acc,   idx, dst+5,  3); else {dst[5]=0;dst[6]=1;dst[7]=0;}
            if (joints_acc)  { unsigned ji[4]={0,0,0,0}; cgltf_accessor_read_uint(joints_acc, idx, ji, 4);
                               dst[8]=(float)ji[0];dst[9]=(float)ji[1];dst[10]=(float)ji[2];dst[11]=(float)ji[3]; }
            if (weights_acc) cgltf_accessor_read_float(weights_acc, idx, dst+12, 4);
        }
        GLuint tex = 0;
        if (prim->material) {
            cgltf_texture* ct = prim->material->pbr_metallic_roughness.base_color_texture.texture;
            if (ct && ct->image && ct->image->uri) {
                char tex_path[512];
                snprintf(tex_path, sizeof(tex_path), "%s%s", base_dir, ct->image->uri);
                tex = load_tex(tex_path);
            }
        }
        AvatarMesh am; am.start = vstart; am.count = icount; am.tex = tex;
        dest->meshes.push_back(am);
    }
    glGenBuffers(1, &dest->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, dest->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vdata.size()*4), vdata.data(), GL_STATIC_DRAW);

    // ── Animations ───────────────────────────────────────────────────────────
    dest->idle_anim = 0; dest->walk_anim = 0;
    for (cgltf_size ai = 0; ai < gltf->animations_count; ai++) {
        cgltf_animation* ca = &gltf->animations[ai];
        const char* name = ca->name ? ca->name : "";
        if (strstr(name, "idle")) dest->idle_anim = (int)ai;
        if (strstr(name, "walk")) dest->walk_anim = (int)ai;
        CatAnim anim; anim.duration = 0.f;
        for (cgltf_size si = 0; si < ca->samplers_count; si++) {
            float last = 0.f;
            cgltf_accessor_read_float(ca->samplers[si].input, ca->samplers[si].input->count-1, &last, 1);
            if (last > anim.duration) anim.duration = last;
        }
        for (cgltf_size ci = 0; ci < ca->channels_count; ci++) {
            cgltf_animation_channel* ch = &ca->channels[ci];
            int path = -1;
            if (ch->target_path == cgltf_animation_path_type_translation) path=0;
            else if (ch->target_path == cgltf_animation_path_type_rotation)    path=1;
            else if (ch->target_path == cgltf_animation_path_type_scale)       path=2;
            if (path < 0) continue;
            CatChannel chan;
            chan.node = (int)(ch->target_node - gltf->nodes);
            chan.path = path;
            cgltf_accessor* inp = ch->sampler->input;
            chan.times.resize(inp->count);
            for (cgltf_size k=0;k<inp->count;k++) cgltf_accessor_read_float(inp,k,&chan.times[k],1);
            cgltf_accessor* out = ch->sampler->output;
            int comp = (path==1)?4:3;
            chan.values.resize(out->count*comp);
            for (cgltf_size k=0;k<out->count;k++) cgltf_accessor_read_float(out,k,&chan.values[k*comp],comp);
            anim.channels.push_back(std::move(chan));
        }
        dest->anims.push_back(std::move(anim));
    }
    printf("avatar: %d joints, %zu meshes, %zu anims, idle=%d walk=%d\n",
           dest->joint_count, dest->meshes.size(), dest->anims.size(), dest->idle_anim, dest->walk_anim);

    // ── Shader (compile only once) ────────────────────────────────────────────
    if (!g_skin_prog) {
        GLuint vs=make_shader(GL_VERTEX_SHADER,SKIN_VS);
        GLuint fs=make_shader(GL_FRAGMENT_SHADER,SKIN_FS);
        g_skin_prog=glCreateProgram();
        glAttachShader(g_skin_prog,vs); glAttachShader(g_skin_prog,fs);
        glLinkProgram(g_skin_prog);
        { GLint ok; glGetProgramiv(g_skin_prog,GL_LINK_STATUS,&ok);
          if(!ok){char buf[512];glGetProgramInfoLog(g_skin_prog,512,nullptr,buf);printf("skin link error: %s\n",buf);} }
        glDeleteShader(vs); glDeleteShader(fs);
        g_skin_u_vp    =glGetUniformLocation(g_skin_prog,"u_vp");
        g_skin_u_world =glGetUniformLocation(g_skin_prog,"u_world");
        g_skin_u_bones =glGetUniformLocation(g_skin_prog,"u_bones");
        g_skin_u_tex        =glGetUniformLocation(g_skin_prog,"u_tex");
        g_skin_u_tv_quad_pos=glGetUniformLocation(g_skin_prog,"u_tv_quad_pos");
        g_skin_u_tv_quad_col=glGetUniformLocation(g_skin_prog,"u_tv_quad_col");
        g_skin_u_tv_normal  =glGetUniformLocation(g_skin_prog,"u_tv_normal");
        g_skin_u_cone_power =glGetUniformLocation(g_skin_prog,"u_cone_power");
        g_skin_u_lamp_pos=glGetUniformLocation(g_skin_prog,"u_lamp_pos");
        g_skin_u_lamp_intensity=glGetUniformLocation(g_skin_prog,"u_lamp_intensity");
        g_skin_a_pos    =glGetAttribLocation(g_skin_prog,"a_pos");
        g_skin_a_uv     =glGetAttribLocation(g_skin_prog,"a_uv");
        g_skin_a_norm   =glGetAttribLocation(g_skin_prog,"a_norm");
        g_skin_a_joints =glGetAttribLocation(g_skin_prog,"a_joints");
        g_skin_a_weights=glGetAttribLocation(g_skin_prog,"a_weights");
    }

    cgltf_free(gltf);
    dest->loaded = true;
}

static void load_cat()        { load_avatar(&g_models[0], "/tv/cat/scene.gltf"); }
static void load_incidental() { load_avatar(&g_models[1], "/tv/incidental_70/scene.gltf"); }

// ============================================================
//  GL init
// ============================================================
static void init_crt() {
    GLuint vs = make_shader(GL_VERTEX_SHADER,   CRT_VS);
    GLuint fs = make_shader(GL_FRAGMENT_SHADER, CRT_FS);
    g_crt_prog = glCreateProgram();
    glAttachShader(g_crt_prog, vs); glAttachShader(g_crt_prog, fs);
    glLinkProgram(g_crt_prog);
    glDeleteShader(vs); glDeleteShader(fs);

    g_crt_a_vert  = glGetAttribLocation (g_crt_prog, "VertexCoord");
    g_crt_a_uv    = glGetAttribLocation (g_crt_prog, "TexCoord");
    g_crt_u_mvp   = glGetUniformLocation(g_crt_prog, "MVPMatrix");
    g_crt_u_fcount= glGetUniformLocation(g_crt_prog, "FrameCount");
    g_crt_u_out   = glGetUniformLocation(g_crt_prog, "OutputSize");
    g_crt_u_texsz = glGetUniformLocation(g_crt_prog, "TextureSize");
    g_crt_u_insz  = glGetUniformLocation(g_crt_prog, "InputSize");
    g_crt_u_tex   = glGetUniformLocation(g_crt_prog, "Texture");

    // Fullscreen quad: VertexCoord(vec4) + TexCoord(vec4), 6 verts (2 triangles)
    static const float quad[] = {
        -1,-1,0,1,  0,0,0,0,
         1,-1,0,1,  1,0,0,0,
        -1, 1,0,1,  0,1,0,0,
         1,-1,0,1,  1,0,0,0,
         1, 1,0,1,  1,1,0,0,
        -1, 1,0,1,  0,1,0,0,
    };
    glGenBuffers(1, &g_crt_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_crt_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // FBO texture
    glGenTextures(1, &g_crt_tex);
    glBindTexture(GL_TEXTURE_2D, g_crt_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CRT_W, CRT_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &g_crt_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_crt_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_crt_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        printf("CRT FBO incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void render_crt_pass() {
    glBindFramebuffer(GL_FRAMEBUFFER, g_crt_fbo);
    glViewport(0, 0, CRT_W, CRT_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClear(GL_COLOR_BUFFER_BIT);

    M4 identity; m4_identity(identity);
    glUseProgram(g_crt_prog);
    glUniformMatrix4fv(g_crt_u_mvp,    1, GL_FALSE, identity);
    glUniform1i(g_crt_u_fcount, g_frame_count++);
    glUniform2f(g_crt_u_out,   (float)CRT_W, (float)CRT_H);
    glUniform2f(g_crt_u_texsz, (float)g_frame_w, (float)g_frame_h);
    glUniform2f(g_crt_u_insz,  (float)g_frame_w, (float)g_frame_h);
    glUniform1i(g_crt_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_game_tex);

    glBindBuffer(GL_ARRAY_BUFFER, g_crt_vbo);
    int stride = 8 * sizeof(float);
    glEnableVertexAttribArray(g_crt_a_vert);
    glVertexAttribPointer(g_crt_a_vert, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(g_crt_a_uv);
    glVertexAttribPointer(g_crt_a_uv,   4, GL_FLOAT, GL_FALSE, stride, (void*)(4*sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(g_crt_a_vert);
    glDisableVertexAttribArray(g_crt_a_uv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

static void gl_init() {
    GLuint vs=make_shader(GL_VERTEX_SHADER,VS);
    GLuint fs=make_shader(GL_FRAGMENT_SHADER,FS);
    g_prog=glCreateProgram();
    glAttachShader(g_prog,vs); glAttachShader(g_prog,fs);
    glLinkProgram(g_prog);
    glDeleteShader(vs); glDeleteShader(fs);

    g_a_pos   =glGetAttribLocation (g_prog,"a_pos");
    g_a_uv    =glGetAttribLocation (g_prog,"a_uv");
    g_a_norm  =glGetAttribLocation (g_prog,"a_norm");
    g_u_mvp   =glGetUniformLocation(g_prog,"u_mvp");
    g_u_model =glGetUniformLocation(g_prog,"u_model");
    g_u_tex   =glGetUniformLocation(g_prog,"u_tex");
    g_u_screen      =glGetUniformLocation(g_prog,"u_screen");
    g_u_overscan    =glGetUniformLocation(g_prog,"u_overscan");
    g_u_tv_quad_pos =glGetUniformLocation(g_prog,"u_tv_quad_pos");
    g_u_tv_quad_col =glGetUniformLocation(g_prog,"u_tv_quad_col");
    g_u_tv_normal   =glGetUniformLocation(g_prog,"u_tv_normal");
    g_u_cone_power  =glGetUniformLocation(g_prog,"u_cone_power");
    g_u_lamp_pos      =glGetUniformLocation(g_prog,"u_lamp_pos");
    g_u_lamp_intensity=glGetUniformLocation(g_prog,"u_lamp_intensity");

    // Game texture (1×1 dark placeholder until first frame arrives from worker)
    glGenTextures(1,&g_game_tex);
    glBindTexture(GL_TEXTURE_2D,g_game_tex);
    uint8_t dark[4]={10,10,10,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,dark);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);  // Y-flip correction reverses winding order

    load_tv();
    for (const auto& p : g_prims)
        if (p.is_screen) {
            g_tv_screen_pos[0] = p.world[12];
            g_tv_screen_pos[1] = p.world[13];
            g_tv_screen_pos[2] = p.world[14];
            printf("TV screen world pos: %.2f %.2f %.2f\n", p.world[12], p.world[13], p.world[14]);
            // World-space half-extent vectors (col 0 = local X, col 1 = local Y)
            float rx=p.world[0]*g_tv_half_x, ry=p.world[1]*g_tv_half_x, rz=p.world[2]*g_tv_half_x;
            float ux=p.world[4]*g_tv_half_y, uy=p.world[5]*g_tv_half_y, uz=p.world[6]*g_tv_half_y;
            // quadrant centres: q0=(-r,-u), q1=(+r,-u), q2=(-r,+u), q3=(+r,+u)
            const float sgn[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}};
            for (int q=0;q<4;q++) {
                g_tv_quad_pos[q][0] = g_tv_screen_pos[0] + sgn[q][0]*rx*0.5f + sgn[q][1]*ux*0.5f;
                g_tv_quad_pos[q][1] = g_tv_screen_pos[1] + sgn[q][0]*ry*0.5f + sgn[q][1]*uy*0.5f;
                g_tv_quad_pos[q][2] = g_tv_screen_pos[2] + sgn[q][0]*rz*0.5f + sgn[q][1]*uz*0.5f;
            }
            // Push lights out toward the viewer (player start position).
            float nx = 0.f         - g_tv_screen_pos[0];
            float ny = 20.f        - g_tv_screen_pos[1]; // player eye-level
            float nz = -80.f       - g_tv_screen_pos[2]; // player start Z
            float nl = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nl > 0.f) { nx/=nl; ny/=nl; nz/=nl; }
            g_tv_screen_normal[0]=nx; g_tv_screen_normal[1]=ny; g_tv_screen_normal[2]=nz;
            const float push = 15.f;
            for (int q=0;q<4;q++) {
                g_tv_quad_pos[q][0] += nx*push;
                g_tv_quad_pos[q][1] += ny*push;
                g_tv_quad_pos[q][2] += nz*push;
            }
            printf("Screen outward normal: %.3f %.3f %.3f\n", nx, ny, nz);
        }
    load_room();
    init_crt();

    // 1×1 white fallback texture (used for room prims with no material/texture)
    {
        glGenTextures(1, &g_white_tex);
        glBindTexture(GL_TEXTURE_2D, g_white_tex);
        uint8_t white[4]={255,255,255,255};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    load_cat();
    load_incidental();
}

// ============================================================
//  Render
// ============================================================
static void update_anim(AvatarModel* mdl, int anim_idx, float anim_time) {
    if (mdl->anims.empty() || anim_idx < 0 || anim_idx >= (int)mdl->anims.size()) return;
    CatAnim& anim = mdl->anims[anim_idx];

    // Reset to default pose
    for (int ni=0;ni<mdl->node_count;ni++){
        memcpy(g_cat_node_t[ni],mdl->node_def_t[ni],12);
        memcpy(g_cat_node_r[ni],mdl->node_def_r[ni],16);
        memcpy(g_cat_node_s[ni],mdl->node_def_s[ni],12);
    }

    // Sample channels
    for (const CatChannel& ch : anim.channels) {
        int n=ch.node; if(n<0||n>=mdl->node_count) continue;
        int kc=(int)ch.times.size(); if(kc==0) continue;
        int lo=kc-1;
        for(int k=0;k<kc;k++){ if(ch.times[k]>anim_time){lo=k-1;break;} }
        if(lo<0)lo=0;
        int hi=(lo+1<kc)?lo+1:lo;
        float t=0.f;
        if(hi!=lo) t=(anim_time-ch.times[lo])/(ch.times[hi]-ch.times[lo]);
        if(t<0.f)t=0.f; if(t>1.f)t=1.f;
        if(ch.path==0){
            const float*a=&ch.values[lo*3],*b=&ch.values[hi*3];
            g_cat_node_t[n][0]=a[0]+(b[0]-a[0])*t;
            g_cat_node_t[n][1]=a[1]+(b[1]-a[1])*t;
            g_cat_node_t[n][2]=a[2]+(b[2]-a[2])*t;
        } else if(ch.path==1){
            q_slerp(g_cat_node_r[n],&ch.values[lo*4],&ch.values[hi*4],t);
        } else {
            const float*a=&ch.values[lo*3],*b=&ch.values[hi*3];
            g_cat_node_s[n][0]=a[0]+(b[0]-a[0])*t;
            g_cat_node_s[n][1]=a[1]+(b[1]-a[1])*t;
            g_cat_node_s[n][2]=a[2]+(b[2]-a[2])*t;
        }
    }

    // Compute global transforms
    bool done[CAT_NODES]={};
    int left=mdl->node_count;
    while(left>0){
        int prev=left;
        for(int ni=0;ni<mdl->node_count;ni++){
            if(done[ni]) continue;
            int p=mdl->node_parent[ni];
            if(p>=0&&!done[p]) continue;
            M4 local; m4_from_trs(local,g_cat_node_t[ni],g_cat_node_r[ni],g_cat_node_s[ni]);
            if(p<0) memcpy(g_cat_node_global[ni],local,64);
            else    m4_mul(g_cat_node_global[ni],g_cat_node_global[p],local);
            done[ni]=true; left--;
        }
        if(left==prev) break;
    }

    // Compute bone matrices = global[joint] * inv_bind[j]
    for(int j=0;j<mdl->joint_count;j++){
        M4 bone; m4_mul(bone, g_cat_node_global[mdl->joint_nodes[j]], mdl->inv_bind[j]);
        memcpy(g_cat_bone_mats[j],bone,64);
    }
}

static void update_player() {
    const float speed = 0.8f;
    float fw_x = sinf(g_local.yaw);
    float fw_z = cosf(g_local.yaw);
    float rt_x = cosf(g_local.yaw);
    float rt_z = -sinf(g_local.yaw);

    if (g_move[0]) { g_local.x += fw_x*speed; g_local.z += fw_z*speed; } // W
    if (g_move[1]) { g_local.x -= fw_x*speed; g_local.z -= fw_z*speed; } // S
    if (g_move[2]) { g_local.x -= rt_x*speed; g_local.z -= rt_z*speed; } // A
    if (g_move[3]) { g_local.x += rt_x*speed; g_local.z += rt_z*speed; } // D
}

static void render() {
    int w,h;
    emscripten_get_canvas_element_size("#canvas",&w,&h);
    glViewport(0,0,w,h);
    glClearColor(0.35f,0.35f,0.35f,1.f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    // First-person camera from local player
    // g_local.y is feet/floor position; g_cat_eye_height lifts camera to head level
    float eye_y = g_local.y + g_cat_eye_height;
    float tx = g_local.x + cosf(g_local.pitch) * sinf(g_local.yaw);
    float ty = eye_y + sinf(g_local.pitch);
    float tz = g_local.z + cosf(g_local.pitch) * cosf(g_local.yaw);

    M4 proj, view, vp;
    m4_persp(proj, 1.0f, (float)w/h, 0.5f, 1000.f);
    m4_lookat(view, g_local.x, eye_y, g_local.z, tx, ty, tz);
    m4_mul(vp, proj, view);

    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    glUniform2f(g_u_overscan, g_overscan_x, g_overscan_y);
    float scaled_col[4][3];
    for (int q=0;q<4;q++) for (int k=0;k<3;k++)
        scaled_col[q][k] = g_tv_quad_col[q][k] * g_tv_light_intensity;
    // Rotate base screen normal by yaw (Y-axis) and pitch (X-axis)
    float yaw_r   = g_cone_yaw   * 3.14159265f / 180.f;
    float pitch_r = g_cone_pitch * 3.14159265f / 180.f;
    float cy=cosf(yaw_r), sy=sinf(yaw_r);
    float bn0=g_tv_screen_normal[0], bn1=g_tv_screen_normal[1], bn2=g_tv_screen_normal[2];
    float n0= cy*bn0 + sy*bn2, n1=bn1, n2=-sy*bn0 + cy*bn2; // yaw
    float cp=cosf(pitch_r), sp=sinf(pitch_r);
    float cone_dir[3] = { n0, cp*n1 - sp*n2, sp*n1 + cp*n2 }; // pitch
    glUniform3fv(g_u_tv_quad_pos, 4, &g_tv_quad_pos[0][0]);
    glUniform3fv(g_u_tv_quad_col, 4, &scaled_col[0][0]);
    glUniform3fv(g_u_tv_normal,   1, cone_dir);
    glUniform1f (g_u_cone_power,  g_cone_power);
    glUniform3fv(g_u_lamp_pos,       1, g_lamp_pos);
    glUniform1f (g_u_lamp_intensity, g_lamp_intensity);

    // Pre-build room parent matrix from live tuning params (scale + Y-rot + translate)
    M4 g_room_parent;
    {
        float s = g_room_scale;
        float c = cosf(g_room_rot_y), sr = sinf(g_room_rot_y);
        g_room_parent[0] = s*c;  g_room_parent[1] = 0.f; g_room_parent[2] =-s*sr; g_room_parent[3] = 0.f;
        g_room_parent[4] = 0.f;  g_room_parent[5] = s;   g_room_parent[6] = 0.f;  g_room_parent[7] = 0.f;
        g_room_parent[8] = s*sr; g_room_parent[9] = 0.f; g_room_parent[10]= s*c;  g_room_parent[11]= 0.f;
        g_room_parent[12]= g_room_tx; g_room_parent[13]= g_room_ty;
        g_room_parent[14]= g_room_tz; g_room_parent[15]= 1.f;
    }

    for (auto& p : g_prims) {
        M4 world, mvp;
        if (p.is_room) { m4_mul(world, g_room_parent, p.world); }
        else           { memcpy(world, p.world, sizeof(M4)); }
        m4_mul(mvp, vp, world);
        glUniformMatrix4fv(g_u_mvp,   1, GL_FALSE, mvp);
        glUniformMatrix4fv(g_u_model, 1, GL_FALSE, world);
        glUniform1f(g_u_screen, p.is_screen ? 1.f : 0.f);

        glActiveTexture(GL_TEXTURE0);
        GLuint tex = p.is_screen ? g_crt_tex : (p.base_tex ? p.base_tex : g_white_tex);
        glBindTexture(GL_TEXTURE_2D, tex);

        glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
        // Interleaved layout: pos(12) + uv(8) + normal(12) = stride 32
        glEnableVertexAttribArray(g_a_pos);
        glVertexAttribPointer(g_a_pos,  3,GL_FLOAT,GL_FALSE,32,(void*)0);
        glEnableVertexAttribArray(g_a_uv);
        glVertexAttribPointer(g_a_uv,   2,GL_FLOAT,GL_FALSE,32,(void*)12);
        if (g_a_norm>=0) {
            glEnableVertexAttribArray(g_a_norm);
            glVertexAttribPointer(g_a_norm,3,GL_FLOAT,GL_FALSE,32,(void*)20);
        }

        if (p.double_sided) glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLES, 0, p.vcount);
        if (p.double_sided) glEnable(GL_CULL_FACE);
    }

    // ── Remote player avatar models ──────────────────────────────────────────
    if (g_skin_prog) {
        glUseProgram(g_skin_prog);
        glUniform1i(g_skin_u_tex, 0);
        glActiveTexture(GL_TEXTURE0);
        glUniform3fv(g_skin_u_tv_quad_pos, 4, &g_tv_quad_pos[0][0]);
        glUniform3fv(g_skin_u_tv_quad_col, 4, &scaled_col[0][0]);
        glUniform3fv(g_skin_u_tv_normal,   1, cone_dir);
        glUniform1f (g_skin_u_cone_power,  g_cone_power);
        glUniform3fv(g_skin_u_lamp_pos, 1, g_lamp_pos);
        glUniform1f (g_skin_u_lamp_intensity, g_lamp_intensity);

        glDisable(GL_CULL_FACE);

        if(g_skin_a_pos>=0)    glEnableVertexAttribArray(g_skin_a_pos);
        if(g_skin_a_uv>=0)     glEnableVertexAttribArray(g_skin_a_uv);
        if(g_skin_a_norm>=0)   glEnableVertexAttribArray(g_skin_a_norm);
        if(g_skin_a_joints>=0) glEnableVertexAttribArray(g_skin_a_joints);
        if(g_skin_a_weights>=0)glEnableVertexAttribArray(g_skin_a_weights);

        for (int i=0; i<8; i++) {
            if (!g_remote[i].active) continue;
            AvatarModel* mdl = &g_models[g_remote[i].model_idx];
            if (!mdl->loaded || !mdl->vbo || mdl->meshes.empty()) continue;

            // Advance animation, switching between idle and walk
            int target = g_remote[i].moving ? mdl->walk_anim : mdl->idle_anim;
            if (g_remote[i].anim_idx != target) {
                g_remote[i].anim_idx  = target;
                g_remote[i].anim_time = 0.f;
            }
            if (g_remote[i].anim_idx < (int)mdl->anims.size()) {
                float dur = mdl->anims[g_remote[i].anim_idx].duration;
                if (dur > 0.f) g_remote[i].anim_time = fmodf(g_remote[i].anim_time + 1.f/60.f, dur);
            }
            update_anim(mdl, g_remote[i].anim_idx, g_remote[i].anim_time);

            glUniformMatrix4fv(g_skin_u_vp,    1, GL_FALSE, vp);
            glUniformMatrix4fv(g_skin_u_bones, mdl->joint_count, GL_FALSE, &g_cat_bone_mats[0][0]);

            float c=cosf(g_remote[i].yaw), s=sinf(g_remote[i].yaw);
            float sc=0.2f;
            M4 world;
            world[0]=c*sc; world[1]=0.f;  world[2]=-s*sc; world[3]=0.f;
            world[4]=0.f;  world[5]=-sc;  world[6]=0.f;   world[7]=0.f;
            world[8]=s*sc; world[9]=0.f;  world[10]=c*sc; world[11]=0.f;
            world[12]=g_remote[i].x; world[13]=g_remote[i].y; world[14]=g_remote[i].z; world[15]=1.f;
            glUniformMatrix4fv(g_skin_u_world, 1, GL_FALSE, world);

            glBindBuffer(GL_ARRAY_BUFFER, mdl->vbo);
            if(g_skin_a_pos>=0)    glVertexAttribPointer(g_skin_a_pos,    3,GL_FLOAT,GL_FALSE,64,(void*)0);
            if(g_skin_a_uv>=0)     glVertexAttribPointer(g_skin_a_uv,     2,GL_FLOAT,GL_FALSE,64,(void*)12);
            if(g_skin_a_norm>=0)   glVertexAttribPointer(g_skin_a_norm,   3,GL_FLOAT,GL_FALSE,64,(void*)20);
            if(g_skin_a_joints>=0) glVertexAttribPointer(g_skin_a_joints, 4,GL_FLOAT,GL_FALSE,64,(void*)32);
            if(g_skin_a_weights>=0)glVertexAttribPointer(g_skin_a_weights,4,GL_FLOAT,GL_FALSE,64,(void*)48);

            for (const auto& am : mdl->meshes) {
                glBindTexture(GL_TEXTURE_2D, am.tex);
                glDrawArrays(GL_TRIANGLES, am.start, am.count);
            }
        }

        if(g_skin_a_pos>=0)    glDisableVertexAttribArray(g_skin_a_pos);
        if(g_skin_a_uv>=0)     glDisableVertexAttribArray(g_skin_a_uv);
        if(g_skin_a_norm>=0)   glDisableVertexAttribArray(g_skin_a_norm);
        if(g_skin_a_joints>=0) glDisableVertexAttribArray(g_skin_a_joints);
        if(g_skin_a_weights>=0)glDisableVertexAttribArray(g_skin_a_weights);

        glEnable(GL_CULL_FACE);
    }
}

#endif // RENDERER_ONLY

// ============================================================
//  Exported functions
// ============================================================

// ── RENDERER_ONLY exports ────────────────────────────────────
#ifdef RENDERER_ONLY

extern "C" EMSCRIPTEN_KEEPALIVE void set_move_key(int key, int pressed) {
    // key: 0=W 1=S 2=A 3=D
    if (key>=0&&key<4) g_move[key]=pressed;
}

extern "C" EMSCRIPTEN_KEEPALIVE void add_mouse_delta(float dx, float dy) {
    const float sens = 0.0025f;
    g_local.yaw   += dx * sens;
    g_local.pitch += dy * sens;
    if (g_local.pitch >  1.48f) g_local.pitch =  1.48f;
    if (g_local.pitch < -1.48f) g_local.pitch = -1.48f;
}

extern "C" EMSCRIPTEN_KEEPALIVE int get_game_tex_id() {
    return (int)g_game_tex;
}

extern "C" EMSCRIPTEN_KEEPALIVE void set_frame_size(int w, int h) {
    g_frame_w = (unsigned)w;
    g_frame_h = (unsigned)h;
}

// Upload a frame received from the core worker into the game texture.
// Replaces the glTexImage2D calls that were previously inside retro_video_refresh.
extern "C" EMSCRIPTEN_KEEPALIVE void upload_frame(const uint8_t* rgba, int w, int h) {
    g_frame_w = (unsigned)w;
    g_frame_h = (unsigned)h;
    glBindTexture(GL_TEXTURE_2D, g_game_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

extern "C" EMSCRIPTEN_KEEPALIVE void set_lamp_pos(float x, float y, float z) {
    g_lamp_pos[0]=x; g_lamp_pos[1]=y; g_lamp_pos[2]=z;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_lamp_intensity(float v) {
    g_lamp_intensity = v;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_tv_light_intensity(float v) {
    g_tv_light_intensity = v;
}
// Set all 4 quadrant colours at once — called by JS after receiving each frame from worker
extern "C" EMSCRIPTEN_KEEPALIVE void set_tv_quad_colors(
    float r0,float g0,float b0, float r1,float g1,float b1,
    float r2,float g2,float b2, float r3,float g3,float b3) {
    g_tv_quad_col[0][0]=r0; g_tv_quad_col[0][1]=g0; g_tv_quad_col[0][2]=b0;
    g_tv_quad_col[1][0]=r1; g_tv_quad_col[1][1]=g1; g_tv_quad_col[1][2]=b1;
    g_tv_quad_col[2][0]=r2; g_tv_quad_col[2][1]=g2; g_tv_quad_col[2][2]=b2;
    g_tv_quad_col[3][0]=r3; g_tv_quad_col[3][1]=g3; g_tv_quad_col[3][2]=b3;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_js_colors(int v) { g_js_colors = (v != 0); }
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_yaw(float v)   { g_cone_yaw   = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_pitch(float v) { g_cone_pitch = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_power(float v) { g_cone_power = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_overscan(float x, float y) {
    g_overscan_x = x; g_overscan_y = y;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_room_xform(float scale, float rot_y_deg,
                                                     float tx, float ty, float tz) {
    g_room_scale = scale;
    g_room_rot_y = rot_y_deg * 3.14159265f / 180.f;
    g_room_tx = tx; g_room_ty = ty; g_room_tz = tz;
}

// TV world position (screen prim translation) — for 3D audio panner placement
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_x() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[12];
    return 0.f;
}
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_y() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[13];
    return 0.f;
}
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_z() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[14];
    return 0.f;
}

extern "C" EMSCRIPTEN_KEEPALIVE float get_local_pitch() { return g_local.pitch; }

extern "C" EMSCRIPTEN_KEEPALIVE float get_local_x()   { return g_local.x; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_y()   { return g_local.y; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_z()   { return g_local.z; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_yaw() { return g_local.yaw; }

extern "C" EMSCRIPTEN_KEEPALIVE void set_remote_player(int id, float x, float y, float z, float yaw, int moving) {
    if (id < 0 || id >= 8) return;
    g_remote[id].x = x; g_remote[id].y = y; g_remote[id].z = z;
    g_remote[id].yaw = yaw; g_remote[id].active = true;
    g_remote[id].moving = (moving != 0);
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_cat_eye_height(float v) { g_cat_eye_height = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_local_y(float v) { g_local.y = v; }
extern "C" EMSCRIPTEN_KEEPALIVE int get_local_moving() {
    return (g_move[0] || g_move[1] || g_move[2] || g_move[3]) ? 1 : 0;
}
extern "C" EMSCRIPTEN_KEEPALIVE void remove_remote_player(int id) {
    if (id >= 0 && id < 8) g_remote[id].active = false;
}

extern "C" EMSCRIPTEN_KEEPALIVE void set_remote_player_model(int id, int model) {
    if (id < 0 || id >= 8) return;
    int mdl = (model == 1) ? 1 : 0;
    if (g_remote[id].model_idx != mdl) {
        g_remote[id].model_idx = mdl;
        g_remote[id].anim_idx  = g_models[mdl].idle_anim;
        g_remote[id].anim_time = 0.f;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void set_player_model(int /*model*/) {
    // No-op: model choice is now per remote player via set_remote_player_model
}

#endif // RENDERER_ONLY

// ── CORE_ONLY exports ────────────────────────────────────────
#ifdef CORE_ONLY

extern "C" EMSCRIPTEN_KEEPALIVE void set_button(int id, int pressed) {
    if (id>=0&&id<16) g_buttons[id]=pressed;
}

extern "C" EMSCRIPTEN_KEEPALIVE void start_game(const char* path) {
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

#endif // CORE_ONLY

// ============================================================
//  Main loop + init
// ============================================================
#ifdef RENDERER_ONLY
static void loop() {
    update_player();
    render_crt_pass();  // reads g_game_tex (uploaded by JS via receiveFrame/texImage2D)
    render();
}

int main() {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha=false; attr.depth=true; attr.majorVersion=2;  // WebGL2
    auto ctx=emscripten_webgl_create_context("#canvas",&attr);
    emscripten_webgl_make_context_current(ctx);

    gl_init();

    emscripten_set_main_loop(loop, 60, 1);
    return 0;
}
#endif // RENDERER_ONLY

#ifdef CORE_ONLY
int main() {
    retro_set_environment      (retro_environment);
    retro_set_video_refresh    (retro_video_refresh);
    retro_set_audio_sample     (retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll       (retro_input_poll);
    retro_set_input_state      (retro_input_state);
    retro_init();
    // Keep runtime alive so JS can call step_frame(), start_game(), etc. via ccall
    emscripten_exit_with_live_runtime();
    return 0;
}
#endif // CORE_ONLY
