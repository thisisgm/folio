#include "fileops.h"

#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent>
#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <clocale>
#include <cstdio>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#define signals Q_SIGNALS

// How often the window reads a copy's counts: ten times a second.
static constexpr int kProgressMs = 100;
// A copy's block: big enough to keep a drive busy, small enough for Stop and the count to move within a file.
static constexpr qint64 kBlock = 1 << 20;
// How long a copy waits on a source with nothing to read yet (a pipe nobody is writing to) before it looks at Stop again.
static constexpr int kStopCheckMs = 100;

FileOps::FileOps(QObject *parent) : QObject(parent) {
    if (auto *clipboard = QGuiApplication::clipboard())
        connect(clipboard, &QClipboard::dataChanged, this, &FileOps::clipboardChanged);
    m_progressTimer.setInterval(kProgressMs);
    connect(&m_progressTimer, &QTimer::timeout, this, &FileOps::readProgress);
}

QMimeData *FileOps::mimeDataFor(const QStringList &paths, bool cut) {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls << url;
        gnome += '\n' + url.toEncoded();
    }
    data->setUrls(urls);
    data->setData("x-special/gnome-copied-files", gnome);
    data->setText(paths.join('\n'));
    return data;
}

QStringList FileOps::pathsFrom(const QMimeData *data, bool *cut) {
    if (cut)
        *cut = false;
    QStringList paths;
    if (!data)
        return paths;
    if (data->hasFormat("x-special/gnome-copied-files")) {
        const QList<QByteArray> lines = data->data("x-special/gnome-copied-files").split('\n');
        if (cut && !lines.isEmpty())
            *cut = lines.first().trimmed() == "cut";
        for (int i = 1; i < lines.size(); ++i) {
            const QUrl url = QUrl::fromEncoded(lines[i].trimmed());
            if (url.isLocalFile())
                paths << url.toLocalFile();
        }
        return paths;
    }
    for (const QUrl &url : data->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

bool FileOps::canPaste() const {
    const auto *clipboard = QGuiApplication::clipboard();
    return clipboard && !pathsFrom(clipboard->mimeData(), nullptr).isEmpty();
}

void FileOps::copy(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, false));
}

void FileOps::cut(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, true));
}

void FileOps::yank(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setText(paths.join('\n'));
}

int FileOps::extensionStart(const QString &name, bool isDir) {
    int dot = isDir ? -1 : name.lastIndexOf('.');
    if (dot <= 0)
        return -1;
    if (dot > 4 && name.mid(dot - 4, 4).compare(".tar", Qt::CaseInsensitive) == 0)
        dot -= 4;
    return dot;
}

QString FileOps::freeName(const QString &folder, const QString &name) {
    const QDir dir(folder);
    if (!QFileInfo::exists(dir.filePath(name)))
        return name;
    const QFileInfo info(dir.filePath(name));
    const int dot = extensionStart(name, info.isDir());
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    for (int n = 1;; ++n) {
        const QString candidate = n == 1 ? QObject::tr("%1 (copy)%2").arg(base, ext)
                                         : QObject::tr("%1 (copy %2)%3").arg(base).arg(n).arg(ext);
        if (!QFileInfo::exists(dir.filePath(candidate)))
            return candidate;
    }
}

// QFile::copy a block at a time, counted and stoppable: under a hidden name, then renamed without overwriting.
static bool copyFile(const QString &from, const QString &to, FileOps::Progress *progress) {
    if (QFileInfo::exists(to))
        return false;
    QFile in;
    QTemporaryFile out(QFileInfo(to).absolutePath() + "/.folio-copy-XXXXXX");
    // Opened without waiting: a pipe nobody writes to would hold a plain open, and then a read, where Stop can't reach.
    const int source = ::open(QFile::encodeName(from).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (source < 0 || !in.open(source, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle) || !out.open())
        return false;
    // Where the drive shares blocks (btrfs, XFS), the copy is instant.
    if (::ioctl(out.handle(), FICLONE, in.handle()) == 0) {
        progress->done += in.size();
    } else {
        QByteArray block(kBlock, Qt::Uninitialized);
        qint64 written = 0, before = 0;
        for (;;) {
            // A file always has its next block; a pipe with nothing in it yet is waited on a little at a time, with Stop read in between.
            pollfd waiting{source, POLLIN, 0};
            const int ready = ::poll(&waiting, 1, kStopCheckMs);
            if (progress->stop || (ready < 0 && errno != EINTR))
                return false;
            if (ready <= 0)
                continue;
            const qint64 got = ::read(source, block.data(), block.size());
            if (got == 0)
                break;
            if (got < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (got < 0 || out.write(block.constData(), got) != got || !out.flush())
                return false;
            progress->done += got;
            // Waits for the block before, so the count leads the drive by one block, not gigabytes of cache.
            ::sync_file_range(out.handle(), written, got, SYNC_FILE_RANGE_WRITE);
            if (written > 0)
                ::sync_file_range(out.handle(), written - before, before,
                                  SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
            written += got;
            before = got;
        }
    }
    if (!out.flush() || !out.rename(to))
        return false;
    out.setAutoRemove(false);
    QFile::setPermissions(to, in.permissions());
    return true;
}

// The bytes copying `path` writes: its size, or all its files' sizes; links are made again, not copied.
static qint64 bytesIn(const QString &path, const FileOps::Progress &progress) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir())
        return info.isFile() && !info.isSymLink() ? info.size() : 0;
    qint64 bytes = 0;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext() && !progress.stop) {
        const QFileInfo entry = it.nextFileInfo();
        if (entry.isFile() && !entry.isSymLink())
            bytes += entry.size();
    }
    return bytes;
}

