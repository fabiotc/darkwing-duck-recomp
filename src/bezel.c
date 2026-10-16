#include "bezel.h"

#include <math.h>
#include <stdio.h>

#include "rom_select.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

static SDL_Texture *pattern_tex, *custom_tex;
static bool custom_tried;

/* ---- Built-in pattern: original 90s-style artwork drawn in code -------- */

#define TILE 256

typedef struct {
    float x, y, r;
} circle;

typedef struct {
    float x[3], y[3];
    uint32_t color;
} triangle;

static const circle CIRCLES[] = {{48, 52, 30}, {176, 132, 20}, {100, 200, 24}, {222, 30, 12}};
static const triangle TRIANGLES[] = {
    {{150, 196, 170}, {30, 44, 82}, 0x22E8D8},  /* cyan */
    {{160, 210, 196}, {44, 30, 92}, 0xF020C8},  /* magenta */
    {{20, 64, 40}, {150, 140, 112}, 0x22E8D8},
    {{210, 250, 236}, {190, 214, 240}, 0x7A3CF0}, /* violet */
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Signed distance-ish coverage of a point inside a triangle (anti-aliased). */
static float tri_cover(const triangle *t, float px, float py) {
    float cover = 1.0f;
    float area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->y[1] - t->y[0]) * (t->x[2] - t->x[0]);
    float sign = area < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) {
        float ax = t->x[i], ay = t->y[i], bx = t->x[(i + 1) % 3], by = t->y[(i + 1) % 3];
        float ex = bx - ax, ey = by - ay, len = sqrtf(ex * ex + ey * ey);
        float d = sign * (ex * (py - ay) - ey * (px - ax)) / len; /* > 0 inside */
        cover = fminf(cover, clamp01(d + 0.5f));
    }
    return cover;
}

static uint32_t blend(uint32_t dst, uint32_t src, float a) {
    uint32_t r = (uint32_t)(((dst >> 16) & 0xFF) * (1 - a) + ((src >> 16) & 0xFF) * a);
    uint32_t g = (uint32_t)(((dst >> 8) & 0xFF) * (1 - a) + ((src >> 8) & 0xFF) * a);
    uint32_t b = (uint32_t)((dst & 0xFF) * (1 - a) + (src & 0xFF) * a);
    return r << 16 | g << 8 | b;
}

static SDL_Texture *make_pattern(SDL_Renderer *renderer) {
    static uint32_t px[TILE * TILE];
    for (int y = 0; y < TILE; y++) {
        for (int x = 0; x < TILE; x++) {
            /* Navy background with darker zigzag stripes. */
            int zig = x % 32 < 16 ? x % 32 : 32 - x % 32;
            uint32_t c = ((y + zig) % 32 < 11) ? 0x14103E : 0x1C1A5C;
            /* Shapes, also drawn shifted by one tile so the pattern wraps seamlessly. */
            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    float fx = x + 0.5f + ox * TILE, fy = y + 0.5f + oy * TILE;
                    for (size_t i = 0; i < sizeof TRIANGLES / sizeof *TRIANGLES; i++) {
                        float a = tri_cover(&TRIANGLES[i], fx, fy);
                        if (a > 0) c = blend(c, TRIANGLES[i].color, a);
                    }
                    for (size_t i = 0; i < sizeof CIRCLES / sizeof *CIRCLES; i++) {
                        const circle *k = &CIRCLES[i];
                        float d = sqrtf((fx - k->x) * (fx - k->x) + (fy - k->y) * (fy - k->y));
                        float outline = clamp01(k->r + 3.0f - d + 0.5f);
                        if (outline > 0) c = blend(c, 0xFFFFFF, outline);
                        float fill = clamp01(k->r - d + 0.5f);
                        if (fill > 0) c = blend(c, 0xF8B800, fill); /* moon yellow */
                    }
                }
            }
            px[y * TILE + x] = 0xFF000000u | c;
        }
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom(TILE, TILE, SDL_PIXELFORMAT_XRGB8888, px, TILE * 4);
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(renderer, s) : NULL;
    SDL_DestroySurface(s);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureColorMod(t, 150, 150, 150); /* keep it subdued next to the game */
    }
    return t;
}

/* ---- Custom image from the data folder --------------------------------- */

static bool custom_path(char *out, size_t len) {
    const char *dir = app_data_dir();
    if (!dir) return false;
    const char *names[] = {"bezel.png", "bezel.jpg", "bezel.jpeg"};
    for (int i = 0; i < 3; i++) {
        snprintf(out, len, "%s%s", dir, names[i]);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_FILE) return true;
    }
    return false;
}

bool bezel_custom_available(void) {
    char path[1200];
    return custom_path(path, sizeof path);
}

static SDL_Texture *load_custom(SDL_Renderer *renderer) {
    char path[1200];
    if (!custom_path(path, sizeof path)) return NULL;
    size_t size;
    void *file = SDL_LoadFile(path, &size);
    if (!file) return NULL;
    int w, h, n;
    unsigned char *rgba = stbi_load_from_memory(file, (int)size, &w, &h, &n, 4);
    SDL_free(file);
    if (!rgba) return NULL;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(renderer, s) : NULL;
    SDL_DestroySurface(s);
    stbi_image_free(rgba);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureColorMod(t, 170, 170, 170);
    }
    return t;
}

/* ---- Drawing ------------------------------------------------------------ */

void bezel_draw(SDL_Renderer *renderer, int mode, int fit, const SDL_FRect *game, int w, int h) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (mode == BEZEL_BLACK) return;

    SDL_Texture *tex = NULL;
    if (mode == BEZEL_CUSTOM) {
        if (!custom_tried) {
            custom_tex = load_custom(renderer);
            custom_tried = true;
        }
        tex = custom_tex;
    }
    if (tex) {
        float tw, th;
        SDL_GetTextureSize(tex, &tw, &th);
        bool repeat = fit == BEZEL_FIT_REPEAT || (fit == BEZEL_FIT_AUTO && tw / th < (float)w / h);
        if (repeat) {
            /* Fit the height and repeat sideways, centred (good for patterns). */
            float s = h / th, step = tw * s;
            float x0 = fmodf((w - step) / 2, step);
            if (x0 > 0) x0 -= step;
            for (float x = x0; x < w; x += step) {
                SDL_FRect dst = {x, 0, step, (float)h};
                SDL_RenderTexture(renderer, tex, NULL, &dst);
            }
        } else {
            /* Cover the whole window, cropping what doesn't fit (good for artwork). */
            float s = SDL_max(w / tw, h / th);
            SDL_FRect dst = {(w - tw * s) / 2, (h - th * s) / 2, tw * s, th * s};
            SDL_RenderTexture(renderer, tex, NULL, &dst);
        }
    } else {
        if (!pattern_tex) pattern_tex = make_pattern(renderer);
        if (!pattern_tex) return;
        /* About three tiles per window height, whatever the resolution. */
        float scale = h / (3.0f * TILE);
        SDL_FRect all = {0, 0, (float)w, (float)h};
        SDL_RenderTextureTiled(renderer, pattern_tex, NULL, scale, &all);
    }

    /* A soft shadow so the picture sits "on top" of the artwork. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int i = 1; i <= 12; i++) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(110 / i));
        SDL_FRect r = {game->x - i, game->y - i, game->w + 2 * i, game->h + 2 * i};
        SDL_RenderRect(renderer, &r);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

/* Called when the player switches back to "custom" so a newly added file is picked up. */
void bezel_reload_custom(void) {
    if (custom_tex) SDL_DestroyTexture(custom_tex);
    custom_tex = NULL;
    custom_tried = false;
}
