/* SDL3 frontend for the recompiled game. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/recomp.h"

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
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_AudioStream *audio;
    SDL_Gamepad *gamepad;
    uint64_t next_frame_ns, frame_ns;
    /* scripted input: sorted (frame, buttons) changes */
    long *script_frame;
    uint8_t *script_buttons;
    size_t script_len, script_pos;
} G;

static void to_rgb(uint32_t *dst) {
    const uint8_t *fb = nes_ppu_framebuffer();
    for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) dst[i] = 0xFF000000u | PALETTE[fb[i] & 0x3F];
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

void nes_frame_callback(void) {
    long frame = (long)nes_ppu_frame_count();

    if (G.script_len) {
        while (G.script_pos < G.script_len && G.script_frame[G.script_pos] <= frame)
            nes_pad[0] = G.script_buttons[G.script_pos++];
    }

    if (G.headless) {
        int16_t sink[4096];
        while (nes_apu_take_samples(sink, 4096)) {}
    } else {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) nes_cpu_state.quit = true;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) nes_cpu_state.quit = true;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11)
                SDL_SetWindowFullscreen(G.window, !(SDL_GetWindowFlags(G.window) & SDL_WINDOW_FULLSCREEN));
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) {
                char name[64];
                snprintf(name, sizeof name, "screenshot-%ld.bmp", frame);
                save_screenshot(name);
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !G.gamepad)
                G.gamepad = SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && G.gamepad &&
                     SDL_GetGamepadID(G.gamepad) == e.gdevice.which) {
                SDL_CloseGamepad(G.gamepad);
                G.gamepad = NULL;
            }
        }
        if (!G.script_len) nes_pad[0] = read_keyboard();

        void *pixels;
        int pitch;
        if (SDL_LockTexture(G.texture, NULL, &pixels, &pitch)) {
            to_rgb(pixels); /* texture pitch == 256*4 */
            SDL_UnlockTexture(G.texture);
        }
        SDL_RenderClear(G.renderer);
        SDL_RenderTexture(G.renderer, G.texture, NULL, NULL);
        SDL_RenderPresent(G.renderer);

        int16_t buf[4096];
        size_t n;
        while ((n = nes_apu_take_samples(buf, 4096)) > 0)
            if (G.audio) SDL_PutAudioStreamData(G.audio, buf, (int)(n * sizeof(int16_t)));
        /* Keep audio latency bounded if we ever fall behind. */
        if (G.audio && SDL_GetAudioStreamQueued(G.audio) > 48000 / 4 * 2) SDL_ClearAudioStream(G.audio);

        /* Pace to the console's frame rate. */
        uint64_t now = SDL_GetTicksNS();
        if (G.next_frame_ns > now) SDL_DelayPrecise(G.next_frame_ns - now);
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
            "  --interpret         run everything in the interpreter (reference mode)\n"
            "  --trace FILE        record code the interpreter had to run (feeds the recompiler)\n"
            "  --region ntsc|pal   override the ROM header's region\n"
            "  --scale N           window scale (default 3)\n"
            "  --no-audio\n"
            "  --headless          no window/audio, run as fast as possible\n"
            "  --frames N          quit after N frames\n"
            "  --input FILE        scripted input: lines of '<frame> <buttons>' (ABsSUDLR)\n"
            "  --screenshot FILE   save a BMP of the last frame on exit\n");
}

static const char *find_rom(void) {
    static const char *candidates[] = {"rom/darkwing-duck.nes", "darkwing-duck.nes", "../rom/darkwing-duck.nes"};
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    nes_options opt = {0};
    const char *rom = NULL, *region = NULL;
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
        else if (a[0] == '-') { usage(); return 2; }
        else rom = a;
    }
    if (!rom) rom = find_rom();
    if (!rom) {
        fprintf(stderr, "error: ROM not found. Pass the path to your Darkwing Duck dump.\n");
        usage();
        return 1;
    }

    char err[256];
    if (!nes_cart_load(rom, err, sizeof err) || !nes_mapper_init(err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (nes_cart_state.prg_crc32 != nes_recomp_prg_crc32 && !opt.interpret_only) {
        fprintf(stderr, "warning: %s is not the ROM this build was recompiled from "
                        "(PRG CRC32 %08X, expected %08X); falling back to the interpreter.\n",
                rom, nes_cart_state.prg_crc32, nes_recomp_prg_crc32);
        opt.interpret_only = true;
    }
    opt.region = nes_cart_state.region;
    if (region) opt.region = !strcmp(region, "pal") ? REGION_PAL : REGION_NTSC;
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
        SDL_SetRenderLogicalPresentation(G.renderer, NES_WIDTH, NES_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        G.texture = SDL_CreateTexture(G.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                      NES_WIDTH, NES_HEIGHT);
        SDL_SetTextureScaleMode(G.texture, SDL_SCALEMODE_NEAREST);
        if (!G.no_audio) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, 48000};
            G.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (G.audio) SDL_ResumeAudioStreamDevice(G.audio);
            else fprintf(stderr, "warning: no audio: %s\n", SDL_GetError());
        }
    }

    nes_power_on(&opt);
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
    if (!G.headless) SDL_Quit();
    return 0;
}
