// Where the view of a terminal's history is, and what is selected in it, as plain arithmetic: no Qt, so tests/term_scroll.cpp
// checks it without a window.
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

// What is selected, in lines of the text and not in rows of the view: line 0 is the top row of the live screen, -1 the newest
// line of the history. A selection kept in rows stays where it was on the glass while the text moves under it; this one stays
// on the text when the view scrolls and when the shell writes.
struct TermSelection {
    int startCol = -1, startLine = 0, endCol = 0, endLine = 0;

    bool active() const { return startCol >= 0; }
    bool empty() const { return !active() || (startCol == endCol && startLine == endLine); }
    void clear() { startCol = -1; }
    static int lineOf(int row, int offset) { return row - offset; }   // the line a row of the view shows
    void begin(int col, int line) {
        startCol = endCol = col;
        startLine = endLine = line;
    }
    void extend(int col, int line) {
        endCol = col;
        endLine = line;
    }
    // the ends in reading order
    void ordered(int &fromCol, int &fromLine, int &toCol, int &toLine) const {
        const bool forward = startLine < endLine || (startLine == endLine && startCol <= endCol);
        fromCol = forward ? startCol : endCol;
        fromLine = forward ? startLine : endLine;
        toCol = forward ? endCol : startCol;
        toLine = forward ? endLine : startLine;
    }
    bool contains(int line, int col) const {
        if (!active()) return false;
        int fromCol, fromLine, toCol, toLine;
        ordered(fromCol, fromLine, toCol, toLine);
        if (line < fromLine || line > toLine) return false;
        if (line == fromLine && col < fromCol) return false;
        return !(line == toLine && col > toCol);
    }
    // The top line of the screen went into the history, which now holds `history` lines: the text moved up by one. What the
    // history no longer holds is no longer selected.
    void pushed(int history) {
        if (!active()) return;
        --startLine;
        --endLine;
        trimmed(history);
    }
    // a line came back from the history to the top of the screen (the terminal grew): the text moved down by one
    void popped() {
        if (!active()) return;
        ++startLine;
        ++endLine;
    }
    // the history holds `history` lines now (a smaller limit)
    void trimmed(int history) {
        if (!active()) return;
        const int oldest = -history;
        if (startLine < oldest && endLine < oldest) {
            clear();
        } else if (startLine < oldest) {
            startLine = oldest;
            startCol = 0;
        } else if (endLine < oldest) {
            endLine = oldest;
            endCol = 0;
        }
    }
};
