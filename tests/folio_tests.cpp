#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QMimeDatabase>
#include <memory>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtConcurrent>
#include <QtTest>
#include <cerrno>
#include <clocale>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "fileops.h"
#include "foldermodel.h"
#include "places.h"
#include "previewer.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

// Puts XDG_DATA_HOME, where the trash and recent files live, in `path` for
// as long as the returned guard does.
[[nodiscard]] static auto dataHomeAt(const QString &path)
{
    const QByteArray old = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(path));
    return qScopeGuard([old] {
        if (old.isNull())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", old);
    });
}

// A temporary folder on another drive than QTemporaryDir's, for what has to
// cross drives: beside the tests, when they're on another drive. Null when
// they aren't.
static std::unique_ptr<QTemporaryDir> elsewhere()
{
    auto dir = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-XXXXXX"));
    if (!dir->isValid() || FileOps::sameDrive(dir->path(), QDir::tempPath()))
        return {};
    return dir;
}

class FolioTests : public QObject {
    Q_OBJECT

private slots:
    void folderListsDirectoriesFirstWithoutHidden()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("zeta");
        write(dir.filePath("alpha.md"), "# hi");
        write(dir.filePath("photo10.jpg"), "x");
        write(dir.filePath("photo9.jpg"), "x");
        write(dir.filePath(".secret"), "x");

        QString error;
        const auto entries = FolderModel::list(dir.path(), false, "name", &error);
        QVERIFY(error.isEmpty());
        QStringList names;
        for (const auto &e : entries) names << e.name;
        // Folders first, then natural order, so photo9 comes before photo10.
        QCOMPARE(names, QStringList({"zeta", "alpha.md", "photo9.jpg", "photo10.jpg"}));
        QCOMPARE(entries.at(0).kind, QString("folder"));
        QCOMPARE(entries.at(1).kind, QString("text"));
        QCOMPARE(entries.at(2).kind, QString("image"));
        QCOMPARE(entries.at(2).extension, QString("JPG"));

