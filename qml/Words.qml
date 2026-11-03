pragma Singleton
import QtQuick

// What the background service reports as facts with a key, said in the window's language. The service speaks English; where it
// sends a key or a known shape, the words are made here, and anything unknown is shown as the service said it.
QtObject {
    // how an agent finds its skills (state.c: mechanism_of), from its mode and folder
    function mechanism(a) {
        if (!a) return ""
        if (a.mode === "native") return qsTr("Reads the shared folder directly")
        if (a.mode === "config" && a.folder) return qsTr("Shared folder listed in %1").arg(a.folder)
        if (a.mode === "link" && a.folder) return qsTr("Symlinks in %1").arg(a.folder)
        return a.mechanism || ""
    }
    // what proved an agent is installed (sh_catalog.c): "command `x`", "folder ~/.x", "added by you"
    function foundVia(by) {
        if (!by) return ""
        if (by === "added by you") return qsTr("Found because you added it")
        if (by.startsWith("command ")) return qsTr("Found via the command %1").arg(by.slice(8))
        if (by.startsWith("folder ")) return qsTr("Found via the folder %1").arg(by.slice(7))
        return qsTr("Found via %1").arg(by)
    }
    // the parts of Settings → Storage (jobs.c: body_sizes), by key
    function storagePart(key, fallback) {
        const names = { skills: qsTr("Skills"), latest: qsTr("Latest backup of each skill"), cache: qsTr("Cached repositories"),
                        versions: qsTr("Older versions"), removed: qsTr("Removed skills"), conflicts: qsTr("Conflicts") }
        return names[key] || fallback || ""
    }
}
