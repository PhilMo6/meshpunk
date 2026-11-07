// Canvas, image and tilemap drawing. Port of pyxel-core canvas.rs, image.rs,
// rect_area.rs and the built-in font from settings.rs (MIT, Takashi Kitao).

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

// ---------------------------------------------------------------------------
// Numeric helpers (utils.rs: f32::round() as i32 / as u32)
// ---------------------------------------------------------------------------
int px_f2i(float x) {
    if (x != x) return 0;
    float r = roundf(x);
    if (r >= 2147483647.0f) return 2147483647;
    if (r <= -2147483648.0f) return (-2147483647 - 1);
    return (int)r;
}

int px_f2u(float x) {
    if (!(x > 0.0f)) return 0;
    float r = roundf(x);
    if (r >= 2147483647.0f) return 2147483647;
    return (int)r;
}

// ---------------------------------------------------------------------------
// RectArea
// ---------------------------------------------------------------------------
px_rect px_rect_new(int left, int top, int width, int height) {
    px_rect r;
    r.left = left;
    r.top = top;
    r.width = width < 0 ? 0 : width;
    r.height = height < 0 ? 0 : height;
    r.right = left + r.width - 1;
    r.bottom = top + r.height - 1;
    return r;
}

px_rect px_rect_intersect(px_rect a, px_rect b) {
    int left = a.left > b.left ? a.left : b.left;
    int top = a.top > b.top ? a.top : b.top;
    int right = a.right < b.right ? a.right : b.right;
    int bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    int w = right - left + 1, h = bottom - top + 1;
    if (w > 0 && h > 0) return px_rect_new(left, top, w, h);
    return px_rect_new(0, 0, 0, 0);
}

void px_canvas_init(px_canvas* cv, int w, int h, void* data) {
    cv->w = w;
    cv->h = h;
    cv->self_rect = px_rect_new(0, 0, w, h);
    cv->clip = cv->self_rect;
    cv->cam_x = cv->cam_y = 0;
    cv->alpha = 1.0f;
    cv->data = data;
}

void px_canvas_set_clip(px_canvas* cv, float x, float y, float w, float h) {
    cv->clip = px_rect_intersect(cv->self_rect,
                                 px_rect_new(px_f2i(x), px_f2i(y), px_f2u(w), px_f2u(h)));
}

void px_canvas_reset_clip(px_canvas* cv) {
    cv->clip = cv->self_rect;
}

// ---------------------------------------------------------------------------
// Shared primitive helpers
// ---------------------------------------------------------------------------
static const float DITHER[4][4] = {
    { 1.0f / 16, 9.0f / 16, 3.0f / 16, 11.0f / 16 },
    { 13.0f / 16, 5.0f / 16, 15.0f / 16, 7.0f / 16 },
    { 3.0f / 16, 11.0f / 16, 1.0f / 16, 9.0f / 16 },
    { 15.0f / 16, 7.0f / 16, 13.0f / 16, 5.0f / 16 },
};

static inline bool px_should_write(const px_canvas* cv, int x, int y) {
    if (cv->alpha >= 1.0f) return true;
    if (cv->alpha <= 0.0f) return false;
    return cv->alpha > DITHER[y & 3][x & 3];
}

#define ELLIPSE_ROUNDING_BIAS 0.01f

static void px_ellipse_area(float cx, float cy, float ra, float rb, int x,
                            int* x1, int* y1, int* x2, int* y2) {
    float dx = (float)x - cx;
    float dy = ra > 0.0f ? rb * sqrtf(1.0f - dx * dx / (ra * ra)) : rb;
    *x1 = px_f2i(cx - dx - ELLIPSE_ROUNDING_BIAS);
    *y1 = px_f2i(cy - dy - ELLIPSE_ROUNDING_BIAS);
    *x2 = px_f2i(cx + dx + ELLIPSE_ROUNDING_BIAS);
    *y2 = px_f2i(cy + dy + ELLIPSE_ROUNDING_BIAS);
}

// CopyArea: clip a (possibly flipped) copy against both rectangles.
typedef struct {
    int dst_x, dst_y, src_x, src_y;
    int sign_x, sign_y, offset_x, offset_y;
    int width, height;
} px_copy_area;

static inline int imax(int a, int b) { return a > b ? a : b; }

static void px_copy_area_new(px_copy_area* a, int dst_x, int dst_y, px_rect dst_rect,
                             int src_x, int src_y, px_rect src_rect, int width, int height) {
    bool flip_x = width < 0, flip_y = height < 0;
    width = abs(width);
    height = abs(height);
    int src_left_cut = src_rect.left - src_x;
    int src_top_cut = src_rect.top - src_y;
    int src_right_cut = src_x + width - 1 - src_rect.right;
    int src_bottom_cut = src_y + height - 1 - src_rect.bottom;
    int left_cut = imax(imax(dst_rect.left - dst_x, flip_x ? src_right_cut : src_left_cut), 0);
    int top_cut = imax(imax(dst_rect.top - dst_y, flip_y ? src_bottom_cut : src_top_cut), 0);
    int right_cut = imax(imax(dst_x + width - 1 - dst_rect.right,
                              flip_x ? src_left_cut : src_right_cut), 0);
    int bottom_cut = imax(imax(dst_y + height - 1 - dst_rect.bottom,
                               flip_y ? src_top_cut : src_bottom_cut), 0);
    width = imax(width - left_cut - right_cut, 0);
    height = imax(height - top_cut - bottom_cut, 0);
    a->sign_x = flip_x ? -1 : 1;
    a->offset_x = flip_x ? width - 1 : 0;
    a->sign_y = flip_y ? -1 : 1;
    a->offset_y = flip_y ? height - 1 : 0;
    a->dst_x = dst_x + left_cut;
    a->dst_y = dst_y + top_cut;
    a->src_x = src_x + (flip_x ? right_cut : left_cut);
    a->src_y = src_y + (flip_y ? bottom_cut : top_cut);
    a->width = width;
    a->height = height;
}

// TransformProjection: inverse mapping for rotate/scale blits.
typedef struct {
    float src_cx, src_cy, dst_cx, dst_cy;
    float sign_x, sign_y, cos_s, sin_s;
    int src_x, src_y, width, height;
    int x1, x2, y1, y2;
} px_xform;

static bool px_xform_new(px_xform* p, float x, float y, float src_x, float src_y,
                         float width, float height, int off_x, int off_y,
                         float rotate, float scale, px_rect clip) {
    if (scale < 1.1920929e-7f) return false;
    int ix = px_f2i(x) - off_x, iy = px_f2i(y) - off_y;
    int isx = px_f2i(src_x), isy = px_f2i(src_y);
    p->sign_x = width < 0.0f ? -1.0f : 1.0f;
    p->sign_y = height < 0.0f ? -1.0f : 1.0f;
    int w = abs(px_f2i(width)), h = abs(px_f2i(height));
    float hw = (float)(w - 1) / 2.0f, hh = (float)(h - 1) / 2.0f;
    p->src_cx = (float)isx + hw;
    p->src_cy = (float)isy + hh;
    p->dst_cx = (float)ix + hw;
    p->dst_cy = (float)iy + hh;
    float rad = rotate * 3.14159265358979f / 180.0f;
    float s = -sinf(rad), c = cosf(rad);
    float bx = (hw * fabsf(c) + hh * fabsf(s) + 1.0f) * scale;
    float by = (hw * fabsf(s) + hh * fabsf(c) + 1.0f) * scale;
    p->cos_s = c / scale;
    p->sin_s = s / scale;
    p->src_x = isx;
    p->src_y = isy;
    p->width = w;
    p->height = h;
    p->x1 = imax(px_f2i(p->dst_cx - bx), clip.left);
    int x2 = px_f2i(p->dst_cx + bx);
    p->x2 = x2 < clip.right ? x2 : clip.right;
    p->y1 = imax(px_f2i(p->dst_cy - by), clip.top);
    int y2 = px_f2i(p->dst_cy + by);
    p->y2 = y2 < clip.bottom ? y2 : clip.bottom;
    return true;
}

// ---------------------------------------------------------------------------
// Template instances
// ---------------------------------------------------------------------------
#define PX_CAT_(a, b) a##b
#define PX_CAT(a, b) PX_CAT_(a, b)

