import QtQuick
import QtTest
import SkillHub.Terminal

// The window's shortcuts must not take the keys of a focused terminal ("/" opens the search, Escape dismisses, Ctrl+R refreshes),
// and the terminal must scroll with a wheel or a touchpad, stay where the user left it while output arrives, and follow typing.
// What is selected must stay on its text while all that happens.
Item {
    id: top
    width: 640; height: 400
    property int taken: 0         // shortcuts that fired: the keys the terminal did not get
    property int chords: 0        // the window's own chords, which must still work from the terminal

    VTermItem {
        id: term
        anchors.fill: parent
        program: "/bin/sh"
        arguments: ["-c", "stty raw -echo; seq 1 300; exec cat -v"]   // 300 lines of history, then what is typed is shown as it arrives
        scrollback: 5000
    }
    Shortcut { sequence: "/"; onActivated: top.taken++ }
    Shortcut { sequence: "Escape"; onActivated: top.taken++ }
    Shortcut { sequence: "Ctrl+R"; onActivated: top.taken++ }
    Shortcut { sequence: "Ctrl+F"; onActivated: top.taken++ }
    Shortcut { sequence: "F5"; onActivated: top.taken++ }
    Shortcut { sequence: "Ctrl+`"; onActivated: top.chords++ }
    Shortcut { sequence: "Ctrl+Shift+L"; onActivated: top.chords++ }

    TestCase {
        name: "Terminal"
        when: windowShown

        function init_data() { return [{ tag: "x" }] }
        function initTestCase() {
            term.start()
            term.forceActiveFocus()
            tryVerify(function () { return term.scrollMax > 200 }, 8000)   // seq has run: the lines are in the history
            tryVerify(function () { return term.screenText().indexOf("300") >= 0 }, 8000)
        }
        function typed(text) {   // the shell echoes it back through cat -v
            for (let i = 0; i < text.length; ++i) keyClick(text[i])
        }

        function test_1_slash_and_escape_reach_the_shell() {
            keyClick(Qt.Key_Slash)
            keyClick(Qt.Key_Escape)
            tryVerify(function () { return term.screenText().indexOf("/^[") >= 0 }, 3000)   // cat -v writes Escape as ^[
            compare(top.taken, 0, "no shortcut took a key")
        }
        function test_2_readline_keys_and_function_keys_reach_the_shell() {
            keyClick(Qt.Key_R, Qt.ControlModifier)
            keyClick(Qt.Key_F, Qt.ControlModifier)
            tryVerify(function () { return term.screenText().indexOf("^R^F") >= 0 }, 3000)
            keyClick(Qt.Key_F5)
            tryVerify(function () { return term.screenText().indexOf("^[[15~") >= 0 }, 3000)
            compare(top.taken, 0)
        }
        function test_3_the_windows_own_chords_still_work() {
            keyClick(Qt.Key_QuoteLeft, Qt.ControlModifier)
            keyClick(Qt.Key_L, Qt.ControlModifier | Qt.ShiftModifier)
            compare(top.chords, 2)
        }

        function test_4_wheel_scrolls_by_notches() {
            term.forceActiveFocus()
            keyClick(Qt.Key_End, Qt.ShiftModifier)
            compare(term.scrollOffset, 0)
            mouseWheel(term, 100, 100, 0, 120)
            compare(term.scrollOffset, 3, "one notch is three lines")
            mouseWheel(term, 100, 100, 0, -120)
            compare(term.scrollOffset, 0)
        }
        function test_5_small_steps_add_up() {
            keyClick(Qt.Key_End, Qt.ShiftModifier)
            for (let i = 0; i < 8; ++i) mouseWheel(term, 100, 100, 0, 15)   // a high-resolution wheel: 15 of 120 each
            compare(term.scrollOffset, 3, "eight steps of 0.375 lines are three lines")
        }
        function test_6_output_does_not_pull_the_view_down() {
            keyClick(Qt.Key_End, Qt.ShiftModifier)
            mouseWheel(term, 100, 100, 0, 240)
            const before = term.scrollOffset
            verify(before > 0)
            term.type("line\n")   // as if typed into the shell: it prints and the screen scrolls
            tryVerify(function () { return term.scrollOffset > before }, 3000)   // the view stays on the same text
            verify(!term.atBottom)
        }
        function test_7_typing_returns_to_the_live_screen() {
            verify(!term.atBottom)
            keyClick("a")
            tryVerify(function () { return term.atBottom }, 3000)
        }
        function test_8_a_modifier_alone_does_not() {
            mouseWheel(term, 100, 100, 0, 120)
            verify(!term.atBottom)
            keyClick(Qt.Key_Shift)
            verify(!term.atBottom)
            keyClick(Qt.Key_Home, Qt.ShiftModifier)
            compare(term.scrollOffset, term.scrollMax, "Shift+Home goes to the oldest line")
            keyClick(Qt.Key_End, Qt.ShiftModifier)
            compare(term.scrollOffset, 0)
        }
        function test_9_a_selection_stays_on_its_text() {
            mousePress(term, 2, 5)
            mouseMove(term, term.width - 5, 60)
            mouseRelease(term, term.width - 5, 60)
            const picked = term.selectedText()
            verify(picked.trim() !== "", "a drag over the top rows selects their text")
            mouseWheel(term, 100, 100, 0, 240)   // the view moves into the history
            verify(!term.atBottom)
            compare(term.selectedText(), picked, "the selection is on the same text after scrolling")
            const max = term.scrollMax
            term.type("more\n")                  // and the shell writes
            tryVerify(function () { return term.scrollMax > max }, 3000)
            compare(term.selectedText(), picked, "and after the shell wrote a line")
            keyClick(Qt.Key_End, Qt.ShiftModifier)
            compare(term.selectedText(), picked)
            keyClick("b")                        // typing ends it, as before
            tryVerify(function () { return term.selectedText() === "" }, 3000)
        }
    }
}
