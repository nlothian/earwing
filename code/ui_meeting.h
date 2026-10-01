// The screen's tab strip and its Meeting tab: the in-browser assistant's live
// meeting summary (meeting.h), as a rolling Follow feed or one section at a
// time. main.cpp draws the Settings tab (the device and audio status) below
// the strip and asks tab() before drawing it.
//
//   y   0- 29  tabs: MEETING x 0-101, SETTINGS x 102-203; freshness x 204-308
//   (x is from the viewport, right of the blank left strip: board_io.h)
//   Meeting tab:
//   y  32- 48  header: mode title (section colour), "to 12:34", page "2/3"
//   y  50-202  body: 9 rows of font 2, 17 px apart; a tap turns the page
//   y 206-239  mode buttons Follow | Key points | Decisions | Actions | Open Q
//
// Everything here runs from loop() (core 1), which owns the TFT. The
// snapshot is copied out of the store only when its generation changes, and
// laid out only when the generation, the mode or the tab does; drawing is
// by row, each repainted only when what it shows changes, text drawn over
// its own background (no clear-then-draw). A 1 Hz tick redraws the
// freshness indicator and the Follow feed's ages, nothing else.
#pragma once

#include <stdint.h>

class TFT_eSPI;

namespace ui_meeting {

enum class Tab : uint8_t { Meeting, Settings };

void begin(TFT_eSPI* tft);    // setup(), after tft.init(): allocates (PSRAM), draws the strip
void loop(uint32_t now);      // loop(): picks up new snapshots, draws the strip and the Meeting tab
// A tap (screen coordinates). true if it was ours: the tab strip, or
// anywhere on the Meeting tab. Switching to Settings leaves the area below
// the strip for main.cpp to clear and repaint.
bool handleTap(int x, int y);
Tab tab();
// Switch to Settings, e.g. for the setup portal's instructions (drawn only there).
void showSettings();

#ifdef UI_DEBUG
// Debug aids, -DUI_DEBUG builds only (main.cpp routes them):
//   GET /debug/screen.bmp          the screen, read back from the TFT by loop()
//   GET /debug/tap?x=160&y=120     a tap, as if the panel had reported it
// They run in the web server tasks and hand over to loop() by flag.
}  // namespace ui_meeting
namespace web { class Req; }
namespace ui_meeting {
void debugScreenHandler(web::Req& q);
void debugTapHandler(web::Req& q);
bool debugTakeTap(int* x, int* y);   // touchLoop(): an injected tap, like board_io::takeTap()
#else
inline bool debugTakeTap(int*, int*) { return false; }
#endif

}  // namespace ui_meeting