#define T uint8_t
#define SUF u8
#include "canvas_tmpl.h"
#undef T
#undef SUF

#define T uint16_t
#define SUF u16
#include "canvas_tmpl.h"
#undef T
#undef SUF

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
void px_image_reset_pal(px_image* img) {
    for (int i = 0; i < PX_MAX_COLORS; i++) img->pal[i] = (uint8_t)i;
    img->pal_identity = true;
}

void px_image_init(px_image* img, int w, int h, uint8_t* data) {
    px_canvas_init(&img->cv, w, h, data);
    px_image_reset_pal(img);
}

px_image* px_image_new(int w, int h) {
    px_image* img = (px_image*)malloc(sizeof(px_image));
    if (!img) return NULL;
    uint8_t* data = (uint8_t*)calloc((size_t)w * (size_t)h, 1);
    if (!data) { free(img); return NULL; }
    px_image_init(img, w, h, data);
    return img;
}

void px_image_free(px_image* img) {
    if (!img) return;
    free(img->cv.data);
    free(img);
}

void px_image_blt(px_image* dst, float x, float y, px_image* src, float u, float v,
                  float w, float h, int colkey, float rotate, float scale) {
    // Same image as source and destination: copy the region out first.
    px_canvas tmp;
    const px_canvas* scv = &src->cv;
    uint8_t* tmp_data = NULL;
    if (src == dst) {
        int tw = px_f2u(fabsf(w)), th = px_f2u(fabsf(h));
        tmp_data = (uint8_t*)calloc((size_t)(tw ? tw : 1) * (size_t)(th ? th : 1), 1);
        if (!tmp_data) return;
        px_canvas_init(&tmp, tw, th, tmp_data);
        px_blit_u8(&tmp, 0.0f, 0.0f, &src->cv, u, v, (float)tw, (float)th, -1, NULL);
        scv = &tmp;
        u = 0.0f;
        v = 0.0f;
    }
    const uint8_t* pal = px_image_pal(dst);
    if (rotate != 0.0f || scale != 1.0f)
        px_blit_xform_u8(&dst->cv, x, y, scv, u, v, w, h, colkey, pal, rotate, scale);
    else
        px_blit_u8(&dst->cv, x, y, scv, u, v, w, h, colkey, pal);
    free(tmp_data);
}

static void px_bltm_xform(px_image* dst, float x, float y, px_tilemap* tm, const px_canvas* icv,
                          float fu, float fv, float w, float h, int colkey,
                          float rotate, float scale) {
    px_xform p;
    if (!px_xform_new(&p, x, y, 0.0f, 0.0f, w, h, dst->cv.cam_x, dst->cv.cam_y,
                      rotate, scale, dst->cv.clip))
        return;
    int tu = px_f2i(fu), tv = px_f2i(fv);
    px_rect area = px_rect_intersect(px_rect_new(0, 0, p.width, p.height),
                                     px_rect_new(-tu, -tv, tm->cv.w * PX_TILE_SIZE,
                                                 tm->cv.h * PX_TILE_SIZE));
    if (px_rect_empty(area)) return;
    const uint8_t* pal = px_image_pal(dst);
    const uint16_t* tiles = (const uint16_t*)tm->cv.data;
    const uint8_t* img = (const uint8_t*)icv->data;
    uint8_t* dp = (uint8_t*)dst->cv.data;
    float step_sx = p.sign_x * p.cos_s, step_sy = p.sign_x * p.sin_s;
    for (int yi = p.y1; yi <= p.y2; yi++) {
        float ox = ((float)p.x1 - p.dst_cx) * p.sign_x;
        float oy = ((float)yi - p.dst_cy) * p.sign_y;
        float sx = p.src_cx + ox * p.cos_s - oy * p.sin_s;
        float sy = p.src_cy + ox * p.sin_s + oy * p.cos_s;
        for (int xi = p.x1; xi <= p.x2; xi++) {
            int vx = px_f2i(sx), vy = px_f2i(sy);
            sx += step_sx;
            sy += step_sy;
            if (!px_rect_contains(area, vx, vy)) continue;
            int srcx = tu + vx, srcy = tv + vy;
            uint16_t t = tiles[tm->cv.w * (srcy >> PX_TILE_SHIFT) + (srcx >> PX_TILE_SHIFT)];
            int ix = PX_TILE_X(t) * PX_TILE_SIZE + (srcx & PX_TILE_MASK);
            int iy = PX_TILE_Y(t) * PX_TILE_SIZE + (srcy & PX_TILE_MASK);
            uint8_t val = (ix < icv->w && iy < icv->h) ? img[icv->w * iy + ix] : 0;
            if (colkey >= 0 && val == (uint8_t)colkey) continue;
            if (pal) val = pal[val];
            if (dst->cv.alpha >= 1.0f || px_should_write(&dst->cv, xi, yi))
                dp[dst->cv.w * yi + xi] = val;
        }
    }
}

