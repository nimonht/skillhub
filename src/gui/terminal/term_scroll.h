// Where the view of a terminal's history is, as plain arithmetic: no Qt, so tests/term_scroll.cpp checks it without a window.
// `offset` counts the lines the view has moved up from the live screen (0 is the live screen); `max` is the lines of history.
#pragma once

#include <algorithm>
#include <cmath>

struct TermScroll {
    int offset = 0;
    int max = 0;
    double carry = 0;  // the part of a line the wheel has moved so far: a touchpad sends many small steps

    // positive moves up, into the history. One notch of a wheel is 120 units and three lines; a touchpad reports pixels.
    static double fromAngle(int angle) { return angle / 120.0 * 3.0; }
    static double fromPixels(int pixels, double lineHeight) { return lineHeight > 0 ? pixels / lineHeight : 0; }

    // Moves by `lines`, which may be a fraction; returns how many whole lines the view moved. At an end nothing is stored up,
    // or the first step back would seem to do nothing.
    int wheel(double lines) {
        const int whole = take(lines);
        const int before = offset;
        offset = std::clamp(offset + whole, 0, max);
        if ((offset == 0 && carry < 0) || (offset == max && carry > 0)) carry = 0;
        return offset - before;
    }
    // A whole number of lines taken out of `lines` and kept for the next call: what an application gets instead of a scroll
    int takeWhole(double lines) { return take(lines); }
    void by(int lines) { offset = std::clamp(offset + lines, 0, max); }
    void toBottom() { offset = 0; carry = 0; }
    void toTop() { offset = max; carry = 0; }
    // a line went into the history: a view that is looking back keeps what it shows
    void pushed() {
        if (offset > 0) ++offset;
    }
    // the history is now `lines` long (a line pushed, one taken back, the oldest dropped, a smaller limit)
    void setMax(int lines) {
        max = std::max(0, lines);
        offset = std::clamp(offset, 0, max);
    }
    bool atBottom() const { return offset == 0; }

private:
    // forty steps of 0.1 add up to 3.9999999999999996, and must still be four lines
    int take(double lines) {
        carry += lines;
        const int whole = int(std::trunc(carry + (carry >= 0 ? 1e-6 : -1e-6)));
        carry -= whole;
        return whole;
    }
};