        QCOMPARE(FolderModel::list(dir.path(), true, "name", &error).size(), 5);
    }

    void folderReportsMissingFolders()
    {
        QString error;
        QVERIFY(FolderModel::list("/definitely/not/here", false, "name", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void folderSummaryCountsAndPeeks()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.jpg"), "x");
        write(dir.filePath("b.png"), "x");
        write(dir.filePath("c.png"), "x");
        write(dir.filePath("notes.txt"), "x");
        write(dir.filePath(".hidden"), "x");
        FolderModel::Entry entry;
        entry.path = dir.path();
        FolderModel::summarize(entry, false);
        QCOMPARE(entry.itemCount, 4);
        QCOMPARE(entry.peek.size(), 2);
        // The folder counts just count, the hidden only when they show.
        QCOMPARE(FolderModel::itemCount(dir.path(), false), 4);
        QCOMPARE(FolderModel::itemCount(dir.path(), true), 5);
    }

    void folderModelLoadsInTheBackground()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.get(0).value("name").toString(), QString("one.txt"));
        QCOMPARE(model.indexOf("one.txt"), 0);
    }

    void folderSearchesEveryFolderBelow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos/2024");
        QDir(dir.path()).mkpath(".cache");
        write(dir.filePath("beach notes.txt"), "x");
        write(dir.filePath("Photos/2024/Beach.jpg"), "x");
        write(dir.filePath("Photos/city.jpg"), "x");
        write(dir.filePath(".cache/beach.tmp"), "x");

        auto names = [&](bool hidden) {
            QStringList out;
            for (const auto &e : FolderModel::search(dir.path(), "beach", hidden, "name"))
                out << e.name;
            return out.join(", ");
        };
        // Named by where they are, ignoring case, hidden ones only when shown.
        QCOMPARE(names(false), QString("beach notes.txt, Photos/2024/Beach.jpg"));
        QCOMPARE(names(true), QString(".cache/beach.tmp, beach notes.txt, Photos/2024/Beach.jpg"));
        // Stopped, it gives nothing rather than half an answer.
        QVERIFY(FolderModel::search(dir.path(), "beach", false, "name", [] { return true; }).isEmpty());

        // In the model, a query swaps the folder for its matches; clearing
        // it, or going elsewhere, brings the folder back.
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.setQuery("BEACH");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.get(1).value("path").toString(), dir.filePath("Photos/2024/Beach.jpg"));
        model.setQuery("");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.indexOf("Photos"), 0);
        model.setQuery("city");
        QVERIFY(loaded.wait(3000));
        model.setPath(dir.filePath("Photos"));
        QVERIFY(model.query().isEmpty());
    }

    void folderSearchSkipsSymlinkedFolders()
    {
        // A link back up the tree would otherwise be walked round and round.
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("real");
        write(dir.filePath("real/match.txt"), "x");
        QVERIFY(QFile::link(dir.path(), dir.filePath("loop")));
        const auto found = FolderModel::search(dir.path(), "match", false, "name");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().name, QString("real/match.txt"));
    }

    void folderModelSwapsFoldersWithoutAnEmptyFrame()
    {
        QTemporaryDir a, b;
        write(a.filePath("one.txt"), "x");
        write(b.filePath("two.txt"), "x");
        write(b.filePath("three.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));

        // Moving on keeps the old entries until the new ones are in.
        QSignalSpy counts(&model, &FolderModel::countChanged);
        model.setPath(b.path());
        QCOMPARE(model.count(), 1);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(counts.count(), 1); // straight from one to two, never zero

        // A column with no folder clears at once.
        model.setPath(QString());
        QCOMPARE(model.count(), 0);

        // Going back to a folder just seen shows it in the same step, and
        // the background re-read leaves it alone when nothing changed.
        loaded.clear();
        model.setPath(a.path());
        QCOMPARE(model.count(), 1);
        QCOMPARE(loaded.count(), 1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.count(), 1);
    }

    void folderModelCountsFoldersAfterAQuickReread()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos");
        QDir(dir.path()).mkpath("Music");
        write(dir.filePath("Photos/a.jpg"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        // A re-read before the counts came in still ends with them.
        model.reload();
        QVERIFY(loaded.wait(3000));
        QTRY_COMPARE(model.get(0).value("itemCount").toInt(), 0);
        QTRY_COMPARE(model.get(1).value("itemCount").toInt(), 1);
    }

    void folderModelForgetsAFolderLeftWhileLoading()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(model.loading());
        // Emptied before the listing lands, the column stays empty.
        model.setPath(QString());
        QVERIFY(!model.loading());
        QVERIFY(!loaded.wait(200));
        QCOMPARE(model.count(), 0);
    }

    void folderModelStopsReadingWhatsBeenOvertaken()
    {
        // A big folder left before it's been read, or a search of it, stops
        // being read instead of holding on to a thread of the pool that the
        // next listing, the folder counts and file transfers all wait for.
        // With a pool of one thread, the next folder waits for the overtaken
        // read to stop, so it shows in a fraction of the time the read takes.
        QTemporaryDir big, small, other;
        for (int i = 0; i < 20000; ++i)
            write(big.filePath(QString("%1.txt").arg(i)), "x");
        write(small.filePath("a.txt"), "x");
        write(other.filePath("b.txt"), "x");
        QElapsedTimer timer;
        timer.start();
        FolderModel::list(big.path(), false, "name", nullptr);
        const qint64 listing = timer.restart();
        FolderModel::search(big.path(), "none", false, "name");
        const qint64 searching = timer.elapsed();

        QThreadPool *pool = QThreadPool::globalInstance();
        pool->waitForDone();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        const auto restore = qScopeGuard([&] { pool->setMaxThreadCount(threads); });
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);

        model.setPath(big.path());
        timer.restart();
        model.setPath(small.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < listing / 2);

        model.setPath(big.path());
        QVERIFY(loaded.wait(5000));
        model.setQuery("none");
        timer.restart();
        model.setPath(other.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < searching / 2);
    }

    void folderModelTellsCopiedFoldersApart()
    {
        // A copy (cp -a, rsync -a) lists exactly like its original, but its
        // items are its own.
        QTemporaryDir a, b;
        const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
        for (const QTemporaryDir *dir : {&a, &b}) {
            QFile file(dir->filePath("report.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("same");
            file.flush();
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        }
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));
        model.setPath(b.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.get(0).value("path").toString(), b.filePath("report.txt"));
    }

    void fileOpsNeverOverwrite()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.tar.gz"), "x");
        QDir(dir.path()).mkdir("Photos");
        QCOMPARE(FileOps::freeName(dir.path(), "fresh.txt"), QString("fresh.txt"));
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy).tar.gz"));
        write(dir.filePath("notes (copy).tar.gz"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy 2).tar.gz"));
        QCOMPARE(FileOps::freeName(dir.path(), "Photos"), QString("Photos (copy)"));
        // The extension starts at the last dot, so dots in the name stay.
        QCOMPARE(FileOps::extensionStart("Report v1.2.pdf", false), 11);
        write(dir.filePath("Report v1.2.pdf"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "Report v1.2.pdf"), QString("Report v1.2 (copy).pdf"));
    }

    void fileOpsRenameNeverOverwrites()
    {
        // The rename itself refuses to overwrite. Nothing checks for the new
        // name first, since on drives that ignore case it "exists" when only
        // its case changes; the tests' drive heeds case, though, so the case
        // change at the end is an ordinary rename here.
        QTemporaryDir dir;
        write(dir.filePath("IMG.JPG"), "photo");
        write(dir.filePath("notes.txt"), "notes");
        QDir(dir.path()).mkdir("Album");
        QDir(dir.path()).mkdir("Empty");
        FileOps ops;
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "notes.txt"), QString("notes.txt already exists here."));
        QCOMPARE(ops.rename(dir.filePath("Album"), "Empty"), QString("Empty already exists here."));
        QFile kept(dir.filePath("notes.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("notes"));
        QVERIFY(QFileInfo(dir.filePath("Album")).isDir());
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "img.jpg"), QString());
        QVERIFY(QFile::exists(dir.filePath("img.jpg")));
    }

    void fileOpsCopyAndMove()
    {
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        write(from.filePath("todo.txt"), "list");
        write(to.filePath("todo.txt"), "already here");

        QString error;
        QStringList made = FileOps::transfer({from.filePath("album"), from.filePath("todo.txt")}, to.path(), FileOps::Copy, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"album", "todo (copy).txt"}));
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        QFile kept(to.filePath("todo.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("already here"));
        QVERIFY(QFile::exists(from.filePath("todo.txt"))); // copied, not moved

        made = FileOps::transfer({from.filePath("todo.txt")}, to.path(), FileOps::Move, &error);
        QCOMPARE(made, QStringList({"todo (copy 2).txt"}));
        QVERIFY(!QFile::exists(from.filePath("todo.txt")));

        // A folder can't go inside itself.
        error.clear();
        FileOps::transfer({to.filePath("album")}, to.filePath("album/inner"), FileOps::Move, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        // Nor through a link to a folder inside it, which would copy it into
        // its own copy until the drive was full.
        QVERIFY(QFile::link(to.filePath("album/inner"), from.filePath("inner")));
        error.clear();
        FileOps::transfer({to.filePath("album")}, from.filePath("inner"), FileOps::Copy, &error);
        QCOMPARE(error, QString("Can't put album inside itself."));
        QVERIFY(!QFile::exists(to.filePath("album/inner/album")));
    }

    void fileOpsMoveAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Across drives, a move copies, then removes the original. What
        // turns up in a folder while the rest of it copies was never copied,
        // so it stays where it is.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkdir("box");
        // Copying a pipe waits until whoever writes to it is done, which
        // holds the move after it has looked in the folder.
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QStringList> moved = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error);
        });
        int writer = -1;
        // Even when a check fails, the move is let go of and finishes
        // before what it uses is gone.
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(from.filePath("box/new.txt"), "new");
        close(std::exchange(writer, -1));
        QCOMPARE(moved.result(), QStringList({"box"}));
        QVERIFY(QFile::exists(from.filePath("box/new.txt")));
        QCOMPARE(error, QString("Copied box, but couldn't remove the original."));
        QVERIFY(!QFileInfo::exists(from.filePath("box/pipe")));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes.
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        error.clear();
        QCOMPARE(FileOps::transfer({from.filePath("album")}, other->path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));
    }

    void fileOpsSayWhenAMoveAcrossDrivesStopsPartway()
    {
        // Each original goes as it's copied, so a move that stops partway
        // (a full USB stick) has already taken some of the folder. What
        // arrived is shown and said to be only part, not left looking like
        // a failed copy that's safe to delete.
        if (geteuid() == 0)
            QSKIP("Root can read every file.");
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkpath("box/a");
        QDir(from.path()).mkpath("box/z");
        write(from.filePath("box/a/1.txt"), "1");
        write(from.filePath("box/m.txt"), "m");
        write(from.filePath("box/z/2.txt"), "2");
        QVERIFY(QFile::setPermissions(from.filePath("box/m.txt"), QFile::WriteOwner));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error), QStringList({"box"}));
        QCOMPARE(error, QString("Moved only part of box: Couldn't copy m.txt."));
        QVERIFY(QFile::exists(other->filePath("box/a/1.txt")));
        QVERIFY(!QFile::exists(from.filePath("box/a/1.txt")));
        QVERIFY(QFile::exists(from.filePath("box/z/2.txt")));
    }

    void fileOpsCountWhatACopyWrites()
    {
        // A copy counts what it will write, then each block, and keeps what QFile::copy keeps; a rename writes nothing.
        QTemporaryDir from, to;
        write(from.filePath(".folio-test"), "");
        write(to.filePath(".folio-test"), "");
        QDir(from.path()).mkpath("album/empty");
        // Three blocks and a little more.
        const QByteArray big(3 * (1 << 20) + 5, 'x');
        write(from.filePath("album/big.raw"), big);
        write(from.filePath("album/.hidden"), "hi");
        QVERIFY(QFile::link("big.raw", from.filePath("album/link")));
        // A mode no new file has, executable and the group's, so only copying it gets it there.
        const auto mode = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup;
        QVERIFY(QFile::setPermissions(from.filePath("album/big.raw"), mode));
        const auto copyInto = [&](const QString &folder) {
            FileOps::Progress progress;
            QString error;
            QCOMPARE(FileOps::transfer({from.filePath("album")}, folder, FileOps::Copy, &error, &progress), QStringList({"album"}));
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(progress.total.load(), big.size() + 2);
            QCOMPARE(progress.done.load(), progress.total.load());
            QFile copy(QDir(folder).filePath("album/big.raw"));
            QVERIFY(copy.open(QIODevice::ReadOnly));
            QCOMPARE(copy.readAll(), big);
            QCOMPARE(copy.permissions(), QFile(from.filePath("album/big.raw")).permissions());
            QCOMPARE(QFileInfo(QDir(folder).filePath("album/link")).readSymLink(), QString("big.raw"));
            QCOMPARE(QDir(QDir(folder).filePath("album")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name),
                     QStringList({".hidden", "big.raw", "empty", "link"}));
        };
        // On one drive it's a clone where the drive shares blocks (btrfs, XFS) and blocks where not (ext4, tmpfs): both count the same.
        copyInto(to.path());
        if (QTest::currentTestFailed())
            return;

        FileOps::Progress moving;
        QString error;
        QCOMPARE(FileOps::transfer({to.filePath("album")}, from.path(), FileOps::Move, &error, &moving), QStringList({"album (copy)"}));
        QCOMPARE(moving.total.load(), 0);
        QCOMPARE(moving.done.load(), 0);

        // A clone never crosses drives, so from another drive it's blocks on every drive.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        copyInto(other->path());
        if (QTest::currentTestFailed())
            return;

        // A file whose rename is refused (here by another drive; a drive that can't rename without replacing refuses the same way) is counted: it takes the copy a folder's files take, where Stop is read at every block, not a rename that copies by itself.
        const auto kept = QFile(from.filePath("album/big.raw")).permissions();
        FileOps::Progress crossing;
        QCOMPARE(FileOps::transfer({from.filePath("album/big.raw")}, other->path(), FileOps::Move, &error, &crossing), QStringList({"big.raw"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(crossing.total.load(), big.size());
        QCOMPARE(crossing.done.load(), big.size());
        QVERIFY(!QFileInfo::exists(from.filePath("album/big.raw")));
        QFile moved(other->filePath("big.raw"));
        QVERIFY(moved.open(QIODevice::ReadOnly));
        QCOMPARE(moved.readAll(), big);
        QCOMPARE(moved.permissions(), kept);
    }

    void fileOpsStopACopyOrAMovePartway()
    {
        // Stopped partway, a copy or move keeps what arrived and says it's part; a move leaves the rest where it was.
        QTemporaryDir from, to;
        write(from.filePath(".folio-test"), "");
        write(to.filePath(".folio-test"), "");
        QDir(from.path()).mkdir("box");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/z.txt"), "z");
        // A pipe holds the copy one block into a file: its writer is still there, so the copy waits for more.
        const QByteArray block(1 << 20, 'p');
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        const auto stopInThePipe = [&](const QString &folder, FileOps::Mode mode) {
            FileOps::Progress progress;
            QString error;
            QFuture<QStringList> running = QtConcurrent::run([&] {
                return FileOps::transfer({from.filePath("box")}, folder, mode, &error, &progress);
            });
            int writer = -1;
            // Even when a check fails, the copy is let go of and finishes before what it uses is gone.
            const auto finish = qScopeGuard([&] {
                if (writer >= 0)
                    close(writer);
                running.waitForFinished();
            });
            if (QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; })
                && fcntl(writer, F_SETFL, 0) == 0 && ::write(writer, block.constData(), block.size()) == block.size()
                && QTest::qWaitFor([&] { return progress.done == 1 + block.size(); })) {
                progress.stop = true;
                // Stopped, the copy lets go of the pipe though its writer has neither written more nor left.
                if (!QTest::qWaitFor([&] { return running.isFinished(); }))
                    return qMakePair(QStringList(), QString("the copy went on waiting for the pipe"));
            }
            close(std::exchange(writer, -1));
            const QStringList made = running.result();
            return qMakePair(made, error);
        };
        QCOMPARE(stopInThePipe(to.path(), FileOps::Copy),
                 qMakePair(QStringList({"box"}), QString("Stopped partway: only part of box was copied.")));
        QCOMPARE(QDir(to.filePath("box")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System),
                 QStringList({"a.txt"}));
        QCOMPARE(QDir(from.filePath("box")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::Name),
                 QStringList({"a.txt", "pipe", "z.txt"}));

        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QCOMPARE(stopInThePipe(other->path(), FileOps::Move),
                 qMakePair(QStringList({"box"}),
                           QString("Stopped partway: only part of box was moved, and the rest is still in %1.").arg(from.path())));
        QCOMPARE(QDir(other->filePath("box")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System),
                 QStringList({"a.txt"}));
        QCOMPARE(QDir(from.filePath("box")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::Name),
                 QStringList({"pipe", "z.txt"}));
    }

    void fileOpsMoveAPipeWithinItsDriveAsItIs()
    {
        // The copy a move takes when its rename is refused carries a pipe across as a pipe, within one drive: read, it would wait for a writer who may never come.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("box");
        write(from.filePath("box/a.txt"), "a");
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0640) == 0);
        FileOps::Progress progress;
        QString error;
        QFuture<bool> moved = QtConcurrent::run([&] {
            return FileOps::copyRecursively(from.filePath("box"), to.filePath("box"), &error, true, &progress);
        });
        // Even when a check fails, a move waiting on the pipe is let go of before what it uses is gone.
        const auto finish = qScopeGuard([&] {
            const int writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK);
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QTRY_VERIFY(moved.isFinished());
        QVERIFY2(moved.result(), qPrintable(error));
        struct stat arrived;
        QCOMPARE(lstat(QFile::encodeName(to.filePath("box/pipe")).constData(), &arrived), 0);
        QVERIFY(S_ISFIFO(arrived.st_mode));
        QCOMPARE(int(arrived.st_mode & 0777), 0640);
        QCOMPARE(QDir(to.filePath("box")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::Name),
                 QStringList({"a.txt", "pipe"}));
        QVERIFY(!QFileInfo::exists(from.filePath("box")));
        QCOMPARE(progress.done.load(), 1);
    }

    void fileOpsShowACopysProgressAndStopIt()
    {
        // The window sees a copy's bytes and stops it; stopped before its only file was written, nothing is left.
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        const QByteArray pipe = QFile::encodeName(dir.filePath("pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QVERIFY(!ops.canStop());
        ops.duplicate(dir.filePath("pipe"));
        QVERIFY(ops.canStop());
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            QTRY_VERIFY(ops.busy().isEmpty());
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        // One block in, and the writer stays, so the copy waits for more.
        const QByteArray block(1 << 20, 'p');
        QCOMPARE(fcntl(writer, F_SETFL, 0), 0);
        QCOMPARE(::write(writer, block.constData(), block.size()), block.size());
        QTRY_COMPARE(ops.bytesDone(), block.size());
        QCOMPARE(ops.busy(), QString("Duplicating pipe…"));
        ops.stop();
        // Stop needs nothing more from the pipe: its writer is still there when the copy ends.
        QTRY_COMPARE(failed.count(), 1);
        close(std::exchange(writer, -1));
        QCOMPARE(failed.first().first().toString(), QString("Stopped before pipe was copied."));
        QVERIFY(!ops.canStop());
        QCOMPARE(ops.bytesDone(), 0);
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name),
                 QStringList({".folio-test", "pipe"}));
    }

    void fileOpsCopyLinksAsWritten()
    {
        // A link relative to its folder points into the copy, not back at
        // the original.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("project");
        write(from.filePath("project/data.txt"), "data");
        QVERIFY(QFile::link("data.txt", from.filePath("project/link")));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("project")}, to.path(), FileOps::Copy, &error), QStringList({"project"}));
        QCOMPARE(QFileInfo(to.filePath("project/link")).readSymLink(), QString("data.txt"));
    }

    void fileOpsMoveLinksAcrossDrivesAsLinks()
    {
        // A link moved or restored to another drive arrives as the link it
        // was, not as a copy of what it leads to. One dropped from another
        // drive is copied, wherever it leads.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, data, folder;
        write(here.filePath("data.txt"), "data");
        QVERIFY(QFile::link("data.txt", here.filePath("relative")));
        QVERIFY(QFile::link(here.filePath("data.txt"), here.filePath("absolute")));
        QString error;
        QCOMPARE(FileOps::transfer({here.filePath("relative"), here.filePath("absolute")}, other->path(), FileOps::Move, &error),
                 QStringList({"relative", "absolute"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(other->filePath("relative")).readSymLink(), QString("data.txt"));
        QCOMPARE(QFileInfo(other->filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(!QFileInfo(here.filePath("relative")).isSymLink());

        const auto restore = dataHomeAt(data.path());
        QDir().mkpath(data.filePath("Trash/files"));
        QDir().mkpath(data.filePath("Trash/info"));
        QVERIFY(QFile::link(here.filePath("data.txt"), data.filePath("Trash/files/trashed")));
        write(data.filePath("Trash/info/trashed.trashinfo"), "[Trash Info]\nPath=" + other->filePath("trashed").toUtf8() + "\n");
        QCOMPARE(FileOps::restoreOne(data.filePath("Trash/files/trashed"), &error), other->filePath("trashed"));
        QCOMPARE(QFileInfo(other->filePath("trashed")).readSymLink(), here.filePath("data.txt"));

        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(other->filePath("absolute"))}, folder.path());
        QVERIFY(done.wait(3000));
        QCOMPARE(QFileInfo(folder.filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(QFileInfo(other->filePath("absolute")).isSymLink());
    }

    void fileOpsCopyFolderPermissions()
    {
        // A private folder stays private, and a read-only one is filled
        // before it's made read-only.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("private");
        QDir(from.path()).mkdir("locked");
        write(from.filePath("locked/a.txt"), "a");
        const auto all = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(from.filePath("private"), all));
        QVERIFY(QFile::setPermissions(from.filePath("locked"), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("locked"), all);
            QFile::setPermissions(to.filePath("locked"), all);
        });
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("private"), from.filePath("locked")}, to.path(), FileOps::Copy, &error),
                 QStringList({"private", "locked"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(to.filePath("locked/a.txt")));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
        QCOMPARE(QFileInfo(to.filePath("locked")).permissions(), QFileInfo(from.filePath("locked")).permissions());
    }

    void fileOpsCopyOnlyWhatCanBeRead()
    {
        // A folder that can't be read would look empty, so copying it fails
        // rather than making an empty copy.
        if (geteuid() == 0)
            QSKIP("Root can read every folder.");
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("secret");
        write(from.filePath("secret/a.txt"), "a");
        QVERIFY(QFile::setPermissions(from.filePath("secret"), QFile::WriteOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("secret"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(FileOps::transfer({from.filePath("secret")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't read secret."));
        QVERIFY(!QFile::exists(to.filePath("secret")));

        // A copy that stops at a file that can't be read is left as private
        // as the folder it came from.
        QDir(from.path()).mkpath("private/keys");
        write(from.filePath("private/z.txt"), "z");
        QVERIFY(QFile::setPermissions(from.filePath("private"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(from.filePath("private/z.txt"), QFile::WriteOwner));
        QVERIFY(FileOps::transfer({from.filePath("private")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't copy z.txt."));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
    }

    void fileOpsClipboardFormats()
    {
        std::unique_ptr<QMimeData> data(FileOps::mimeDataFor({"/tmp/a b.txt", "/tmp/c.txt"}, true));
        QCOMPARE(data->text(), QString("/tmp/a b.txt\n/tmp/c.txt"));
        QVERIFY(data->data("x-special/gnome-copied-files").startsWith("cut\nfile:///tmp/a%20b.txt"));
        bool cut = false;
        QCOMPARE(FileOps::pathsFrom(data.get(), &cut), QStringList({"/tmp/a b.txt", "/tmp/c.txt"}));
        QVERIFY(cut);

        // From another app that only offers a URI list: a copy.
        QMimeData plain;
        plain.setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QCOMPARE(FileOps::pathsFrom(&plain, &cut), QStringList({"/tmp/x.png"}));
        QVERIFY(!cut);
    }

    void fileOpsCompressAcrossFolders()
    {
        // Search results and Recents can hold items from different folders:
        // the archive goes in the folder holding them all, with their paths.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("a/inner");
        QDir(dir.path()).mkpath("b");
        write(dir.filePath("a/inner/x.txt"), "x");
        write(dir.filePath("b/y.txt"), "y");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        const auto compress = [&](const QStringList &names) {
            QStringList paths;
            for (const QString &name : names)
                paths << dir.filePath(name);
            ops.compress(paths);
            if (!done.wait(5000))
                return QStringList();
            const QList<QVariant> made = done.takeFirst();
            QProcess list;
            list.start("bsdtar", {"-tf", QDir(made[0].toString()).filePath(made[1].toString())});
            list.waitForFinished();
            return QStringList({made[0].toString(), made[1].toString()})
                 + QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(compress({"a/inner/x.txt", "b/y.txt"}), QStringList({dir.path(), "Archive.zip", "a/inner/x.txt", "b/y.txt"}));
        // A folder beside a file inside it (as search results list them):
        // the archive holds the folder, and the file once, within it.
        QCOMPARE(compress({"a/inner/x.txt", "a/inner"}), QStringList({dir.filePath("a"), "inner.zip", "inner/", "inner/x.txt"}));
    }

    void fileOpsCompressNeverOverwritesAnArchive()
    {
        // Two windows compressing into one folder at once each get an
        // archive of their own, and one that fails takes only its own with
        // it, leaving nothing half-written behind.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt"})
            write(dir.filePath(name), name);
        FileOps one, two, three;
        QSignalSpy doneOne(&one, &FileOps::done), doneTwo(&two, &FileOps::done), failed(&three, &FileOps::failed);
        one.compress({dir.filePath("a.txt"), dir.filePath("b.txt")});
        two.compress({dir.filePath("b.txt"), dir.filePath("c.txt")});
        three.compress({dir.filePath("a.txt"), dir.filePath("gone.txt")});
        QTRY_VERIFY(doneOne.count() == 1 && doneTwo.count() == 1 && failed.count() == 1);
        const auto members = [&](const QSignalSpy &done) {
            QProcess list;
            list.start("bsdtar", {"-tf", dir.filePath(done.first().at(1).toString())});
            list.waitForFinished();
            return QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(members(doneOne), QStringList({"a.txt", "b.txt"}));
        QCOMPARE(members(doneTwo), QStringList({"b.txt", "c.txt"}));
        QStringList names{doneOne.first().at(1).toString(), doneTwo.first().at(1).toString()};
        names.sort();
        QCOMPARE(names, QStringList({"Archive (copy).zip", "Archive.zip"}));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                 QStringList({"Archive (copy).zip", "Archive.zip", "a.txt", "b.txt", "c.txt"}));
    }

    void fileOpsExtractIntoAFolderBeside()
    {
        // An archive unpacks as it is into a folder named after it, beside it; a taken name gets "(copy)", never a merge.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        const QString top = "-my 'odd' ñ";
        QDir(dir.path()).mkpath("src/" + top);
        write(dir.filePath("src/" + top + "/\"quoted\" día.txt"), "hola");
        const QString archive = dir.filePath(top + ".tar.gz");
        QCOMPARE(QProcess::execute("bsdtar", {"-a", "-cf", archive, "-C", dir.filePath("src"), "--", top}), 0);
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done), failed(&ops, &FileOps::failed);
        ops.extract(archive);
        QCOMPARE(ops.busy(), QString("Extracting %1.tar.gz…").arg(top));
        QVERIFY(done.wait(5000));
        QCOMPARE(done.last(), QVariantList({dir.path(), top}));
        QFile file(dir.filePath(top + "/" + top + "/\"quoted\" día.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("hola"));
        ops.extract(archive);
        QVERIFY(done.wait(5000));
        QCOMPARE(done.last(), QVariantList({dir.path(), top + " (copy)"}));
        QVERIFY(QFile::exists(dir.filePath(top + " (copy)/" + top + "/\"quoted\" día.txt")));
        QVERIFY(failed.isEmpty());
        // They get a new folder's permissions, and nothing they were unpacked in is left beside them.
        QVERIFY(QDir(dir.path()).mkdir("usual"));
        QCOMPARE(QFileInfo(dir.filePath(top)).permissions(), QFileInfo(dir.filePath("usual")).permissions());
        QTRY_COMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                     QStringList({top, top + " (copy)", top + ".tar.gz", ".folio-test", "src", "usual"}));
    }

    void fileOpsExtractOnlyArchives()
    {
        // bsdtar unpacks archives, not a lone compressed file, so Extract isn't offered for notes.txt.gz.
        const auto offered = [](const char *name) {
            return FileOps::canExtract(QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name());
        };
        for (const char *name : {"a.zip", "a.tar", "a.tar.gz", "a.tgz", "a.tar.xz", "a.tar.bz2", "a.tar.zst", "a.7z", "a.rar", "a.cpio"})
            QVERIFY2(offered(name), name);
        for (const char *name : {"notes.txt.gz", "notes.txt.xz", "notes.txt.zst", "notes.txt", "a.pdf"})
            QVERIFY2(!offered(name), name);
    }

    void fileOpsExtractLeavesNothingWhenItFails()
    {
        // Nothing looks finished until it's done, and a failed run takes only what it unpacked, not a folder made meanwhile.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        QDir(dir.path()).mkpath("src/via/link");
        QDir(dir.path()).mkpath("outside");
        write(dir.filePath("outside/kept.txt"), "kept");
        write(dir.filePath("src/encryption.md"), "x");
        write(dir.filePath("src/via/link/pwn"), "x");
        QVERIFY(QFile::link(dir.filePath("outside"), dir.filePath("src/link")));
        // Entries "../encryption.md", then "link" to the folder outside and "link/pwn" written through it.
        const QString archive = dir.filePath("evil.tar");
        QCOMPARE(QProcess::execute("bsdtar", {"-P", "-cf", archive, "-s", ",^encryption.md$,../encryption.md,", "-C",
                                              dir.filePath("src"), "encryption.md", "link", "-C", dir.filePath("src/via"), "link/pwn"}),
                 0);
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract(archive);
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name),
                 QStringList({"evil.tar", "outside", "src"}));
        QVERIFY(QDir(dir.path()).mkdir("evil"));
        write(dir.filePath("evil/mine.txt"), "mine");
        QVERIFY(failed.wait(5000));
        // Said as bsdtar says it: the entry's name is no password problem.
        QCOMPARE(failed.last().at(0).toString(), QString("Couldn't extract evil.tar: ../encryption.md: Path contains '..'"));
        QCOMPARE(ops.busy(), QString());
        QVERIFY(!QFileInfo::exists(dir.filePath("encryption.md")));
        QVERIFY(!QFileInfo::exists(dir.filePath("outside/pwn")));
        QVERIFY(QFile::exists(dir.filePath("outside/kept.txt")));
        QVERIFY(QFile::exists(dir.filePath("evil/mine.txt")));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                 QStringList({".folio-test", "evil", "evil.tar", "outside", "src"}));
    }

    void fileOpsExtractSaysWhatsWrong()
    {
        // A password, a crash, bsdtar missing and a read-only folder each fail at once, say so, and leave nothing.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        write(dir.filePath("plans.txt"), "secret");
        const QString archive = dir.filePath("plans.zip");
        QCOMPARE(QProcess::execute("bsdtar", {"--format", "zip", "--options", "zip:encryption=zipcrypt", "--passphrase",
                                              "sesame", "-cf", archive, "-C", dir.path(), "plans.txt"}),
                 0);
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract(archive);
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(0).toString(), QString("Couldn't extract plans.zip: it's protected by a password."));

        // A bsdtar that crashes says nothing, and the sentence still ends.
        const QString bin = dir.filePath("bin");
        QVERIFY(QDir().mkdir(bin));
        write(bin + "/bsdtar", "#!/bin/sh\nkill -KILL $$\n");
        QVERIFY(QFile::setPermissions(bin + "/bsdtar", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QByteArray path = qgetenv("PATH");
        auto restore = qScopeGuard([path] { qputenv("PATH", path); });
        qputenv("PATH", QFile::encodeName(bin));
        ops.extract(archive);
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(0).toString(), QString("Couldn't extract plans.zip."));
        QVERIFY(QFile::remove(bin + "/bsdtar"));
        ops.extract(archive);
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(0).toString(), QString("Extracting needs bsdtar, from the libarchive package."));
        restore.dismiss();
        qputenv("PATH", path);
        QTRY_COMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                     QStringList({".folio-test", "bin", "plans.txt", "plans.zip"}));

        if (geteuid() == 0)
            QSKIP("Root can write in a read-only folder.");
        QVERIFY(QDir().mkdir(dir.filePath("locked")));
        QVERIFY(QFile::copy(archive, dir.filePath("locked/plans.zip")));
        QVERIFY(QFile::setPermissions(dir.filePath("locked"), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] { QFile::setPermissions(dir.filePath("locked"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
        ops.extract(dir.filePath("locked/plans.zip"));
        QCOMPARE(failed.last().at(0).toString(), "Couldn't extract plans.zip: " + qt_error_string(EACCES));
        QCOMPARE(QDir(dir.filePath("locked")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList({"plans.zip"}));
    }

    void fileOpsExtractReadsBsdtarInC()
    {
        // bsdtar's messages come in C under any locale, so a password still reads as one; names keep the user's characters.
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        write(dir.filePath("plans.zip"), "");
        const QString bin = dir.filePath("bin");
        QVERIFY(QDir().mkdir(bin));
        const QByteArray path = qgetenv("PATH");
        const QList<QByteArray> names{"LC_ALL", "LC_MESSAGES", "LC_CTYPE"};
        QList<QByteArray> was;
        for (const QByteArray &name : names)
            was << qgetenv(name.constData());
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            for (int i = 0; i < names.size(); ++i)
                was[i].isNull() ? qunsetenv(names[i].constData()) : qputenv(names[i].constData(), was[i]);
        });
        qputenv("PATH", QFile::encodeName(bin));
        for (const QByteArray &name : names)
            qputenv(name.constData(), "de_DE.UTF-8");
        const auto standIn = [&](const QByteArray &script) {
            write(bin + "/bsdtar", "#!/bin/sh\n" + script + "exit 1\n");
            QVERIFY(QFile::setPermissions(bin + "/bsdtar", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        };
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        // As bsdtar 3.8 says it for a locked zip: in German unless its messages are C.
        standIn("if [ -z \"${LC_ALL-}\" ] && [ \"${LC_MESSAGES-}\" = C ]; then e='Unknown error -1'; else e='Unbekannter Fehler -1'; fi\n"
                "printf 'Enter passphrase:plans.txt: Too many incorrect passphrases: %s\\n' \"$e\" >&2\n");
        ops.extract(dir.filePath("plans.zip"));
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(0).toString(), QString("Couldn't extract plans.zip: it's protected by a password."));
        standIn("echo \"plans.txt: LC_ALL=${LC_ALL-unset} LC_MESSAGES=${LC_MESSAGES-unset} LC_CTYPE=${LC_CTYPE-unset}\" >&2\n");
        ops.extract(dir.filePath("plans.zip"));
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().at(0).toString(), QString("Couldn't extract plans.zip: plans.txt: LC_ALL=unset LC_MESSAGES=C LC_CTYPE=%1")
                                                     .arg(QString::fromLocal8Bit(setlocale(LC_CTYPE, nullptr))));
    }

    void fileOpsExtractNamesWhatARenameLeft()
    {
        // An unpack that can't be named goes the way a failed one does: removed, or named where it's left.
        if (geteuid() == 0)
            QSKIP("Root can write in a read-only folder.");
        QTemporaryDir dir;
        write(dir.filePath(".folio-test"), "");
        QVERIFY(QDir().mkdir(dir.filePath("bin")));
        QVERIFY(QDir().mkdir(dir.filePath("in")));
        write(dir.filePath("in/plans.zip"), "");
        // Unpacks a file into the folder it's given, the last argument, then makes the archive's folder read-only.
        write(dir.filePath("bin/bsdtar"), "#!/bin/sh\nfor last; do :; done\necho x > \"$last/plans.txt\" && chmod a-w \"$(dirname \"$(dirname \"$last\")\")\"\n");
        QVERIFY(QFile::setPermissions(dir.filePath("bin/bsdtar"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QByteArray path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            QFile::setPermissions(dir.filePath("in"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        qputenv("PATH", QFile::encodeName(dir.filePath("bin")) + ':' + path);
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed), done(&ops, &FileOps::done);
        ops.extract(dir.filePath("in/plans.zip"));
        QVERIFY(failed.wait(5000));
        const QStringList left = QDir(dir.filePath("in")).entryList({".extracting-*"}, QDir::Dirs | QDir::Hidden);
        const QString message = failed.last().at(0).toString();
        QVERIFY2(left.size() == 1, qPrintable(message));
        QVERIFY2(message.startsWith("Couldn't extract plans.zip: "), qPrintable(message));
        QVERIFY2(message.endsWith(" What it unpacked is left in " + dir.filePath("in/" + left.first()) + "."), qPrintable(message));
        QVERIFY(done.isEmpty());
        QCOMPARE(ops.busy(), QString());
    }

    void fileOpsTakeAFolderOnceWithWhatsInIt()
    {
        // Search results can list a folder and what's in it. The folder
        // takes those along, so they aren't copied twice, found gone once
        // it has moved, deleted twice, or restored without a note of their
        // own.
        QTemporaryDir from, copies, moves, trash;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        const QStringList both{from.filePath("album"), from.filePath("album/inner/a.jpg")};
        QString error;
        QCOMPARE(FileOps::transfer(both, copies.path(), FileOps::Copy, &error), QStringList({"album"}));
        QVERIFY(!QFile::exists(copies.filePath("a.jpg")));
        QCOMPARE(FileOps::transfer(both, moves.path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("album/inner/a.jpg")));

        QDir(trash.path()).mkpath("files/album");
        write(trash.filePath("files/album/a.jpg"), "picture");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.deleteForever({trash.filePath("files/album"), trash.filePath("files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(!QFileInfo::exists(trash.filePath("files/album")));

        const auto restore = dataHomeAt(trash.path());
        QDir(trash.path()).mkpath("Trash/files/album");
        QDir(trash.path()).mkpath("Trash/info");
        write(trash.filePath("Trash/files/album/a.jpg"), "picture");
        write(trash.filePath("Trash/info/album.trashinfo"), "[Trash Info]\nPath=" + copies.filePath("restored").toUtf8() + "\n");
        ops.restore({trash.filePath("Trash/files/album"), trash.filePath("Trash/files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(QFile::exists(copies.filePath("restored/a.jpg")));
    }

    void fileOpsKeepWhatsChosenBeyondALink()
    {
        // A link is copied as a link, not gone into, so something chosen
        // through it (Recents lists both) is still copied in its own right.
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("real/inner");
        write(from.filePath("real/inner/a.txt"), "a");
        QVERIFY(QFile::link("real", from.filePath("link")));
        QDir(from.path()).mkdir("box");
        QVERIFY(QFile::link("../real", from.filePath("box/through")));
        QString error;
        const QStringList chosen{from.filePath("link"), from.filePath("link/inner/a.txt"),
                                 from.filePath("box"), from.filePath("box/through/inner/a.txt")};
        QCOMPARE(FileOps::transfer(chosen, to.path(), FileOps::Copy, &error),
                 QStringList({"a.txt", "a (copy).txt", "link", "box"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(to.filePath("link")).isSymLink());
        QVERIFY(QFileInfo(to.filePath("box/through")).isSymLink());

        // Moved, what's beyond the link goes before the link does, while
        // it can still be reached through it.
        QTemporaryDir moves;
        QCOMPARE(FileOps::transfer({from.filePath("link"), from.filePath("link/inner/a.txt")}, moves.path(), FileOps::Move, &error),
                 QStringList({"a.txt", "link"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("a.txt")));
        QVERIFY(QFileInfo(moves.filePath("link")).isSymLink());
    }

    void fileOpsRestoreAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Restoring across drives moves the way a move across drives does:
        // what turns up in the trashed folder meanwhile stays in the trash,
        // with the note that can bring it back.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + other->filePath("box").toUtf8() + "\n");
        const QByteArray pipe = QFile::encodeName(trash + "/files/box/pipe");
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QString> restored = QtConcurrent::run([&] { return FileOps::restoreOne(trash + "/files/box", &error); });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            restored.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(trash + "/files/box/new.txt", "new");
        close(std::exchange(writer, -1));
        QCOMPARE(restored.result(), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(trash + "/files/box/new.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes, and its note with it.
        QDir().mkpath(trash + "/files/album/inner");
        write(trash + "/files/album/inner/a.jpg", "picture");
        write(trash + "/info/album.trashinfo", "[Trash Info]\nPath=" + other->filePath("album").toUtf8() + "\n");
        error.clear();
        QCOMPARE(FileOps::restoreOne(trash + "/files/album", &error), other->filePath("album"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(trash + "/files/album"));
        QVERIFY(!QFile::exists(trash + "/info/album.trashinfo"));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));

        // One that can't be copied at all keeps its note.
        if (geteuid() != 0) {
            write(trash + "/files/locked.txt", "x");
            write(trash + "/info/locked.txt.trashinfo", "[Trash Info]\nPath=" + other->filePath("locked.txt").toUtf8() + "\n");
            QVERIFY(QFile::setPermissions(trash + "/files/locked.txt", QFile::WriteOwner));
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/locked.txt", &error), QString());
            QCOMPARE(error, QString("Couldn't copy locked.txt."));
            QVERIFY(QFile::exists(trash + "/info/locked.txt.trashinfo"));

            // One that stops partway says where what arrived went, and keeps
            // its note for the rest.
            QDir().mkpath(trash + "/files/part/a");
            write(trash + "/files/part/a/1.txt", "1");
            write(trash + "/files/part/m.txt", "m");
            QVERIFY(QFile::setPermissions(trash + "/files/part/m.txt", QFile::WriteOwner));
            write(trash + "/info/part.trashinfo", "[Trash Info]\nPath=" + other->filePath("part").toUtf8() + "\n");
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/part", &error), QString());
            QCOMPARE(error, QString("Restored only part of part, to %1: Couldn't copy m.txt.").arg(other->filePath("part")));
            QVERIFY(QFile::exists(other->filePath("part/a/1.txt")));
            QVERIFY(QFile::exists(trash + "/info/part.trashinfo"));
        }
    }

    void fileOpsWeedOutWhatsInsideQuickly()
    {
        // Select All in a big folder hands over every item. Comparing each
        // with every other to find what's inside another took most of a
        // minute for 20,000 photos before the first was touched.
        QTemporaryDir dir;
        QStringList paths{dir.filePath("gone")};
        for (int i = 0; i < 20000; ++i)
            paths << dir.filePath(QString("photos/%1.jpg").arg(i));
        QElapsedTimer timer;
        timer.start();
        // The first is missing, so it stops there, once they're weeded out.
        QString error;
        FileOps::transfer(paths, dir.path(), FileOps::Copy, &error);
        QCOMPARE(error, QString("gone is no longer there."));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString("Took %1 ms.").arg(timer.elapsed())));
    }

    void fileOpsLetGoOfOnlyThePastedCut()
    {
        // Cut files are gone once pasted, so the clipboard lets go of them,
        // but not of something copied while they moved.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        write(from.filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.cut({from.filePath("a.txt")});
        ops.paste(to.path());
        ops.copy({from.filePath("b.txt")});
        QVERIFY(done.wait(3000));
        bool cut = true;
        QCOMPARE(FileOps::pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut), QStringList({from.filePath("b.txt")}));
        QVERIFY(!cut);

        ops.cut({from.filePath("b.txt")});
        ops.paste(to.path());
        QVERIFY(done.wait(3000));
        QVERIFY(!ops.canPaste());
    }

    void fileOpsDropMovesOnlyWhatsOnTheSameDrive()
    {
        // Each dropped file is moved from the folder's drive and copied from
        // any other, whichever comes first.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, folder;
        write(here.filePath("a.txt"), "a");
        write(other->filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(here.filePath("a.txt")), QUrl::fromLocalFile(other->filePath("b.txt"))},
                     folder.path());
        QVERIFY(done.wait(3000));
        QVERIFY(QFile::exists(folder.filePath("a.txt")));
        QVERIFY(QFile::exists(folder.filePath("b.txt")));
        QVERIFY(!QFile::exists(here.filePath("a.txt")));
        QVERIFY(QFile::exists(other->filePath("b.txt")));
    }

    void fileOpsDragUriLists()
    {
        // Names holding what means something in a URI ("#", "?", "%") are
        // dragged as the same paths they came from.
        FileOps ops;
        const QStringList paths{"/tmp/Track #1.mp3", "/tmp/what?.txt", "/tmp/100% done/caf\u00e9.txt"};
        QMimeData data;
        data.setData("text/uri-list", ops.uriList(paths).toUtf8());
        QCOMPARE(ops.localPaths(data.urls()), paths);
        // Only local files are dropped as paths.
        QCOMPARE(ops.localPaths({QUrl("https://example.com/a"), QUrl::fromLocalFile("/tmp/x")}), QStringList({"/tmp/x"}));
    }

    void fileOpsGuardDropsRenamesAndNames()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("album/inner");
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.txt"), "c");
        const auto urls = [&](const QString &name) { return QList<QUrl>{QUrl::fromLocalFile(dir.filePath(name))}; };
        FileOps ops;

        // A drop has to take something somewhere new: not where it already
        // is, not onto or into itself, and not onto a file.
        QVERIFY(ops.canDrop(urls("a.txt"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("a.txt"), dir.path()));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album/inner")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("a.txt")));
        QVERIFY(QFile::link(dir.filePath("album/inner"), dir.filePath("shortcut")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("shortcut")));
        // A link is moved as a link, so it can go into the folder it leads to.
        QVERIFY(ops.canDrop(urls("shortcut"), dir.filePath("album/inner")));
        // Moving something into the folder it's in leaves it alone rather
        // than making a "(copy)" of it.
        QString error;
        QVERIFY(FileOps::transfer({dir.filePath("a.txt")}, dir.path(), FileOps::Move, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));

        // A slash would move the file elsewhere, so it's refused; the same
        // name changes nothing, and spaces around a name are dropped.
        QVERIFY(!ops.rename(dir.filePath("a.txt"), "x/y").isEmpty());
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "a.txt"), QString());
        QCOMPARE(ops.rename(dir.filePath("c.txt"), " d.txt "), QString());
        QVERIFY(QFile::exists(dir.filePath("d.txt")));

        QCOMPARE(FileOps::archiveName({dir.filePath("d.txt")}, dir.path()), QString("d.txt.zip"));
        QCOMPARE(FileOps::archiveName({dir.filePath("a.txt"), dir.filePath("b.txt")}, dir.path()), QString("Archive.zip"));

        QSignalSpy done(&ops, &FileOps::done);
        ops.newFolder(dir.path());
        ops.newFolder(dir.path());
        QCOMPARE(done.count(), 2);
        QCOMPARE(done.takeFirst().at(1).toString(), QString("New Folder"));
        QCOMPARE(done.takeFirst().at(1).toString(), QString("New Folder 2"));
    }

    void recentsReadGtkRecentFiles()
    {
        QTemporaryDir dir;
        write(dir.filePath("old.txt"), "x");
        write(dir.filePath("new.pdf"), "x");
        write(dir.filePath(".secret"), "x");
        const QByteArray url = QUrl::fromLocalFile(dir.path()).toEncoded();
        write(dir.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/old.txt\" modified=\"2026-01-01T10:00:00Z\" visited=\"2026-01-02T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/gone.txt\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/.secret\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"https://example.com/\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/new.pdf\" modified=\"2026-03-01T10:00:00Z\" visited=\"2026-03-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto entries = FolderModel::listRecent(dir.filePath("recently-used.xbel"), false);
        // Missing, hidden and web entries are left out; newest use first.
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).name, QString("new.pdf"));
        QCOMPARE(entries.at(1).name, QString("old.txt"));
        QCOMPARE(entries.at(0).kind, QString("pdf"));
        // Left before it's done, it stops checking what's on the list.
        int checks = 0;
        QVERIFY(FolderModel::listRecent(dir.filePath("recently-used.xbel"), false, 200, [&] { return ++checks > 3; }).size() == 1);
    }

    void folderSortsFiveWays()
    {
        QVector<FolderModel::Entry> e(4);
        e[0].name = "b.txt"; e[0].kind = "text"; e[0].extension = "TXT"; e[0].size = 10; e[0].modified = QDateTime::fromSecsSinceEpoch(300);
        e[1].name = "a.png"; e[1].kind = "image"; e[1].extension = "PNG"; e[1].size = 500; e[1].modified = QDateTime::fromSecsSinceEpoch(100);
        e[2].name = "c.txt"; e[2].kind = "text"; e[2].extension = "TXT"; e[2].size = 99; e[2].modified = QDateTime::fromSecsSinceEpoch(200);
        e[3].name = "Zeta"; e[3].isDir = true; e[3].modified = QDateTime::fromSecsSinceEpoch(50);
        auto order = [&](const QString &sort) {
            auto copy = e;
            FolderModel::sortEntries(copy, sort);
            QStringList names;
            for (const auto &x : copy) names << x.name;
            return names.join(' ');
        };
        QCOMPARE(order("name"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("kind"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("modified"), QString("Zeta b.txt c.txt a.png"));
        QCOMPARE(order("oldest"), QString("Zeta a.png c.txt b.txt"));
        QCOMPARE(order("size"), QString("Zeta a.png c.txt b.txt"));
    }

    void folderKeepsASelection()
    {
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt", "d.txt"})
            write(dir.filePath(name), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));

        model.selectOnly(1);
        QCOMPARE(model.selectedCount(), 1);
        model.selectRange(1, 3, false);
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("b.txt"), dir.filePath("c.txt"), dir.filePath("d.txt")}));
        model.toggleSelected(2);
        QCOMPARE(model.selectedCount(), 2);
        QVERIFY(!model.isSelected(2));
        model.selectRange(0, 0, true);
        QCOMPARE(model.selectedCount(), 3);

        // A file that goes away leaves the selection; the rest stay.
        QFile::remove(dir.filePath("b.txt"));
        model.reload();
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("a.txt"), dir.filePath("d.txt")}));

        model.selectAll();
        QCOMPARE(model.selectedCount(), 3);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
    }

    void recentsSelectOneOfTwoSameNamedFiles()
    {
        // Recents can hold two files of the same name from different
        // folders; selecting one leaves the other alone.
        QTemporaryDir data;
        QDir(data.path()).mkpath("a");
        QDir(data.path()).mkpath("b");
        write(data.filePath("a/README.md"), "x");
        write(data.filePath("b/README.md"), "x");
        const QByteArray url = QUrl::fromLocalFile(data.path()).toEncoded();
        write(data.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/a/README.md\" modified=\"2026-01-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/b/README.md\" modified=\"2026-02-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto restore = dataHomeAt(data.path());

        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(FolderModel::recentPath());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.selectOnly(0);
        QCOMPARE(model.selectedCount(), 1);
        QVERIFY(!model.isSelected(1));
        QCOMPARE(model.selectedPaths(), QStringList({data.filePath("b/README.md")}));
        // The cursor comes back by path, which tells them apart too.
        QCOMPARE(model.indexOfPath(data.filePath("a/README.md")), 1);
        QCOMPARE(model.indexOfPath(data.filePath("b/README.md")), 0);
    }

    void trashRestoresAndEmpties()
    {
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        QDir().mkpath(home.filePath("Docs"));
        auto trashed = [&](const QString &name, const QString &from) {
            write(trash + "/files/" + name, "x");
            write(trash + "/info/" + name + ".trashinfo",
                  "[Trash Info]\nPath=" + QUrl::toPercentEncoding(from, "/") + "\nDeletionDate=2026-11-10T12:00:00\n");
        };
        trashed("notes 1.txt", home.filePath("Docs/notes 1.txt"));
        trashed("gone.txt", home.filePath("Gone/Deep/gone.txt"));
        trashed("clash.txt", home.filePath("Docs/clash.txt"));
        write(home.filePath("Docs/clash.txt"), "newer");

        QCOMPARE(FileOps::originalPath(trash + "/files/notes 1.txt"), home.filePath("Docs/notes 1.txt"));
        QCOMPARE(FileOps::deletedAt(trash + "/files/notes 1.txt"), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));

        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/notes 1.txt", &error), home.filePath("Docs/notes 1.txt"));
        QVERIFY(!QFile::exists(trash + "/info/notes 1.txt.trashinfo"));
        // A folder that's gone is made again; a name that's taken isn't overwritten.
        QCOMPARE(FileOps::restoreOne(trash + "/files/gone.txt", &error), home.filePath("Gone/Deep/gone.txt"));
        QCOMPARE(FileOps::restoreOne(trash + "/files/clash.txt", &error), home.filePath("Docs/clash (copy).txt"));
        QFile kept(home.filePath("Docs/clash.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("newer"));

        trashed("a.txt", home.filePath("a.txt"));
        QDir().mkpath(trash + "/files/folder/inner");
        write(trash + "/info/folder.trashinfo", "[Trash Info]\nPath=" + home.filePath("folder").toUtf8() + "\n");
        // Something being trashed gets its note first, then its file, so a
        // note without a file may be for something arriving.
        write(trash + "/info/arriving.txt.trashinfo", "[Trash Info]\nPath=" + home.filePath("arriving.txt").toUtf8() + "\n");
        QCOMPARE(FileOps::emptyTrashDir(trash), 2);
        QVERIFY(QDir(trash + "/files").isEmpty());
        QCOMPARE(QDir(trash + "/info").entryList(QDir::Files), QStringList({"arriving.txt.trashinfo"}));
    }

    void trashKeepsTheNoteOfWhatStays()
    {
        // A note goes only once its item is gone, so what couldn't be
        // deleted, or removed from the trash once restored to another drive,
        // can still be restored.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/box/x.txt", "x");
        const auto other = elsewhere();
        const QString original = other ? other->filePath("box") : data.filePath("box");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + QUrl::toPercentEncoding(original, "/") + "\n");
        // Nothing can be taken out of a folder that can't be written to.
        const auto locked = QFile::ReadOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(trash + "/files/box", locked));
        const auto unlock = qScopeGuard([&] {
            for (const QString &box : {trash + "/files/box", original})
                QFile::setPermissions(box, locked | QFile::WriteOwner);
        });
        QVERIFY(!FileOps::deleteTrashed(trash + "/files/box"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));

        if (!other)
            QSKIP("Needs a second drive.");
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/box", &error), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(original + "/x.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
    }

    void trashFindsNotesOnlyForWhatWasTrashed()
    {
        // Only what's directly in Trash/files has a note in Trash/info.
        // Something inside a trashed folder came along with it and has none;
        // the same name in Trash/files/info is a file in a trashed folder
        // called "info".
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/photos");
        QDir().mkpath(trash + "/files/info");
        write(trash + "/files/photos/a", "a");
        write(trash + "/files/info/a.trashinfo", "[Trash Info]\nPath=" + data.filePath("elsewhere/a").toUtf8() + "\n");

        QCOMPARE(FileOps::originalPath(trash + "/files/photos/a"), QString());
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/photos/a", &error), QString());
        QCOMPARE(error, QString("a has no record of where it came from."));
        QVERIFY(QFile::exists(trash + "/files/photos/a"));
        QVERIFY(FileOps::deleteTrashed(trash + "/files/photos/a"));
        QVERIFY(QFile::exists(trash + "/files/info/a.trashinfo"));
    }

    void trashAndRestoreRunInTheBackground()
    {
        // Trashing asks GIO about each item, and restoring can copy a whole
        // folder from another drive, so neither holds up the window: each
        // shows that it's busy, and says how it went once it's done.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/a.txt", "a");
        write(trash + "/info/a.txt.trashinfo", "[Trash Info]\nPath=" + data.filePath("a.txt").toUtf8() + "\n");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.restore({trash + "/files/a.txt"});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(notice.count(), 0);
        QTRY_COMPARE(notice.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.first().first().toString(), QString("Restored a.txt to %1.").arg(data.path()));
        QVERIFY(QFile::exists(data.filePath("a.txt")));

        // What's no longer there fails before GIO goes near a trash.
        ops.trash({data.filePath("gone.txt")});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(failed.first().first().toString().startsWith("Couldn't move to the trash: gone.txt ("));

        // One piece of work runs at a time. What's asked for meanwhile
        // doesn't start, and says so rather than doing nothing.
        ops.trash({data.filePath("gone.txt")});
        ops.restore({trash + "/files/a.txt"});
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_COMPARE(failed.count(), 3);
    }

    void trashTakesNothingButDeletes()
    {
        // A paste or a drop into the trash would leave items with no note
        // saying where they came from.
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        const QString files = data.filePath("Trash/files");
        QDir().mkpath(files);
        write(from.filePath("a.txt"), "a");
        FileOps ops;
        QVERIFY(ops.inTrash(files));
        QVERIFY(!ops.canDrop({QUrl::fromLocalFile(from.filePath("a.txt"))}, files));
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("a.txt")});
        ops.paste(files);
        QCOMPARE(failed.count(), 1);
        QVERIFY(QDir(files).isEmpty());
    }

    void kindsFromMimeTypes()
    {
        QCOMPARE(FolderModel::kindForMime("application/pdf"), QString("pdf"));
        QCOMPARE(FolderModel::kindForMime("video/mp4"), QString("video"));
        QCOMPARE(FolderModel::kindForMime("audio/mp4"), QString("audio"));
        QCOMPARE(FolderModel::kindForMime("application/zip"), QString("archive"));
        QCOMPARE(FolderModel::kindForMime("application/octet-stream"), QString("other"));
    }

    void backendKeepsHistoryAndCrumbs()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Pictures/Trip");
        Backend backend(home.path());
        backend.navigate(home.path());
        backend.navigate(home.filePath("Pictures/Trip"));
        QCOMPARE(backend.title(), QString("Trip"));
        const auto crumbs = backend.crumbs();
        QCOMPARE(crumbs.size(), 3);
        QCOMPARE(crumbs.first().toMap().value("name").toString(), QString("~"));

        backend.goUp();
        QCOMPARE(backend.path(), home.filePath("Pictures"));
        // Coming back up selects the folder we left.
        QCOMPARE(backend.takeSelection(), QString("Trip"));
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Pictures/Trip"));
        QVERIFY(backend.canForward());
    }

    void backendStaysPutWhenTheHistoryIsGone()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Old");
        QDir(home.path()).mkpath("New");
        Backend backend(home.path());
        backend.navigate(home.filePath("Old"));
        backend.navigate(home.filePath("New"));
        QDir(home.filePath("Old")).removeRecursively();

        // Going back to a folder that's gone says so and leaves the history
        // on the folder shown, with what it was to select next untouched.
        backend.expectSelection("pasted.txt");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("New"));
        QVERIFY(!backend.message().isEmpty());
        QVERIFY(backend.canBack());
        QVERIFY(!backend.canForward());
        QCOMPARE(backend.takeSelection(), QString("pasted.txt"));

        QDir(home.path()).mkpath("Old");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QDir(home.filePath("New")).removeRecursively();
        backend.forward();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QVERIFY(!backend.canBack());
        QVERIFY(backend.canForward());
    }

    void backendCompletesTypedPaths()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Work/forge", "Documents", ".config"})
            QDir(home.path()).mkpath(dir);
        const QString h = home.path();
        QCOMPARE(Backend::expandTyped("~/Work", h, h), h + "/Work");
        QCOMPARE(Backend::expandTyped("Work/folio", h, h), h + "/Work/folio");
        QCOMPARE(Backend::expandTyped("/etc", h, h), QString("/etc"));
        QCOMPARE(Backend::expandTyped("Work", h, "recent:"), h + "/Work");
        QCOMPARE(Backend::complete("~/Doc", h, h), QString("~/Documents/"));
        QCOMPARE(Backend::complete("~/Work/fo", h, h), QString("~/Work/fo"));   // folio or forge
        QCOMPARE(Backend::complete("~/Work/fol", h, h), QString("~/Work/folio/"));
        QCOMPARE(Backend::complete("~/.co", h, h), QString("~/.config/"));
        QCOMPARE(Backend::complete("~/c", h, h), QString("~/c"));               // hidden stays hidden
        QCOMPARE(Backend::complete("Wo", h, h), QString("Work/"));
        QCOMPARE(Backend::complete("Wo", h, "recent:"), QString("Work/"));
    }

    void backendShowItemSelectsTheFile()
    {
        QTemporaryDir home;
        write(home.filePath("report.pdf"), "x");
        Backend backend(home.path());
        backend.showItem(home.filePath("report.pdf"));
        QCOMPARE(backend.path(), home.path());
        QCOMPARE(backend.takeSelection(), QString("report.pdf"));
    }

    void backendReadsDetailsOffTheUiThread()
    {
        // A picture's size and a text's opening lines are read in the
        // background: on a slow mount, reading them as the selection moves
        // would hold up the window.
        QTemporaryDir dir;
        const QString photo = dir.filePath("photo.png"), notes = dir.filePath("notes.txt");
        QVERIFY(QImage(40, 30, QImage::Format_RGB32).save(photo));
        write(notes, "hello");
        Backend backend(dir.path());
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QCOMPARE(backend.details().value("type").toString(), QMimeDatabase().mimeTypeForName("image/png").comment());
        QVERIFY(!backend.details().contains("dimensions"));
        QTRY_COMPARE(backend.details().value("dimensions").toString(), QString("40 × 30"));

        // The next item starts from its type alone, and what was read for
        // one left behind is dropped.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        backend.describe({{"path", notes}, {"kind", "text"}, {"mime", "text/plain"}});
        QVERIFY(!backend.details().contains("dimensions"));
        QTRY_COMPARE(backend.details().value("text").toString(), QString("hello"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!backend.details().contains("dimensions"));

        // A picture replaced by a folder of the same name doesn't keep the
        // picture's size.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QTRY_VERIFY(backend.details().contains("dimensions"));
        backend.describe({{"path", photo}, {"kind", "folder"}, {"mime", "inode/directory"}});
        QVERIFY(!backend.details().contains("dimensions"));
    }

    void backendReadsTrashNotesOffTheUiThread()
    {
        // Finding a trashed item's note looks over every mounted drive, so
        // it's read with the rest of the details.
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        QDir().mkpath(home.filePath("Trash/files"));
        QDir().mkpath(home.filePath("Trash/info"));
        write(home.filePath("Trash/files/old.pdf"), "x");
        write(home.filePath("Trash/info/old.pdf.trashinfo"),
              "[Trash Info]\nPath=" + home.filePath("Docs/old.pdf").toUtf8() + "\nDeletionDate=2026-11-10T12:00:00\n");
        Backend backend(home.path());
        backend.describe({{"path", home.filePath("Trash/files/old.pdf")}, {"kind", "pdf"}, {"mime", "application/pdf"}}, true);
        QVERIFY(!backend.details().contains("origin"));
        QTRY_COMPARE(backend.details().value("origin").toString(), home.filePath("Docs/old.pdf"));
        QCOMPARE(backend.details().value("deleted").toDateTime(), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));
    }

    void mounterOutlivedByGiosReply()
    {
        // A local folder can't be mounted, so GIO answers with an error on
        // a later turn of the main loop, as it does for a slow tailnet
        // machine. By then the first Mounter's window has closed.
        QTemporaryDir dir;
        const QString uri = QUrl::fromLocalFile(dir.path()).toString();
        auto *closed = new Mounter;
        closed->mount(uri);
        delete closed;
        Mounter open;
        QSignalSpy failed(&open, &Mounter::failed);
        open.mount(uri);
        QVERIFY(failed.wait(2000));
        QVERIFY(!open.busy());
    }

    void trashFollowsXdgDataHome()
    {
        // g_file_trash puts things in $XDG_DATA_HOME/Trash, wherever that
        // is, so the sidebar's Trash opens there and is called "Trash".
        QTemporaryDir home, data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash/files");
        Places places(home.path());
        QCOMPARE(places.views().last().toMap().value("path").toString(), trash);
        Backend backend(home.path());
        backend.navigate(trash);
        QCOMPARE(backend.path(), trash);
        QCOMPARE(backend.title(), QString("Trash"));
    }

    void placesListEveryFolderInHome()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work", "Documents", "omarchy", ".config", "project10", "project9"})
            QDir(home.path()).mkpath(dir);
        write(home.filePath("notes.txt"), "not a folder");
        auto names = [](const Places &p) {
            QStringList list;
            for (const auto &v : p.places()) list << v.toMap().value("name").toString();
            return list;
        };
        QCOMPARE(names(Places(home.path())),
                 QStringList({"Documents", "Omarchy", "project9", "project10", "Work"}));

        // Dropbox shows once its folder exists, with its own mark.
        QDir(home.path()).mkpath("Dropbox");
        const Places withDropbox(home.path());
        QCOMPARE(names(withDropbox).at(1), QString("Dropbox"));
        QCOMPARE(withDropbox.places().at(1).toMap().value("icon").toString(), QString("dropbox"));
        QCOMPARE(withDropbox.places().at(0).toMap().value("icon").toString(), QString("documents"));
        QCOMPARE(withDropbox.places().at(5).toMap().value("icon").toString(), QString("folder"));
    }

    void placesPinFoldersAsGtkBookmarks()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Downloads", "Work/hype"})
            QDir(home.path()).mkpath(dir);
        const QString bookmarks = home.filePath(".config/gtk-3.0/bookmarks");
        QDir().mkpath(QFileInfo(bookmarks).absolutePath());
        // Omarchy's seeded home folders and a remote bookmark are kept in
        // the file but not shown as pins.
        write(bookmarks, QUrl::fromLocalFile(home.filePath("Downloads")).toEncoded() + " Downloads\n"
                         "sftp://studio/srv Studio\n"
                         + QUrl::fromLocalFile(home.filePath("Work/hype")).toEncoded() + " Slides\n");

        Places places(home.path());
        QCOMPARE(places.pinned().size(), 1);
        QCOMPARE(places.pinned().at(0).toMap().value("name").toString(), QString("Slides"));

        QCOMPARE(places.pin(home.filePath("Work/folio")), QString());
        QVERIFY(places.isPinned(home.filePath("Work/folio")));
        QCOMPARE(places.pinned().size(), 2);
        QVERIFY(!places.pin(home.filePath("Downloads")).isEmpty()); // already under Home
        // Hidden folders in home aren't listed under Home, so they can be
        // pinned.
        QCOMPARE(places.pin(home.filePath(".config")), QString());
        QVERIFY(places.isPinned(home.filePath(".config")));
        places.unpin(home.filePath(".config"));
        QVERIFY(!places.isPinned(home.filePath(".config")));

        places.unpin(home.filePath("Work/hype"));
        QFile file(bookmarks);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("sftp://studio/srv Studio"));
        QVERIFY(saved.contains("Downloads"));
        QVERIFY(saved.contains("Work/folio"));
        QVERIFY(!saved.contains("Work/hype"));
        QCOMPARE(places.pinned().size(), 1);
    }

    void placesParseTailscalePeers()
    {
        const QByteArray json = R"({"Peer":{
            "a":{"HostName":"nas","DNSName":"nas.example.ts.net.","OS":"linux","Online":false},
            "b":{"HostName":"studio","DNSName":"studio.example.ts.net.","OS":"linux","Online":true},
            "c":{"HostName":"phone","DNSName":"phone.example.ts.net.","OS":"iOS","Online":true}}})";
        const auto peers = Places::parseTailscale(json);
        // Offline machines and phones are left out.
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers.at(0).toMap().value("name").toString(), QString("studio"));
        QCOMPARE(peers.at(0).toMap().value("host").toString(), QString("studio.example.ts.net"));
    }

    void placesListOnlyPhysicalDrives()
    {
        const QList<Places::Mount> mounts{
            {"/", "/dev/mapper/root", "btrfs", ""},
            {"/home", "/dev/mapper/root", "btrfs", ""},
            {"/.snapshots", "/dev/mapper/root", "btrfs", ""},
            {"/boot", "/dev/nvme0n1p1", "vfat", "EFI"},
            {"/tmp", "tmpfs", "tmpfs", ""},
            {"/var/lib/snapd/snap/core", "/dev/loop3", "squashfs", ""},
            {"/mnt/archive", "/dev/sda1", "ext4", ""},
            {"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"},
            {"/run/media/me/Photos/again", "/dev/sdb1", "exfat", "Photos"},
            {"/run/user/1000/gvfs", "gvfsd-fuse", "fuse.gvfsd-fuse", ""},
        };
        const auto drives = Places::physicalDrives(mounts, "/dev/mapper/root");
        QCOMPARE(drives.size(), 2);
        QCOMPARE(drives.at(0).toMap().value("name").toString(), QString("archive"));
        QCOMPARE(drives.at(1).toMap().value("name").toString(), QString("Photos"));
        QCOMPARE(drives.at(1).toMap().value("path").toString(), QString("/run/media/me/Photos"));
    }

    void placesParseDropboxStatus()
    {
        QCOMPARE(Places::parseDropboxStatus("Up to date\n"), QString("synced"));
        QCOMPARE(Places::parseDropboxStatus("Syncing 3 files\n"), QString("syncing"));
        QCOMPARE(Places::parseDropboxStatus("Dropbox isn't running!\n"), QString("stopped"));
    }

    void thumbnailsFollowTheFreedesktopCache()
    {
        QTemporaryDir dir;
        QImage picture(800, 400, QImage::Format_RGB32);
        picture.fill(Qt::red);
        const QString file = dir.filePath("wide.png");
        QVERIFY(picture.save(file));

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        const QImage thumb = thumbnails.load(file, "image/png", 256);
        QCOMPARE(thumb.size(), QSize(256, 128));

        // Cached under the spec's name, tagged so other apps trust it.
        const QString cached = dir.filePath("cache/large/" + Thumbnails::cacheKey(file));
        QVERIFY(QFile::exists(cached));
        const QImage stored(cached);
        QCOMPARE(stored.text("Thumb::URI"), QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
        QVERIFY(!stored.text("Thumb::MTime").isEmpty());

        // A fresh cache entry is served as is: overwrite it with a marker
        // color and the next load must return the cached pixels.
        QImage marked = stored;
        marked.fill(Qt::blue);
        QVERIFY(marked.save(cached));
        QCOMPARE(thumbnails.load(file, "image/png", 256).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(Thumbnails::bucketFor(128), QString("normal"));
        QCOMPARE(Thumbnails::bucketFor(512), QString("x-large"));
    }

    void thumbnailsFollowAPictureSavedOver()
    {
        // Qt keeps the images it has shown by their URL, and a re-read makes
        // new tiles that ask again, so a picture saved over must ask with a
        // URL of its own, and the cache must make it anew.
        QTemporaryDir dir;
        const QString file = dir.filePath("photo.png");
        QVERIFY(QImage(64, 32, QImage::Format_RGB32).save(file));
        const QDateTime then = QFileInfo(file).lastModified();

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
        QQmlComponent tile(&engine);
        tile.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                     "Image { required property string path; required property var modified\n"
                     "        asynchronous: true; source: Format.thumb(path, \"image/png\", 128, modified) }",
                     QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        auto shownWidth = [&](const QDateTime &modified) {
            std::unique_ptr<QObject> image(tile.createWithInitialProperties({{"path", file}, {"modified", modified}}));
            const bool shown = image && QTest::qWaitFor([&] { return image->property("implicitWidth").toInt() > 0; }, 5000);
            return shown ? image->property("implicitWidth").toInt() : -1;
        };
        QCOMPARE(shownWidth(then), 128);

        // Saved over twice within one second, as an editor's quick saves
        // can be: the spec's whole seconds can't tell them apart.
        const QDateTime second = QDateTime::fromSecsSinceEpoch(then.toSecsSinceEpoch() + 60);
        auto saveAt = [&](const QSize &size, const QDateTime &when) {
            QVERIFY(QImage(size, QImage::Format_RGB32).save(file));
            QFile saved(file);
            QVERIFY(saved.open(QIODevice::ReadWrite));
            QVERIFY(saved.setFileTime(when, QFileDevice::FileModificationTime));
        };
        saveAt({32, 64}, second.addMSecs(100));
        QCOMPARE(shownWidth(second.addMSecs(100)), 64);
        saveAt({64, 32}, second.addMSecs(600));
        QCOMPARE(shownWidth(second.addMSecs(600)), 128);
    }

    void thumbnailsReadInstalledThumbnailers()
    {
        QTemporaryDir dir;
        write(dir.filePath("demo.thumbnailer"),
              "[Thumbnailer Entry]\nTryExec=/bin/sh\nExec=/bin/sh -c \"exit 1\" %i %o %s\nMimeType=video/mp4;video/webm;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        QVERIFY(thumbnails.thumbnailerFor("video/webm").startsWith("/bin/sh"));
        QVERIFY(thumbnails.thumbnailerFor("video/ogg").isEmpty());

        // A thumbnailer that fails leaves a failure marker, so it isn't retried.
        write(dir.filePath("clip.mp4"), "not a video");
        QVERIFY(thumbnails.load(dir.filePath("clip.mp4"), "video/mp4", 128).isNull());
        QVERIFY(QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("clip.mp4")))));
    }

    void thumbnailsRememberOnlyWhatFailed()
    {
        QTemporaryDir dir;
        QImage frame(64, 32, QImage::Format_RGB32);
        frame.fill(Qt::green);
        QVERIFY(frame.save(dir.filePath("frame.png")));
        write(dir.filePath("heif.thumbnailer"),
              "[Thumbnailer Entry]\nExec=/bin/cp \"" + dir.filePath("frame.png").toUtf8() + "\" %o\nMimeType=image/heif;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});

        // Nothing could make one, so nothing failed: a thumbnailer installed
        // later still gets its turn.
        write(dir.filePath("bundle.zip"), "not a zip");
        QVERIFY(thumbnails.load(dir.filePath("bundle.zip"), "application/zip", 128).isNull());
        QVERIFY(!QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("bundle.zip")))));

        // A folder's peek asks for any picture, and gets the thumbnailer for
        // the one it names.
        write(dir.filePath("photo.heic"), "not a real heic");
        QCOMPARE(thumbnails.load(dir.filePath("photo.heic"), "image/*", 128).size(), QSize(64, 32));
    }

    void previewerPicksAViewerForEachType()
    {
        QCOMPARE(Previewer::viewerFor("image/png", false), QString("image"));
        QCOMPARE(Previewer::viewerFor("image/gif", false), QString("animated"));
        QCOMPARE(Previewer::viewerFor("image/x-canon-cr2", false), QString("thumbnail"));
        QCOMPARE(Previewer::viewerFor("video/mp4", false), QString("video"));
        QCOMPARE(Previewer::viewerFor("audio/mpeg", false), QString("audio"));
        QCOMPARE(Previewer::viewerFor("application/pdf", false), QString("pdf"));
        QCOMPARE(Previewer::viewerFor("text/markdown", false), QString("markdown"));
        QCOMPARE(Previewer::viewerFor("text/x-c++src", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/x-shellscript", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/zip", false), QString("other"));
        QCOMPARE(Previewer::viewerFor("inode/directory", true), QString("folder"));
    }

    void previewerReadsTextButNotBinaries()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello\nworld\n");
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt")), QString("hello\nworld\n"));
        QVERIFY(Previewer::readText(dir.filePath("blob.txt")).isEmpty());
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt"), 5), QString("hello"));

        // A binary with a text name falls back to the plain file view.
        Previewer previewer;
        previewer.start(dir.filePath("blob.txt"), false, "name");
        QTRY_COMPARE(previewer.viewer(), QString("other"));
    }

    void previewerStepsThroughTheFolder()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("a.md"), "# A");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.pdf"), "%PDF");
        write(dir.filePath(".hidden"), "h");

        Previewer previewer;
        QSignalSpy changed(&previewer, &Previewer::changed);
        previewer.start(dir.filePath("b.txt"), false, "name");
        // Shown at once, before the folder is listed.
        QCOMPARE(previewer.item().value("name").toString(), QString("b.txt"));
        QTRY_COMPARE(previewer.text(), QString("b"));

        // The listing moves the index without touching what's shown, which
        // would restart a playing video.
        const int shown = changed.count();
        QTRY_COMPARE(previewer.count(), 4);
        QCOMPARE(previewer.index(), 2);
        QCOMPARE(changed.count(), shown);
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("markdown"));
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("folder"));
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 0);
        previewer.previous();
        QCOMPARE(previewer.index(), 0);
        previewer.next();
        previewer.next();
        previewer.next();
        QCOMPARE(previewer.viewer(), QString("pdf"));
        previewer.next();
        QCOMPARE(previewer.index(), 3);

        // Folio pointing at a sibling keeps the list; another folder starts over.
        previewer.show(dir.filePath("a.md"));
        QCOMPARE(previewer.index(), 1);
        QCOMPARE(previewer.count(), 4);
        QTemporaryDir other;
        write(other.filePath("x.txt"), "x");
        write(other.filePath("y.txt"), "y");
        previewer.show(other.filePath("y.txt"));
        QCOMPARE(previewer.count(), 1);
        QCOMPARE(previewer.index(), 0);
        QTRY_COMPARE(previewer.count(), 2);
        QCOMPARE(previewer.index(), 1);
        QVERIFY(changed.count() > 0);
    }

    void previewerReadsOffTheUiThread()
    {
        // A big folder, or any file on a slow mount, would hold up the
        // window for as long as reading it takes, so an item shows at once
        // and its text, or a folder's count and first pictures, follow.
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello");
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("notes.txt"), false, "name");
        QCOMPARE(previewer.text(), QString());
        QTRY_COMPARE(previewer.text(), QString("hello"));
        previewer.show(dir.filePath("album"));
        QCOMPARE(previewer.item().value("itemCount").toInt(), -1);
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);

        // What's read for an item left behind is dropped.
        previewer.show(dir.filePath("notes.txt"));
        previewer.show(dir.filePath("album"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(previewer.text(), QString());
    }

    void previewerCountsHiddenFilesWhenTheyShow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("dots");
        write(dir.filePath("dots/.config"), "c");

        // The first folder shown counts what Folio's tile counts.
        Previewer previewer;
        previewer.start(dir.filePath("dots"), true, "name");
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);
    }

    void previewerPeeksIntoFolders()
    {
        // A folder's first pictures are for the preview window, so its item
        // has them and the folder model's (which never fills them) doesn't.
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("album"), false, "name");
        QTRY_COMPARE(previewer.item().value("peek").toStringList(), QStringList({dir.filePath("album/a.jpg")}));
        QVERIFY(!FolderModel::toMap(FolderModel::Entry()).contains("peek"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#FFFCF0\"\nforeground = \"#100F0F\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#FFFCF0"));
        // Missing derived backgrounds fall back to the base.
        QCOMPARE(theme.darkBackground(), QColor("#FFFCF0"));
        QVERIFY(theme.light());
    }

    void themeDerivesALighterBackgroundWhenItMatches()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#101315\"\n");
        Theme same(dir.path());
        QCOMPARE(same.background(), QColor("#101315"));
        QVERIFY(same.lighterBackground() != same.background());
        QVERIFY(same.lighterBackground().lightnessF() > same.background().lightnessF());

        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#2a2e36\"\n");
        Theme distinct(dir.path());
        QCOMPARE(distinct.lighterBackground(), QColor("#2a2e36"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":4}");

        Theme theme(dir.path(), nullptr, command);
        QTRY_COMPARE(theme.radius(), 4);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void themeAsksForRoundingOnlyWhileActive()
    {
        // The fake hyprctl lives outside the theme folder: Theme watches that
        // folder, and a change there re-reads the rounding whatever the state.
        QTemporaryDir themeDir, dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":3}");

        Theme theme(themeDir.path(), nullptr, command);
        QTRY_COMPARE(theme.radius(), 3);
        // Away from Folio, a change goes unnoticed for longer than the poll,
        // and it's picked up once Folio is back.
        emit qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
        write(response, "{\"int\":6}");
        QTest::qWait(1500);
        QCOMPARE(theme.radius(), 3);
        emit qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Open").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n items": {"one": "%n item", "other": "%n items"}})");
        write(dir.path() + "/ru.json", R"({"%n items": {"one": "%n элемент", "few": "%n элемента", "many": "%n элементов", "other": "%n элемента"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 21), QString("%n элемент"));
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 25), QString("%n элементов"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
