// Canvas primitives, instantiated by gfx.c for T = uint8_t (SUF u8) and
// T = uint16_t (SUF u16). Port of pyxel-core canvas.rs (MIT, Takashi Kitao).
// Expects T, SUF and PX_CAT to be defined by the includer.

#define FN(name) PX_CAT(name, SUF)

static inline T FN(px_read_)(const px_canvas* cv, int x, int y) {
    return ((const T*)cv->data)[cv->w * y + x];
}

static inline void FN(px_write_)(px_canvas* cv, int x, int y, T v) {
    if (px_should_write(cv, x, y)) ((T*)cv->data)[cv->w * y + x] = v;
}

void FN(px_write_clipped_)(px_canvas* cv, int x, int y, T v) {
    if (px_rect_contains(cv->clip, x, y) && px_should_write(cv, x, y))
        ((T*)cv->data)[cv->w * y + x] = v;
}

static void FN(px_fill_row_)(px_canvas* cv, int x1, int x2, int y, T v) {
    if (y < cv->clip.top || y > cv->clip.bottom) return;
    int left = x1 > cv->clip.left ? x1 : cv->clip.left;
    int right = x2 < cv->clip.right ? x2 : cv->clip.right;
    if (left > right) return;
    T* row = (T*)cv->data + cv->w * y;
    if (cv->alpha >= 1.0f) {
        for (int x = left; x <= right; x++) row[x] = v;
    } else {
        for (int x = left; x <= right; x++)
            if (px_should_write(cv, x, y)) row[x] = v;
    }
}

static void FN(px_fill_col_)(px_canvas* cv, int y1, int y2, int x, T v) {
    if (x < cv->clip.left || x > cv->clip.right) return;
    int top = y1 > cv->clip.top ? y1 : cv->clip.top;
    int bottom = y2 < cv->clip.bottom ? y2 : cv->clip.bottom;
    T* p = (T*)cv->data;
    for (int y = top; y <= bottom; y++)
        if (px_should_write(cv, x, y)) p[cv->w * y + x] = v;
}

void FN(px_clear_)(px_canvas* cv, T v) {
    T* p = (T*)cv->data;
    int n = cv->w * cv->h;
    for (int i = 0; i < n; i++) p[i] = v;
}

T FN(px_value_)(px_canvas* cv, float fx, float fy) {
    int x = px_f2i(fx), y = px_f2i(fy);
    if (px_rect_contains(cv->clip, x, y)) return FN(px_read_)(cv, x, y);
    return 0;
}

void FN(px_set_value_)(px_canvas* cv, float fx, float fy, T v) {
    FN(px_write_clipped_)(cv, px_f2i(fx) - cv->cam_x, px_f2i(fy) - cv->cam_y, v);
}

void FN(px_line_)(px_canvas* cv, float fx1, float fy1, float fx2, float fy2, T v) {
    int x1 = px_f2i(fx1) - cv->cam_x, y1 = px_f2i(fy1) - cv->cam_y;
    int x2 = px_f2i(fx2) - cv->cam_x, y2 = px_f2i(fy2) - cv->cam_y;
    if (y1 == y2) {
        FN(px_fill_row_)(cv, x1 < x2 ? x1 : x2, x1 < x2 ? x2 : x1, y1, v);
    } else if (x1 == x2) {
        FN(px_fill_col_)(cv, y1 < y2 ? y1 : y2, y1 < y2 ? y2 : y1, x1, v);
    } else if (abs(x1 - x2) > abs(y1 - y2)) {
        int sx = x1, sy = y1, ex = x2, ey = y2;
        if (x1 >= x2) { sx = x2; sy = y2; ex = x1; ey = y1; }
        int len = ex - sx + 1;
        float slope = (float)(ey - sy) / (float)(ex - sx);
        for (int i = 0; i < len; i++)
            FN(px_write_clipped_)(cv, sx + i, sy + px_f2i(slope * (float)i), v);
    } else {
        int sx = x1, sy = y1, ex = x2, ey = y2;
        if (y1 >= y2) { sx = x2; sy = y2; ex = x1; ey = y1; }
        int len = ey - sy + 1;
        float slope = (float)(ex - sx) / (float)(ey - sy);
        for (int i = 0; i < len; i++)
            FN(px_write_clipped_)(cv, sx + px_f2i(slope * (float)i), sy + i, v);
    }
}

