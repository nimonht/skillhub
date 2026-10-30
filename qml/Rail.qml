import QtQuick
import QtQuick.Controls.Basic
import "."

// Left rail: brand, quick filters with real counts, automation switches, appearance.
Rectangle {
    id: root
    color: Theme.bg1
    implicitWidth: Theme.railW

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }

    component FilterItem: AbstractButton {
        id: item
        property string mode: "all"
        property string iconName: "all"
        property int count: 0
        property color countColor: Theme.text2
        property bool asAction: false
        property bool hideCount: false
        signal triggered()
        property bool on: app.filter.mode === mode
        width: parent ? parent.width : 180
        height: 38
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.RadioButton
        Accessible.name: text + ", " + count
        Accessible.checked: on
        onClicked: asAction ? triggered() : app.filter.mode = mode
        background: Rectangle {
            radius: 4
            color: item.on ? Theme.bg4 : item.hovered ? Theme.bg3 : "transparent"
            border.color: item.visualFocus ? Theme.focus : "transparent"
            border.width: 2
            Behavior on color { ColorAnimation { duration: Theme.dFast } }
            Item {
                visible: item.on
                anchors.fill: parent
                Rectangle { x: 0; y: 0; width: 8; height: 2; color: Theme.mark }
                Rectangle { x: 0; y: 0; width: 2; height: 8; color: Theme.mark }
                Rectangle { x: parent.width - 8; y: parent.height - 2; width: 8; height: 2; color: Theme.mark }
                Rectangle { x: parent.width - 2; y: parent.height - 8; width: 2; height: 8; color: Theme.mark }
            }
        }
        contentItem: Item {
            Icon { x: 12; anchors.verticalCenter: parent.verticalCenter; name: item.iconName; size: 17; color: item.on ? Theme.markText : Theme.text2 }
            Text {
                x: 40
                anchors.verticalCenter: parent.verticalCenter
                text: item.text
                color: item.on ? Theme.text : Theme.text2
                font.family: Theme.ui
                font.pixelSize: Theme.fsBody
                font.weight: item.on ? Font.DemiBold : Font.Medium
            }
            NumText {
                visible: !item.hideCount
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: item.count
                color: item.count > 0 ? item.countColor : Theme.text3
                font.family: Theme.display
                font.pixelSize: Theme.fsLead
                font.weight: Font.DemiBold
            }
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: !app.noHover }
    }

    Flickable {   // short windows scroll the rail instead of overlapping it; the appearance block stays pinned when there is room
        id: scroller
        anchors.fill: parent
        anchors.rightMargin: 1
        contentWidth: width
        contentHeight: Math.max(height, bottomCol.y + bottomCol.height + 16)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: Scroll { policy: scroller.contentHeight > scroller.height + 2 ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

    Column {
        id: topCol
        x: 12
        width: scroller.width - 24
        spacing: 2

        Item {   // brand
            width: parent.width
            height: 64
            Logo { x: 4; anchors.verticalCenter: parent.verticalCenter; size: 34 }
            Text {
                x: 48
                anchors.verticalCenter: parent.verticalCenter
                text: "Skill Hub"
                color: Theme.text
                font.family: Theme.display
                font.pixelSize: Theme.fsTitle + 2
                font.weight: Font.Bold
            }
        }

        ScopeMenu { width: parent.width }
        Item { width: 1; height: 8 }

        SectionLabel { width: parent.width; text: "Skills" }
        FilterItem { text: "All skills"; mode: "all"; iconName: "all"; count: app.skillCount }
        FilterItem { text: "Updates"; mode: "updates"; iconName: "upgrade"; count: app.updateCount; countColor: Theme.notifyText }
        FilterItem { text: "Local only"; mode: "local"; iconName: "folder"; count: app.localCount }
        FilterItem { text: "Link problems"; mode: "problems"; iconName: "alert"; count: app.problemCount; countColor: Theme.dangerText }

        Item { width: 1; height: 10 }
        SectionLabel { width: parent.width; text: "Tools" }
        FilterItem { text: "Agents"; mode: "@providers"; iconName: "sliders"; count: app.agents.length; asAction: true; onTriggered: app.detailMode = "providers"; on: app.detailMode === "providers" }
        FilterItem { visible: !app.inProject; text: "Transfer skills"; mode: "@transfer"; iconName: "export"; count: 0; asAction: true; hideCount: true; onTriggered: app.detailMode = "transfer"; on: app.detailMode === "transfer" }

        Item { width: 1; height: 10; visible: app.automationSupported }
        SectionLabel { width: parent.width; text: "Automation"; visible: app.automationSupported }
        Toggle {
            visible: app.automationSupported
            width: parent.width
            text: "Daily update"
            hint: app.automationMechanism === "launchd" ? "launchd agent" : app.automationMechanism === "Task Scheduler" ? "Task Scheduler" : "systemd user timer"
            checked: app.timerState === "on"
            onToggled: app.setTimer(checked)
        }
        Toggle {
            visible: app.automationSupported
            width: parent.width
            text: "Adopt strays"
            hint: "watch agent folders"
            checked: app.watcherState === "on"
            onToggled: app.setWatcher(checked)
        }
    }

    Column {
        id: bottomCol
        x: 16
        width: scroller.width - 32
        y: Math.max(topCol.height + 16, scroller.height - height - 16)
        spacing: 10
        SectionLabel { width: parent.width; text: "Appearance" }
        Segmented {
            width: parent.width
            options: [ { value: "system", label: "Auto", icon: "auto" }, { value: "dark", label: "Dark", icon: "moon" }, { value: "light", label: "Light", icon: "sun" } ]
            current: app.themeMode
            onPicked: (v) => app.themeMode = v
        }
        Toggle { width: parent.width + 8; x: -8; text: "Reduce motion"; checked: app.reduceMotion; onToggled: app.reduceMotion = checked }
        NumText {
            text: "Skill Hub " + app.version
            color: Theme.text3
            font.family: Theme.ui
            font.pixelSize: Theme.fsMicro
        }
    }
    }
}
