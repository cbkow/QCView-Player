// AnnotationManager — direct lift from old QCView's
// src/annotations/annotation_manager.{h,cpp} per Guide 19 §2.4.
//
// In-memory list of AnnotationNotes for the currently loaded media.
// Notes are kept sorted by timecode (string compare). All mutations
// trigger an async save to disk; loads run on a worker thread.
//
// Adaptation notes vs old app:
//   - QObject (was POD class with std::function callback)
//   - Q_SIGNALS notesChanged / loadingChanged / savingChanged
//     (was: NotesChangedCallback)
//   - QString throughout (was std::string)
//   - QMutex (was std::mutex)
//   - std::atomic stays (loading_, saving_, batch_mode_)

#pragma once

#include "annotation_note.h"

#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <vector>

namespace qcv {

class AnnotationManager : public QObject {
    Q_OBJECT

public:
    explicit AnnotationManager(QObject *parent = nullptr);
    ~AnnotationManager() override;

    // ---- Media lifecycle ----
    void setMediaPath(const QString &mediaPath);
    void loadNotesForMedia(const QString &mediaPath);
    // Notes on a live stream (plan B1): a folder in the project sidecar
    // (annotation_io::getStreamDir) and the header its notes.json
    // carries (stream, stream_id, stream_url). Same list, same CRUD.
    void loadNotesForStream(const QString &streamDir, const QJsonObject &header);
    bool isStreamSource() const;
    QString streamDir() const;   // Stream source: the absolute folder; empty otherwise
    QJsonObject streamHeader() const;
    // The stream fields of one note, set once at capture (one save).
    void setNoteStreamFields(const QString &key, const QString &capturedAt,
                             const QString &hostTimecode, const QString &streamName);
    void unloadNotes();
    void clearNotes();   // Drop notes without saving (used when switching to a
                         // disabled state where saving would clobber data).

    // ---- Note CRUD ----
    void addNote(double timestampSeconds,
                 const QString &timecode,
                 int frame,
                 const QString &text = QString());
    void updateNoteText(const QString &timecode, const QString &text);
    void updateNoteAnnotationData(const QString &timecode,
                                  const QString &annotationData);
    void updateNoteImagePath(const QString &timecode,
                             const QString &imagePath);
    // Viewer aids in effect for the note's thumbnail (see
    // AnnotationNote::viewer_tag).
    void updateNoteViewerTag(const QString &timecode, const QString &tag);
    // The OCIO chain the thumbnail went through (AnnotationNote::ocio_chain).
    void updateNoteOcioChain(const QString &timecode, const QString &chain);
    void updateNoteAddressed(const QString &timecode, bool addressed);
    void deleteNote(const QString &timecode);

    // ---- Query ----
    std::vector<AnnotationNote> notes() const;          // copy
    AnnotationNote *noteAtTimecode(const QString &tc);
    const AnnotationNote *noteAtTimecode(const QString &tc) const;
    bool   hasNotes()  const;
    qsizetype noteCount() const;
    QString imagesFolder() const;
    QString annotationsDirectory() const;

    bool isLoading() const { return is_loading_.load(); }
    bool isSaving()  const { return is_saving_.load();  }

    // Batch mode skips the autosave on each update; call forceSave() when done.
    void setBatchMode(bool enabled) { batch_mode_.store(enabled); }
    void forceSave();

Q_SIGNALS:
    void notesChanged();
    void loadingChanged(bool loading);
    void savingChanged(bool saving);

private:
    void sortNotesByTimecode();
    void saveNotesAsyncLocked();   // assumes notes_mutex_ NOT held
    void notifyNotesChanged();

    // Where the notes live: a media file's sidecar, or a stream folder in
    // the project's sidecar. Guarded by notes_mutex_ like the list.
    struct Source {
        enum class Kind { None, Media, Stream } kind = Kind::None;
        QString     mediaPath;    // Media
        QString     streamDir;    // Stream: absolute folder
        QJsonObject streamHeader; // Stream: the notes.json header
    };
    QString imagesFolderLocked() const;   // caller holds notes_mutex_

    mutable QMutex              notes_mutex_;
    std::vector<AnnotationNote> notes_;
    Source                      source_;

    std::atomic<bool> is_loading_{false};
    std::atomic<bool> is_saving_{false};
    std::atomic<bool> batch_mode_{false};
};

} // namespace qcv
