#include "annotation_manager.h"

#include "annotation_io.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <algorithm>

namespace qcv {

AnnotationManager::AnnotationManager(QObject *parent)
    : QObject(parent)
{
}

AnnotationManager::~AnnotationManager() = default;

void AnnotationManager::setMediaPath(const QString &mediaPath)
{
    QMutexLocker lk(&notes_mutex_);
    source_ = Source{};
    source_.kind = Source::Kind::Media;
    source_.mediaPath = mediaPath;
}

void AnnotationManager::loadNotesForStream(const QString &streamDir, const QJsonObject &header)
{
    is_loading_.store(true);
    Q_EMIT loadingChanged(true);
    {
        QMutexLocker lk(&notes_mutex_);
        source_ = Source{};
        source_.kind = Source::Kind::Stream;
        source_.streamDir = streamDir;
        source_.streamHeader = header;
    }
    std::vector<AnnotationNote> loaded;
    QJsonObject onDisk;
    const bool ok = annotation_io::loadNotesAt(streamDir, loaded, &onDisk);
    {
        QMutexLocker lk(&notes_mutex_);
        notes_ = ok ? std::move(loaded) : std::vector<AnnotationNote>{};
        if (ok) sortNotesByTimecode();
        // Keep what the file already says (created, anything minNotes
        // added) and refresh what we know now (the display name).
        for (auto it = header.begin(); it != header.end(); ++it) onDisk.insert(it.key(), it.value());
        source_.streamHeader = onDisk;
    }
    is_loading_.store(false);
    Q_EMIT loadingChanged(false);
    notifyNotesChanged();
}

bool AnnotationManager::isStreamSource() const
{
    QMutexLocker lk(&notes_mutex_);
    return source_.kind == Source::Kind::Stream;
}

QJsonObject AnnotationManager::streamHeader() const
{
    QMutexLocker lk(&notes_mutex_);
    return source_.streamHeader;
}

void AnnotationManager::setNoteStreamFields(const QString &key, const QString &capturedAt,
                                            const QString &hostTimecode, const QString &streamName)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == key; });
        if (it == notes_.end()) return;
        it->captured_at   = capturedAt;
        it->host_timecode = hostTimecode;
        it->stream        = streamName;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::loadNotesForMedia(const QString &mediaPath)
{
    is_loading_.store(true);
    Q_EMIT loadingChanged(true);

    setMediaPath(mediaPath);

    std::vector<AnnotationNote> loaded;
    const bool ok = annotation_io::loadNotes(loaded, mediaPath);

    if (ok) {
        QMutexLocker lk(&notes_mutex_);
        notes_ = std::move(loaded);
        sortNotesByTimecode();
    }

    is_loading_.store(false);
    Q_EMIT loadingChanged(false);
    notifyNotesChanged();
}

void AnnotationManager::unloadNotes()
{
    {
        QMutexLocker lk(&notes_mutex_);
        notes_.clear();
        source_ = Source{};
    }
    notifyNotesChanged();
}

void AnnotationManager::clearNotes()
{
    {
        QMutexLocker lk(&notes_mutex_);
        notes_.clear();
        source_ = Source{};
    }
    notifyNotesChanged();
}

