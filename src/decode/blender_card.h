#pragma once
// The Blender input's card (QCBridge 0.4, 2026-10-28): QCView never gets
// launched by Blender any more. The QCBridge host add-on writes one JSON
// card into QCView's Inputs folder on the same machine — port, passphrase,
// the colour chain, the mismatch rows — and QCView watches that folder,
// opens an SRT LISTENER on the card's port, and applies the chain. The
// replica calls in. The folder rule is the output manifests' (OutputBus):
//   macOS    /Library/Application Support/QCView/Inputs, ~/Library/…/QCView/Inputs
//   Windows  %ProgramData%\QCView\Inputs (never virtualized; the Store build
//            reads it), then AppDataLocation/Inputs
//   other    AppDataLocation/Inputs
// QCV_INPUTS_DIR overrides (tests, the colour harness).
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class QFileSystemWatcher;
class QTimer;

namespace qcv {

class BlenderCardWatcher : public QObject {
    Q_OBJECT
public:
    explicit BlenderCardWatcher(QObject *parent = nullptr);

    static QStringList inputsDirs();
    static QString     cardFileName() { return QStringLiteral("blender.json"); }

    // Off at every launch, never remembered: QCView spends nothing on
    // Blender until File > Connect to Blender is toggled on (2026-10-28,
    // with chris: "QCView has a greater purpose than Blender streaming").
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    bool        present() const { return m_present; }
    QVariantMap card() const { return m_card; }
    QString     path() const { return m_path; }
    QString     lastError() const { return m_lastError; }

    // Re-read now (the menu action, a settings change).
    void poll();

signals:
    // The card appeared or its content changed (the timestamp alone does not count).
    void cardChanged(const QVariantMap &card);
    void cardRemoved();
    void enabledChanged();

private:
    void rewatch();
    bool readCard(const QString &path, QVariantMap &out, QString &err) const;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer             *m_poll = nullptr;
    bool        m_enabled = false;
    bool        m_present = false;
    QVariantMap m_card;
    QString     m_path;
    QString     m_digest;      // content without "ts"
    QString     m_lastError;
};

} // namespace qcv