bool FileOps::copyRecursively(const QString &from, const QString &to, QString *error, bool move, Progress *progress) {
    // transfer() says what a stop left, so there's no error here.
    if (progress && progress->stop)
        return false;
    const QFileInfo info(from);
    if (info.isSymLink()) {
        // Links are copied as links, not followed, and as written: one
        // relative to its folder points into the copy, not at the original.
        if (!QFile::link(info.readSymLink(), to)) {
            *error = QObject::tr("Couldn't copy the link %1.").arg(info.fileName());
            return false;
        }
        if (move)
            QFile::remove(from);
        return true;
    }
    if (!info.isDir()) {
        // A pipe, a socket or a device moved within its drive is linked across, as the refused rename would have carried it; anywhere else it's read like a file.
        const bool carried = move && !info.isFile() && ::link(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
        if (!carried && !(progress ? copyFile(from, to, progress) : QFile::copy(from, to))) {
            *error = QObject::tr("Couldn't copy %1.").arg(info.fileName());
            return false;
        }
        // Linked or copied, the original then goes by its name, as for every file this copies (nothing removes a name only while it's still the same file); one that stays is the caller's to report.
        if (move)
            QFile::remove(from);
        return true;
    }
    // A folder that can't be read would list as empty and be copied empty.
    if (!info.isReadable()) {
        *error = QObject::tr("Couldn't read %1.").arg(info.fileName());
        return false;
    }
    // A folder keeps its permissions, as QFile::copy keeps a file's. It's
    // made with them, plus its owner's, so a private one is never open to
    // others even when the copy stops partway, and a read-only one can be
    // filled before it gets them exactly.
    if (!QDir().mkdir(to, info.permissions() | QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        *error = QObject::tr("Couldn't create %1.").arg(QFileInfo(to).fileName());
        return false;
    }
    const QDir source(from);
    const auto entries = source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries)
        if (!copyRecursively(entry.filePath(), QDir(to).filePath(entry.fileName()), error, move, progress))
            return false;
    QFile::setPermissions(to, info.permissions());
    // Only once it's empty: what turned up in it while the rest moved was
    // never copied, so it stays where it is, and the folder with it.
    if (move)
        QDir().rmdir(from);
    return true;
}

// The paths that aren't inside another of them. A folder takes what's in it
// along, so anything also chosen inside it (search results list both) would
// be copied or archived twice, be gone by its turn once the folder moved, or
// be deleted twice. Each path looks for the folders above it among the
// chosen, rather than at every other path, which for a Select All of
// thousands took minutes. A link on the way up is copied as a link, not
// gone into, so what was chosen beyond it stays chosen, and goes first:
// once the link had moved or gone to the trash, it would be out of reach.
static QStringList outermost(const QStringList &paths) {
    QSet<QString> chosen;
    for (const QString &path : paths)
        chosen.insert(QDir::cleanPath(path));
    QStringList beyondLinks, outer;
    for (const QString &path : paths) {
        const QString clean = QDir::cleanPath(path);
        bool inside = false, beyondLink = false;
        for (qsizetype slash = clean.lastIndexOf('/'); slash > 0 && !inside && !beyondLink;
             slash = clean.lastIndexOf('/', slash - 1)) {
            if (!chosen.contains(clean.left(slash)))
                continue;
            // Only now, for the few that have a chosen folder above them,
            // is each step between looked at on disk.
            for (qsizetype step = clean.lastIndexOf('/'); step >= slash && !beyondLink; step = clean.lastIndexOf('/', step - 1))
                beyondLink = QFileInfo(clean.left(step)).isSymLink();
            inside = !beyondLink;
        }
        if (beyondLink)
            beyondLinks << path;
        else if (!inside)
            outer << path;
    }
    return beyondLinks + outer;
}

// Whether `folder` is the folder at `path` or inside it, wherever links lead,
// so a link to a folder inside it counts. A link at `path` itself isn't a
// folder here, since it's copied as a link.
static bool inside(const QString &folder, const QString &path) {
    const QFileInfo item(path);
    const QString self = item.canonicalFilePath(), target = QFileInfo(folder).canonicalFilePath();
    return item.isDir() && !item.isSymLink() && (target == self || target.startsWith(self + '/'));
}

// A rename and nothing more, never onto a name that's taken. QFile::rename copies a file itself where a rename can't reach, uncounted and unstoppable.
static bool renameOnly(const QString &from, const QString &to) {
    return ::renameat2(AT_FDCWD, QFile::encodeName(from).constData(), AT_FDCWD, QFile::encodeName(to).constData(), RENAME_NOREPLACE) == 0;
}

QStringList FileOps::transfer(const QStringList &paths, const QString &folder, Mode mode, QString *error, Progress *progress) {
    QStringList made;
    const QString target = QDir::cleanPath(folder);
    const QStringList items = outermost(paths);
    // Counted first, to say how much of how much; a move on one drive is a rename and writes nothing.
    if (progress)
        for (const QString &path : items)
            if (!((mode == Move || mode == Drop) && sameDrive(path, target)))
                progress->total += bytesIn(path, *progress);
    for (const QString &path : items) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            *error = QObject::tr("%1 is no longer there.").arg(info.fileName());
            break;
        }
        const bool move = mode == Move || (mode == Drop && sameDrive(path, target));
        // Moving something into the folder it's already in does nothing.
        if (move && QDir::cleanPath(info.absolutePath()) == target)
            continue;
        if (progress && progress->stop) {
            *error = move ? QObject::tr("Stopped before %1 was moved.").arg(info.fileName())
                          : QObject::tr("Stopped before %1 was copied.").arg(info.fileName());
            break;
        }
        if (inside(target, path)) {
            *error = QObject::tr("Can't put %1 inside itself.").arg(info.fileName());
            break;
        }
        const QString name = freeName(target, info.fileName());
        const QString destination = QDir(target).filePath(name);
        // A rename is instant on the same drive; across drives a move is a
        // copy that removes each original once it's copied. A link is made
        // again rather than renamed, which across drives would copy what it
        // leads to.
        if (move && !info.isSymLink() && renameOnly(path, destination)) {
            made << name;
            continue;
        }
        // A refused rename is copied, whatever refused it (another drive, or one that can't rename without replacing: NFS, some FUSE mounts); on one drive that copy wasn't counted yet.
        if (progress && move && sameDrive(path, target))
            progress->total += bytesIn(path, *progress);
        if (!copyRecursively(path, destination, error, move, progress)) {
            // A stopped copy is shown and said to be part too, so it isn't taken for whole.
            const bool stopped = progress && progress->stop;
            // Each original went as it was copied, so what arrived is now the
            // only copy of it: it's shown, and said to be only part.
            if ((move || stopped) && QFileInfo::exists(destination)) {
                made << name;
                *error = !stopped ? QObject::tr("Moved only part of %1: %2").arg(info.fileName(), *error)
                       : move     ? QObject::tr("Stopped partway: only part of %1 was moved, and the rest is still in %2.")
                                    .arg(info.fileName(), info.absolutePath())
                                  : QObject::tr("Stopped partway: only part of %1 was copied.").arg(info.fileName());
            } else if (stopped) {
                *error = move ? QObject::tr("Stopped before %1 was moved.").arg(info.fileName())
                              : QObject::tr("Stopped before %1 was copied.").arg(info.fileName());
            }
            break;
        }
        made << name;
        // copyRecursively doesn't say when an original couldn't be removed: it's looked for here.
        const QFileInfo left(path);
        if (move && (left.exists() || left.isSymLink())) {
            *error = QObject::tr("Copied %1, but couldn't remove the original.").arg(info.fileName());
            break;
        }
    }
    return made;
}