void FN(px_rect_)(px_canvas* cv, float fx, float fy, float fw, float fh, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    px_rect r = px_rect_intersect(px_rect_new(x, y, px_f2u(fw), px_f2u(fh)), cv->clip);
    if (px_rect_empty(r)) return;
    T* p = (T*)cv->data;
    for (int yy = r.top; yy <= r.bottom; yy++) {
        T* row = p + cv->w * yy;
        if (cv->alpha >= 1.0f) {
            for (int xx = r.left; xx <= r.right; xx++) row[xx] = v;
        } else {
            for (int xx = r.left; xx <= r.right; xx++)
                if (px_should_write(cv, xx, yy)) row[xx] = v;
        }
    }
}

void FN(px_rectb_)(px_canvas* cv, float fx, float fy, float fw, float fh, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    px_rect r = px_rect_new(x, y, px_f2u(fw), px_f2u(fh));
    if (px_rect_empty(px_rect_intersect(r, cv->clip))) return;
    FN(px_fill_row_)(cv, r.left, r.right, r.top, v);
    FN(px_fill_row_)(cv, r.left, r.right, r.bottom, v);
    FN(px_fill_col_)(cv, r.top, r.bottom, r.left, v);
    FN(px_fill_col_)(cv, r.top, r.bottom, r.right, v);
}

void FN(px_circ_)(px_canvas* cv, float fx, float fy, float fr, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    int radius = px_f2u(fr);
    float r = (float)radius;
    for (int xi = 0; xi <= radius; xi++) {
        int x1, y1, x2, y2;
        px_ellipse_area(0.0f, 0.0f, r, r, xi, &x1, &y1, &x2, &y2);
        FN(px_fill_col_)(cv, y + y1, y + y2, x + x1, v);
        FN(px_fill_col_)(cv, y + y1, y + y2, x + x2, v);
        FN(px_fill_row_)(cv, x + y1, x + y2, y + x1, v);
        FN(px_fill_row_)(cv, x + y1, x + y2, y + x2, v);
    }
}

void FN(px_circb_)(px_canvas* cv, float fx, float fy, float fr, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    int radius = px_f2u(fr);
    float r = (float)radius;
    for (int xi = 0; xi <= radius; xi++) {
        int x1, y1, x2, y2;
        px_ellipse_area(0.0f, 0.0f, r, r, xi, &x1, &y1, &x2, &y2);
        FN(px_write_clipped_)(cv, x + x1, y + y1, v);
        FN(px_write_clipped_)(cv, x + x2, y + y1, v);
        FN(px_write_clipped_)(cv, x + x1, y + y2, v);
        FN(px_write_clipped_)(cv, x + x2, y + y2, v);
        FN(px_write_clipped_)(cv, x + y1, y + x1, v);
        FN(px_write_clipped_)(cv, x + y1, y + x2, v);
        FN(px_write_clipped_)(cv, x + y2, y + x1, v);
        FN(px_write_clipped_)(cv, x + y2, y + x2, v);
    }
}

void FN(px_elli_)(px_canvas* cv, float fx, float fy, float fw, float fh, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    int w = px_f2u(fw), h = px_f2u(fh);
    if (w == 0 || h == 0) return;
    float ra = (float)(w - 1) / 2.0f, rb = (float)(h - 1) / 2.0f;
    float cx = (float)x + ra, cy = (float)y + rb;
    for (int xi = x; xi <= x + w / 2; xi++) {
        int x1, y1, x2, y2;
        px_ellipse_area(cx, cy, ra, rb, xi, &x1, &y1, &x2, &y2);
        FN(px_fill_col_)(cv, y1, y2, x1, v);
        FN(px_fill_col_)(cv, y1, y2, x2, v);
    }
    for (int yi = y; yi <= y + h / 2; yi++) {
        int y1, x1, y2, x2;
        px_ellipse_area(cy, cx, rb, ra, yi, &y1, &x1, &y2, &x2);
        FN(px_fill_row_)(cv, x1, x2, y1, v);
        FN(px_fill_row_)(cv, x1, x2, y2, v);
    }
}

