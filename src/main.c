/* SDL3 frontend for the recompiled game. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/recomp.h"
#include "bezel.h"
#include "rom_select.h"
#include "ui.h"

static const uint32_t PALETTE[64] = {
    0x626262, 0x001FB2, 0x2404C8, 0x5200B2, 0x730076, 0x800024, 0x730B00, 0x522800,
    0x244400, 0x005700, 0x005C00, 0x005324, 0x003C76, 0x000000, 0x000000, 0x000000,
    0xABABAB, 0x0D57FF, 0x4B30FF, 0x8A13FF, 0xBC08D6, 0xD21269, 0xC72E00, 0x9D5400,
    0x607B00, 0x209800, 0x00A300, 0x009942, 0x007DB4, 0x000000, 0x000000, 0x000000,
    0xFFFFFF, 0x53AEFF, 0x9085FF, 0xD365FF, 0xFF57FF, 0xFF5DCF, 0xFF7757, 0xFA9E00,
    0xBDC700, 0x7AE700, 0x43F611, 0x26EF7E, 0x2CD5F6, 0x4E4E4E, 0x000000, 0x000000,
    0xFFFFFF, 0xB6E1FF, 0xCED1FF, 0xE9C3FF, 0xFFBCFF, 0xFFBDF4, 0xFFC6C3, 0xFFD59A,
    0xE9E681, 0xCEF481, 0xB6FB9A, 0xA9FAC3, 0xA9F0F4, 0xB8B8B8, 0x000000, 0x000000,
};

static struct {
    bool headless, no_audio;
    long max_frames;
    const char *screenshot;
    const char *input_script;
    FILE *ram_log;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    uint8_t interp_fb[NES_WIDTH * 2 * NES_HEIGHT * 2]; /* in-between frame, 2x resolution */
    uint8_t big_fb[NES_WIDTH * 2 * NES_HEIGHT * 2];    /* real frame, doubled */
    SDL_AudioStream *audio;
    SDL_Gamepad *gamepad;
    uint64_t next_frame_ns, frame_ns;
    bool hold_menu_buttons; /* ignore Select/Start until released after the menu combo */
    long save_at_frame;     /* --save-state-at (testing) */
    const char *save_at_path;
    /* scripted input: sorted (frame, buttons) changes */
    long *script_frame;
    uint8_t *script_buttons;
    size_t script_len, script_pos;
} G;

static void to_rgb(uint32_t *dst) {
    const uint8_t *fb = nes_ppu_framebuffer();
    for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) dst[i] = 0xFF000000u | PALETTE[fb[i] & 0x3F];
}

/* ---- Video filters (operate on NES palette indices, any input size) ---- */

static inline uint8_t px_at(const uint8_t *fb, int w, int h, int x, int y) {
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return fb[y * w + x];
}

static inline uint32_t rgb(uint8_t i) { return 0xFF000000u | PALETTE[i & 0x3F]; }

static void filter_scale2x(const uint8_t *fb, int w, int h, uint32_t *out, int pitch) {
    for (int y = 0; y < h; y++) {
        uint32_t *r0 = out + (y * 2) * pitch, *r1 = r0 + pitch;
        for (int x = 0; x < w; x++) {
            uint8_t B = px_at(fb, w, h, x, y - 1), D = px_at(fb, w, h, x - 1, y), E = fb[y * w + x],
                    F = px_at(fb, w, h, x + 1, y), H = px_at(fb, w, h, x, y + 1);
            uint8_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                e0 = D == B ? D : E;
                e1 = B == F ? F : E;
                e2 = D == H ? D : E;
                e3 = H == F ? F : E;
            }
            r0[x * 2] = rgb(e0); r0[x * 2 + 1] = rgb(e1);
            r1[x * 2] = rgb(e2); r1[x * 2 + 1] = rgb(e3);
        }
    }
}