void FileOps::paste(const QString &folder) {
    bool cut = false;
    const QStringList paths = pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut);
    if (paths.isEmpty() || !idle())
        return;
    // Only Delete puts things in the trash, with the note Restore needs.
    if (!QFileInfo(folder).isDir() || inTrash(folder)) {
        emit failed(tr("Files can't be pasted here."));
        return;
    }
    // Cut files are gone from where they were once moved; pasting them
    // again would find nothing, so the clipboard lets go of them.
    transferInBackground(paths, folder, cut ? Move : Copy, cut);
}

bool FileOps::sameDrive(const QString &path, const QString &folder) {
    // QStorageInfo follows a link, but a link is on the drive of the folder
    // it's in, wherever it leads.
    const QFileInfo item(path);
    return QStorageInfo(item.isSymLink() ? item.absolutePath() : path).device() == QStorageInfo(folder).device();
}

bool FileOps::canDrop(const QList<QUrl> &urls, const QString &folder) const {
    if (!QFileInfo(folder).isDir() || inTrash(folder))
        return false;
    const QString target = QDir::cleanPath(folder);
    bool any = false;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        // Not onto itself or inside itself.
        if (path == target || inside(target, path))
            return false;
        // Already there: nothing would happen.
        if (QFileInfo(path).absolutePath() != target)
            any = true;
    }
    return any;
}

