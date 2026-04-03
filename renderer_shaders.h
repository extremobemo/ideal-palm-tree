// renderer_shaders.h
// GLSL shader source strings for the 3D room renderer.
// Included once by renderer.cpp — not a standalone translation unit.

// ============================================================
//  Static geometry shaders (GLSL ES 1.0 / WebGL 1 compatible)
// ============================================================
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


// ============================================================
//  Flat-color shader — used for debug wireframe overlays
// ============================================================
static const char* VS_FLAT = R"(
attribute vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";

static const char* FS_FLAT = R"(
precision mediump float;
uniform vec3 u_color;
void main() { gl_FragColor = vec4(u_color, 1.0); }
)";

// ============================================================
//  Billboard shader — textured quad always facing the camera
// ============================================================
static const char* VS_BILL = R"(
attribute vec2 a_corner;
uniform vec3  u_center;
uniform vec3  u_cam_right;
uniform vec3  u_cam_up;
uniform float u_hw;
uniform float u_hh;
uniform mat4  u_vp;
varying vec2  v_uv;
void main() {
    vec3 world = u_center
               + u_cam_right * a_corner.x * u_hw
               + u_cam_up    * a_corner.y * u_hh;
    gl_Position = u_vp * vec4(world, 1.0);
    v_uv = vec2(a_corner.x * 0.5 + 0.5, 0.5 - a_corner.y * 0.5);
}
)";

static const char* FS_BILL = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    if (c.a < 0.05) discard;
    gl_FragColor = c;
}
)";