static void filter_scale3x(const uint8_t *fb, int w, int h, uint32_t *out, int pitch) {
    for (int y = 0; y < h; y++) {
        uint32_t *r[3] = {out + (y * 3) * pitch, out + (y * 3 + 1) * pitch, out + (y * 3 + 2) * pitch};
        for (int x = 0; x < w; x++) {
            uint8_t A = px_at(fb, w, h, x - 1, y - 1), B = px_at(fb, w, h, x, y - 1),
                    C = px_at(fb, w, h, x + 1, y - 1), D = px_at(fb, w, h, x - 1, y), E = fb[y * w + x],
                    F = px_at(fb, w, h, x + 1, y), G = px_at(fb, w, h, x - 1, y + 1),
                    H = px_at(fb, w, h, x, y + 1), I = px_at(fb, w, h, x + 1, y + 1);
            uint8_t e[9] = {E, E, E, E, E, E, E, E, E};
            if (B != H && D != F) {
                e[0] = D == B ? D : E;
                e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                e[2] = B == F ? F : E;
                e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                e[6] = D == H ? D : E;
                e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                e[8] = H == F ? F : E;
            }
            for (int k = 0; k < 9; k++) r[k / 3][x * 3 + k % 3] = rgb(e[k]);
        }
    }
}

/* Always 768x720: three output rows per NES scanline, the last one darker. */
static void filter_crt(const uint8_t *fb, int w, int h, uint32_t *out, int pitch) {
    static const int level[3] = {256, 230, 110};
    for (int r = 0; r < NES_HEIGHT * 3; r++) {
        const uint8_t *src = fb + (r * h / (NES_HEIGHT * 3)) * w;
        int lv = level[r % 3];
        uint32_t *row = out + r * pitch;
        for (int c = 0; c < NES_WIDTH * 3; c++) {
            uint32_t v = PALETTE[src[c * w / (NES_WIDTH * 3)] & 0x3F];
            row[c] = 0xFF000000u | ((((v >> 16) & 0xFF) * lv >> 8) << 16) | ((((v >> 8) & 0xFF) * lv >> 8) << 8) |
                     ((v & 0xFF) * lv >> 8);
        }
    }
}

/* Convert a w x h frame through the selected filter into the streaming texture. */
static void upload(const uint8_t *fb, int w, int h) {
    int f = ui_config.filter, tw = w, th = h;
    if (f == FILTER_SCALE2X) { tw = w * 2; th = h * 2; }
    else if (f == FILTER_SCALE3X) { tw = w * 3; th = h * 3; }
    else if (f == FILTER_CRT) { tw = NES_WIDTH * 3; th = NES_HEIGHT * 3; }
    if (!G.texture || G.tex_w != tw || G.tex_h != th) {
        if (G.texture) SDL_DestroyTexture(G.texture);
        G.texture = SDL_CreateTexture(G.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, tw, th);
        G.tex_w = tw;
        G.tex_h = th;
    }
    SDL_SetTextureScaleMode(G.texture, f == FILTER_CRT ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(G.texture, NULL, &pixels, &pitch)) return;
    uint32_t *out = pixels;
    int p = pitch / 4;
    switch (f) {
    case FILTER_SCALE2X: filter_scale2x(fb, w, h, out, p); break;
    case FILTER_SCALE3X: filter_scale3x(fb, w, h, out, p); break;
    case FILTER_CRT: filter_crt(fb, w, h, out, p); break;
    default:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) out[y * p + x] = rgb(fb[y * w + x]);
    }
    SDL_UnlockTexture(G.texture);
}

/* Nearest-neighbour 2x copy, so real frames match the 2x interpolated ones. */
static void double_frame(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < NES_HEIGHT * 2; y++)
        for (int x = 0; x < NES_WIDTH * 2; x++) dst[y * NES_WIDTH * 2 + x] = src[(y / 2) * NES_WIDTH + x / 2];
}

static void wait_until(uint64_t t_ns) {
    uint64_t now = SDL_GetTicksNS();
    if (t_ns > now) SDL_DelayPrecise(t_ns - now);
}

static void save_screenshot(const char *path) {
    static uint32_t px[NES_WIDTH * NES_HEIGHT];
    to_rgb(px);
    SDL_Surface *s = SDL_CreateSurfaceFrom(NES_WIDTH, NES_HEIGHT, SDL_PIXELFORMAT_XRGB8888, px, NES_WIDTH * 4);
    if (s) {
        if (!SDL_SaveBMP(s, path)) fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
        SDL_DestroySurface(s);
    }
}

