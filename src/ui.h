/* In-game options menu (Dear ImGui overlay). */
#ifndef UI_H
#define UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int volume;         /* 0-100 */
    bool integer_scale; /* only scale the picture by whole numbers (sharpest pixels) */
} ui_settings;

extern ui_settings ui_config;

bool ui_init(SDL_Window *window, SDL_Renderer *renderer);
void ui_shutdown(void); /* also saves settings */
void ui_process_event(const SDL_Event *e);
bool ui_is_open(void);
void ui_toggle(void);
void ui_draw(void); /* call after drawing the game, before SDL_RenderPresent */

#ifdef __cplusplus
}
#endif
#endif
