#pragma once

namespace clock_face {
namespace reference_data {

#include "src/reference_readout/glyphs.h"

// The original 607 x 611 source crop is centered at (1679.5,377.5).
// Removing the outer bezel expands its dial radius from 203 to 233 pixels.
constexpr float greenZoom=233.f/203.f;
constexpr float sourcePerX=607.f/(466*greenZoom), sourcePerY=611.f/(466*greenZoom);
static constexpr int greenCoord(int old) { return int(233.f+(old-232.5f)*greenZoom); }

static void dateDigit(int cx, char digit) {
  const Glyph* glyph=find(dateType,digit);
  if (!glyph) return;
  const int x=cx-glyph->advance/2+glyph->left;
  const uint8_t* run=dateType.runs+glyph->offset;
  constexpr uint16_t ink=rgb(12,12,12);
  for (int y=0; y<glyph->height; ++y) {
    uint16_t* pixel=fb+(greenCoord(330)+glyph->top+y)*size+x;
    for (int col=0; col<glyph->width;) {
      const int value=*run++, count=(value&15)+1, alpha=value>>4;
      if (alpha) for (int i=0; i<count; ++i)
        pixel[i]=alpha==15 ? ink : mix(pixel[i],ink,alpha);
      col+=count;
      pixel+=count;
    }
  }
}

// Only the engraved silver moon is shaded. Coordinates trace its silhouette on the original artwork.
static void moon(const tm& t) {
  struct Edge { int y,left,right; };
  static constexpr Edge margin[]={
    {216,1817,1834},{218,1819,1848},{224,1830,1861},{233,1837,1873},
    {245,1841,1883},{255,1841,1890},{265,1838,1894},{275,1833,1897},
    {277,1835,1898},{286,1850,1899},{291,1855,1899},{293,1871,1899},
    {300,1881,1898},{309,1886,1896},{317,1888,1893},{324,1889,1890}
  };
  tm local=t;
  const double elapsed=(double(mktime(&local))-947182440.)/(29.53059*86400.);
  const float angle=float((elapsed-floor(elapsed))*6.283185307);
  const float sx=sinf(angle), sz=-cosf(angle);
  int span=0;
  for (int y=greenCoord(108); y<=greenCoord(194); ++y) {
    const float sy=377.5f+(y-232.5f)*sourcePerY;
    if (sy<margin[0].y || sy>margin[15].y) continue;
    while (sy>margin[span+1].y) ++span;
    const Edge& left=margin[span]; const Edge& right=margin[span+1];
    const float u=(sy-left.y)/(right.y-left.y);
    const float lo=left.left+(right.left-left.left)*u, hi=left.right+(right.right-left.right)*u;
    for (int x=int(232.5f+(lo-1679.5f)/sourcePerX); x<=int(233.5f+(hi-1679.5f)/sourcePerX); ++x) {
      const float px=1679.5f+(x-232.5f)*sourcePerX, nx=(px-1856)/52, ny=(sy-270)/59;
      const float z=sqrtf(fmaxf(0,1-nx*nx-ny*ny));
      const float light=nx*sx+z*sz;
      const float edge=fminf(1,fminf(px-lo,hi-px));
      const int shade=int(5.f*fmaxf(0,edge)*fminf(1,fmaxf(0,.5f-3*light))+.5f);
      if (shade) {
        uint16_t& pixel=fb[y*size+x];
        pixel=mix(pixel,rgb(36,46,41),shade);
      }
    }
  }
}
} // namespace reference_data

static void referenceCalendarGreen(const tm* local) {
  if (local) reference_data::moon(*local);
  reference_data::dateDigit(reference_data::greenCoord(325),local ? char('0'+local->tm_mday/10) : '-');
  reference_data::dateDigit(reference_data::greenCoord(359),local ? char('0'+local->tm_mday%10) : '-');
}
} // namespace clock_face
