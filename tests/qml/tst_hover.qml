import QtQuick
import QtTest
import "../../qml"

// The fill that hover and selection fade in and out (SkillRow, Rail, Segmented, Toggle, Btn). A ColorAnimation blends red, green,
// blue and alpha one by one, so a fade from "transparent" (black at alpha 0) to a light fill passed through grey: on the light
// theme every hover flashed dark, and moving over a list flickered. Theme.clear() is the same colour with no opacity, so only the
// alpha moves. Up to 2.0.0-beta5 the "off" end of these fills was "transparent".
// Run: qmltestrunner -platform offscreen -input tests/qml
Item {
    id: top
    width: 100; height: 100
    property bool on: false
    property color fill: "white"
    Rectangle {
        id: box
        anchors.fill: parent
        color: top.on ? top.fill : Theme.clear(top.fill)
        Behavior on color { ColorAnimation { duration: 240 } }
    }
    TestCase {
        name: "HoverFade"
        when: windowShown

        function test_clear_keeps_the_colour() {
            for (const css of ["#FFFFFF", "#1B212A", "#DCEFE8", "#FBE3E0"]) {
                const c = Theme.clear(Qt.color(css)), want = Qt.color(css)
                compare(c.a, 0, css + " has no opacity")
                fuzzyCompare(c.r, want.r, 0.002, css + " red")
                fuzzyCompare(c.g, want.g, 0.002, css + " green")
                fuzzyCompare(c.b, want.b, 0.002, css + " blue")
            }
        }

        // every value on the way in and out has the colour of the fill: only the alpha changes
        function fade(css) {
            top.on = false
            top.fill = css
            wait(300)   // the animation is 240 ms: let it settle on the invisible end
            compare(box.color.a, 0, "the fill starts invisible")
            const want = Qt.color(css)
            for (const direction of [true, false]) {
                top.on = direction
                for (let i = 0; i < 16; ++i) {
                    wait(20)
                    const c = box.color
                    fuzzyCompare(c.r, want.r, 0.01, css + " red while fading " + (direction ? "in" : "out"))
                    fuzzyCompare(c.g, want.g, 0.01, css + " green while fading " + (direction ? "in" : "out"))
                    fuzzyCompare(c.b, want.b, 0.01, css + " blue while fading " + (direction ? "in" : "out"))
                }
            }
        }
        function test_light_fill_does_not_go_dark() { fade("#FFFFFF") }
        function test_dark_fill_keeps_its_colour() { fade("#1B212A") }
    }
}
