// renderer.cpp
// Main-thread 3D room renderer entry point.
// Owns all global state, GL initialization, exported JS API, and the main loop.
// Implementation is split across separate .cpp files linked together at build time.

#include <emscripten.h>
#include <emscripten/html5.h>
#include "renderer_types.h"
#include "renderer_shaders.h"

// ============================================================
//  Global definitions (declared extern in renderer_types.h)
// ============================================================
std::vector<TvPrim> g_prims;
GLuint g_game_tex   = 0;
GLuint g_white_tex  = 0;
LightState g_light;
SceneState g_scene;
GLuint g_prog     = 0;
int    g_a_pos=-1, g_a_uv=-1, g_a_norm=-1;
int    g_u_mvp=-1, g_u_model=-1, g_u_tex=-1, g_u_screen=-1, g_u_overscan=-1;
int    g_u_tv_quad_pos=-1, g_u_tv_quad_col=-1, g_u_tv_normal=-1;
int    g_u_lamp_pos=-1, g_u_lamp_intensity=-1;
int    g_u_cone_power=-1, g_skin_u_cone_power=-1;
float  g_cat_eye_height = CAT_EYE_HEIGHT_DEFAULT;
Player g_local;
RemotePlayer g_remote[MAX_REMOTE];
bool  g_preview_active = false;
int   g_preview_model  = 0;
float g_preview_anim_t = 0.f;
float g_preview_spin   = 0.f;
PreviewXform g_preview_xform[4] = {
    {0.f,  40.f, -25.f, 1.f},
    {0.f,  37.f, -25.f, 1.f},
    {0.f, 150.f, -100.f, 5.f},
    {0.f,  40.f, -25.f, 1.f},
};
AvatarModel g_models[MAX_AVATAR_MODELS];
GLuint g_skin_prog  = 0;
AnimState g_anim;
int    g_skin_u_vp=-1, g_skin_u_world=-1, g_skin_u_bones=-1;
int    g_skin_u_tex=-1, g_skin_u_tv_quad_pos=-1, g_skin_u_tv_quad_col=-1, g_skin_u_tv_normal=-1;
int    g_skin_u_lamp_pos=-1, g_skin_u_lamp_intensity=-1, g_skin_u_flat_shade=-1;
int    g_skin_a_pos=-1, g_skin_a_uv=-1, g_skin_a_norm=-1;
int    g_skin_a_joints=-1, g_skin_a_weights=-1;
bool g_move[4] = {};
GLuint g_flat_prog   = 0;
int    g_flat_a_pos  = -1, g_flat_u_mvp = -1, g_flat_u_color = -1;
GLuint g_cube_vbo    = 0;
float  g_debug_pos[3] = {};
bool   g_debug_visible = false;
GLuint g_bill_prog       = 0;
int    g_bill_a_corner   = -1;
int    g_bill_u_center   = -1, g_bill_u_cam_right = -1, g_bill_u_cam_up = -1;
int    g_bill_u_hw       = -1, g_bill_u_hh = -1, g_bill_u_vp = -1;
GLuint g_bill_vbo        = 0;
uint8_t g_name_upload_buf[NAMEPLATE_BUF_SIZE];
unsigned g_frame_w = 160, g_frame_h = 144;
GLuint g_crt_fbo  = 0;
GLuint g_crt_tex  = 0;
GLuint g_crt_prog = 0;
GLuint g_crt_vbo  = 0;
int    g_crt_a_vert=-1, g_crt_a_uv=-1;
int    g_crt_u_mvp=-1, g_crt_u_fcount=-1;
int    g_crt_u_out=-1, g_crt_u_texsz=-1, g_crt_u_insz=-1, g_crt_u_tex=-1;

