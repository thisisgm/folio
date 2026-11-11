import QtQuick

// The right-click menu. On an item: Open, Open With (each app that can),
// Preview, Rename, Duplicate, Copy, Cut, Paste Into, Copy Path, Compress,
// Extract, Share (Dropbox link, Taildrop, LocalSend: whichever are available), Pin,
// Terminal, Delete. On empty space: Paste, New Folder, Copy Path, Terminal,
// hidden files. In the trash: Restore, Empty Trash and Delete Permanently,
// without the entries that change files. Entries show their keys, so the
// menu also teaches the shortcuts.
FolioMenu {
    id: fileMenu

    // What was clicked ({path, name, isDir}), or null for empty space.
    property var target: null
    // The folder the click happened in.
    property string folder

    signal renameRequested(var target)
    signal deleteRequested(var paths)
    signal emptyTrashRequested()
    signal pinRequested(string path)
    // In the trash the menu is about getting things back, or letting go.
    readonly property bool trashView: fileOps.inTrash(folder)

    // Named for…, not on…: QML treats on… names as signal handlers, and an
    // onFolder beside the folder property never updated.
    readonly property bool forItem: !!target && target.path !== undefined
    readonly property bool forFolder: forItem && target.isDir === true
    readonly property var apps: forItem ? fileOps.appsFor(target.path) : []
    // Open With only when there's something besides what Open already does.
    readonly property bool canOpenWith: apps.length > 1
    // A selected item in the folder being shown stands for the whole
    // selection, so Copy, Compress or Delete act on all of it.
    readonly property bool forSelection: forItem && target.selected === true && backend.folder.selectedCount > 1
                                         && backend.folder.indexOf(target.name) >= 0
    readonly property var paths: !forItem ? [] : forSelection ? backend.folder.selectedPaths() : [target.path]

    // Opens at the pointer, or (from the keyboard, Alt+Return) beside the
    // item it's about.
    function show(clicked, where, near) {
        target = clicked
        folder = where
        // Submenus (Open With, Share) show only when they have entries.
        // Menu makes their entries itself, so they're set here rather than
        // bound where they're declared.
        for (let i = 0; i < count; ++i) {
            const entry = itemAt(i)
            if (entry && entry.subMenu)
                entry.shown = entry.subMenu === shareMenu ? canShare : canOpenWith
        }
        if (near)
            popup(near, Math.min(near.width / 2, 120), near.height / 2)
        else
            popup()
    }

    FolioMenuItem {
        shown: fileMenu.forItem && fileMenu.trashView
        text: fileMenu.forSelection ? qsTr("Restore %n Items", "", fileMenu.paths.length) : qsTr("Restore")
        onTriggered: fileOps.restore(fileMenu.paths)
    }
    FolioMenuItem {
        shown: !fileMenu.forItem && fileMenu.trashView
        text: qsTr("Empty Trash…")
        onTriggered: fileMenu.emptyTrashRequested()
    }
    FolioMenuItem {
        shown: fileMenu.forItem
        text: fileMenu.forFolder ? qsTr("Open Folder") : qsTr("Open")
        keys: "⏎"
        onTriggered: backend.open(fileMenu.target.path)
    }
    // Every app that says it can open this type, the default first.
    FolioMenu {
        title: qsTr("Open With")
        Repeater {
            model: fileMenu.apps
            FolioMenuItem {
                required property var modelData
                text: modelData.isDefault ? qsTr("%1 (default)").arg(modelData.name) : modelData.name
                onTriggered: fileOps.openWith(fileMenu.target.path, modelData.id)
            }
        }
    }
    FolioMenuItem {
        shown: fileMenu.forItem
        text: qsTr("Preview")
        keys: "Space"
        onTriggered: backend.togglePreview(fileMenu.target.path)
    }
    MenuLine { shown: fileMenu.forItem }
    FolioMenuItem {
        shown: !fileMenu.trashView && fileMenu.forItem
        text: qsTr("Rename…")
        keys: "F2"
        onTriggered: fileMenu.renameRequested(fileMenu.target)
    }
    FolioMenuItem {
        shown: !fileMenu.trashView && fileMenu.forItem
        text: qsTr("Duplicate")
        keys: "Ctrl+Shift+D"
        onTriggered: fileOps.duplicate(fileMenu.target.path)
    }
    FolioMenuItem {
        shown: fileMenu.forItem
        text: qsTr("Copy")
        keys: "Ctrl+C"
        onTriggered: fileOps.copy(fileMenu.paths)
    }
    FolioMenuItem {
        shown: !fileMenu.trashView && fileMenu.forItem
        text: qsTr("Cut")
        keys: "Ctrl+X"
        onTriggered: fileOps.cut(fileMenu.paths)
    }
    FolioMenuItem {
        shown: !fileMenu.trashView && (!fileMenu.forItem || fileMenu.forFolder)
        text: fileMenu.forFolder ? qsTr("Paste Into Folder") : qsTr("Paste")
        keys: fileMenu.forItem ? "" : "Ctrl+V"
        enabled: fileOps.canPaste
        onTriggered: fileOps.paste(fileMenu.forItem ? fileMenu.target.path : fileMenu.folder)
    }
    FolioMenuItem {
        text: qsTr("Copy Path")
        keys: "Ctrl+Shift+C"
        onTriggered: fileOps.yank(fileMenu.forItem ? fileMenu.paths : [fileMenu.folder])
    }
    FolioMenuItem {
        shown: !fileMenu.forItem && !fileMenu.trashView
        text: qsTr("New Folder")
        keys: "Ctrl+Shift+N"
        onTriggered: fileOps.newFolder(fileMenu.folder)
    }
    MenuLine { }
    FolioMenuItem {
        shown: !fileMenu.trashView && fileMenu.forItem
        text: qsTr("Compress")
        onTriggered: fileOps.compress(fileMenu.paths)
    }
    // One archive unpacks into a new folder beside it.
    FolioMenuItem {
        shown: !fileMenu.trashView && fileMenu.forItem && !fileMenu.forSelection && fileOps.canExtract(fileMenu.target.mime)
        text: qsTr("Extract")
        onTriggered: fileOps.extract(fileMenu.target.path)
    }
    // Share offers only what this machine has: a Dropbox link for things in
    // the Dropbox folder (while Dropbox runs), Taildrop to the online
    // tailnet machines, and LocalSend when it's installed. With none of
    // them, there's no Share at all.
    readonly property bool canDropbox: forItem && target.path.startsWith(places.home + "/Dropbox/")
                                       && places.dropboxStatus !== "" && places.dropboxStatus !== "stopped"
    readonly property bool canTaildrop: forItem && places.peers.length > 0
    readonly property bool canLocalSend: forItem && fileOps.hasLocalSend()
    readonly property bool canShare: canDropbox || canTaildrop || canLocalSend

    // The entries are made only while they apply, rather than hidden:
    // opening Share from the keyboard highlights its first entry, even a
    // hidden one, and Return would run it.
    FolioMenu {
        id: shareMenu
        title: qsTr("Share")
        Repeater {
            model: fileMenu.canDropbox ? 1 : 0
            FolioMenuItem {
                text: qsTr("Dropbox Link")
                onTriggered: fileOps.dropboxLink(fileMenu.target.path)
            }
        }
        Repeater {
            model: fileMenu.canLocalSend ? 1 : 0
            FolioMenuItem {
                text: qsTr("LocalSend…")
                onTriggered: fileOps.shareLocalSend(fileMenu.paths)
            }
        }
    }
    // Taildrop is its own menu inside Share, one entry per machine; it's
    // added, after the other entries, only while there are machines to
    // send to.
    Instantiator {
        model: fileMenu.canTaildrop ? 1 : 0
        delegate: FolioMenu {
            title: qsTr("Taildrop")
            Repeater {
                model: places.peers
                FolioMenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: fileOps.shareTaildrop(fileMenu.paths, modelData.host)
                }
            }
        }
        onObjectAdded: (index, object) => shareMenu.addMenu(object)
        onObjectRemoved: (index, object) => shareMenu.removeMenu(object)
    }
    FolioMenuItem {
        shown: fileMenu.forFolder
        text: fileMenu.forFolder && places.isPinned(fileMenu.target.path) ? qsTr("Unpin") : qsTr("Pin")
        keys: "Ctrl+D"
        onTriggered: fileMenu.pinRequested(fileMenu.target.path)
    }
    FolioMenuItem {
        shown: !fileMenu.forItem || fileMenu.forFolder
        text: qsTr("Open Terminal Here")
        onTriggered: fileOps.openTerminal(fileMenu.forItem ? fileMenu.target.path : fileMenu.folder)
    }
    FolioMenuItem {
        shown: !fileMenu.forItem
        text: backend.folder.showHidden ? qsTr("Hide Hidden Files") : qsTr("Show Hidden Files")
        keys: "Ctrl+H"
        onTriggered: backend.folder.showHidden = !backend.folder.showHidden
    }
    MenuLine { shown: fileMenu.forItem }
    // Delete moves to the trash; Restore in any file manager brings it back.
    FolioMenuItem {
        shown: fileMenu.forItem
        text: fileMenu.trashView ? qsTr("Delete Permanently…")
            : fileMenu.forSelection ? qsTr("Delete %n Items", "", fileMenu.paths.length) : qsTr("Delete")
        keys: "Delete"
        onTriggered: fileMenu.trashView ? fileMenu.deleteRequested(fileMenu.paths) : fileOps.trash(fileMenu.paths)
    }
}
