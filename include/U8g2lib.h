// Stand-in for the U8g2 library. Arduino_GFX turns on its U8g2 font support
// (UTF-8 text in src/fonts.h) only when this header exists; nothing else from
// U8g2 is needed, so the real library isn't pulled into the build.
#pragma once
#define U8G2_WITH_UNICODE  // glyphs above U+00FF, e.g. Cyrillic
#ifndef U8G2_FONT_SECTION
#define U8G2_FONT_SECTION(name)
#endif
