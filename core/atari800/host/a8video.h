/* The Atari's picture as XRGB8888 (video.c). Emulation thread only. */
#ifndef A8VIDEO_H
#define A8VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { A8VIDEO_AREA_NARROW = 0, A8VIDEO_AREA_TV, A8VIDEO_AREA_FULL };
enum { A8VIDEO_NONE = 0, A8VIDEO_XEP80, A8VIDEO_PROTO80, A8VIDEO_AF80, A8VIDEO_BIT3 };

typedef struct {
	int area;           /* A8VIDEO_AREA_* */
	int show_80col;     /* the 80-column card's picture, if one is fitted */
} a8video_opts;

/* Renders the last frame into dst (maxw x maxh). Returns 0 if it would not
   fit. par_out: pixel aspect x1000. */
int a8video_render(uint32_t *dst, int maxw, int maxh, int *w_out, int *h_out,
                   int *par_out, const a8video_opts *o);
int a8video_80col_card(void);
/* A point in the last rendered frame to Screen_atari coordinates; 0 while an
   80-column card's picture is shown. */
int a8video_to_screen(int fx, int fy, int *sx, int *sy);
uint32_t a8video_colour(int index);
void a8video_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
