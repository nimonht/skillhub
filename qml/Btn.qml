import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import "."

// Action button. Primary/secondary/danger use cut corners (action = machinery); ghost is a plain hit area.
AbstractButton {
    id: root
    property string kind: "secondary"   // primary | secondary | danger | ghost
    property string iconName: ""
    property bool compact: false
    readonly property real cut: compact ? 6 : Theme.cut

    implicitHeight: compact ? 30 : Theme.minTap
    implicitWidth: Math.max(compact ? 30 : 44, row.implicitWidth + (compact ? 22 : 30))
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : iconName

    readonly property color fg: !enabled ? Theme.text3
        : kind === "primary" ? Theme.actionInk
        : kind === "danger" ? Theme.dangerText
        : kind === "ghost" ? (hovered ? Theme.text : Theme.text2) : Theme.text
    property color _fill: !enabled ? Theme.bg2
        : kind === "primary" ? (down ? Qt.darker(Theme.action, 1.18) : hovered ? Qt.lighter(Theme.action, Theme.dark ? 1.1 : 1.35) : Theme.action)
        : kind === "danger" ? (down ? Theme.dangerLow : hovered ? Theme.dangerLow : "transparent")
        : kind === "ghost" ? (down ? Theme.bg4 : hovered ? Theme.bg3 : "transparent")
        : (down ? Theme.bg2 : hovered ? Theme.bg4 : Theme.bg3)
    Behavior on _fill { ColorAnimation { duration: Theme.dFast } }

    background: Item {
        Chamfer {
            anchors.fill: parent
            visible: root.kind === "primary" || root.kind === "secondary" || root.kind === "danger"
            fill: root._fill
            cutTR: root.cut
            cutBL: root.cut
        }
        Rectangle { anchors.fill: parent; visible: root.kind === "ghost"; radius: 4; color: root._fill }
        Chamfer {   // keyboard focus ring, outside the shape so nothing clips it
            anchors.fill: parent
            anchors.margins: -4
            visible: root.visualFocus
            stroke: Theme.focus
            strokeWidth: 2
            cutTR: root.cut + 3
            cutBL: root.cut + 3
        }
    }
    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 8
            Icon { visible: root.iconName !== ""; name: root.iconName; size: root.compact ? 16 : 18; color: root.fg; anchors.verticalCenter: parent.verticalCenter }
            Text {
                visible: root.text !== ""
                text: root.text
                color: root.fg
                font.family: Theme.ui
                font.pixelSize: Theme.fsBody
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
    HoverHandler { cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor; enabled: !app.noHover }

    // icon-only buttons name themselves on hover
    Timer { id: tipDelay; interval: 450; running: root.hovered && root.text === "" && root.Accessible.name !== ""; onTriggered: tip.shown = true }
    onHoveredChanged: if (!hovered) tip.shown = false
    Rectangle {
        id: tip
        property bool shown: false
        visible: shown && root.hovered
        z: 100
        y: root.height + 8
        x: root.width / 2 - width / 2 - Math.max(0, root.mapToItem(null, root.width / 2 + width / 2, 0).x - (root.Window.width - 8))
        width: tipText.implicitWidth + 18
        height: 26
        radius: 4
        color: Theme.inkBg
        Text { id: tipText; anchors.centerIn: parent; text: root.Accessible.name; color: Theme.inkText; font.family: Theme.ui; font.pixelSize: Theme.fsSmall }
    }
}