void AnnotationManager::addNote(double timestampSeconds,
                                const QString &timecode,
                                int frame,
                                const QString &text)
{
    {
        QMutexLocker lk(&notes_mutex_);
        const QString filename = annotation_io::generateImageFilename(timecode);
        const QString imagePath = QStringLiteral("images/") + filename;
        notes_.emplace_back(timecode, timestampSeconds, frame, imagePath, text);
        sortNotesByTimecode();
    }
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

void AnnotationManager::updateNoteText(const QString &timecode,
                                       const QString &text)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->text = text;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteAnnotationData(const QString &timecode,
                                                 const QString &annotationData)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->annotation_data = annotationData;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteImagePath(const QString &timecode,
                                            const QString &imagePath)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->image_path = imagePath;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteViewerTag(const QString &timecode, const QString &tag)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it == notes_.end() || it->viewer_tag == tag) return;
        it->viewer_tag = tag;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteOcioChain(const QString &timecode, const QString &chain)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it == notes_.end() || it->ocio_chain == chain) return;
        it->ocio_chain = chain;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteAddressed(const QString &timecode, bool addressed)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->addressed = addressed;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::deleteNote(const QString &timecode)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        // We deliberately do NOT delete the on-disk thumbnails
        // (note_<TC>.png + _annotated.png) here. This method is
        // called from BOTH the user-click delete path AND the
        // annotation-undo path (when undoing the stroke that
        // created a fresh note). The undo case must preserve the
        // images so a subsequent redo can restore them.
        //
        // PNG file deletion is intentionally tied to an explicit
        // user click only — handled in
        // WindowManager::deleteAnnotationNote, which fires this
        // method after wiping the files.
        if (it != notes_.end()) notes_.erase(it);
    }
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

std::vector<AnnotationNote> AnnotationManager::notes() const
{
    QMutexLocker lk(&notes_mutex_);
    return notes_;   // copy
}

AnnotationNote *AnnotationManager::noteAtTimecode(const QString &tc)
{
    QMutexLocker lk(&notes_mutex_);
    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&](const AnnotationNote &n) { return n.timecode == tc; });
    return it == notes_.end() ? nullptr : &(*it);
}

const AnnotationNote *AnnotationManager::noteAtTimecode(const QString &tc) const
{
    QMutexLocker lk(&notes_mutex_);
    auto it = std::find_if(notes_.cbegin(), notes_.cend(),
        [&](const AnnotationNote &n) { return n.timecode == tc; });
    return it == notes_.cend() ? nullptr : &(*it);
}

bool AnnotationManager::hasNotes() const
{
    QMutexLocker lk(&notes_mutex_);
    return !notes_.empty();
}

qsizetype AnnotationManager::noteCount() const
{
    QMutexLocker lk(&notes_mutex_);
    return static_cast<qsizetype>(notes_.size());
}

QString AnnotationManager::imagesFolderLocked() const
{
    switch (source_.kind) {
    case Source::Kind::Media:  return annotation_io::getImagesFolder(source_.mediaPath);
    case Source::Kind::Stream: return annotation_io::imagesFolderIn(source_.streamDir);
    case Source::Kind::None:   break;
    }
    return {};
}

QString AnnotationManager::imagesFolder() const
{
    QMutexLocker lk(&notes_mutex_);
    return imagesFolderLocked();
}

QString AnnotationManager::annotationsDirectory() const
{
    QMutexLocker lk(&notes_mutex_);
    const QString images = imagesFolderLocked();
    if (images.isEmpty()) return {};
    return QFileInfo(images).absolutePath();
}

void AnnotationManager::forceSave()
{
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

void AnnotationManager::sortNotesByTimecode()
{
    // Caller holds notes_mutex_.
    std::sort(notes_.begin(), notes_.end());
}

void AnnotationManager::saveNotesAsyncLocked()
{
    is_saving_.store(true);
    Q_EMIT savingChanged(true);

    Source src;
    std::vector<AnnotationNote> copy;
    {
        QMutexLocker lk(&notes_mutex_);
        src  = source_;
        copy = notes_;
    }

    // Off-thread, fire-and-forget — we don't observe completion.
    switch (src.kind) {
    case Source::Kind::Media:  annotation_io::saveNotesAsync(copy, src.mediaPath); break;
    case Source::Kind::Stream: annotation_io::saveNotesAtAsync(src.streamDir, src.streamHeader, copy); break;
    case Source::Kind::None:   break;
    }

    is_saving_.store(false);
    Q_EMIT savingChanged(false);
}

void AnnotationManager::notifyNotesChanged()
{
    Q_EMIT notesChanged();
}

} // namespace qcv
