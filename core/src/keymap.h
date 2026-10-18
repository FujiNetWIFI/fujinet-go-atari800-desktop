/* Host keys to Atari keys (keymap.c). */
#ifndef A800_KEYMAP_H
#define A800_KEYMAP_H

#include <stdint.h>

#include "a800session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Translates a key going down; 1 and *akey set if it is an Atari key.
   positional: map by position (US layout) rather than character.
   fkeys_1200: the 1200XL's F1-F4 replace the cursor keys on the arrows. */
int keymap_translate(uint32_t keysym, uint32_t unicode, int shift, int ctrl,
                     int positional, int fkeys_1200, int *akey);
/* ASCII -> AKEY with the shift it needs, or AKEY_NONE. */
int keymap_char(uint32_t c);

#ifdef __cplusplus
}
#endif

#endif
