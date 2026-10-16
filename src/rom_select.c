#include "rom_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesrt/recomp.h"
#include "ui.h"

#define ROM_NAME "darkwing-duck.nes"

const char *app_data_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        char *pref = SDL_GetPrefPath("darkwing-duck-recomp", "darkwing-duck");
        if (!pref) return NULL;
        snprintf(dir, sizeof dir, "%s", pref);
        SDL_free(pref);
    }
    return dir;
}

const char *rom_saved_path(void) {
    static char path[1100];
    if (!path[0]) {
        const char *dir = app_data_dir();
        if (!dir) return NULL;
        snprintf(path, sizeof path, "%s%s", dir, ROM_NAME);
    }
    return path;
}

static bool file_exists(const char *path) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) fclose(f);
    return f != NULL;
}

static char *dir_join(const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = SDL_malloc(n);
    size_t len = strlen(dir);
    snprintf(p, n, "%s%s%s", dir, len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/", name);
    return p;
}

char *rom_find(void) {
    const char *saved = rom_saved_path();
    if (file_exists(saved)) return SDL_strdup(saved);

    /* Next to the AppImage file (Steam Deck: just drop the ROM beside it). */
    const char *appimage = getenv("APPIMAGE");
    if (appimage) {
        char *dir = SDL_strdup(appimage), *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        char *p = dir_join(dir, ROM_NAME);
        SDL_free(dir);
        if (file_exists(p)) return p;
        SDL_free(p);
    }
    /* Next to the executable (and next to the .app bundle on macOS). */
    const char *base = SDL_GetBasePath();
    if (base) {
        const char *rel[] = {ROM_NAME, "../../../" ROM_NAME};
        for (int i = 0; i < 2; i++) {
            char *p = dir_join(base, rel[i]);
            if (file_exists(p)) return p;
            SDL_free(p);
        }
    }
    const char *dev[] = {"rom/" ROM_NAME, ROM_NAME};
    for (int i = 0; i < 2; i++)
        if (file_exists(dev[i])) return SDL_strdup(dev[i]);
    return NULL;
}

/* ---- Checking and saving ----------------------------------------------- */

typedef enum { ROM_OK, ROM_OTHER_VERSION, ROM_INVALID } rom_status;

static rom_status check_rom(const char *path, char *msg, size_t msglen) {
    size_t size;
    uint8_t *data = SDL_LoadFile(path, &size);
    if (!data) {
        snprintf(msg, msglen, "Couldn't read that file.");
        return ROM_INVALID;
    }
    rom_status st = ROM_INVALID;
    if (size < 16 || memcmp(data, "NES\x1a", 4) != 0) {
        snprintf(msg, msglen, "That file isn't an NES ROM (.nes).");
    } else {
        size_t prg = (size_t)data[4] * 0x4000, off = 16 + ((data[6] & 4) ? 512 : 0);
        if (prg == 0 || off + prg > size) {
            snprintf(msg, msglen, "That ROM file looks damaged (too short).");
        } else if (nes_crc32(data + off, prg) == nes_recomp_prg_crc32) {
            st = ROM_OK;
        } else {
            snprintf(msg, msglen,
                     "This isn't the version this build was made from (Darkwing Duck, Europe). "
                     "It can still run in compatibility mode: slower, and with cheats disabled.");
            st = ROM_OTHER_VERSION;
        }
    }
    SDL_free(data);
    return st;
}

static char *install_rom(const char *src, char *msg, size_t msglen) {
    size_t size;
    void *data = SDL_LoadFile(src, &size);
    const char *dst = rom_saved_path();
    if (!data || !dst || !SDL_SaveFile(dst, data, size)) {
        snprintf(msg, msglen, "Couldn't save a copy of the ROM: %s", SDL_GetError());
        SDL_free(data);
        return NULL;
    }
    SDL_free(data);
    return SDL_strdup(dst);
}

/* ---- Welcome screen ---------------------------------------------------- */

/* The file dialog may answer from another thread. */
static SDL_AtomicInt dialog_done;
static char *dialog_path;

static void SDLCALL dialog_callback(void *userdata, const char *const *files, int filter) {
    (void)userdata;
    (void)filter;
    dialog_path = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_SetAtomicInt(&dialog_done, 1);
}

char *rom_picker_run(SDL_Window *window, SDL_Renderer *renderer) {
    static const SDL_DialogFileFilter filters[] = {{"NES ROM", "nes"}, {"All files", "*"}};
    char msg[512] = "";
    char *pending = NULL; /* a valid ROM of another version, awaiting "use anyway" */
    bool warn = false;

    const char *dir = rom_saved_path();
    char folder[1024] = "";
    if (dir) {
        snprintf(folder, sizeof folder, "%s", dir);
        char *slash = strrchr(folder, '/');
        if (!slash) slash = strrchr(folder, '\\');
        if (slash) slash[1] = 0;
    }

    for (;;) {
        char *candidate = NULL;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ui_process_event(&e);
            if (e.type == SDL_EVENT_QUIT) {
                SDL_free(pending);
                return NULL;
            }
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) candidate = SDL_strdup(e.drop.data);
        }
        if (SDL_GetAtomicInt(&dialog_done)) {
            SDL_SetAtomicInt(&dialog_done, 0);
            candidate = dialog_path;
            dialog_path = NULL;
        }

        if (candidate) {
            SDL_free(pending);
            pending = NULL;
            rom_status st = check_rom(candidate, msg, sizeof msg);
            if (st == ROM_OK) {
                char *saved = install_rom(candidate, msg, sizeof msg);
                SDL_free(candidate);
                if (saved) return saved;
                warn = false;
            } else {
                warn = st == ROM_OTHER_VERSION;
                if (warn) pending = candidate;
                else SDL_free(candidate);
            }
        }

        SDL_SetRenderDrawColor(renderer, 16, 12, 32, 255);
        SDL_RenderClear(renderer);
        switch (ui_rom_picker(msg, pending != NULL, warn, folder)) {
        case UI_PICK_BROWSE:
            SDL_ShowOpenFileDialog(dialog_callback, NULL, window, filters, 2, NULL, false);
            break;
        case UI_PICK_USE_ANYWAY: {
            char *saved = install_rom(pending, msg, sizeof msg);
            if (saved) {
                SDL_free(pending);
                return saved;
            }
            break;
        }
        case UI_PICK_OPEN_FOLDER: {
            char url[1100];
            snprintf(url, sizeof url, "file://%s", folder);
            SDL_OpenURL(url);
            break;
        }
        case UI_PICK_QUIT:
            SDL_free(pending);
            return NULL;
        default: break;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);

        /* A ROM copied into the data folder by hand also counts. */
        char scratch[512];
        if (file_exists(rom_saved_path()) && check_rom(rom_saved_path(), scratch, sizeof scratch) == ROM_OK) {
            SDL_free(pending);
            return SDL_strdup(rom_saved_path());
        }
    }
}