void px_image_bltm(px_image* dst, float fx, float fy, px_tilemap* tm, px_image* tm_img,
                   float fu, float fv, float fw, float fh, int colkey, float rotate, float scale) {
    // Tilemap image aliasing the destination: render from a copy of it.
    px_canvas copy;
    const px_canvas* icv = &tm_img->cv;
    uint8_t* copy_data = NULL;
    if (tm_img == dst) {
        size_t n = (size_t)dst->cv.w * (size_t)dst->cv.h;
        copy_data = (uint8_t*)malloc(n);
        if (!copy_data) return;
        memcpy(copy_data, dst->cv.data, n);
        px_canvas_init(&copy, dst->cv.w, dst->cv.h, copy_data);
        icv = &copy;
    }
    if (rotate != 0.0f || scale != 1.0f) {
        px_bltm_xform(dst, fx, fy, tm, icv, fu, fv, fw, fh, colkey, rotate, scale);
        free(copy_data);
        return;
    }

    int x = px_f2i(fx) - dst->cv.cam_x, y = px_f2i(fy) - dst->cv.cam_y;
    px_rect tm_rect = px_rect_new(0, 0, tm->cv.w * PX_TILE_SIZE, tm->cv.h * PX_TILE_SIZE);
    px_copy_area a;
    px_copy_area_new(&a, x, y, dst->cv.clip, px_f2i(fu), px_f2i(fv), tm_rect,
                     px_f2i(fw), px_f2i(fh));
    if (a.width == 0 || a.height == 0) { free(copy_data); return; }

    const uint16_t* tiles = (const uint16_t*)tm->cv.data;
    const uint8_t* img = (const uint8_t*)icv->data;
    int img_w = icv->w, img_h = icv->h;
    uint8_t* dp = (uint8_t*)dst->cv.data;
    const uint8_t* pal = px_image_pal(dst);

    if (a.sign_x == 1 && a.sign_y == 1 && dst->cv.alpha >= 1.0f) {
        for (int yi = 0; yi < a.height; yi++) {
            int ty_px = a.src_y + yi;
            int tile_y = ty_px >> PX_TILE_SHIFT, pixel_y = ty_px & PX_TILE_MASK;
            uint8_t* drow = dp + dst->cv.w * (a.dst_y + yi) + a.dst_x;
            int xi = 0;
            while (xi < a.width) {
                int tx_px = a.src_x + xi;
                uint16_t t = tiles[tm->cv.w * tile_y + (tx_px >> PX_TILE_SHIFT)];
                int pixel_x = tx_px & PX_TILE_MASK;
                int chunk = PX_TILE_SIZE - pixel_x;
                if (chunk > a.width - xi) chunk = a.width - xi;
                int ix = PX_TILE_X(t) * PX_TILE_SIZE + pixel_x;
                int iy = PX_TILE_Y(t) * PX_TILE_SIZE + pixel_y;
                if (iy < img_h && ix < img_w) {
                    int valid = chunk < img_w - ix ? chunk : img_w - ix;
                    const uint8_t* s = img + img_w * iy + ix;
                    uint8_t* d = drow + xi;
                    if (colkey < 0 && !pal) {
                        memcpy(d, s, (size_t)valid);
                    } else {
                        for (int i = 0; i < valid; i++) {
                            uint8_t val = s[i];
                            if (colkey >= 0 && val == (uint8_t)colkey) continue;
                            d[i] = pal ? pal[val] : val;
                        }
                    }
                }
                xi += chunk;
            }
        }
        free(copy_data);
        return;
    }

    for (int yi = 0; yi < a.height; yi++) {
        int ty_px = a.src_y + a.sign_y * yi + a.offset_y;
        int tile_y = ty_px >> PX_TILE_SHIFT, pixel_y = ty_px & PX_TILE_MASK;
        int dy = a.dst_y + yi;
        for (int xi = 0; xi < a.width; xi++) {
            int tx_px = a.src_x + a.sign_x * xi + a.offset_x;
            uint16_t t = tiles[tm->cv.w * tile_y + (tx_px >> PX_TILE_SHIFT)];
            int ix = PX_TILE_X(t) * PX_TILE_SIZE + (tx_px & PX_TILE_MASK);
            int iy = PX_TILE_Y(t) * PX_TILE_SIZE + pixel_y;
            if (ix >= img_w || iy >= img_h) continue;
            uint8_t val = img[img_w * iy + ix];
            if (colkey >= 0 && val == (uint8_t)colkey) continue;
            if (pal) val = pal[val];
            int dx = a.dst_x + xi;
            if (dst->cv.alpha >= 1.0f || px_should_write(&dst->cv, dx, dy))
                dp[dst->cv.w * dy + dx] = val;
        }
    }
    free(copy_data);
}

