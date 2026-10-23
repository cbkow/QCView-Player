// Interface language: which translation catalog the app runs with.
//
// Strings are English in the source (qsTr in QML, tr() in C++). The
// build compiles translations/qcview_<lang>.ts into .qm catalogs under
// the :/i18n resource prefix (src/app/CMakeLists.txt); install() picks
// one from the user's setting or the system locale and installs it
// before any QML loads. The choice takes effect on the next launch: the
// native menus and the strings C++ hands to QML do not re-evaluate.
//
// Dev hooks (environment):
//   QCV_LANG=<code>             use this catalog, ignoring the setting
//   QCV_TRANSLATIONS_DIR=<dir>  look for qcview_<code>.qm there first,
//                               so a translator can test a catalog
//                               without rebuilding the app
//   QCV_PSEUDO_LOC=1            every string becomes a padded, bracketed
//                               copy of itself — finds clipped labels
//                               and strings that bypass tr()

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QQmlEngine;
class QTranslator;

namespace qcv {

class I18n : public QObject
{
    Q_OBJECT
    // "" follows the system; otherwise a code such as "de" or "pt_BR".
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // Codes with a bundled catalog, "en" first.
    Q_PROPERTY(QStringList availableLanguages READ availableLanguages CONSTANT)
    // The catalog in use; follows the setting at once.
    Q_PROPERTY(QString activeLanguage READ activeLanguage NOTIFY activeLanguageChanged)

public:
    explicit I18n(QObject *parent = nullptr);

    // Once, after QGuiApplication exists and before QML loads.
    void install();
    // The engine whose scene retranslates when the language changes.
    void attachEngine(QQmlEngine *engine);

    QString     language() const;
    void        setLanguage(const QString &code);
    QStringList availableLanguages() const;
    QString     activeLanguage() const;

    // Native name of a language for pickers ("Deutsch"); "" gives the
    // "System default" label.
    Q_INVOKABLE QString displayName(const QString &code) const;
    Q_INVOKABLE bool reviewed(const QString &code) const;

signals:
    void languageChanged();
    void activeLanguageChanged();

private:
    void apply(const QString &want);

    QString      m_setting;              // the stored choice
    QString      m_active;               // the catalog actually loaded
    QTranslator *m_translator = nullptr; // the installed app catalog
    QQmlEngine  *m_engine = nullptr;
};

} // namespace qcv
