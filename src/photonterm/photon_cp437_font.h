/* photon_cp437_font.h - IBM CP437 8x16 bitmap font
 *
 * 256 glyphs, each 16 rows of 8 pixels (MSB=leftmost pixel).
 * Original font: IBM PC ROM Bios font, public domain.
 */
#pragma once
#include <stdint.h>

/* photon_cp437_8x16[codepoint][row] - bit 7 = leftmost pixel */
extern const uint8_t photon_cp437_8x16[256][16];

/* Convert Unicode codepoint to CP437 byte index.
 * Returns the CP437 index (0-255), or 0x3f ('?') if not representable. */
uint8_t photon_unicode_to_cp437(uint32_t codepoint);