static uint8_t read_keyboard(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K]) b |= BTN_A;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) b |= BTN_B;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= BTN_SELECT;
    if (k[SDL_SCANCODE_RETURN]) b |= BTN_START;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) b |= BTN_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) b |= BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) b |= BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) b |= BTN_RIGHT;
    if (G.gamepad) {
        SDL_Gamepad *g = G.gamepad;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) b |= BTN_A;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST) || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) b |= BTN_B;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK)) b |= BTN_SELECT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) b |= BTN_START;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= BTN_UP;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= BTN_DOWN;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= BTN_LEFT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= BTN_RIGHT;
        int16_t ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -16000) b |= BTN_LEFT;
        if (ax > 16000) b |= BTN_RIGHT;
        if (ay < -16000) b |= BTN_UP;
        if (ay > 16000) b |= BTN_DOWN;
    }
    /* Opposite directions at once confuse many games. */
    if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= ~(BTN_LEFT | BTN_RIGHT);
    if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN)) b &= ~(BTN_UP | BTN_DOWN);
    return b;
}

/* ---- Save states (run at a safe point between instructions) ---------- */

static char state_file[1200];

static void do_save_state(void *user) {
    (void)user;
    char err[256], msg[300];
    if (nes_state_save(state_file, err, sizeof err))
        snprintf(msg, sizeof msg, "Saved to slot %d", ui_config.state_slot);
    else
        snprintf(msg, sizeof msg, "Save failed: %s", err);
    if (G.headless) fprintf(stderr, "%s\n", msg);
    else ui_toast(msg);
}

static void do_load_state(void *user) {
    (void)user;
    char err[256], msg[300];
    if (nes_state_load(state_file, err, sizeof err)) {
        snprintf(msg, sizeof msg, "Loaded slot %d", ui_config.state_slot);
        G.next_frame_ns = SDL_GetTicksNS() + G.frame_ns;
        if (G.audio) SDL_ClearAudioStream(G.audio);
    } else {
        snprintf(msg, sizeof msg, "Can't load slot %d: %s", ui_config.state_slot, err);
    }
    if (G.headless) fprintf(stderr, "%s\n", msg);
    else ui_toast(msg);
}

static void request_state(int req) {
    ui_state_path(ui_config.state_slot, state_file, sizeof state_file);
    nes_at_safe_point(req == UI_REQ_SAVE ? do_save_state : do_load_state, NULL);
}

static void handle_events(long frame) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ui_process_event(&e);
        if (e.type == SDL_EVENT_QUIT) {
            nes_cpu_state.quit = true;
        } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            switch (e.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_F1: ui_toggle(); break;
            case SDL_SCANCODE_F11:
                SDL_SetWindowFullscreen(G.window, !(SDL_GetWindowFlags(G.window) & SDL_WINDOW_FULLSCREEN));
                break;
            case SDL_SCANCODE_F5: request_state(UI_REQ_SAVE); break;
            case SDL_SCANCODE_F9: request_state(UI_REQ_LOAD); break;
            case SDL_SCANCODE_F6: {
                char msg[32];
                ui_config.state_slot = ui_config.state_slot % UI_STATE_SLOTS + 1;
                snprintf(msg, sizeof msg, "Slot %d", ui_config.state_slot);
                ui_toast(msg);
                break;
            }
            case SDL_SCANCODE_F12: {
                char name[64];
                snprintf(name, sizeof name, "screenshot-%ld.bmp", frame);
                save_screenshot(name);
                break;
            }
            default: break;
            }
        } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            ui_toggle();
        } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && G.gamepad &&
                   ((e.gbutton.button == SDL_GAMEPAD_BUTTON_START &&
                     SDL_GetGamepadButton(G.gamepad, SDL_GAMEPAD_BUTTON_BACK)) ||
                    (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK &&
                     SDL_GetGamepadButton(G.gamepad, SDL_GAMEPAD_BUTTON_START)))) {
            /* Select+Start: the Steam Deck reserves the Guide button. */
            ui_toggle();
            G.hold_menu_buttons = true;
        } else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !G.gamepad) {
            G.gamepad = SDL_OpenGamepad(e.gdevice.which);
        } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && G.gamepad && SDL_GetGamepadID(G.gamepad) == e.gdevice.which) {
            SDL_CloseGamepad(G.gamepad);
            G.gamepad = NULL;
        }
    }
}