// ---------------------------------------------------------------------------
// Perspective blits (canvas.rs PerspectiveProjection / blit_perspective,
// image.rs draw_image_3d / draw_tilemap_3d)
// ---------------------------------------------------------------------------
#define F32_EPSILON 1.1920929e-7f

typedef struct {
    float cam_x, cam_y, cam_z;
    float r00, r01, r02, r10, r11, r12, r21, r22;
    float sin_z, cos_z, tan_hfov, aspect, hw, hh;
    int dst_x, dst_y, w, h;
} px_persp;

static bool persp_new(px_persp* p, float x, float y, float width, float height, int off_x, int off_y,
                      const float pos[3], const float rot[3], float fov) {
    p->cam_x = pos[0];
    p->cam_y = pos[1];
    p->cam_z = pos[2];
    if (fabsf(p->cam_z) < F32_EPSILON) return false;
    p->w = px_f2i(width);
    p->h = px_f2i(height);
    if (p->w <= 0 || p->h <= 0) return false;
    p->tan_hfov = tanf(fov * 3.14159265358979f / 360.0f);
    if (!isfinite(p->tan_hfov) || fabsf(p->tan_hfov) < F32_EPSILON) return false;
    float rx = rot[0] * 3.14159265358979f / 180.0f;
    float ry = rot[1] * 3.14159265358979f / 180.0f;
    float rz = rot[2] * 3.14159265358979f / 180.0f;
    float sx = sinf(rx), cx = cosf(rx), sy = sinf(ry), cy = cosf(ry);
    p->r00 = cy;
    p->r01 = sy * cx;
    p->r02 = -sy * sx;
    p->r10 = sy;
    p->r11 = -cy * cx;
    p->r12 = cy * sx;
    p->r21 = sx;
    p->r22 = cx;
    p->sin_z = sinf(rz);
    p->cos_z = cosf(rz);
    p->aspect = (float)p->w / (float)p->h;
    p->hw = (float)p->w / 2.0f;
    p->hh = (float)p->h / 2.0f;
    p->dst_x = px_f2i(x) - off_x;
    p->dst_y = px_f2i(y) - off_y;
    return true;
}

static void persp_step(const px_persp* p, float* wx, float* wy, float* wz) {
    float vx_step = p->tan_hfov * p->aspect / p->hw;
    float vx2 = vx_step * p->cos_z;
    float vy2 = -vx_step * p->sin_z;
    *wx = p->r00 * vx2 + p->r01 * vy2;
    *wy = p->r10 * vx2 + p->r11 * vy2;
    *wz = p->r21 * vy2;
}