void FN(px_ellib_)(px_canvas* cv, float fx, float fy, float fw, float fh, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    int w = px_f2u(fw), h = px_f2u(fh);
    if (w == 0 || h == 0) return;
    float ra = (float)(w - 1) / 2.0f, rb = (float)(h - 1) / 2.0f;
    float cx = (float)x + ra, cy = (float)y + rb;
    for (int xi = x; xi <= x + w / 2; xi++) {
        int x1, y1, x2, y2;
        px_ellipse_area(cx, cy, ra, rb, xi, &x1, &y1, &x2, &y2);
        FN(px_write_clipped_)(cv, x1, y1, v);
        FN(px_write_clipped_)(cv, x2, y1, v);
        FN(px_write_clipped_)(cv, x1, y2, v);
        FN(px_write_clipped_)(cv, x2, y2, v);
    }
    for (int yi = y; yi <= y + h / 2; yi++) {
        int y1, x1, y2, x2;
        px_ellipse_area(cy, cx, rb, ra, yi, &y1, &x1, &y2, &x2);
        FN(px_write_clipped_)(cv, x1, y1, v);
        FN(px_write_clipped_)(cv, x2, y1, v);
        FN(px_write_clipped_)(cv, x1, y2, v);
        FN(px_write_clipped_)(cv, x2, y2, v);
    }
}

void FN(px_tri_)(px_canvas* cv, float fx1, float fy1, float fx2, float fy2,
                 float fx3, float fy3, T v) {
    int x1 = px_f2i(fx1) - cv->cam_x, y1 = px_f2i(fy1) - cv->cam_y;
    int x2 = px_f2i(fx2) - cv->cam_x, y2 = px_f2i(fy2) - cv->cam_y;
    int x3 = px_f2i(fx3) - cv->cam_x, y3 = px_f2i(fy3) - cv->cam_y;
    int t;
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y1 > y3) { t = y1; y1 = y3; y3 = t; t = x1; x1 = x3; x3 = t; }
    if (y2 > y3) { t = y2; y2 = y3; y3 = t; t = x2; x2 = x3; x3 = t; }
    if (y1 == y3) {
        int lo = x1, hi = x1;
        if (x2 < lo) lo = x2;
        if (x3 < lo) lo = x3;
        if (x2 > hi) hi = x2;
        if (x3 > hi) hi = x3;
        FN(px_fill_row_)(cv, lo, hi, y1, v);
        return;
    }
    float s12 = y2 == y1 ? 0.0f : (float)(x2 - x1) / (float)(y2 - y1);
    float s13 = y3 == y1 ? 0.0f : (float)(x3 - x1) / (float)(y3 - y1);
    float s23 = y3 == y2 ? 0.0f : (float)(x3 - x2) / (float)(y3 - y2);
    int x_split = px_f2i((float)x1 + s13 * (float)(y2 - y1));
    for (int y = y1; y <= y2; y++) {
        int a = px_f2i((float)x_split + s13 * (float)(y - y2));
        int b = px_f2i((float)x2 + s12 * (float)(y - y2));
        if (x_split < x2) FN(px_fill_row_)(cv, a, b, y, v);
        else              FN(px_fill_row_)(cv, b, a, y, v);
    }
    for (int y = y2 + 1; y <= y3; y++) {
        int a = px_f2i((float)x_split + s13 * (float)(y - y2));
        int b = px_f2i((float)x2 + s23 * (float)(y - y2));
        if (x_split < x2) FN(px_fill_row_)(cv, a, b, y, v);
        else              FN(px_fill_row_)(cv, b, a, y, v);
    }
}

void FN(px_trib_)(px_canvas* cv, float x1, float y1, float x2, float y2,
                  float x3, float y3, T v) {
    FN(px_line_)(cv, x1, y1, x2, y2, v);
    FN(px_line_)(cv, x1, y1, x3, y3, v);
    FN(px_line_)(cv, x2, y2, x3, y3, v);
}

