// Short colourspace names for badges (colour plan stage 3): the project
// panel rows and the A/B chips name a clip's own Input in ~20 characters.
//
// Generic word rules, never a nickname table, so they survive config
// upgrades (checked against Blender 5.2 and ACES 2.0):
//   1. tidy — drop " - Display", " - Texture" and "Encoded"; Linear → Lin;
//      Wide Gamut → WG; "to ACES2065-1" → "→ AP0";
//   2. cut at a word boundary near 20 characters, ending in "…";
//   3. names whose short forms collide in the same config keep their full
//      tidied name — or the exact name, when tidying alone collides
//      ("X - Display" / "X - Texture") (shortNamesFor);
//   4. the tooltip always shows the exact colourspace name.
// CONFIG UPGRADE: recheck the rules against new configs (checklist in
// assets/OCIO/patches/README.md).

#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace qcv::colourspace_names {

inline QString tidy(QString n)
{
    n.replace(QStringLiteral(" - Display"), QString());
    n.replace(QStringLiteral(" - Texture"), QString());
    n.replace(QRegularExpression(QStringLiteral("\\bEncoded\\b")), QString());
    n.replace(QRegularExpression(QStringLiteral("\\bLinear\\b")), QStringLiteral("Lin"));
    n.replace(QStringLiteral("Wide Gamut"), QStringLiteral("WG"));
    n.replace(QStringLiteral("to ACES2065-1"), QStringLiteral("→ AP0"));
    return n.simplified();
}

inline QString cut(const QString &n, int limit = 20)
{
    if (n.size() <= limit) return n;
    int at = n.lastIndexOf(QLatin1Char(' '), limit);
    if (at < limit / 2) at = limit;   // no sensible word boundary
    return n.left(at).trimmed() + QStringLiteral("…");
}

inline QString shortName(const QString &name) { return cut(tidy(name)); }

// Short names for every colourspace of a config; colliding short forms
// fall back to the full tidied name, then to the exact name.
inline QHash<QString, QString> shortNamesFor(const QStringList &colourspaces)
{
    auto uses = [&](auto nameOf) {
        QHash<QString, int> n;
        for (const QString &c : colourspaces) n[nameOf(c)] += 1;
        return n;
    };
    const QHash<QString, int> shortUses = uses(shortName);
    const QHash<QString, int> tidyUses  = uses(tidy);
    QHash<QString, QString> out;
    for (const QString &c : colourspaces) {
        const QString s = shortName(c);
        if (shortUses.value(s) <= 1)         out.insert(c, s);
        else if (tidyUses.value(tidy(c)) <= 1) out.insert(c, tidy(c));
        else                                  out.insert(c, c);
    }
    return out;
}

} // namespace qcv::colourspace_names