static void persp_base(const px_persp* p, int xi, int yi, float* wx, float* wy, float* wz) {
    float ndc_x = ((float)(xi - p->dst_x) + 0.5f - p->hw) / p->hw;
    float ndc_y = ((float)(yi - p->dst_y) + 0.5f - p->hh) / p->hh;
    float vx = ndc_x * p->tan_hfov * p->aspect;
    float vy = -ndc_y * p->tan_hfov;
    float vx2 = vx * p->cos_z + vy * p->sin_z;
    float vy2 = -vx * p->sin_z + vy * p->cos_z;
    *wx = p->r00 * vx2 + p->r01 * vy2 - p->r02;
    *wy = p->r10 * vx2 + p->r11 * vy2 - p->r12;
    *wz = p->r21 * vy2 - p->r22;
}

// Source pixel (image-space) for each destination pixel of the projection.
typedef bool (*persp_sample_fn)(void* ctx, int sx, int sy, uint8_t* out);

static void persp_blit(px_image* dst, const px_persp* p, persp_sample_fn sample, void* ctx, int colkey) {
    px_canvas* cv = &dst->cv;
    int x1 = p->dst_x > cv->clip.left ? p->dst_x : cv->clip.left;
    int x2 = p->dst_x + p->w - 1 < cv->clip.right ? p->dst_x + p->w - 1 : cv->clip.right;
    int y1 = p->dst_y > cv->clip.top ? p->dst_y : cv->clip.top;
    int y2 = p->dst_y + p->h - 1 < cv->clip.bottom ? p->dst_y + p->h - 1 : cv->clip.bottom;
    const uint8_t* pal = px_image_pal(dst);
    uint8_t* dp = (uint8_t*)cv->data;
    float sx_, sy_, sz_;
    persp_step(p, &sx_, &sy_, &sz_);
    for (int yi = y1; yi <= y2; yi++) {
        float wx, wy, wz;
        persp_base(p, x1, yi, &wx, &wy, &wz);
        for (int xi = x1; xi <= x2; xi++) {
            if (fabsf(wz) >= F32_EPSILON) {
                float t = -p->cam_z / wz;
                if (t > 0.0f) {
                    uint8_t v;
                    if (sample(ctx, px_f2i(p->cam_x + t * wx), px_f2i(p->cam_y + t * wy), &v) &&
                        (colkey < 0 || v != (uint8_t)colkey)) {
                        if (pal) v = pal[v];
                        if (cv->alpha >= 1.0f || px_should_write(cv, xi, yi)) dp[cv->w * yi + xi] = v;
                    }
                }
            }
            wx += sx_;
            wy += sy_;
            wz += sz_;
        }
    }
}

static bool sample_image(void* ctx, int sx, int sy, uint8_t* out) {
    const px_canvas* src = (const px_canvas*)ctx;
    if (sx < 0 || sy < 0 || sx >= src->w || sy >= src->h) return false;
    *out = ((const uint8_t*)src->data)[src->w * sy + sx];
    return true;
}

typedef struct {
    const px_canvas* tiles;
    const px_canvas* img;
} tm3d_ctx;

static bool sample_tilemap(void* ctx, int sx, int sy, uint8_t* out) {
    const tm3d_ctx* c = (const tm3d_ctx*)ctx;
    int tx = sx >> PX_TILE_SHIFT, ty = sy >> PX_TILE_SHIFT;
    if (tx < 0 || ty < 0 || tx >= c->tiles->w || ty >= c->tiles->h) return false;
    uint16_t t = ((const uint16_t*)c->tiles->data)[c->tiles->w * ty + tx];
    int px = PX_TILE_X(t) * PX_TILE_SIZE + (sx & PX_TILE_MASK);
    int py = PX_TILE_Y(t) * PX_TILE_SIZE + (sy & PX_TILE_MASK);
    if (px < 0 || py < 0 || px >= c->img->w || py >= c->img->h) return false;
    *out = ((const uint8_t*)c->img->data)[c->img->w * py + px];
    return true;
}

// A canvas copy of src when src aliases dst (both read and written).
static uint8_t* alias_copy(const px_image* dst, const px_image* src, px_canvas* copy) {
    if (src != dst) return NULL;
    size_t n = (size_t)src->cv.w * (size_t)src->cv.h;
    uint8_t* data = (uint8_t*)malloc(n);
    if (!data) return NULL;
    memcpy(data, src->cv.data, n);
    px_canvas_init(copy, src->cv.w, src->cv.h, data);
    return data;
}

