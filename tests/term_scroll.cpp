// The terminal's view of its history (src/gui/terminal/term_scroll.h) and the byte sequences a wheel step becomes in libvterm.
// No window needed: `ctest` runs it.
#include <cstdio>
#include <cstring>
#include <string>

#include "gui/terminal/term_scroll.h"

extern "C" {
#include <vterm.h>
}

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void slow_steps_add_up() {   // a touchpad: forty small steps of a tenth of a line are four lines, not none
    TermScroll s;
    s.setMax(100);
    int moved = 0;
    for (int i = 0; i < 40; ++i) moved += s.wheel(0.1);
    CHECK(moved == 4);
    CHECK(s.offset == 4);
}

static void wheel_notch() {
    TermScroll s;
    s.setMax(100);
    CHECK(s.wheel(TermScroll::fromAngle(120)) == 3);   // one notch, three lines
    CHECK(s.wheel(TermScroll::fromAngle(-120)) == -3);
    CHECK(s.atBottom());
    CHECK(s.wheel(TermScroll::fromAngle(-120)) == 0);   // nowhere below the live screen
    CHECK(s.wheel(TermScroll::fromAngle(15)) == 0);     // a high-resolution wheel: 15 of 120 is not yet a line
    CHECK(s.wheel(TermScroll::fromAngle(15)) == 0);
    CHECK(s.wheel(TermScroll::fromAngle(15)) == 1);     // 45 of 120 is more than a line
}

static void ends_store_nothing() {
    TermScroll s;
    s.setMax(10);
    s.wheel(1000);
    CHECK(s.offset == 10);
    CHECK(s.wheel(-1) == -1);   // the first step back moves at once: nothing was stored up beyond the top
    s.toBottom();
    s.wheel(-5);
    CHECK(s.wheel(1) == 1);
}

static void anchored_while_output_arrives() {
    TermScroll s;
    s.setMax(50);
    s.wheel(10);
    CHECK(s.offset == 10);
    for (int i = 0; i < 5; ++i) {   // the shell writes five lines: each goes into the history
        s.pushed();
        s.setMax(51 + i);
    }
    CHECK(s.offset == 15);          // the view still shows the same text
    s.toBottom();
    s.pushed();
    s.setMax(60);
    CHECK(s.offset == 0);           // at the bottom it follows the output
}

static void clamps_when_history_shrinks() {
    TermScroll s;
    s.setMax(30);
    s.by(25);
    s.setMax(10);
    CHECK(s.offset == 10);
    s.setMax(0);
    CHECK(s.offset == 0);
    s.by(5);
    CHECK(s.offset == 0);
}

static void application_steps() {
    TermScroll s;
    CHECK(s.takeWhole(0.4) == 0);
    CHECK(s.takeWhole(0.4) == 0);
    CHECK(s.takeWhole(0.4) == 1);   // 1.2 lines in all
    CHECK(s.takeWhole(-2.0) == -1); // 0.2 - 2.0 = -1.8
    CHECK(s.takeWhole(-0.2) == -1); // -0.8 - 0.2 = -1.0, stored 0.8 earlier
}

// A selection is kept in lines of the text. Kept in rows of the view, it stayed on the glass while the text moved under it.
static void selection_stays_on_its_text() {
    TermScroll view;
    view.setMax(100);
    TermSelection sel;
    CHECK(!sel.active() && sel.empty() && !sel.contains(0, 0));
    sel.begin(4, TermSelection::lineOf(10, view.offset));    // dragged on the live screen, from row 10 to row 12
    sel.extend(7, TermSelection::lineOf(12, view.offset));
    CHECK(sel.active() && !sel.empty());
    CHECK(sel.contains(10, 4) && !sel.contains(10, 3) && sel.contains(11, 0) && sel.contains(11, 500) && sel.contains(12, 7) && !sel.contains(12, 8));
    view.wheel(3);                                           // the view moves up three lines: the text is three rows lower
    CHECK(sel.contains(TermSelection::lineOf(13, view.offset), 4));
    CHECK(!sel.contains(TermSelection::lineOf(10, view.offset), 4));
    view.toBottom();
    for (int i = 0; i < 12; ++i) sel.pushed(100 + i + 1);    // the shell writes twelve lines: the text goes up, into the history
    CHECK(sel.contains(-2, 4) && sel.contains(0, 7) && !sel.contains(10, 4));
    sel.popped();                                            // the terminal grew by a row: one line came back
    CHECK(sel.contains(-1, 4) && sel.contains(1, 7));
}

