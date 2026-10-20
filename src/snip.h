#ifndef CALLASSIST_SNIP_H
#define CALLASSIST_SNIP_H

#include <windows.h>
#include <stddef.h>

/*
 * Drag-select a region of the screen and hand back a PNG of it.
 *
 * The caller hides anything that must stay out of the shot (the overlay); the
 * selection chrome takes itself down before the pixels are read, so it never
 * lands in the image either.
 *
 * Returns 0 and fills *png / *png_len (caller frees) on success, 1 if the user
 * cancelled, -1 on failure.
 */
int snip_region(HINSTANCE inst, unsigned char **png, size_t *png_len);

/* base64 of an arbitrary buffer; caller frees. NULL on failure. */
char *base64_encode(const unsigned char *data, size_t n);

#endif