void px_image_blt3d(px_image* dst, float x, float y, float w, float h, px_image* src,
                    const float pos[3], const float rot[3], float fov, int colkey) {
    px_persp p;
    if (!persp_new(&p, x, y, w, h, dst->cv.cam_x, dst->cv.cam_y, pos, rot, fov)) return;
    px_canvas copy;
    uint8_t* data = alias_copy(dst, src, &copy);
    persp_blit(dst, &p, sample_image, data ? (void*)&copy : (void*)&src->cv, colkey);
    free(data);
}

void px_image_bltm3d(px_image* dst, float x, float y, float w, float h, px_tilemap* tm,
                     px_image* tm_img, const float pos[3], const float rot[3], float fov, int colkey) {
    px_persp p;
    if (!persp_new(&p, x, y, w, h, dst->cv.cam_x, dst->cv.cam_y, pos, rot, fov)) return;
    px_canvas copy;
    uint8_t* data = alias_copy(dst, tm_img, &copy);
    tm3d_ctx ctx = { &tm->cv, data ? &copy : &tm_img->cv };
    persp_blit(dst, &p, sample_tilemap, &ctx, colkey);
    free(data);
}

// ---------------------------------------------------------------------------
// Built-in font (settings.rs FONT_DATA): 4x6 glyphs for ' '..'\x7F', one
// nibble per row, top row in bits 23-20, leftmost pixel = bit 3 of the nibble.
// ---------------------------------------------------------------------------
static const uint32_t FONT_DATA[96] = {
    0x000000, 0x444040, 0xaa0000, 0xaeaea0, 0x6c6c40, 0x824820, 0x4a4ac0, 0x440000, 0x244420,
    0x844480, 0xa4e4a0, 0x04e400, 0x000480, 0x00e000, 0x000040, 0x224880, 0x6aaac0, 0x4c4440,
    0xc248e0, 0xc242c0, 0xaae220, 0xe8c2c0, 0x68eae0, 0xe24880, 0xeaeae0, 0xeae2c0, 0x040400,
    0x040480, 0x248420, 0x0e0e00, 0x842480, 0xe24040, 0x4aa860, 0x4aeaa0, 0xcacac0, 0x688860,
    0xcaaac0, 0xe8e8e0, 0xe8e880, 0x68ea60, 0xaaeaa0, 0xe444e0, 0x222a40, 0xaacaa0, 0x8888e0,
    0xaeeaa0, 0xcaaaa0, 0x4aaa40, 0xcac880, 0x4aae60, 0xcaeca0, 0x6842c0, 0xe44440, 0xaaaa60,
    0xaaaa40, 0xaaeea0, 0xaa4aa0, 0xaa4440, 0xe248e0, 0x644460, 0x884220, 0xc444c0, 0x4a0000,
    0x0000e0, 0x840000, 0x06aa60, 0x8caac0, 0x068860, 0x26aa60, 0x06ac60, 0x24e440, 0x06ae24,
    0x8caaa0, 0x404440, 0x2022a4, 0x8acca0, 0xc444e0, 0x0eeea0, 0x0caaa0, 0x04aa40, 0x0caac8,
    0x06aa62, 0x068880, 0x06c6c0, 0x4e4460, 0x0aaa60, 0x0aaa40, 0x0aaee0, 0x0a44a0, 0x0aa624,
    0x0e24e0, 0x64c460, 0x444440, 0xc464c0, 0x6c0000, 0xeeeee0,
};

void px_image_text(px_image* dst, float fx, float fy, const char* s, size_t len, int col) {
    px_canvas* cv = &dst->cv;
    int x = px_f2i(fx) - cv->cam_x;
    int y = px_f2i(fy) - cv->cam_y;
    uint8_t color = dst->pal[col & 0xFF];
    int start_x = x;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            x = start_x;
            y += PX_FONT_HEIGHT;
            continue;
        }
        // UTF-8 lead/continuation bytes are one non-ASCII char each: skip
        // the whole sequence without advancing, as upstream skips the char.
        if (c < 0x20 || c > 0x7F) continue;
        uint32_t g = FONT_DATA[c - 0x20];
        for (int fy_ = 0; fy_ < PX_FONT_HEIGHT; fy_++) {
            uint32_t bits = (g >> (20 - 4 * fy_)) & 0xF;
            if (!bits) continue;
            for (int fx_ = 0; fx_ < PX_FONT_WIDTH; fx_++)
                if (bits & (0x8u >> fx_)) px_write_clipped_u8(cv, x + fx_, y + fy_, color);
        }
        x += PX_FONT_WIDTH;
    }
}