/* Draw the last game frame letterboxed in the window, plus the menu. */
static void present(void) {
    int w, h;
    SDL_GetCurrentRenderOutputSize(G.renderer, &w, &h);
    float scale = SDL_min((float)w / NES_WIDTH, (float)h / NES_HEIGHT);
    if (ui_config.integer_scale && scale >= 1.0f) scale = SDL_floorf(scale);
    SDL_FRect dst = {(w - NES_WIDTH * scale) / 2, (h - NES_HEIGHT * scale) / 2, NES_WIDTH * scale, NES_HEIGHT * scale};
    bezel_draw(G.renderer, ui_config.bezel, ui_config.bezel_fit, &dst, w, h);
    SDL_RenderTexture(G.renderer, G.texture, NULL, &dst);
    ui_draw();
    SDL_RenderPresent(G.renderer);
}

void nes_frame_callback(void) {
    long frame = (long)nes_ppu_frame_count();
    nes_ppu_set_overclock(ui_config.remove_slowdown);
    nes_ppu_set_unlimited_sprites(ui_config.no_flicker);
    if (G.save_at_path && frame == G.save_at_frame) {
        snprintf(state_file, sizeof state_file, "%s", G.save_at_path);
        nes_at_safe_point(do_save_state, NULL);
    }

    if (G.script_len) {
        while (G.script_pos < G.script_len && G.script_frame[G.script_pos] <= frame)
            nes_pad[0] = G.script_buttons[G.script_pos++];
    }

    if (G.ram_log) fwrite(nes_bus_state.ram, 1, sizeof nes_bus_state.ram, G.ram_log);

    if (G.headless) {
        int16_t sink[4096];
        while (nes_apu_take_samples(sink, 4096)) {}
    } else {
        handle_events(frame);
        if (!G.script_len) {
            uint8_t b = ui_is_open() ? 0 : read_keyboard();
            if (G.hold_menu_buttons) {
                if (!(read_keyboard() & (BTN_START | BTN_SELECT))) G.hold_menu_buttons = false;
                b &= (uint8_t)~(BTN_START | BTN_SELECT);
            }
            nes_pad[0] = b;
        }

        /* Frame interpolation: show a generated in-between frame first, then
         * the real one half a frame later (twice the frame rate). */
        if (ui_config.frame_interp && !ui_is_open() && nes_ppu_render_interpolated(G.interp_fb, 0.5f)) {
            upload(G.interp_fb, NES_WIDTH * 2, NES_HEIGHT * 2);
            present();
            wait_until(G.next_frame_ns - G.frame_ns / 2);
            double_frame(nes_ppu_framebuffer(), G.big_fb);
            upload(G.big_fb, NES_WIDTH * 2, NES_HEIGHT * 2);
        } else {
            upload(nes_ppu_framebuffer(), NES_WIDTH, NES_HEIGHT);
        }
        present();

        /* The menu pauses the game: keep drawing it until it is closed. */
        while (ui_is_open() && !nes_cpu_state.quit) {
            SDL_Delay(16);
            handle_events(frame);
            present();
        }
        if (G.next_frame_ns < SDL_GetTicksNS()) G.next_frame_ns = SDL_GetTicksNS();
        int req = ui_take_request();
        if (req != UI_REQ_NONE) request_state(req);

        int16_t buf[4096];
        size_t n;
        while ((n = nes_apu_take_samples(buf, 4096)) > 0) {
            if (!G.audio) continue;
            for (size_t i = 0; i < n; i++) buf[i] = (int16_t)(buf[i] * ui_config.volume / 100);
            SDL_PutAudioStreamData(G.audio, buf, (int)(n * sizeof(int16_t)));
        }
        /* Keep audio latency bounded if we ever fall behind. */
        if (G.audio && SDL_GetAudioStreamQueued(G.audio) > 48000 / 4 * 2) SDL_ClearAudioStream(G.audio);

        /* Pace to the console's frame rate. */
        uint64_t now = SDL_GetTicksNS();
        if (G.next_frame_ns > now) wait_until(G.next_frame_ns);
        else if (now - G.next_frame_ns > 100000000) G.next_frame_ns = now;
        G.next_frame_ns += G.frame_ns;
    }

    if (G.max_frames && frame >= G.max_frames) nes_cpu_state.quit = true;
}

