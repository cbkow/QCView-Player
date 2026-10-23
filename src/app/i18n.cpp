// Interface language — see header.

#include "i18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <QtLogging>

namespace qcv {

namespace {

const auto kSettingsKey  = QStringLiteral("ui/language");
const auto kResourceDir  = QStringLiteral(":/i18n");
const auto kCatalogBase  = QStringLiteral("qcview");

// QCV_PSEUDO_LOC: "[Always on Top ~~~~~]" for every string. Padding is
// appended, so %1 and %n placeholders are untouched and Qt's %n
// substitution still runs on the result.
class PseudoTranslator : public QTranslator
{
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char *, const char *sourceText,
                      const char *, int) const override
    {
        const QString src = QString::fromUtf8(sourceText);
        if (src.isEmpty()) return QString();
        const int pad = qMax(2, (src.size() * 35) / 100);
        return QLatin1Char('[') + src + QLatin1Char(' ')
             + QString(pad, QLatin1Char('~')) + QLatin1Char(']');
    }
};

// qcview_<code>.qm → <code>, from the bundled resources.
QStringList bundledLanguages()
{
    QStringList out;
    const QStringList names = QDir(kResourceDir).entryList(
        {kCatalogBase + QStringLiteral("_*.qm")}, QDir::Files, QDir::Name);
    const int head = kCatalogBase.size() + 1, tail = 3;   // "qcview_" … ".qm"
    for (const QString &n : names) {
        const QString code = n.mid(head, n.size() - head - tail);
        if (!code.isEmpty() && code != QLatin1String("en")) out << code;
    }
    out.prepend(QStringLiteral("en"));   // the source language, always present
    return out;
}

} // namespace

I18n::I18n(QObject *parent) : QObject(parent) {}

void I18n::install()
{
    m_setting = QSettings().value(kSettingsKey).toString();
    m_installedSetting = m_setting;

    if (qEnvironmentVariableIntValue("QCV_PSEUDO_LOC") > 0) {
        QCoreApplication::installTranslator(new PseudoTranslator(qApp));
        m_active = QStringLiteral("pseudo");
        qInfo("i18n: pseudo-localisation on (QCV_PSEUDO_LOC)");
        return;
    }

    QString want = qEnvironmentVariable("QCV_LANG");
    if (want.isEmpty()) want = m_setting;
    const QLocale locale = want.isEmpty() ? QLocale::system() : QLocale(want);

    auto *tr = new QTranslator(qApp);
    const QString looseDir = qEnvironmentVariable("QCV_TRANSLATIONS_DIR");
    bool ok = false;
    if (!looseDir.isEmpty())
        ok = tr->load(locale, kCatalogBase, QStringLiteral("_"), looseDir);
    if (!ok)
        ok = tr->load(locale, kCatalogBase, QStringLiteral("_"), kResourceDir);
    // No catalog for that locale: English, but still with its plural
    // forms (qcview_en.qm carries nothing else).
    if (!ok)
        ok = tr->load(QLocale(QStringLiteral("en")), kCatalogBase,
                      QStringLiteral("_"), kResourceDir);
    if (ok) {
        QCoreApplication::installTranslator(tr);
        m_active = tr->language().isEmpty() ? locale.name() : tr->language();
    } else {
        delete tr;
        m_active = QStringLiteral("en");
    }
    qInfo("i18n: locale %s, setting '%s', catalog %s (%s)",
          qPrintable(locale.name()), qPrintable(m_setting),
          ok ? qPrintable(tr->filePath()) : "none", qPrintable(m_active));
}

QString I18n::language() const { return m_setting; }

void I18n::setLanguage(const QString &code)
{
    if (m_setting == code) return;
    m_setting = code;
    QSettings s;
    if (code.isEmpty()) s.remove(kSettingsKey);
    else                s.setValue(kSettingsKey, code);
    emit languageChanged();
}

QStringList I18n::availableLanguages() const { return bundledLanguages(); }
QString     I18n::activeLanguage() const     { return m_active; }
bool        I18n::restartRequired() const    { return m_setting != m_installedSetting; }

QString I18n::displayName(const QString &code) const
{
    if (code.isEmpty()) return tr("System default");
    const QLocale loc(code);
    QString name = loc.nativeLanguageName();
    if (name.isEmpty()) return code;
    name[0] = name[0].toUpper();
    if (code.contains(QLatin1Char('_')) && !loc.nativeTerritoryName().isEmpty())
        name += QStringLiteral(" (%1)").arg(loc.nativeTerritoryName());
    return name;
}

} // namespace qcv