static void selection_backwards_and_empty() {
    TermSelection sel;
    sel.begin(9, 5);
    CHECK(sel.empty());                                      // a click selects nothing
    sel.extend(2, 3);                                        // dragged up and to the left
    int fromCol, fromLine, toCol, toLine;
    sel.ordered(fromCol, fromLine, toCol, toLine);
    CHECK(fromCol == 2 && fromLine == 3 && toCol == 9 && toLine == 5);
    CHECK(sel.contains(4, 0) && sel.contains(3, 2) && !sel.contains(3, 1) && !sel.contains(5, 10));
    sel.clear();
    CHECK(!sel.active() && !sel.contains(4, 0));
}

static void selection_leaves_with_its_history() {
    TermSelection sel;
    sel.begin(3, -8);
    sel.extend(5, -2);
    sel.trimmed(5);                                          // the history holds five lines now: the selection starts at its oldest
    int fromCol, fromLine, toCol, toLine;
    sel.ordered(fromCol, fromLine, toCol, toLine);
    CHECK(fromCol == 0 && fromLine == -5 && toCol == 5 && toLine == -2);
    sel.trimmed(1);                                          // and now none of it is left
    CHECK(!sel.active());
    sel.begin(5, -2);                                        // the same, selected backwards
    sel.extend(3, -8);
    sel.trimmed(5);
    sel.ordered(fromCol, fromLine, toCol, toLine);
    CHECK(fromCol == 0 && fromLine == -5 && toCol == 5 && toLine == -2);
    sel.begin(0, -3);                                        // a full history drops its oldest line for each new one
    sel.extend(4, -3);
    for (int i = 0; i < 2; ++i) sel.pushed(5);
    CHECK(sel.contains(-5, 2));
    sel.pushed(5);
    CHECK(!sel.active());
}

// what the wheel is to a program that asked for mouse reports, in the encoding it asked for
static std::string output;
static void collect(const char *s, size_t len, void *) { output.append(s, len); }

static void mouse_reports() {
    VTerm *vt = vterm_new(24, 80);
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, collect, nullptr);
    vterm_obtain_state(vt);
    VTermScreen *screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);
    const char *on = "\x1b[?1000h\x1b[?1006h";   // report buttons, as SGR
    vterm_input_write(vt, on, std::strlen(on));
    vterm_mouse_move(vt, 4, 9, VTERM_MOD_NONE);
    vterm_mouse_button(vt, 4, true, VTERM_MOD_NONE);   // the wheel up
    CHECK(output == "\x1b[<64;10;5M");
    output.clear();
    vterm_mouse_button(vt, 5, true, VTERM_MOD_NONE);   // down
    CHECK(output == "\x1b[<65;10;5M");
    output.clear();
    const char *off = "\x1b[?1000l";
    vterm_input_write(vt, off, std::strlen(off));
    vterm_mouse_button(vt, 4, true, VTERM_MOD_NONE);
    CHECK(output.empty());                              // a program that wants none gets none
    vterm_free(vt);
}

static void arrow_keys_for_the_alternate_screen() {
    VTerm *vt = vterm_new(24, 80);
    vterm_output_set_callback(vt, collect, nullptr);
    vterm_obtain_state(vt);
    vterm_screen_reset(vterm_obtain_screen(vt), 1);
    output.clear();
    vterm_keyboard_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
    CHECK(output == "\x1b[A");
    output.clear();
    const char *app = "\x1b[?1h";   // application cursor keys, as less and vim ask for
    vterm_input_write(vt, app, std::strlen(app));
    vterm_keyboard_key(vt, VTERM_KEY_DOWN, VTERM_MOD_NONE);
    CHECK(output == "\x1bOB");
    vterm_free(vt);
}

int main() {
    slow_steps_add_up();
    wheel_notch();
    ends_store_nothing();
    anchored_while_output_arrives();
    clamps_when_history_shrinks();
    application_steps();
    selection_stays_on_its_text();
    selection_backwards_and_empty();
    selection_leaves_with_its_history();
    mouse_reports();
    arrow_keys_for_the_alternate_screen();
    std::printf("%s\n", failures ? "term scroll: FAILED" : "term scroll: all passed");
    return failures ? 1 : 0;
}