static bool load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t cap = 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        long fr;
        char btn[32] = "";
        if (line[0] == '#' || sscanf(line, "%ld %31s", &fr, btn) < 1) continue;
        uint8_t b = 0;
        for (char *p = btn; *p; p++) {
            switch (*p) {
            case 'A': b |= BTN_A; break;
            case 'B': b |= BTN_B; break;
            case 's': b |= BTN_SELECT; break;
            case 'S': b |= BTN_START; break;
            case 'U': b |= BTN_UP; break;
            case 'D': b |= BTN_DOWN; break;
            case 'L': b |= BTN_LEFT; break;
            case 'R': b |= BTN_RIGHT; break;
            }
        }
        if (G.script_len == cap) {
            cap = cap ? cap * 2 : 64;
            G.script_frame = realloc(G.script_frame, cap * sizeof(long));
            G.script_buttons = realloc(G.script_buttons, cap);
        }
        G.script_frame[G.script_len] = fr;
        G.script_buttons[G.script_len++] = b;
    }
    fclose(f);
    return true;
}

static void usage(void) {
    fprintf(stderr,
            "usage: darkwing-duck [options] [rom.nes]\n"
            "  (without a ROM argument the remembered ROM is used, or you are asked once)\n"
            "  --interpret         run everything in the interpreter (reference mode)\n"
            "  --trace FILE        record code the interpreter had to run (feeds the recompiler)\n"
            "  --region ntsc|pal   override the ROM header's region\n"
            "  --scale N           window scale (default 3)\n"
            "  --no-audio\n"
            "  --headless          no window/audio, run as fast as possible\n"
            "  --frames N          quit after N frames\n"
            "  --input FILE        scripted input: lines of '<frame> <buttons>' (ABsSUDLR)\n"
            "  --screenshot FILE   save a BMP of the last frame on exit\n"
            "  --cheat ID[=N]      enable a cheat, e.g. invincible or stage_select=5\n"
            "  --overclock         remove slowdown (extra CPU time per frame)\n"
            "  --no-flicker        draw more than 8 sprites per line\n"
            "  --save-state-at N FILE   save a state at frame N\n"
            "  --load-state FILE   start from a save state\n"
            "  --watch ADDR        with --interpret: log instructions that change RAM byte ADDR (hex)\n"
            "  --ram-log FILE      append the 2 KiB of RAM every frame (for finding cheats)\n");
}