void FileOps::dropInto(const QList<QUrl> &urls, const QString &folder) {
    if (!canDrop(urls, folder) || !idle())
        return;
    transferInBackground(localPaths(urls), folder, Drop, false);
}

QString FileOps::uriList(const QStringList &paths) const {
    QStringList uris;
    for (const QString &path : paths)
        uris << QString::fromLatin1(QUrl::fromLocalFile(path).toEncoded());
    return uris.join("\r\n");
}

QStringList FileOps::localPaths(const QList<QUrl> &urls) const {
    QStringList paths;
    for (const QUrl &url : urls)
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

template <typename Work, typename Done>
void FileOps::inBackground(const QString &busy, Work work, Done done) {
    setBusy(busy);
    auto *watcher = new QFutureWatcher<std::invoke_result_t<Work>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done = std::move(done)] {
        watcher->deleteLater();
        setBusy({});
        done(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

void FileOps::transferInBackground(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard,
                                   const QString &busy) {
    // A drop says what its first item does; each is moved or copied by its
    // own drive.
    const bool moving = mode == Move || (mode == Drop && sameDrive(paths.first(), folder));
    const QString showing = !busy.isEmpty() ? busy
                            : moving        ? tr("Moving %n items…", "", paths.size())
                                            : tr("Copying %n items…", "", paths.size());
    // The copy only writes `progress` and keeps it while it runs; the window reads it on a timer.
    auto progress = std::make_shared<Progress>();
    m_progress = progress;
    m_progressTimer.start();
    readProgress();
    inBackground(
        showing,
        [paths, folder, mode, progress] {
            QString error;
            const QStringList made = transfer(paths, folder, mode, &error, progress.get());
            return qMakePair(made, error);
        },
        [this, paths, folder, releaseClipboard](const QPair<QStringList, QString> &result) {
            m_progress.reset();
            m_progressTimer.stop();
            readProgress();
            const auto &[made, error] = result;
            // Only the cut that was pasted is let go of; something copied
            // while it moved stays on the clipboard.
            bool cut = false;
            if (releaseClipboard && error.isEmpty() && pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut) == paths && cut)
                QGuiApplication::clipboard()->clear();
            if (!error.isEmpty())
                emit failed(error);
            if (!made.isEmpty())
                emit done(folder, made.first());
        });
}

void FileOps::duplicate(const QString &path) {
    if (!idle())
        return;
    const QFileInfo info(path);
    transferInBackground({path}, info.absolutePath(), Copy, false, tr("Duplicating %1…").arg(info.fileName()));
}

QString FileOps::archiveName(const QStringList &paths, const QString &folder) {
    const QString base = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : tr("Archive");
    return freeName(folder, base + ".zip");
}

void FileOps::compress(const QStringList &selected) {
    const QStringList paths = outermost(selected);
    if (paths.isEmpty() || !idle())
        return;
    // Search results and Recents can span folders, so the archive goes in
    // the deepest folder holding them all, with their paths from there.
    const auto holds = [](const QString &folder, const QString &path) {
        const QString relative = QDir(folder).relativeFilePath(QFileInfo(path).absolutePath());
        return relative != ".." && !relative.startsWith("../");
    };
    QString folder = QFileInfo(paths.first()).absolutePath();
    for (const QString &path : paths)
        while (!holds(folder, path))
            folder = QFileInfo(folder).absolutePath();
    // bsdtar writes into a hidden folder of its own beside the archive, so a
    // half-written archive stays out of sight, and another compression into
    // this folder (from another window, or another Folio) can't write into
    // the same file or remove it when that one fails. The folder goes away
    // with what's left in it once bsdtar is done.
    auto temporary = std::make_shared<QTemporaryDir>(QDir(folder).filePath(".compressing-XXXXXX"));
    if (!temporary->isValid()) {
        emit failed(tr("Couldn't compress: %1").arg(temporary->errorString()));
        return;
    }
    const QString archive = archiveName(paths, folder);
    QStringList args{"-a", "-cf", temporary->filePath(archive), "-C", folder, "--"};
    for (const QString &path : paths)
        args << QDir(folder).relativeFilePath(path);
    runTool("bsdtar", args, tr("Compressing into %1…").arg(archive),
            tr("Compressing needs bsdtar, from the libarchive package."),
            [this, paths, folder, temporary, archive](QProcess *process, int code) {
                if (code != 0) {
                    emit failed(tr("Couldn't compress: %1").arg(QString::fromUtf8(process->readAllStandardError()).trimmed()));
                    return;
                }
                // Only a finished archive is named, with a name that's free
                // now. A rename never overwrites, so if another took that
                // name first, it takes the next.
                QFile made(temporary->filePath(archive));
                QString name = archiveName(paths, folder);
                while (!made.rename(QDir(folder).filePath(name))) {
                    if (!QFileInfo::exists(QDir(folder).filePath(name))) {
                        emit failed(tr("Couldn't compress: %1").arg(made.errorString()));
                        return;
                    }
                    name = archiveName(paths, folder);
                }
                emit done(folder, name);
            });
}

bool FileOps::canExtract(const QString &mime) {
    static const QStringList archives{"application/zip", "application/x-tar", "application/x-compressed-tar",
                                      "application/x-xz-compressed-tar", "application/x-bzip2-compressed-tar",
                                      "application/x-zstd-compressed-tar", "application/x-7z-compressed",
                                      "application/vnd.rar", "application/x-cpio"};
    return archives.contains(mime);
}

// From "Enter passphrase:plans.txt: Too many incorrect passphrases: Unknown error -1\nbsdtar: Error exit delayed from previous errors".
static QString extractError(const QString &archive, const QByteArray &errors) {
    // libarchive's endings for an entry it can't read without a password, after the entry's name.
    static const QStringList locked{"Passphrase required for this entry", "Incorrect passphrase",
                                    "Too many incorrect passphrases", "Encryption is not supported",
                                    "RAR encryption support unavailable", "is encrypted, but currently not supported",
                                    "Reading encrypted data is not currently supported"};
    // Its prompts, with no terminal to show them, and an error number that says nothing to a person.
    static const QRegularExpression prompts("^(Enter passphrase:)+"), number(": Unknown error -?\\d+$");
    QString why;
    for (QString line : QString::fromUtf8(errors).split('\n', Qt::SkipEmptyParts)) {
        line = line.remove(prompts).remove(number).trimmed();
        if (line.startsWith("bsdtar: "))
            line = line.mid(QStringLiteral("bsdtar: ").size());
        for (const QString &ending : locked)
            if (line.endsWith(ending))
                return FileOps::tr("Couldn't extract %1: it's protected by a password.").arg(archive);
        if (why.isEmpty() && !line.startsWith("Removing leading") && line != "Error exit delayed from previous errors")
            why = line;
    }
    return why.isEmpty() ? FileOps::tr("Couldn't extract %1.").arg(archive) : FileOps::tr("Couldn't extract %1: %2").arg(archive, why);
}

void FileOps::extract(const QString &path) {
    if (path.isEmpty() || !idle())
        return;
    const QFileInfo info(path);
    const QString archive = info.fileName(), folder = info.absolutePath();
    const int dot = extensionStart(archive, false);
    // "photos" for photos.tar.gz; "..zip" would make "." of it.
    QString name = dot > 0 ? archive.left(dot) : archive;
    if (name == "." || name == "..")
        name = archive;
    // Unpacked out of sight beside the archive and named once done; removed off the UI thread, as it can hold many files.
    std::shared_ptr<QTemporaryDir> temporary(new QTemporaryDir(QDir(folder).filePath(".extracting-XXXXXX")),
                                             [](QTemporaryDir *dir) { QThreadPool::globalInstance()->start([dir] { delete dir; }); });
    // A folder made inside it gets the usual permissions; the hidden one is its owner's alone.
    if (!temporary->isValid()) {
        emit failed(tr("Couldn't extract %1: %2").arg(archive, temporary->errorString()));
        return;
    }
    const QString unpacked = temporary->filePath(name);
    if (!QDir().mkdir(unpacked)) {
        emit failed(tr("Couldn't extract %1.").arg(archive));
        return;
    }
    // Never -P, so bsdtar refuses "..", drops a leading "/", and won't write through a link it unpacked.
    const QStringList args{"-x", "--no-same-owner", "--no-same-permissions", "-f", info.absoluteFilePath(), "-C", unpacked};
    // No input and no terminal, so a password prompt fails at once instead of waiting.
    auto *process = new QProcess(this);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    // Its messages in C, the one wording extractError reads; names stay in the user's character set.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("LC_ALL");
    environment.insert("LC_CTYPE", QString::fromLocal8Bit(setlocale(LC_CTYPE, nullptr)));
    environment.insert("LC_MESSAGES", "C");
    process->setProcessEnvironment(environment);
    startTool(process, "bsdtar", args, tr("Extracting %1…").arg(archive), tr("Extracting needs bsdtar, from the libarchive package."),
              [this, temporary, unpacked, folder, name, archive](QProcess *process, int code) {
                  // What it unpacked goes before saying why, still busy so nothing starts on top of it.
                  const auto fail = [this, temporary, archive](const QString &why) {
                      temporary->setAutoRemove(false);
                      inBackground(
                          tr("Extracting %1…").arg(archive), [path = temporary->path()] { return QDir(path).removeRecursively(); },
                          [this, why, path = temporary->path()](bool removed) {
                              emit failed(removed ? why : why + ' ' + tr("What it unpacked is left in %1.").arg(path));
                          });
                  };
                  if (code != 0) {
                      fail(extractError(archive, process->readAllStandardError()));
                      return;
                  }
                  // A rename never overwrites, so if another took the free name first, it takes the next.
                  QFile made(unpacked);
                  QString named = freeName(folder, name);
                  while (!made.rename(QDir(folder).filePath(named))) {
                      if (!QFileInfo::exists(QDir(folder).filePath(named))) {
                          fail(tr("Couldn't extract %1: %2").arg(archive, made.errorString()));
                          return;
                      }
                      named = freeName(folder, name);
                  }
                  emit done(folder, named);
              });
}

void FileOps::shareLocalSend(const QStringList &paths) {
    // The same command Omarchy's Nautilus extension runs.
    if (paths.isEmpty())
        return;
    if (!QProcess::startDetached("localsend", QStringList{"--headless", "send"} + paths))
        emit failed(tr("Sharing nearby needs LocalSend, which isn't installed."));
}

bool FileOps::hasLocalSend() const {
    return !QStandardPaths::findExecutable("localsend").isEmpty();
}

void FileOps::dropboxLink(const QString &path) {
    const QString name = QFileInfo(path).fileName();
    runTool("dropbox-cli", {"sharelink", path}, tr("Getting a Dropbox link for %1…").arg(name),
            tr("Dropbox links need dropbox-cli, which isn't installed."),
            [this, name](QProcess *process, int code) {
                const QString out = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                // dropbox-cli prints the link, or a sentence saying why not.
                const QString link = out.section('\n', -1).trimmed();
                if (code != 0 || !link.startsWith("https://")) {
                    const QString why = out.isEmpty() ? QString::fromUtf8(process->readAllStandardError()).trimmed() : out;
                    emit failed(tr("Couldn't get a Dropbox link for %1: %2").arg(name, why));
                    return;
                }
                QGuiApplication::clipboard()->setText(link);
                emit notice(tr("Dropbox link for %1 copied.").arg(name));
            });
}

void FileOps::shareTaildrop(const QStringList &paths, const QString &host) {
    if (paths.isEmpty())
        return;
    const QString machine = host.section('.', 0, 0);
    runTool("tailscale", QStringList{"file", "cp"} + paths + QStringList{host + ":"}, tr("Sending to %1…").arg(machine),
            tr("Sending with Taildrop needs tailscale, which isn't installed."),
            [this, machine](QProcess *process, int code) {
                if (code != 0)
                    emit failed(tr("Couldn't send to %1: %2").arg(machine, QString::fromUtf8(process->readAllStandardError()).trimmed()));
            });
}

void FileOps::trash(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    // GIO follows the freedesktop Trash spec, including each drive's own
    // .Trash folder, so Restore in any file manager puts things back. It
    // takes one item at a time, which on a slow drive adds up.
    inBackground(
        tr("Moving %n items to the trash…", "", paths.size()),
        [paths] {
            QStringList failedNames;
            for (const QString &path : outermost(paths)) {
                GFile *file = g_file_new_for_path(path.toUtf8().constData());
                GError *error = nullptr;
                if (!g_file_trash(file, nullptr, &error)) {
                    failedNames << QFileInfo(path).fileName() + (error ? QStringLiteral(" (") + QString::fromUtf8(error->message) + ")" : QString());
                    if (error)
                        g_error_free(error);
                }
                g_object_unref(file);
            }
            return failedNames;
        },
        [this](const QStringList &failedNames) {
            if (!failedNames.isEmpty())
                emit failed(tr("Couldn't move to the trash: %1").arg(failedNames.join(", ")));
        });
}

QString FileOps::homeTrash() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
}

QStringList FileOps::trashDirs() {
    QStringList dirs{homeTrash()};
    // Drives keep their own trash at the top, so deleting never copies
    // across disks: .Trash-<uid> (or .Trash/<uid>).
    const QString uid = QString::number(getuid());
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        for (const QString &dir : {volume.rootPath() + "/.Trash-" + uid, volume.rootPath() + "/.Trash/" + uid})
            if (QFileInfo(dir + "/files").isDir() && !dirs.contains(dir))
                dirs << dir;
    }
    return dirs;
}

