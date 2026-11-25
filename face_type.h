#pragma once

#include <stddef.h>
#include <stdint.h>

// All coordinates refer to the unrotated 466 × 466 RGB565 canvas. The y
// coordinate aligns an entire line, including the ascenders of its digits.
namespace face_type {
enum Size { Small, Text, Medium, Large, Condition };
enum Align { Left, Center, Right };

struct Glyph {
  uint32_t offset;
  uint16_t code;
  uint8_t width, height, advance;
  int8_t left, top;
};

struct Font {
  const Glyph* glyphs;
  const uint8_t* runs;
  size_t count;
};

#include "face_glyphs.h"

inline uint32_t nextCode(const char*& text) {
  uint8_t first = static_cast<uint8_t>(*text++);
  if (first < 0x80) return first;
  int remaining = first < 0xC2 ? 0 : first < 0xE0 ? 1 : first < 0xF0 ? 2 : 3;
  uint32_t code = first & (0x7F >> (remaining + 1));
  if (!remaining || first > 0xF4) return '?';
  while (remaining--) {
    uint8_t next = static_cast<uint8_t>(*text);
    if ((next & 0xC0) != 0x80) return '?';
    code = (code << 6) | (next & 0x3F);
    ++text;
  }
  return code;
}

inline const Glyph* find(const Font& font, uint32_t code) {
  size_t low = 0, high = font.count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (font.glyphs[mid].code < code) low = mid + 1;
    else high = mid;
  }
  return low < font.count && font.glyphs[low].code == code ? &font.glyphs[low] : nullptr;
}

inline int measure(const char* text, Size size) {
  int width = 0;
  const Font& font = fonts[size];
  while (*text) {
    const Glyph* glyph = find(font, nextCode(text));
    if (glyph) width += glyph->advance;
  }
  return width;
}

inline uint16_t mix(uint16_t bg, uint16_t fg, unsigned alpha) {
  unsigned inverse = 15 - alpha;
  unsigned red = ((bg >> 11) * inverse + (fg >> 11) * alpha + 7) / 15;
  unsigned green = (((bg >> 5) & 63) * inverse + ((fg >> 5) & 63) * alpha + 7) / 15;
  unsigned blue = ((bg & 31) * inverse + (fg & 31) * alpha + 7) / 15;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

inline void draw(uint16_t* fb, int x, int top, const char* text, Size size,
                 uint16_t color, Align align = Left) {
  if (!fb || !text) return;
  if (align != Left) x -= measure(text, size) / (align == Center ? 2 : 1);
  const Font& font = fonts[size];
  while (*text) {
    const Glyph* glyph = find(font, nextCode(text));
    if (!glyph) continue;
    const uint8_t* data = font.runs + glyph->offset;
    int startX = x + glyph->left;
    for (int row = 0; row < glyph->height; ++row) {
      int y = top + glyph->top + row;
      int col = 0;
      while (col < glyph->width) {
        uint8_t run = *data++;
        int stop = col + (run & 15) + 1;
        unsigned alpha = run >> 4;
        if (alpha && static_cast<unsigned>(y) < 466) {
          int from = startX + col < 0 ? 0 : startX + col;
          int to = startX + stop > 466 ? 466 : startX + stop;
          for (int px = from; px < to; ++px) {
            uint16_t& pixel = fb[y * 466 + px];
            pixel = alpha == 15 ? color : mix(pixel, color, alpha);
          }
        }
        col = stop;
      }
    }
    x += glyph->advance;
  }
}
}  // namespace face_type