// ============================================================
//  GL init
// ============================================================
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
    glFrontFace(GL_CW);

    {
        GLuint vs = make_shader(GL_VERTEX_SHADER,  VS_FLAT);
        GLuint fs = make_shader(GL_FRAGMENT_SHADER, FS_FLAT);
        g_flat_prog = glCreateProgram();
        glAttachShader(g_flat_prog, vs); glAttachShader(g_flat_prog, fs);
        glLinkProgram(g_flat_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        g_flat_a_pos   = glGetAttribLocation (g_flat_prog, "a_pos");
        g_flat_u_mvp   = glGetUniformLocation(g_flat_prog, "u_mvp");
        g_flat_u_color = glGetUniformLocation(g_flat_prog, "u_color");
    }
    {
        static const float cv[] = {
            -5,-5,-5,  5,-5,-5,   5,-5,-5,  5,-5, 5,
             5,-5, 5, -5,-5, 5,  -5,-5, 5, -5,-5,-5,
            -5, 5,-5,  5, 5,-5,   5, 5,-5,  5, 5, 5,
             5, 5, 5, -5, 5, 5,  -5, 5, 5, -5, 5,-5,
            -5,-5,-5, -5, 5,-5,   5,-5,-5,  5, 5,-5,
             5,-5, 5,  5, 5, 5,  -5,-5, 5, -5, 5, 5,
        };
        glGenBuffers(1, &g_cube_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_cube_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(cv), cv, GL_STATIC_DRAW);
    }
    {
        GLuint vs = make_shader(GL_VERTEX_SHADER,   VS_BILL);
        GLuint fs = make_shader(GL_FRAGMENT_SHADER, FS_BILL);
        g_bill_prog = glCreateProgram();
        glAttachShader(g_bill_prog, vs); glAttachShader(g_bill_prog, fs);
        glLinkProgram(g_bill_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        g_bill_a_corner    = glGetAttribLocation (g_bill_prog, "a_corner");
        g_bill_u_center    = glGetUniformLocation(g_bill_prog, "u_center");
        g_bill_u_cam_right = glGetUniformLocation(g_bill_prog, "u_cam_right");
        g_bill_u_cam_up    = glGetUniformLocation(g_bill_prog, "u_cam_up");
        g_bill_u_hw        = glGetUniformLocation(g_bill_prog, "u_hw");
        g_bill_u_hh        = glGetUniformLocation(g_bill_prog, "u_hh");
        g_bill_u_vp        = glGetUniformLocation(g_bill_prog, "u_vp");
        static const float bq[] = { -1.f,-1.f,  1.f,-1.f,  -1.f,1.f,  1.f,1.f };
        glGenBuffers(1, &g_bill_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g_bill_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bq), bq, GL_STATIC_DRAW);
    }

    load_tv();
    for (const auto& p : g_prims)
        if (p.is_screen) {
            g_light.screen_pos[0] = p.world[12];
            g_light.screen_pos[1] = p.world[13];
            g_light.screen_pos[2] = p.world[14];
            printf("TV screen world pos: %.2f %.2f %.2f\n", p.world[12], p.world[13], p.world[14]);
            float rx=p.world[0]*g_light.half_x, ry=p.world[1]*g_light.half_x, rz=p.world[2]*g_light.half_x;
            float ux=p.world[4]*g_light.half_y, uy=p.world[5]*g_light.half_y, uz=p.world[6]*g_light.half_y;
            const float sgn[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}};
            for (int q=0;q<4;q++) {
                g_light.quad_pos[q][0] = g_light.screen_pos[0] + sgn[q][0]*rx*0.5f + sgn[q][1]*ux*0.5f;
                g_light.quad_pos[q][1] = g_light.screen_pos[1] + sgn[q][0]*ry*0.5f + sgn[q][1]*uy*0.5f;
                g_light.quad_pos[q][2] = g_light.screen_pos[2] + sgn[q][0]*rz*0.5f + sgn[q][1]*uz*0.5f;
            }
            float nx = 0.f         - g_light.screen_pos[0];
            float ny = 20.f        - g_light.screen_pos[1];
            float nz = -80.f       - g_light.screen_pos[2];
            float nl = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nl > 0.f) { nx/=nl; ny/=nl; nz/=nl; }
            g_light.screen_normal[0]=nx; g_light.screen_normal[1]=ny; g_light.screen_normal[2]=nz;
            for (int q=0;q<4;q++) {
                g_light.quad_pos[q][0] += nx*TV_LIGHT_PUSH;
                g_light.quad_pos[q][1] += ny*TV_LIGHT_PUSH;
                g_light.quad_pos[q][2] += nz*TV_LIGHT_PUSH;
            }
            printf("Screen outward normal: %.3f %.3f %.3f\n", nx, ny, nz);
        }
    load_room();
    init_crt();

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
    load_mech();
    load_knight();
    emscripten_get_canvas_element_size("#canvas", &g_scene.canvas_w, &g_scene.canvas_h);
}

// ============================================================
//  Exported functions (called by JS via ccall)
// ============================================================

