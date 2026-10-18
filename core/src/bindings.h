/* The key/gamepad bindings table (bindings.c). */
#ifndef A800_BINDINGS_H
#define A800_BINDINGS_H

#include "a800session.h"

#ifdef __cplusplus
extern "C" {
#endif

void bindings_init(struct a800session *s);
int  session_binding_target_for_key(struct a800session *s, uint32_t keysym);

#ifdef __cplusplus
}
#endif

#endif
