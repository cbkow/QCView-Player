#include "annotation_io.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QtLogging>

#include <algorithm>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace qcv::annotation_io {

namespace {

// Map a media-file extension to the canonical media_type label
// embedded in notes.json. The field is metadata only — currently
// no consumer reads it — but accurate values let external tools
// (Frame.io export, future migration scripts) trust the kind.
//
// Extensions come from the same lists used in the LeftRail's
// Add Media file dialog. Default falls back to "video" so we
// match the old app's hardcoded behavior on anything we don't
// recognize.
QString kindForExtension(const QString &mediaPath)
{
    const QString ext = QFileInfo(mediaPath).suffix().toLower();
    if (ext.isEmpty()) return QStringLiteral("video");

    static const QSet<QString> videoExts = {
        QStringLiteral("mov"), QStringLiteral("mp4"),
        QStringLiteral("m4v"), QStringLiteral("mxf"),
        QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("webm")
    };
    static const QSet<QString> audioExts = {
        QStringLiteral("wav"), QStringLiteral("aif"),
        QStringLiteral("aiff"), QStringLiteral("mp3"),
        QStringLiteral("flac"), QStringLiteral("m4a")
    };
    static const QSet<QString> imageExts = {
        QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("exr"),
        QStringLiteral("dpx"), QStringLiteral("bmp"),
        QStringLiteral("gif")
    };

    if (videoExts.contains(ext)) return QStringLiteral("video");
    if (audioExts.contains(ext)) return QStringLiteral("audio");
    if (imageExts.contains(ext)) return QStringLiteral("image");
    return QStringLiteral("video");
}

// Read-side: prefer .qcview, fall back to legacy .ump for back-compat.
QString sidecarPathWithFallback(const QString &mediaPath)
{
    const QFileInfo info(mediaPath);
    const QDir parent = info.absoluteDir();

    const QString qcview = parent.filePath(QStringLiteral(".qcview"));
    if (QFileInfo::exists(qcview)) return qcview;

    const QString ump = parent.filePath(QStringLiteral(".ump"));
    if (QFileInfo::exists(ump)) return ump;

    // Neither exists yet — return .qcview (will be created on save).
    return qcview;
}

QDir projectSidecarWithFallback(const QDir &projectDir)
{
    const QString qcview = projectDir.filePath(QStringLiteral(".qcview"));
    if (QFileInfo::exists(qcview)) return QDir(qcview);

    const QString ump = projectDir.filePath(QStringLiteral(".ump"));
    if (QFileInfo::exists(ump)) return QDir(ump);

    return QDir(qcview);
}

void setHiddenAttrIfWindows(const QString &path)
{
#ifdef Q_OS_WIN
    SetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()),
                       FILE_ATTRIBUTE_HIDDEN);
#else
    Q_UNUSED(path);
#endif
}

} // namespace

QString sanitizeMediaName(const QString &filename)
{
    QString out = filename;
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar &c : out) {
        if (invalid.contains(c)) c = QLatin1Char('_');
    }
    return out;
}

QString getQcViewPath(const QString &mediaPath)
{
    const QFileInfo info(mediaPath);
    return info.absoluteDir().filePath(QStringLiteral(".qcview"));
}

QString getNotesJsonPath(const QString &mediaPath)
{
    const QFileInfo info(mediaPath);
    const QString mediaName = sanitizeMediaName(info.fileName());
    const QDir sidecar(sidecarPathWithFallback(mediaPath));
    return sidecar.filePath(mediaName + QStringLiteral("/notes.json"));
}

QString getImagesFolder(const QString &mediaPath)
{
    if (mediaPath.isEmpty()) return {};
    const QFileInfo info(mediaPath);
    const QString mediaName = sanitizeMediaName(info.fileName());
    const QDir sidecar(sidecarPathWithFallback(mediaPath));
    return sidecar.filePath(mediaName + QStringLiteral("/images"));
}

QString fileStemForKey(const QString &key)
{
    return QString(key)
        .replace(QLatin1Char(':'), QLatin1Char('_'))
        .replace(QLatin1Char(';'), QLatin1Char('_'));
}