int main(int argc, char **argv) {
    nes_options opt = {.watch_addr = -1};
    const char *rom = NULL, *region = NULL, *load_state = NULL;
    int scale = 3;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--interpret")) opt.interpret_only = true;
        else if (!strcmp(a, "--trace") && more) opt.trace_path = argv[++i];
        else if (!strcmp(a, "--region") && more) region = argv[++i];
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--no-audio")) G.no_audio = true;
        else if (!strcmp(a, "--headless")) G.headless = true;
        else if (!strcmp(a, "--frames") && more) G.max_frames = atol(argv[++i]);
        else if (!strcmp(a, "--input") && more) G.input_script = argv[++i];
        else if (!strcmp(a, "--screenshot") && more) G.screenshot = argv[++i];
        else if (!strcmp(a, "--cheat") && more) {
            /* --cheat ID, or --cheat ID=N for cheats with choices (N = 1..choices) */
            char id[64];
            snprintf(id, sizeof id, "%s", argv[++i]);
            char *eq = strchr(id, '=');
            int value = 1;
            if (eq) { *eq = 0; value = atoi(eq + 1); }
            int k = 0;
            while (k < nes_cheat_count && strcmp(nes_cheat_defs[k].id, id)) k++;
            if (k == nes_cheat_count) { fprintf(stderr, "unknown cheat '%s'\n", id); return 2; }
            int max = nes_cheat_defs[k].choice_count ? nes_cheat_defs[k].choice_count : 1;
            if (value < 1 || value > max) { fprintf(stderr, "cheat '%s' takes 1..%d\n", id, max); return 2; }
            nes_cheats[k] = (uint8_t)value;
        }
        else if (!strcmp(a, "--overclock")) ui_config.remove_slowdown = true;
        else if (!strcmp(a, "--no-flicker")) ui_config.no_flicker = true;
        else if (!strcmp(a, "--load-state") && more) load_state = argv[++i];
        else if (!strcmp(a, "--save-state-at") && i + 2 < argc) {
            G.save_at_frame = atol(argv[++i]);
            G.save_at_path = argv[++i];
        }
        else if (!strcmp(a, "--watch") && more) opt.watch_addr = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(a, "--ram-log") && more) G.ram_log = fopen(argv[++i], "wb");
        else if (a[0] == '-') { usage(); return 2; }
        else rom = a;
    }
    if (G.input_script && !load_script(G.input_script)) {
        fprintf(stderr, "error: cannot read %s\n", G.input_script);
        return 1;
    }

    if (!G.headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (G.no_audio ? 0 : SDL_INIT_AUDIO))) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        if (!SDL_CreateWindowAndRenderer("Darkwing Duck", NES_WIDTH * scale, NES_HEIGHT * scale,
                                         SDL_WINDOW_RESIZABLE, &G.window, &G.renderer)) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return 1;
        }
        SDL_SetRenderVSync(G.renderer, 0);
        if (!ui_init(G.window, G.renderer)) fprintf(stderr, "warning: menu unavailable\n");
    }

    /* Command line, then the remembered copy / nearby files, then ask. */
    char *found = NULL;
    if (!rom) rom = found = rom_find();
    if (!rom && !G.headless) rom = found = rom_picker_run(G.window, G.renderer);
    if (!rom) {
        if (G.headless) {
            fprintf(stderr, "error: ROM not found. Pass the path to your Darkwing Duck dump.\n");
            usage();
        }
        return 1;
    }

    char err[256];
    if (!nes_cart_load(rom, err, sizeof err) || !nes_mapper_init(err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        if (!G.headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkwing Duck", err, G.window);
        return 1;
    }
    if (nes_cart_state.prg_crc32 != nes_recomp_prg_crc32) {
        fprintf(stderr, "warning: %s is not the ROM this build was recompiled from "
                        "(PRG CRC32 %08X, expected %08X); running in the interpreter, cheats disabled.\n",
                rom, nes_cart_state.prg_crc32, nes_recomp_prg_crc32);
        opt.interpret_only = true;
        ui_cheats_available = false; /* cheats patch addresses of the supported version */
        memset(nes_cheats, 0, (size_t)nes_cheat_count);
    }
    opt.region = nes_cart_state.region;
    if (region) opt.region = !strcmp(region, "pal") ? REGION_PAL : REGION_NTSC;

    if (!G.headless && !G.no_audio) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, 48000};
        G.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (G.audio) SDL_ResumeAudioStreamDevice(G.audio);
        else fprintf(stderr, "warning: no audio: %s\n", SDL_GetError());
    }

    nes_power_on(&opt);
    if (load_state) {
        char err[256];
        if (!nes_state_load(load_state, err, sizeof err)) {
            fprintf(stderr, "error: %s: %s\n", load_state, err);
            return 1;
        }
    }
    G.frame_ns = (uint64_t)(1e9 / nes_frame_hz());
    G.next_frame_ns = SDL_GetTicksNS() + G.frame_ns;
    nes_run();

    if (G.screenshot) save_screenshot(G.screenshot);
    nes_shutdown();
    nes_stats *s = &nes_stats_state;
    fprintf(stderr, "frames: %llu, recompiled entries: %llu, interpreted blocks: %llu (%llu instructions, %llu unique)\n",
            (unsigned long long)nes_ppu_frame_count(), (unsigned long long)s->recomp_entries,
            (unsigned long long)s->interp_blocks, (unsigned long long)s->interp_insns,
            (unsigned long long)s->unique_misses);
    if (!G.headless) {
        ui_shutdown();
        SDL_Quit();
    }
    return 0;
}
