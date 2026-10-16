/* Artwork shown around the game picture instead of black bars. */
#ifndef BEZEL_H
#define BEZEL_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BEZEL_PATTERN, BEZEL_CUSTOM, BEZEL_BLACK, BEZEL_COUNT };
/* How a custom image is laid out: Auto repeats images narrower than the
 * window and fills with wider ones. */
enum { BEZEL_FIT_AUTO, BEZEL_FIT_FILL, BEZEL_FIT_REPEAT, BEZEL_FIT_COUNT };

/* Fills the window behind the game rectangle `game` (in output pixels). */
void bezel_draw(SDL_Renderer *renderer, int mode, int fit, const SDL_FRect *game, int w, int h);

/* True if bezel.png / bezel.jpg exists in the data folder. */
bool bezel_custom_available(void);

/* Forget the loaded custom image, so a new or changed file is picked up. */
void bezel_reload_custom(void);

#ifdef __cplusplus
}
#endif
#endif