// ---------------------------------------------------------------------------
// Tilemaps
// ---------------------------------------------------------------------------
void px_tilemap_init(px_tilemap* tm, int w, int h, uint16_t* data, int imgsrc) {
    px_canvas_init(&tm->cv, w, h, data);
    tm->imgsrc = imgsrc;
}

// ---------------------------------------------------------------------------
// Tilemap.collide (tilemap.rs): sweep the larger axis first, then the other.
// ---------------------------------------------------------------------------
static bool is_wall(const px_tilemap* tm, int tx, int ty, const uint16_t* walls, int nwalls) {
    if (tx < 0 || ty < 0 || tx >= tm->cv.w || ty >= tm->cv.h) return false;
    uint16_t t = ((const uint16_t*)tm->cv.data)[tm->cv.w * ty + tx];
    for (int i = 0; i < nwalls; i++)
        if (walls[i] == t) return true;
    return false;
}

// One axis: `pos`/`size` along the moving axis, tiles cross_start..cross_end
// on the other; x_primary says which axis is moving.
static float collide_axis(const px_tilemap* tm, float pos, float size, float delta,
                          int cross_start, int cross_end, bool x_primary,
                          const uint16_t* walls, int nwalls) {
    if (delta == 0.0f) return delta;
    const float ts = (float)PX_TILE_SIZE;
    if (delta > 0.0f) {
        float cur_edge = pos + size - 1.0f;
        float new_edge = cur_edge + delta;
        int start = (int)floorf(cur_edge / ts) + 1;
        int end = (int)floorf(new_edge / ts);
        for (int primary = start; primary <= end; primary++)
            for (int cross = cross_start; cross <= cross_end; cross++)
                if (is_wall(tm, x_primary ? primary : cross, x_primary ? cross : primary, walls, nwalls))
                    return (float)primary * ts - size - pos;
    } else {
        float cur_edge = pos;
        float new_edge = cur_edge + delta;
        int start = (int)floorf(cur_edge / ts) - 1;
        int end = (int)floorf(new_edge / ts);
        for (int primary = start; primary >= end; primary--)
            for (int cross = cross_start; cross <= cross_end; cross++)
                if (is_wall(tm, x_primary ? primary : cross, x_primary ? cross : primary, walls, nwalls))
                    return (float)(primary + 1) * ts - pos;
    }
    return delta;
}

static float collide_x(const px_tilemap* tm, float x, float y, float w, float h, float dx,
                       const uint16_t* walls, int nwalls) {
    const float ts = (float)PX_TILE_SIZE;
    int ty0 = (int)floorf(y / ts), ty1 = (int)floorf((y + h - 1.0f) / ts);
    return collide_axis(tm, x, w, dx, ty0, ty1, true, walls, nwalls);
}

static float collide_y(const px_tilemap* tm, float x, float y, float w, float h, float dy,
                       const uint16_t* walls, int nwalls) {
    const float ts = (float)PX_TILE_SIZE;
    int tx0 = (int)floorf(x / ts), tx1 = (int)floorf((x + w - 1.0f) / ts);
    return collide_axis(tm, y, h, dy, tx0, tx1, false, walls, nwalls);
}

void px_tilemap_collide(const px_tilemap* tm, float x, float y, float w, float h,
                        float* dx, float* dy, const uint16_t* walls, int nwalls) {
    float ndx = *dx, ndy = *dy;
    if (fabsf(*dx) >= fabsf(*dy)) {
        ndx = collide_x(tm, x, y, w, h, ndx, walls, nwalls);
        ndy = collide_y(tm, x + ndx, y, w, h, ndy, walls, nwalls);
    } else {
        ndy = collide_y(tm, x, y, w, h, ndy, walls, nwalls);
        ndx = collide_x(tm, x, y + ndy, w, h, ndx, walls, nwalls);
    }
    *dx = ndx;
    *dy = ndy;
}
