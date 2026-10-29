// The one list of file types QCView opens (2026-10-29). Before this, the
// Open / Add dialogs, the project classifier, the dual controller, the
// annotation sidecar naming and the thumbnail cache each carried their
// own copy, no two alike, and nothing refused a file: an unknown suffix
// was filed as video and failed in the decoder. Now every path a user
// hands the app — dialog, drop, recents, command line — is checked here
// first, and the dialogs' filters are generated from the same table.
//
// Video is deliberately generous: anything FFmpeg demuxes and decodes
// with the codecs we build. Camera raw with no decoder (.r3d, .braw,
// .ari) is left out on purpose so it is refused at the door with a
// reason; ARRIRAW inside an .mxf passes the suffix test and is reported
// by the decoder path instead (unsupported-media toast + row badge).
// Images are the closed set the native loaders handle (DPX has none:
// it rides the FFmpeg path, hence its place in the video list).
#pragma once

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace qcv {
namespace mediaext {

enum class Kind { Video, Audio, Image, Unknown };

inline const QStringList &videoExtensions()
{
    static const QStringList v = {
        QStringLiteral("mov"),  QStringLiteral("mp4"),  QStringLiteral("m4v"),
        QStringLiteral("mxf"),  QStringLiteral("mkv"),  QStringLiteral("avi"),
        QStringLiteral("webm"), QStringLiteral("webp"), QStringLiteral("gif"),
        QStringLiteral("ts"),   QStringLiteral("mts"),  QStringLiteral("m2ts"),
        QStringLiteral("m2t"),  QStringLiteral("mpg"),  QStringLiteral("mpeg"),
        QStringLiteral("mpe"),  QStringLiteral("m2v"),  QStringLiteral("vob"),
        QStringLiteral("3gp"),  QStringLiteral("3g2"),  QStringLiteral("wmv"),
        QStringLiteral("asf"),  QStringLiteral("flv"),  QStringLiteral("f4v"),
        QStringLiteral("ogv"),  QStringLiteral("ogm"),  QStringLiteral("mjpeg"),
        QStringLiteral("mjpg"), QStringLiteral("y4m"),  QStringLiteral("dv"),
        QStringLiteral("mj2"),  QStringLiteral("dpx"),
    };
    return v;
}

inline const QStringList &audioExtensions()
{
    static const QStringList a = {
        QStringLiteral("wav"),  QStringLiteral("aif"),  QStringLiteral("aiff"),
        QStringLiteral("aifc"), QStringLiteral("mp3"),  QStringLiteral("flac"),
        QStringLiteral("m4a"),  QStringLiteral("aac"),  QStringLiteral("ogg"),
        QStringLiteral("oga"),  QStringLiteral("opus"), QStringLiteral("wma"),
        QStringLiteral("caf"),
    };
    return a;
}

// Must stay in lockstep with ImageSequenceCache's createLoaderForExtension.
inline const QStringList &imageExtensions()
{
    static const QStringList i = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("exr"),
    };
    return i;
}

inline Kind kindForSuffix(const QString &suffixLower)
{
    if (videoExtensions().contains(suffixLower)) return Kind::Video;
    if (audioExtensions().contains(suffixLower)) return Kind::Audio;
    if (imageExtensions().contains(suffixLower)) return Kind::Image;
    return Kind::Unknown;
}

inline Kind kindForPath(const QString &path)
{
    return kindForSuffix(QFileInfo(path).suffix().toLower());
}

// The gate: a regular file with a known suffix. Folders and anything
// else are refused by the callers with a toast.
inline bool isSupportedMediaFile(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isDir()) return false;
    return kindForSuffix(fi.suffix().toLower()) != Kind::Unknown;
}

inline QString globPattern(const QStringList &exts)
{
    QStringList g;
    g.reserve(exts.size());
    for (const QString &e : exts) g << (QStringLiteral("*.") + e);
    return g.join(QLatin1Char(' '));
}

// FileDialog.nameFilters for every media picker, generated so the
// dialogs can never drift from the gate.
inline QStringList nameFilters()
{
    const QString all = globPattern(videoExtensions()) + QLatin1Char(' ')
                      + globPattern(audioExtensions()) + QLatin1Char(' ')
                      + globPattern(imageExtensions());
    return {
        //: File dialog filter; %1 = the list of patterns
        QCoreApplication::translate("MediaFilters", "All media (%1)").arg(all),
        QCoreApplication::translate("MediaFilters", "Video files (%1)").arg(globPattern(videoExtensions())),
        QCoreApplication::translate("MediaFilters", "Audio files (%1)").arg(globPattern(audioExtensions())),
        QCoreApplication::translate("MediaFilters", "Image / sequence files (%1)").arg(globPattern(imageExtensions())),
        QCoreApplication::translate("MediaFilters", "All files (*)"),
    };
}

} // namespace mediaext
} // namespace qcv
