# Files

![Files showing a folder of photos in the grid, with the details pane](docs/grid.png)

Files (`folio`) is Omarchy's file manager. It's one quiet window that follows your Omarchy theme and language, works from the keyboard, and never crawls your home folder in the background: folders are read when you open them, and thumbnails are made when a tile needs one, shared with every other app through `~/.cache/thumbnails`.

- **Three views**: a grid with real previews, a list, and Finder-style columns. Ctrl+1/2/3 switches.
- **Space previews** pictures, video, audio, PDFs, text and Markdown at full size, in their own window.
- **The sidebar** holds everything in your home folder, pinned folders (shared with GTK file dialogs), Recents, Trash with restore, your drives, and the machines on your tailnet, which open over SFTP.
- **Right-click** (or Alt+Enter) to open with another app, rename, duplicate, compress, extract, share by Dropbox link, Taildrop or LocalSend, and delete. Copy and paste work with other apps, and nothing is ever overwritten.
- **Split** the window in two with Ctrl+Alt+N to move things between folders.

| Columns | Preview |
| --- | --- |
| ![The column view, four folders deep](docs/columns.png) | ![A photo in the preview window](docs/preview.png) |

## Keys

| Key | Does |
| --- | --- |
| Arrows, ⏎ | Move, open |
| Space | Preview |
| Backspace, Alt+← / Alt+→ | Up, back, forward |
| Ctrl+L | Type a path |
| / | Search here and below |
| Ctrl+C / X / V | Copy, cut, paste |
| F2, Delete | Rename, move to the trash |
| Ctrl+Shift+N | New folder |
| Ctrl+D | Pin the folder |
| Ctrl+N, Ctrl+Alt+N | New window, split |
| Tab | Sidebar and back |
| ? | Every other shortcut |

## Building

Files is C++ and Qt Quick. `bin/build` compiles it into `build/folio`, `bin/test` runs the tests, and `bin/install` builds and installs the Arch package. Omarchy installs it from [omarchy-pkgs](https://github.com/omacom/omarchy-pkgs).

## License

Files is released under the [MIT License](LICENSE).