bool FileOps::inTrash(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    for (const QString &dir : trashDirs())
        if (clean == dir + "/files" || clean.startsWith(dir + "/files/"))
            return true;
    return false;
}

QString FileOps::infoFileFor(const QString &trashedPath) {
    // Only what's directly in a trash's files folder was trashed on its own,
    // with a note. What's inside a trashed folder came along with it and has
    // none; looking one level up would find a file in a trashed folder that
    // happens to be called "info".
    const QFileInfo item(trashedPath);
    const QString folder = QDir::cleanPath(item.absolutePath());
    for (const QString &dir : trashDirs())
        if (folder == QDir::cleanPath(dir + "/files"))
            return dir + "/info/" + item.fileName() + ".trashinfo";
    return {};
}

QString FileOps::originalPath(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n')) {
        if (!line.startsWith("Path="))
            continue;
        const QString path = QUrl::fromPercentEncoding(line.mid(5).trimmed());
        if (path.startsWith('/'))
            return path;
        // A drive's own trash records paths from the top of that drive.
        const QString trashDir = QFileInfo(QFileInfo(trashedPath).absolutePath()).absolutePath();
        const QString top = trashDir.contains("/.Trash/") ? trashDir.section("/.Trash/", 0, 0)
                                                          : QFileInfo(trashDir).absolutePath();
        return QDir(top).filePath(path);
    }
    return {};
}

