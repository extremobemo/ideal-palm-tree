#define CGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <libretro.h>
#include <cgltf.h>
#include <stb_image.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>

// ============================================================
//  Matrix math  (column-major, matches GLSL)
// ============================================================
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

// ============================================================
//  GL state
// ============================================================
struct TvPrim {
    GLuint vbo;
    int    vcount;
    GLuint base_tex;  // 0 if none
    bool   is_screen; // use game texture
    M4     world;
};
static std::vector<TvPrim> g_prims;
static GLuint g_game_tex = 0;
static GLuint g_prog     = 0;
static int    g_a_pos=-1, g_a_uv=-1, g_a_norm=-1;
static int    g_u_mvp=-1, g_u_model=-1, g_u_tex=-1, g_u_screen=-1;

// ============================================================
//  Player  (structured for future multiplayer)
// ============================================================
struct Player {
    float x = 0.f, y = -40.f, z = -80.f; // world position (eye height)
    float yaw = 0.f, pitch = 0.f;         // look angles (radians)
};

static Player g_local;                 // the local player
// Future: std::vector<Player> g_remote_players;

static bool g_move[4] = {};  // W S A D

// ============================================================
//  CRT pass state
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

// ============================================================
//  Libretro state
// ============================================================
static retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static bool     g_running  = false;
static bool     g_buttons[16] = {};
static unsigned g_frame_w  = 160, g_frame_h = 144; // updated by retro_video_refresh

// ============================================================
//  Libretro callbacks
// ============================================================
static void retro_log_cb(retro_log_level, const char* fmt, ...) {
    va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a);
}

void retro_video_refresh(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    g_frame_w = w; g_frame_h = h;
    std::vector<uint8_t> buf(w*h*4);
    if (g_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (unsigned y=0;y<h;y++) {
            const uint8_t* s=(const uint8_t*)data+y*pitch;
            uint8_t* d=buf.data()+y*w*4;
            for (unsigned x=0;x<w;x++) {
                d[x*4+0]=s[x*4+2]; d[x*4+1]=s[x*4+1];
                d[x*4+2]=s[x*4+0]; d[x*4+3]=255;
            }
        }
    } else {
        for (unsigned y=0;y<h;y++) {
            const uint16_t* s=(const uint16_t*)((const uint8_t*)data+y*pitch);
            uint8_t* d=buf.data()+y*w*4;
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
    glBindTexture(GL_TEXTURE_2D, g_game_tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,buf.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
}

void   retro_audio_sample(int16_t,int16_t) {}
size_t retro_audio_sample_batch(const int16_t*,size_t f) { return f; }
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
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data="/"; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((retro_log_callback*)data)->log=retro_log_cb; return true;
        default: return false;
    }
}

// ============================================================
//  Shaders
// ============================================================
static const char* VS = R"(
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec3 a_norm;
uniform mat4 u_mvp;
uniform mat4 u_model;
varying vec2 v_uv;
varying float v_ndotl;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_uv;
    vec3 wn = normalize(vec3(u_model * vec4(a_norm, 0.0)));
    vec3 light = normalize(vec3(0.5, -1.0, 0.8));
    v_ndotl = max(dot(wn, light), 0.0);
}
)";

static const char* FS = R"(
precision mediump float;
varying vec2 v_uv;
varying float v_ndotl;
uniform sampler2D u_tex;
uniform float u_screen;  // 1.0 = screen (emissive), 0.0 = normal
void main() {
    vec4 col = texture2D(u_tex, v_uv);
    if (u_screen > 0.5) {
        gl_FragColor = texture2D(u_tex, vec2(1.0 - v_uv.x, v_uv.y));
    } else {
        float light = 0.35 + v_ndotl * 0.65;
        gl_FragColor = vec4(col.rgb * light, col.a);
    }
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
    glGenerateMipmap(GL_TEXTURE_2D);
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

            TvPrim tp;
            tp.vbo       = vbo;
            tp.vcount    = (int)pos_acc->count;
            tp.base_tex  = base_tex;
            tp.is_screen = is_screen;
            memcpy(tp.world, world, sizeof(M4));
            g_prims.push_back(tp);
        }
    }
    printf("Loaded %zu TV primitives\n", g_prims.size());
    cgltf_free(gltf);
}

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
    g_u_screen=glGetUniformLocation(g_prog,"u_screen");

    // Game texture (1×1 placeholder until first frame)
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
    init_crt();
}

// ============================================================
//  Render
// ============================================================
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
    float tx = g_local.x + cosf(g_local.pitch) * sinf(g_local.yaw);
    float ty = g_local.y + sinf(g_local.pitch);
    float tz = g_local.z + cosf(g_local.pitch) * cosf(g_local.yaw);

    M4 proj, view, vp;
    m4_persp(proj, 1.0f, (float)w/h, 0.5f, 1000.f);
    m4_lookat(view, g_local.x, g_local.y, g_local.z, tx, ty, tz);
    m4_mul(vp, proj, view);

    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);

    for (auto& p : g_prims) {
        M4 mvp;
        m4_mul(mvp, vp, p.world);
        glUniformMatrix4fv(g_u_mvp,   1, GL_FALSE, mvp);
        glUniformMatrix4fv(g_u_model, 1, GL_FALSE, p.world);
        glUniform1f(g_u_screen, p.is_screen ? 1.f : 0.f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p.is_screen ? g_crt_tex : p.base_tex);

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

        glDrawArrays(GL_TRIANGLES, 0, p.vcount);
    }
}

// ============================================================
//  Input / ROM loading
// ============================================================
extern "C" EMSCRIPTEN_KEEPALIVE void set_button(int id, int pressed) {
    if (id>=0&&id<16) g_buttons[id]=pressed;
}

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

extern "C" EMSCRIPTEN_KEEPALIVE void start_game(const char* path) {
    FILE* f=fopen(path,"rb");
    if (!f) { printf("Cannot open %s\n",path); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(),1,sz,f); fclose(f);
    retro_game_info info={}; info.path=path; info.data=buf.data(); info.size=(size_t)sz;
    if (retro_load_game(&info)) { g_running=true; printf("ROM loaded (%ld bytes)\n",sz); }
    else printf("retro_load_game failed\n");
}

extern "C" EMSCRIPTEN_KEEPALIVE int get_game_tex_id() {
    return (int)g_game_tex;
}

extern "C" EMSCRIPTEN_KEEPALIVE void set_frame_size(int w, int h) {
    g_frame_w = (unsigned)w;
    g_frame_h = (unsigned)h;
}

// ============================================================
//  Main loop + init
// ============================================================
static void loop() {
    update_player();
    if (g_running) retro_run();
    render_crt_pass();
    render();
}

int main() {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha=false; attr.depth=true; attr.majorVersion=1;
    auto ctx=emscripten_webgl_create_context("#canvas",&attr);
    emscripten_webgl_make_context_current(ctx);

    gl_init();

    retro_set_environment      (retro_environment);
    retro_set_video_refresh    (retro_video_refresh);
    retro_set_audio_sample     (retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll       (retro_input_poll);
    retro_set_input_state      (retro_input_state);
    retro_init();

    emscripten_set_main_loop(loop, 60, 1);
    return 0;
}
