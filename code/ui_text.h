// Word wrap and paging for the Meeting tab (ui_meeting.cpp). Pure C++, no
// TFT: text widths come from a callback (tft.textWidth on the device, a
// table in the host tests: make -C code/test). STUB (contract commit).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ui_text {

using WidthFn = int (*)(const char* s, size_t n, void* ctx);   // pixel width of s[0..n)

}  // namespace ui_text
