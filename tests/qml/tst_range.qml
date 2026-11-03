import QtQuick
import QtTest
import "../../qml"

// The settings sliders (Range.qml): what the knob shows when it is let go is what gets saved, once, and it stays there when
// another setting changes. Before 2.0.0-beta5 a drag saved the value from before it, and the knob fell back on the next change.
// Run: qmltestrunner -platform offscreen -input tests/qml
Item {
    id: top
    width: 400; height: 120
    property real stored: 0.8
    property int rev: 0
    property var saved: []
    Range {
        id: r
        width: 300
        from: 0; to: 1; stepSize: 0.05
        value: { void top.rev; return top.stored }   // as Theme.setting() does: read again whenever any setting changes
        shown: Math.round(value * 100) + " %"
        onMoved: (v) => { top.saved.push(v); top.stored = v; top.rev++ }
    }
    TestCase {
        name: "Range"
        when: windowShown
        function slider() { return r.children[2] }
        function init() { top.saved = [] }
        function test_drag() {
            const s = slider()
            top.stored = 0.8
            top.rev++
            mousePress(s, s.width * 0.8, 13)
            let during = ""
            for (let x = s.width * 0.8; x >= s.width * 0.5; x -= 10) { mouseMove(s, x, 13); during = r.shown }
            compare(top.saved.length, 0, "nothing is saved while the knob moves")
            mouseRelease(s, s.width * 0.5, 13)
            compare(top.saved.length, 1, "one save when the drag ends")
            fuzzyCompare(top.saved[0], 0.5, 0.051)
            verify(during !== "80 %", "the value in words follows the knob")
            top.rev++   // another setting changes
            fuzzyCompare(r.value, 0.5, 0.051)
        }
        function test_click() {
            const s = slider()
            mouseClick(s, s.width * 0.2, 13)
            compare(top.saved.length, 1)
            top.rev++
            fuzzyCompare(r.value, 0.2, 0.051)
        }
        function test_keys() {
            const s = slider()
            s.forceActiveFocus()
            const at = r.value
            keyClick(Qt.Key_Right)
            keyClick(Qt.Key_Right)
            top.rev++
            fuzzyCompare(top.stored, at + 0.1, 0.001)
            fuzzyCompare(r.value, at + 0.1, 0.001)
        }
    }
}
