/* photon_terminus_font.h - Embedded Terminus TTF font data
 *
 * Terminus TTF 4.49.3 - SIL Open Font License 1.1
 * https://files.ax86.net/terminus-ttf/
 *
 * Embedded as a C array for platforms without a bundled font file.
 * Use with TTF_OpenFontRW(SDL_RWFromConstMem(data, size), ...).
 */
#ifndef PHOTON_TERMINUS_FONT_H
#define PHOTON_TERMINUS_FONT_H

#include <stddef.h>

extern const unsigned char photon_terminus_ttf[];
extern const size_t        photon_terminus_ttf_size;

#endif /* PHOTON_TERMINUS_FONT_H */