QString generateImageFilename(const QString &timecode)
{
    return QStringLiteral("note_") + fileStemForKey(timecode) + QStringLiteral(".png");
}

// ---- streams ----

QString streamFolderName(const QString &itemName, const QString &itemId)
{
    QString name = sanitizeMediaName(itemName.trimmed());
    if (name.isEmpty()) name = QStringLiteral("stream");
    return name + QStringLiteral("__") + itemId.left(8);
}

QString findStreamFolder(const QString &projectPath, const QString &itemId)
{
    if (projectPath.isEmpty() || itemId.isEmpty()) return {};
    const QDir sidecar = projectSidecarWithFallback(QFileInfo(projectPath).absoluteDir());
    const QDir streams(sidecar.filePath(QStringLiteral("streams")));
    if (!streams.exists()) return {};
    const QString suffix = QStringLiteral("__") + itemId.left(8);
    for (const QString &d : streams.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.endsWith(suffix)) return d;
    }
    return {};
}

QString getStreamDir(const QString &projectPath, const QString &folderName)
{
    if (projectPath.isEmpty() || folderName.isEmpty()) return {};
    const QDir sidecar = projectSidecarWithFallback(QFileInfo(projectPath).absoluteDir());
    return sidecar.filePath(QStringLiteral("streams/") + folderName);
}

bool createStreamFolder(const QString &projectPath, const QString &folderName)
{
    if (projectPath.isEmpty() || folderName.isEmpty()) return false;
    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    const QString sidecarPath = projectDir.filePath(QStringLiteral(".qcview"));
    const bool sidecarExisted = QDir(sidecarPath).exists();
    const QString dir = QDir(sidecarPath).filePath(QStringLiteral("streams/") + folderName + QStringLiteral("/images"));
    if (!QDir().mkpath(dir)) {
        qWarning("annotation_io: failed to create %s", qPrintable(dir));
        return false;
    }
    if (!sidecarExisted) setHiddenAttrIfWindows(sidecarPath);
    return true;
}

// ---- folder-rooted I/O ----

QString notesJsonPathIn(const QString &dir)
{
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/notes.json");
}

QString imagesFolderIn(const QString &dir)
{
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/images");
}

bool saveNotesAt(const QString &dir, const QJsonObject &header,
                 const std::vector<AnnotationNote> &notes)
{
    if (dir.isEmpty()) return false;
    if (!QDir().mkpath(dir)) {
        qWarning("annotation_io: failed to create %s", qPrintable(dir));
        return false;
    }
    QJsonObject root = header;
    if (!root.contains(QStringLiteral("created"))) {
        root.insert(QStringLiteral("created"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    }
    QJsonArray notesArr;
    for (const AnnotationNote &n : notes) notesArr.append(noteToJson(n));
    root.insert(QStringLiteral("notes"), notesArr);

    const QString path = notesJsonPathIn(dir);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("annotation_io: cannot open %s for write: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        qWarning("annotation_io: commit failed for %s", qPrintable(path));
        return false;
    }
    return true;
}

bool loadNotesAt(const QString &dir, std::vector<AnnotationNote> &notes, QJsonObject *header)
{
    notes.clear();
    if (header) *header = QJsonObject();
    const QString path = notesJsonPathIn(dir);
    if (path.isEmpty()) return false;
    if (!QFileInfo::exists(path)) return true;   // no notes yet — not an error

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("annotation_io: cannot open %s: %s", qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("annotation_io: %s parse error: %s", qPrintable(path), qPrintable(err.errorString()));
        return false;
    }
    const QJsonObject root = doc.object();
    if (header) {
        QJsonObject h = root;
        h.remove(QStringLiteral("notes"));
        *header = h;
    }
    const QJsonArray arr = root.value(QStringLiteral("notes")).toArray();
    notes.reserve(static_cast<std::size_t>(arr.size()));
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) continue;
        AnnotationNote n;
        if (noteFromJson(v.toObject(), n)) notes.push_back(std::move(n));
    }
    std::sort(notes.begin(), notes.end());
    return true;
}

void saveNotesAtAsync(const QString &dir, const QJsonObject &header,
                      const std::vector<AnnotationNote> &notes)
{
    QThreadPool::globalInstance()->start([dir, header, notes]() {
        saveNotesAt(dir, header, notes);
    });
}