void FN(px_fill_)(px_canvas* cv, float fx, float fy, T v) {
    int x = px_f2i(fx) - cv->cam_x, y = px_f2i(fy) - cv->cam_y;
    if (!px_rect_contains(cv->clip, x, y)) return;
    T dst = FN(px_read_)(cv, x, y);
    if (v == dst) return;
    int cap = 64, n = 0;
    int* stack = (int*)malloc(sizeof(int) * 2 * cap);
    if (!stack) return;
    stack[n++] = x; stack[n++] = y;
    while (n > 0) {
        int sy = stack[--n], sx = stack[--n];
        if (!px_rect_contains(cv->clip, sx, sy) || FN(px_read_)(cv, sx, sy) != dst) continue;
        int left = sx, right = sx;
        while (left > cv->clip.left && FN(px_read_)(cv, left - 1, sy) == dst) left--;
        while (right < cv->clip.right && FN(px_read_)(cv, right + 1, sy) == dst) right++;
        if (cv->alpha >= 1.0f) {
            T* row = (T*)cv->data + cv->w * sy;
            for (int xi = left; xi <= right; xi++) row[xi] = v;
        } else {
            for (int xi = left; xi <= right; xi++) FN(px_write_)(cv, xi, sy, v);
        }
        for (int k = 0; k < 2; k++) {
            int scan_y = k == 0 ? sy - 1 : sy + 1;
            if (scan_y < cv->clip.top || scan_y > cv->clip.bottom) continue;
            bool in_seg = false;
            for (int scan_x = left; scan_x <= right; scan_x++) {
                bool target = FN(px_read_)(cv, scan_x, scan_y) == dst;
                if (target && !in_seg) {
                    if (n + 2 > cap * 2) {
                        cap *= 2;
                        int* grown = (int*)realloc(stack, sizeof(int) * 2 * cap);
                        if (!grown) { free(stack); return; }
                        stack = grown;
                    }
                    stack[n++] = scan_x; stack[n++] = scan_y;
                    in_seg = true;
                } else if (!target) {
                    in_seg = false;
                }
            }
        }
    }
    free(stack);
}

void FN(px_blit_)(px_canvas* dst, float fx, float fy, const px_canvas* src, float fsx, float fsy,
                  float fw, float fh, int colkey, const T* pal) {
    int x = px_f2i(fx) - dst->cam_x, y = px_f2i(fy) - dst->cam_y;
    px_copy_area a;
    px_copy_area_new(&a, x, y, dst->clip, px_f2i(fsx), px_f2i(fsy), src->self_rect,
                     px_f2i(fw), px_f2i(fh));
    if (a.width == 0 || a.height == 0) return;
    const T* sp = (const T*)src->data;
    T* dp = (T*)dst->data;
    bool plain = colkey < 0 && pal == NULL;
    for (int yi = 0; yi < a.height; yi++) {
        int sy = a.src_y + a.sign_y * yi + a.offset_y;
        int dy = a.dst_y + yi;
        const T* srow = sp + src->w * sy;
        T* drow = dp + dst->w * dy;
        if (plain && a.sign_x == 1 && dst->alpha >= 1.0f) {
            memcpy(drow + a.dst_x, srow + a.src_x, sizeof(T) * (size_t)a.width);
            continue;
        }
        for (int xi = 0; xi < a.width; xi++) {
            int sx = a.src_x + a.sign_x * xi + a.offset_x;
            int dx = a.dst_x + xi;
            T val = srow[sx];
            if (!plain) {
                if (colkey >= 0 && val == (T)colkey) continue;
                if (pal) val = pal[val];
            }
            if (dst->alpha >= 1.0f || px_should_write(dst, dx, dy)) drow[dx] = val;
        }
    }
}

void FN(px_blit_xform_)(px_canvas* dst, float x, float y, const px_canvas* src, float sx, float sy,
                        float w, float h, int colkey, const T* pal, float rotate, float scale) {
    px_xform p;
    if (!px_xform_new(&p, x, y, sx, sy, w, h, dst->cam_x, dst->cam_y, rotate, scale, dst->clip))
        return;
    px_rect area = px_rect_intersect(px_rect_new(p.src_x, p.src_y, p.width, p.height),
                                     src->self_rect);
    if (px_rect_empty(area)) return;
    float step_sx = p.sign_x * p.cos_s, step_sy = p.sign_x * p.sin_s;
    const T* sp = (const T*)src->data;
    T* dp = (T*)dst->data;
    for (int yi = p.y1; yi <= p.y2; yi++) {
        float ox = ((float)p.x1 - p.dst_cx) * p.sign_x;
        float oy = ((float)yi - p.dst_cy) * p.sign_y;
        float fsx = p.src_cx + ox * p.cos_s - oy * p.sin_s;
        float fsy = p.src_cy + ox * p.sin_s + oy * p.cos_s;
        for (int xi = p.x1; xi <= p.x2; xi++) {
            int vx = px_f2i(fsx), vy = px_f2i(fsy);
            fsx += step_sx;
            fsy += step_sy;
            if (!px_rect_contains(area, vx, vy)) continue;
            T val = sp[src->w * vy + vx];
            if (colkey >= 0 && val == (T)colkey) continue;
            if (pal) val = pal[val];
            if (dst->alpha >= 1.0f || px_should_write(dst, xi, yi)) dp[dst->w * yi + xi] = val;
        }
    }
}

#undef FN
