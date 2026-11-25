#pragma once

namespace clock_face {
namespace reference_art {
#include "src/reference_art/black.h"
#include "src/reference_art/white.h"
#include "src/reference_art/green.h"
}

inline void referenceDialBase(uint8_t face) {
  const char* image = face == ClockExplorer ? reference_art::black :
                      face == ClockMoonwatch ? reference_art::white : reference_art::green;
  const uint8_t* src = reinterpret_cast<const uint8_t*>(image);
  uint16_t* out = fb;
  const uint16_t* end = fb + size*size;
  while (out < end) {
    const uint8_t tag = *src++;
    const int count = (tag & 127)+1;
    if (tag & 128) {
      const uint16_t pixel = src[0] | (uint16_t(src[1]) << 8);
      src += 2;
      for (int i=0; i<count; ++i) *out++ = pixel;
    } else {
      memcpy(out,src,count*sizeof(uint16_t));
      out += count;
      src += count*sizeof(uint16_t);
    }
  }
}
}  // namespace clock_face
