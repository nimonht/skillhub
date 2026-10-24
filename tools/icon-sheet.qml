import QtQuick
import "../qml"

// Contact sheet of every agent mark at its real tile sizes, for checking a regenerated data/provider-icons.json by eye:
//   QML_XHR_ALLOW_FILE_READ=1 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
//     qml6 tools/icon-sheet.qml -- OUT.png          (follows the system colour scheme; the app's --theme only applies to the app)
Rectangle {
    id: root
    readonly property var args: Qt.application.arguments
    readonly property string out: args.length >= 3 ? args[args.length - 1] : "icon-sheet.png"
    property var icons: ({})
    property var labels: ({})
    property var ids: []
    width: 1080
    height: 60 + Math.ceil(ids.length / 6) * 112
    color: Theme.bg1

    function read(name) {
        const x = new XMLHttpRequest()
        x.open("GET", Qt.resolvedUrl("../data/" + name), false)
        x.send()
        return JSON.parse(x.responseText)
    }

    Component.onCompleted: {
        icons = read("provider-icons.json").icons
        const list = read("providers.json").providers
        const l = {}
        const all = []
        for (const p of list) { l[p.id] = p.label; all.push(p.id) }
        labels = l
        ids = all.filter(id => icons[id]).concat(all.filter(id => !icons[id]))
        shot.start()
    }

    Text {
        x: 24; y: 18
        color: Theme.text2
        font.family: Theme.ui
        font.pixelSize: 13
        text: Object.keys(icons).length + " marks, " + (ids.length - Object.keys(icons).length) + " monograms"
    }

    Flow {
        x: 24; y: 52
        width: root.width - 48
        spacing: 0
        Repeater {
            model: root.ids
            Item {
                width: (root.width - 48) / 6
                height: 112
                AgentTile { x: 0; y: 4; size: 36; icon: root.icons[modelData] || null; text: root.labels[modelData].slice(0, 2).toUpperCase() }
                AgentTile { x: 44; y: 4; size: 36; tone: "off"; icon: root.icons[modelData] || null; text: root.labels[modelData].slice(0, 2).toUpperCase() }
                AgentTile { x: 88; y: 4; size: 64; selected: true; icon: root.icons[modelData] || null; text: root.labels[modelData].slice(0, 2).toUpperCase() }
                Text {
                    x: 0; y: 74; width: parent.width - 8
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.ui
                    font.pixelSize: 12
                    text: root.labels[modelData]
                }
            }
        }
    }

    Timer {
        id: shot
        interval: 600
        onTriggered: root.grabToImage(function (r) { r.saveToFile(root.out); Qt.quit() })
    }
}