QString getProjectAnnotationPath(const QString &projectPath,
                                 const QString &timelineName)
{
    if (projectPath.isEmpty() || timelineName.isEmpty()) return {};
    const QFileInfo info(projectPath);
    const QDir projectDir = info.absoluteDir();
    const QString name = sanitizeMediaName(timelineName);
    const QDir sidecar = projectSidecarWithFallback(projectDir);
    return sidecar.filePath(name + QStringLiteral("/notes.json"));
}

QString getProjectImagesFolder(const QString &projectPath,
                               const QString &timelineName)
{
    if (projectPath.isEmpty() || timelineName.isEmpty()) return {};
    const QFileInfo info(projectPath);
    const QDir projectDir = info.absoluteDir();
    const QString name = sanitizeMediaName(timelineName);
    const QDir sidecar = projectSidecarWithFallback(projectDir);
    return sidecar.filePath(name + QStringLiteral("/images"));
}

bool createProjectQcViewFolder(const QString &projectPath,
                               const QString &timelineName)
{
    if (projectPath.isEmpty() || timelineName.isEmpty()) {
        qWarning("annotation_io: cannot create project sidecar — empty path or timeline");
        return false;
    }

    const QFileInfo info(projectPath);
    const QDir projectDir = info.absoluteDir();
    const QString name = sanitizeMediaName(timelineName);

    const QString sidecarPath = projectDir.filePath(QStringLiteral(".qcview"));
    const QDir sidecar(sidecarPath);
    const bool sidecarExisted = sidecar.exists();

    if (!QDir().mkpath(sidecar.filePath(name))) {
        qWarning("annotation_io: failed to create %s",
                 qPrintable(sidecar.filePath(name)));
        return false;
    }
    if (!sidecarExisted) setHiddenAttrIfWindows(sidecarPath);
    return true;
}

bool createQcViewFolder(const QString &mediaPath)
{
    const QFileInfo info(mediaPath);
    const QString mediaName = sanitizeMediaName(info.fileName());

    const QString sidecarPath = getQcViewPath(mediaPath);
    const QDir sidecar(sidecarPath);
    const bool sidecarExisted = sidecar.exists();

    if (!QDir().mkpath(sidecar.filePath(mediaName))) {
        qWarning("annotation_io: failed to create %s",
                 qPrintable(sidecar.filePath(mediaName)));
        return false;
    }
    if (!sidecarExisted) setHiddenAttrIfWindows(sidecarPath);
    return true;
}

bool ensureImagesFolderExists(const QString &mediaPath)
{
    const QString folder = getImagesFolder(mediaPath);
    if (folder.isEmpty()) return false;
    if (!QDir().mkpath(folder)) {
        qWarning("annotation_io: failed to create %s", qPrintable(folder));
        return false;
    }
    return true;
}

bool saveNotes(const std::vector<AnnotationNote> &notes,
               const QString &mediaPath)
{
    if (!createQcViewFolder(mediaPath)) return false;
    QJsonObject header;
    header.insert(QStringLiteral("media_file"), QFileInfo(mediaPath).fileName());
    header.insert(QStringLiteral("media_type"), kindForExtension(mediaPath));
    return saveNotesAt(QFileInfo(getNotesJsonPath(mediaPath)).absolutePath(), header, notes);
}

bool loadNotes(std::vector<AnnotationNote> &notes, const QString &mediaPath)
{
    return loadNotesAt(QFileInfo(getNotesJsonPath(mediaPath)).absolutePath(), notes);
}

void loadNotesAsync(const QString &mediaPath, LoadCallback callback)
{
    // Fire-and-forget on the global pool; we don't observe completion.
    QThreadPool::globalInstance()->start(
        [mediaPath, cb = std::move(callback)]() {
            std::vector<AnnotationNote> notes;
            const bool ok = loadNotes(notes, mediaPath);
            cb(ok, notes);
        });
}

void saveNotesAsync(const std::vector<AnnotationNote> &notes,
                    const QString &mediaPath)
{
    QThreadPool::globalInstance()->start([notes, mediaPath]() {
        saveNotes(notes, mediaPath);
    });
}

} // namespace qcv::annotation_io