extern "C" EMSCRIPTEN_KEEPALIVE void set_move_key(int key, int pressed) {
    if (key>=0&&key<4) g_move[key]=pressed;
}
extern "C" EMSCRIPTEN_KEEPALIVE void add_mouse_delta(float dx, float dy) {
    g_local.yaw   += dx * MOUSE_SENSITIVITY;
    g_local.pitch += dy * MOUSE_SENSITIVITY;
    if (g_local.pitch >  1.48f) g_local.pitch =  1.48f;
    if (g_local.pitch < -1.48f) g_local.pitch = -1.48f;
}
extern "C" EMSCRIPTEN_KEEPALIVE int get_game_tex_id() { return (int)g_game_tex; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_frame_size(int w, int h) {
    g_frame_w = (unsigned)w; g_frame_h = (unsigned)h;
}
extern "C" EMSCRIPTEN_KEEPALIVE void upload_frame(const uint8_t* rgba, int w, int h) {
    g_frame_w = (unsigned)w; g_frame_h = (unsigned)h;
    glBindTexture(GL_TEXTURE_2D, g_game_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_lamp_pos(float x, float y, float z) {
    g_light.lamp_pos[0]=x; g_light.lamp_pos[1]=y; g_light.lamp_pos[2]=z;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_lamp_intensity(float v) { g_light.lamp_intensity = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_tv_light_intensity(float v) { g_light.tv_intensity = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_tv_quad_colors(
    float r0,float g0,float b0, float r1,float g1,float b1,
    float r2,float g2,float b2, float r3,float g3,float b3) {
    g_light.quad_col[0][0]=r0; g_light.quad_col[0][1]=g0; g_light.quad_col[0][2]=b0;
    g_light.quad_col[1][0]=r1; g_light.quad_col[1][1]=g1; g_light.quad_col[1][2]=b1;
    g_light.quad_col[2][0]=r2; g_light.quad_col[2][1]=g2; g_light.quad_col[2][2]=b2;
    g_light.quad_col[3][0]=r3; g_light.quad_col[3][1]=g3; g_light.quad_col[3][2]=b3;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_yaw(float v)   { g_light.cone_yaw   = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_pitch(float v) { g_light.cone_pitch = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_cone_power(float v) { g_light.cone_power = v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_overscan(float x, float y) {
    g_scene.overscan_x = x; g_scene.overscan_y = y;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_room_xform(float scale, float rot_y_deg,
                                                     float tx, float ty, float tz) {
    g_scene.room_scale = scale;
    g_scene.room_rot_y = rot_y_deg * 3.14159265f / 180.f;
    g_scene.room_tx = tx; g_scene.room_ty = ty; g_scene.room_tz = tz;
}
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_x() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[12]; return 0.f;
}
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_y() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[13]; return 0.f;
}
extern "C" EMSCRIPTEN_KEEPALIVE float get_tv_z() {
    for (const auto& p : g_prims) if (p.is_screen) return p.world[14]; return 0.f;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_debug_cube_pos(float x, float y, float z) {
    g_debug_pos[0]=x; g_debug_pos[1]=y; g_debug_pos[2]=z;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_debug_cube_visible(int v) { g_debug_visible=(v!=0); }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_pitch() { return g_local.pitch; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_x()   { return g_local.x; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_y()   { return g_local.y; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_z()   { return g_local.z; }
extern "C" EMSCRIPTEN_KEEPALIVE float get_local_yaw() { return g_local.yaw; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_remote_player(int id, float x, float y, float z, float yaw, int moving) {
    if (id<0||id>=MAX_REMOTE) return;
    g_remote[id].x=x; g_remote[id].y=y; g_remote[id].z=z;
    g_remote[id].yaw=yaw; g_remote[id].active=true; g_remote[id].moving=(moving!=0);
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_cat_eye_height(float v) { g_cat_eye_height=v; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_local_y(float v) { g_local.y=v; }
extern "C" EMSCRIPTEN_KEEPALIVE int get_local_moving() {
    return (g_move[0]||g_move[1]||g_move[2]||g_move[3])?1:0;
}
extern "C" EMSCRIPTEN_KEEPALIVE void remove_remote_player(int id) {
    if (id>=0&&id<MAX_REMOTE) g_remote[id].active=false;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_remote_player_model(int id, int model) {
    if (id<0||id>=MAX_REMOTE) return;
    int mdl=(model>=0&&model<MAX_AVATAR_MODELS)?model:0;
    if (g_remote[id].model_idx!=mdl) {
        g_remote[id].model_idx=mdl;
        g_remote[id].anim_idx=g_models[mdl].idle_anim;
        g_remote[id].anim_time=0.f;
    }
}
extern "C" EMSCRIPTEN_KEEPALIVE uint8_t* get_name_upload_buf() { return g_name_upload_buf; }
extern "C" EMSCRIPTEN_KEEPALIVE void set_remote_player_name_tex(int id, int w, int h) {
    if (id<0||id>=MAX_REMOTE) return;
    if (!g_remote[id].name_tex) glGenTextures(1,&g_remote[id].name_tex);
    glBindTexture(GL_TEXTURE_2D,g_remote[id].name_tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,g_name_upload_buf);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    g_remote[id].name_w=w; g_remote[id].name_h=h;
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_preview_transform(int model, float x, float y, float z, float scale) {
    if (model<0||model>3) return;
    g_preview_xform[model]={x,y,z,scale};
}
extern "C" EMSCRIPTEN_KEEPALIVE void set_preview_mode(int model_idx) {
    if (g_preview_model!=model_idx) { g_preview_model=model_idx; g_preview_anim_t=0.f; }
    g_preview_active=true;
}
extern "C" EMSCRIPTEN_KEEPALIVE void exit_preview_mode() { g_preview_active=false; }
extern "C" EMSCRIPTEN_KEEPALIVE void resize_canvas() {
    emscripten_get_canvas_element_size("#canvas", &g_scene.canvas_w, &g_scene.canvas_h);
}

// ============================================================
//  Main loop
// ============================================================
static void loop() {
    update_player();
    render_crt_pass();
    render();
}

int main() {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha=false; attr.depth=true; attr.majorVersion=2;
    auto ctx=emscripten_webgl_create_context("#canvas",&attr);
    emscripten_webgl_make_context_current(ctx);
    gl_init();
    emscripten_set_main_loop(loop, 60, 1);
    return 0;
}
