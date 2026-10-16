/* Finding, checking and remembering the player's ROM. */
#ifndef ROM_SELECT_H
#define ROM_SELECT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's data folder (ROM copy, settings, save states), ending in a slash. */
const char *app_data_dir(void);

/* Where the selected ROM is kept, so it only has to be chosen once. */
const char *rom_saved_path(void);

/* Saved copy, then a ROM next to the app/AppImage, then ./rom/. NULL if none. */
char *rom_find(void);

/* First-launch screen: lets the player choose their ROM (file dialog or drag
 * and drop), checks it and saves a copy. Returns the saved path, or NULL if
 * the window was closed. */
char *rom_picker_run(SDL_Window *window, SDL_Renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif
