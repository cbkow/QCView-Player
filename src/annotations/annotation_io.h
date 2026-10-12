// AnnotationIO — direct lift from old QCView's
// src/annotations/annotation_io.{h,cpp} per Guide 19 §2.4.
//
// Folder layout + JSON read/write for note metadata. Two layouts:
//
//   1. Per-media (single clip):
//        <media_dir>/.qcview/<media_filename>/notes.json
//        <media_dir>/.qcview/<media_filename>/images/note_<TC>.png
//
//   2. Per-project-timeline (when notes belong to a timeline, not a
//      single clip):
//        <project_dir>/.qcview/<timeline_name>/notes.json
//        <project_dir>/.qcview/<timeline_name>/images/note_<TC>.png
//
//   3. Per-stream (plan B1, 2026-10-12): notes on a live feed live in
//      the project's sidecar, one folder per stream item, named after
//      the item and the first eight characters of its id so a rename
//      never moves it:
//        <project_dir>/.qcview/streams/<name>__<id8>/notes.json
//        <project_dir>/.qcview/streams/<name>__<id8>/images/note_<stamp>.png
//      The header carries stream / stream_id / stream_url instead of
//      media_file / media_type. minNotes reads the same folder.
//
// Read-side falls back to legacy `.ump` folder if `.qcview` doesn't
// exist (old "UMP" alpha used .ump). Write-side always creates
// `.qcview`.
//
// Adaptation notes vs old app:
//   - Namespace qcview::AnnotationIO → qcv::annotation_io
//   - std::string → QString throughout
//   - std::filesystem → QDir / QFile / QFileInfo
//   - Async via QtConcurrent::run (was std::thread::detach)
//   - SaveScreenshot dropped — Phase B annotation pass writes PNGs via
//     QImage::save (no separate stb_image_write dep needed)
//   - Per-Windows hidden-attr: keep (AttributesNormal | Hidden via
//     SetFileAttributesA, guarded by Q_OS_WIN)

#pragma once

#include "annotation_note.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

namespace qcv::annotation_io {

// ---- Per-media path helpers ----
QString getQcViewPath(const QString &mediaPath);
QString getNotesJsonPath(const QString &mediaPath);
QString getImagesFolder(const QString &mediaPath);
QString sanitizeMediaName(const QString &filename);
// The one filesystem-safe stem for a note key: `:` and `;` (drop-frame)
// become `_`; a capture stamp passes through. Every file name derived
// from a key — images, exports — goes through here.
QString fileStemForKey(const QString &key);
QString generateImageFilename(const QString &timecode);

// ---- Per-stream folders (project sidecar) ----
// The folder name for a stream item: sanitized name + "__" + id8.
QString streamFolderName(const QString &itemName, const QString &itemId);
// The existing folder for this id under <projectDir>/.qcview/streams
// (matched on the "__<id8>" suffix, so a renamed item still finds its
// notes), or empty when there is none yet.
QString findStreamFolder(const QString &projectPath, const QString &itemId);
// Absolute folder for a stream folder name (exists or not).
QString getStreamDir(const QString &projectPath, const QString &folderName);
bool    createStreamFolder(const QString &projectPath, const QString &folderName);

// ---- Folder-rooted JSON I/O: any <dir>/notes.json + <dir>/images ----
QString notesJsonPathIn(const QString &dir);
QString imagesFolderIn(const QString &dir);
// `header` is written as the root object's non-"notes" keys (media_file
// / media_type for media, stream / stream_id / stream_url for streams);
// "created" is added when missing. On load the header comes back.
bool saveNotesAt(const QString &dir, const QJsonObject &header,
                 const std::vector<AnnotationNote> &notes);
bool loadNotesAt(const QString &dir, std::vector<AnnotationNote> &notes,
                 QJsonObject *header = nullptr);
void saveNotesAtAsync(const QString &dir, const QJsonObject &header,
                      const std::vector<AnnotationNote> &notes);

// ---- Per-project-timeline path helpers ----
// `projectPath` is the path to the project file (e.g. "foo.qcvproj");
// the project dir is its parent. timelineName is sanitized.
QString getProjectAnnotationPath(const QString &projectPath,
                                 const QString &timelineName);
QString getProjectImagesFolder(const QString &projectPath,
                               const QString &timelineName);
bool createProjectQcViewFolder(const QString &projectPath,
                               const QString &timelineName);

// ---- Folder management ----
bool createQcViewFolder(const QString &mediaPath);
bool ensureImagesFolderExists(const QString &mediaPath);

// ---- JSON I/O (sync) ----
bool saveNotes(const std::vector<AnnotationNote> &notes,
               const QString &mediaPath);
bool loadNotes(std::vector<AnnotationNote> &notes,
               const QString &mediaPath);

// ---- Async wrappers (fire-and-forget, off the calling thread) ----
using LoadCallback =
    std::function<void(bool success, const std::vector<AnnotationNote> &)>;
void loadNotesAsync(const QString &mediaPath, LoadCallback callback);
void saveNotesAsync(const std::vector<AnnotationNote> &notes,
                    const QString &mediaPath);

} // namespace qcv::annotation_io
