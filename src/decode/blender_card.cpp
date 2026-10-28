#include "blender_card.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace qcv {

QStringList BlenderCardWatcher::inputsDirs()
{
    QStringList dirs;
    const QString env = qEnvironmentVariable("QCV_INPUTS_DIR");
    if (!env.isEmpty()) dirs << env;
#if defined(Q_OS_MACOS)
    dirs << QStringLiteral("/Library/Application Support/QCView/Inputs");
    dirs << QDir::homePath() + QStringLiteral("/Library/Application Support/QCView/Inputs");
#elif defined(Q_OS_WIN)
    const QString pd = qEnvironmentVariable("ProgramData");
    if (!pd.isEmpty()) dirs << pd + QStringLiteral("\\QCView\\Inputs");
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Inputs");
#else
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Inputs");
#endif
    return dirs;
}

BlenderCardWatcher::BlenderCardWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_poll(new QTimer(this))
{
    m_enabled = QSettings().value(QStringLiteral("input/blender/enabled"), true).toBool();
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) { poll(); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) { poll(); rewatch(); });
    // The folder may not exist until the first session, and a watcher on a
    // missing folder watches nothing: a slow poll covers creation, the
    // atomic-replace write (a rename the watcher sees as removal) and
    // Windows file-system quirks. 2 s is well under a session's start.
    m_poll->setInterval(2000);
    connect(m_poll, &QTimer::timeout, this, [this] { poll(); rewatch(); });
    rewatch();
    if (m_enabled) {
        m_poll->start();
        // The first look comes after the owner has connected to the
        // signals and the window exists (a card found at construction
        // time was announced to nobody, 2026-10-28).
        QTimer::singleShot(1500, this, [this] { if (m_enabled) poll(); });
    }
}

void BlenderCardWatcher::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    QSettings().setValue(QStringLiteral("input/blender/enabled"), on);
    if (on) {
        m_poll->start();
        rewatch();
        poll();
    } else {
        m_poll->stop();
        if (m_present) {
            m_present = false;
            m_card.clear();
            m_digest.clear();
            emit cardRemoved();
        }
    }
    emit enabledChanged();
}

void BlenderCardWatcher::rewatch()
{
    for (const QString &d : inputsDirs()) {
        if (QDir(d).exists() && !m_watcher->directories().contains(d)) m_watcher->addPath(d);
        const QString f = QDir(d).filePath(cardFileName());
        if (QFileInfo::exists(f) && !m_watcher->files().contains(f)) m_watcher->addPath(f);
    }
}

bool BlenderCardWatcher::readCard(const QString &path, QVariantMap &out, QString &err) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = f.errorString(); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("not a JSON object");
        return false;
    }
    out = doc.object().toVariantMap();
    if (out.value(QStringLiteral("kind")).toString() != QLatin1String("blender")
        || out.value(QStringLiteral("url")).toString().isEmpty()) {
        err = QStringLiteral("not a Blender card");
        return false;
    }
    return true;
}

void BlenderCardWatcher::poll()
{
    if (!m_enabled) return;
    QString found;
    for (const QString &d : inputsDirs()) {
        const QString f = QDir(d).filePath(cardFileName());
        if (QFileInfo::exists(f)) { found = f; break; }
    }
    if (found.isEmpty()) {
        if (m_present) {
            m_present = false;
            m_card.clear();
            m_digest.clear();
            m_path.clear();
            emit cardRemoved();
        }
        return;
    }
    QVariantMap card;
    QString err;
    if (!readCard(found, card, err)) {
        // A half-written file is impossible (the add-on replaces atomically);
        // anything else is a real problem, said once.
        if (m_lastError != err) {
            m_lastError = err;
            qWarning("BlenderCardWatcher: %s: %s", qPrintable(found), qPrintable(err));
        }
        return;
    }
    m_lastError.clear();
    QVariantMap body = card;
    body.remove(QStringLiteral("ts"));
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(QJsonObject::fromVariantMap(body)).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha1).toHex().left(16));
    const bool changed = !m_present || digest != m_digest;
    m_present = true;
    m_path = found;
    m_card = card;
    m_digest = digest;
    if (changed) emit cardChanged(card);
}

} // namespace qcv