QDateTime FileOps::deletedAt(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n'))
        if (line.startsWith("DeletionDate="))
            // Local time, without a zone, as the spec writes it.
            return QDateTime::fromString(QString::fromUtf8(line.mid(13).trimmed()), Qt::ISODate);
    return {};
}

QString FileOps::restoreOne(const QString &trashedPath, QString *error) {
    const QString original = originalPath(trashedPath);
    if (original.isEmpty()) {
        *error = tr("%1 has no record of where it came from.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    const QString folder = QFileInfo(original).absolutePath();
    if (!QDir().mkpath(folder)) {
        *error = tr("Couldn't recreate %1.").arg(folder);
        return {};
    }
    const QString destination = QDir(folder).filePath(freeName(folder, QFileInfo(original).fileName()));
    // A link is made again rather than renamed, which across drives would
    // copy what it leads to.
    if (!QFileInfo(trashedPath).isSymLink() && QFile::rename(trashedPath, destination)) {
        QFile::remove(infoFileFor(trashedPath));
        return destination;
    }
    // Across drives (rare: trash lives on the item's own drive), it moves
    // the way a move across drives does, each original removed once it's
    // copied. The note stays while anything of it is left in the trash.
    if (!copyRecursively(trashedPath, destination, error, true)) {
        // What arrived is now the only copy of it, so where it went is said.
        if (QFileInfo::exists(destination))
            *error = tr("Restored only part of %1, to %2: %3").arg(QFileInfo(trashedPath).fileName(), destination, *error);
        return {};
    }
    if (QFileInfo::exists(trashedPath) || QFileInfo(trashedPath).isSymLink()) {
        *error = tr("Restored %1, but couldn't remove it from the trash.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    QFile::remove(infoFileFor(trashedPath));
    return destination;
}

bool FileOps::deleteTrashed(const QString &trashedPath) {
    const QFileInfo item(trashedPath);
    const bool removed = item.isDir() && !item.isSymLink() ? QDir(trashedPath).removeRecursively()
                                                           : QFile::remove(trashedPath);
    // The note goes once its item is gone, so what's left can be restored.
    if (const QString note = infoFileFor(trashedPath); removed && !note.isEmpty())
        QFile::remove(note);
    return removed;
}

int FileOps::emptyTrashDir(const QString &trashDir) {
    int count = 0;
    const QDir files(trashDir + "/files");
    for (const QFileInfo &item : files.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        if (deleteTrashed(item.absoluteFilePath()))
            ++count;
    // The size cache goes too. A note without its item stays: something
    // being trashed gets its note first, so it may be on its way.
    QFile::remove(trashDir + "/directorysizes");
    return count;
}

int FileOps::trashCount() const {
    int count = 0;
    for (const QString &dir : trashDirs())
        count += QDir(dir + "/files").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
    return count;
}

void FileOps::restore(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    // Restoring across drives copies, which for a folder can take a while.
    inBackground(
        tr("Restoring %n items…", "", paths.size()),
        [paths] {
            QStringList failures;
            QString last;
            for (const QString &path : outermost(paths)) {
                QString error;
                const QString to = restoreOne(path, &error);
                if (to.isEmpty())
                    failures << error;
                else
                    last = to;
            }
            return qMakePair(failures, last);
        },
        [this, count = paths.size()](const QPair<QStringList, QString> &result) {
            const auto &[failures, last] = result;
            if (!failures.isEmpty())
                emit failed(failures.join(' '));
            else if (!last.isEmpty())
                emit notice(count == 1 ? tr("Restored %1 to %2.").arg(QFileInfo(last).fileName(), QFileInfo(last).absolutePath())
                                       : tr("Restored %n items.", "", count));
        });
}

void FileOps::deleteForever(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    inBackground(
        tr("Deleting %n items…", "", paths.size()),
        [paths] {
            int failures = 0;
            for (const QString &path : outermost(paths))
                if (!deleteTrashed(path))
                    ++failures;
            return failures;
        },
        [this](int failures) {
            if (failures > 0)
                emit failed(tr("Couldn't delete %n items.", "", failures));
        });
}

void FileOps::emptyTrash() {
    if (!idle())
        return;
    inBackground(
        tr("Emptying the trash…"),
        [] {
            int count = 0;
            for (const QString &dir : trashDirs())
                count += emptyTrashDir(dir);
            return count;
        },
        [this](int count) { emit notice(tr("Emptied the trash: %n items deleted.", "", count)); });
}

QString FileOps::rename(const QString &path, const QString &name) {
    const QString clean = name.trimmed();
    const QFileInfo info(path);
    if (clean.isEmpty() || clean == info.fileName())
        return {};
    if (clean.contains('/'))
        return tr("A name can't contain \"/\".");
    const QString destination = QDir(info.absolutePath()).filePath(clean);
    // QFile::rename never overwrites, and lets a name change only its case
    // on drives that ignore case, where the new name already "exists".
    if (!QFile::rename(path, destination))
        return QFileInfo::exists(destination) ? tr("%1 already exists here.").arg(clean)
                                              : tr("Couldn't rename %1.").arg(info.fileName());
    emit done(info.absolutePath(), clean);
    return {};
}

void FileOps::newFolder(const QString &folder) {
    if (!QFileInfo(folder).isDir()) {
        emit failed(tr("Folders can't be made here."));
        return;
    }
    QString name = tr("New Folder");
    for (int n = 2; QFileInfo::exists(QDir(folder).filePath(name)); ++n)
        name = tr("New Folder %1").arg(n);
    if (!QDir(folder).mkdir(name)) {
        emit failed(tr("Couldn't create a folder here."));
        return;
    }
    emit done(folder, name);
}

QVariantList FileOps::appsFor(const QString &path) const {
    QVariantList apps;
    const QString mime = QFileInfo(path).isDir() ? QStringLiteral("inode/directory")
                                                  : QMimeDatabase().mimeTypeForFile(path).name();
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return apps;
    GAppInfo *fallback = g_app_info_get_default_for_type(type, FALSE);
    const QString defaultId = fallback ? QString::fromUtf8(g_app_info_get_id(fallback)) : QString();
    if (fallback)
        g_object_unref(fallback);
    GList *all = g_app_info_get_all_for_type(type);
    for (GList *l = all; l; l = l->next) {
        auto *info = G_APP_INFO(l->data);
        if (!g_app_info_should_show(info))
            continue;
        const QString id = QString::fromUtf8(g_app_info_get_id(info));
        const QVariantMap app{{"id", id}, {"name", QString::fromUtf8(g_app_info_get_name(info))},
                              {"isDefault", id == defaultId}};
        if (id == defaultId)
            apps.prepend(app);
        else
            apps << app;
    }
    g_list_free_full(all, g_object_unref);
    g_free(type);
    return apps;
}

void FileOps::openWith(const QString &path, const QString &appId) {
    GDesktopAppInfo *app = g_desktop_app_info_new(appId.toUtf8().constData());
    if (!app)
        return;
    GList *files = g_list_append(nullptr, g_file_new_for_path(path.toUtf8().constData()));
    GError *error = nullptr;
    if (!g_app_info_launch(G_APP_INFO(app), files, nullptr, &error))
        emit failed(tr("Couldn't open %1 with %2: %3")
                        .arg(QFileInfo(path).fileName(), QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))),
                             QString::fromUtf8(error ? error->message : "")));
    if (error)
        g_error_free(error);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
}

void FileOps::openTerminal(const QString &folder) {
    // xdg-terminal-exec starts the terminal Omarchy is set up with.
    if (!QProcess::startDetached("xdg-terminal-exec", {"--dir=" + folder}, folder))
        emit failed(tr("Couldn't start a terminal: xdg-terminal-exec isn't installed."));
}

void FileOps::runTool(const QString &program, const QStringList &args, const QString &busy, const QString &missing,
                      const std::function<void(QProcess *, int)> &finished) {
    if (!idle())
        return;
    startTool(new QProcess(this), program, args, busy, missing, finished);
}

void FileOps::startTool(QProcess *process, const QString &program, const QStringList &args, const QString &busy,
                        const QString &missing, const std::function<void(QProcess *, int)> &finished) {
    setBusy(busy);
    connect(process, &QProcess::finished, this, [this, process, finished](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        setBusy({});
        finished(process, status == QProcess::NormalExit ? code : -1);
    });
    // A program that isn't installed never finishes, so this is the only
    // word from it.
    connect(process, &QProcess::errorOccurred, this, [this, process, missing](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        setBusy({});
        emit failed(missing);
    });
    process->start(program, args);
}

void FileOps::stop() {
    if (m_progress)
        m_progress->stop = true;
}

void FileOps::readProgress() {
    const qint64 done = m_progress ? m_progress->done.load() : 0, total = m_progress ? m_progress->total.load() : 0;
    if (done == m_bytesDone && total == m_bytesTotal && m_canStop == !!m_progress)
        return;
    m_bytesDone = done;
    m_bytesTotal = total;
    m_canStop = !!m_progress;
    emit progressChanged();
}

bool FileOps::idle() {
    if (m_busy.isEmpty())
        return true;
    emit failed(tr("Try again once the work under way is done."));
    return false;
}

void FileOps::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
