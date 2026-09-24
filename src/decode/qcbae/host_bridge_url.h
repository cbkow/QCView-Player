// qcbae:// URLs — the only place they are mapped.
//
// A QCBridgeAE live item is a MediaType::LiveStream whose path is a qcbae://
// URL, so every "://"-keyed live behaviour (routing, persistence, dual
// guards) applies to it unchanged. This header turns that URL into the
// shared-memory ring name the Transmit device publishes, and into the name
// the user sees. Header-only and platform-neutral: the project model uses it
// for naming on every platform, while the ring reader itself
// (HostBridgeSource) is macOS-only until the Windows port.

#pragma once

#include "decode/qcbae/shared_ring.h"   // ring-name constants only

#include <QString>

namespace qcv::hostbridge {

inline bool isUrl(const QString &url)
{
    return url.startsWith(QStringLiteral("qcbae://"), Qt::CaseInsensitive);
}

// "ae", "premiere", "probe" — lower-case host part, or empty.
inline QString hostOf(const QString &url)
{
    if (!isUrl(url)) return {};
    return url.mid(8).section(QLatin1Char('/'), 0, 0)
                     .section(QLatin1Char('?'), 0, 0).toLower();
}

// qcbae://probe is QCBridgeAE's synthetic producer (qcbae-probe produce),
// for testing without an Adobe host.
inline QString ringName(const QString &url)
{
    const QString h = hostOf(url);
    if (h == QLatin1String("ae"))       return QString::fromLatin1(qcbae::kRingNameAfterEffects);
    if (h == QLatin1String("premiere")) return QString::fromLatin1(qcbae::kRingNamePremiere);
    if (h == QLatin1String("probe"))    return QStringLiteral("/qcbae-probe");
    return {};
}

// The source as the user sees it — bin row, LiveStrip, recents. Named for
// the product that delivers it, not just the host application.
inline QString label(const QString &url)
{
    const QString h = hostOf(url);
    if (h == QLatin1String("ae"))       return QStringLiteral("QCBridge After Effects");
    if (h == QLatin1String("premiere")) return QStringLiteral("QCBridge Premiere Pro");
    if (h == QLatin1String("probe"))    return QStringLiteral("QCBridge Test Signal");
    return {};
}

// The application the user acts on when the feed stops ("After Effects is
// not running"). The test signal's "host" is QCBridgeAE's qcbae-probe tool.
inline QString hostApp(const QString &url)
{
    const QString h = hostOf(url);
    if (h == QLatin1String("ae"))       return QStringLiteral("After Effects");
    if (h == QLatin1String("premiere")) return QStringLiteral("Premiere Pro");
    if (h == QLatin1String("probe"))    return QStringLiteral("the QCBridge test producer");
    return {};
}

// What the Inspector states about a source. Each claim was measured in
// QCBridgeAE (lab/results/2026-09-21-a4-transmit-probe/, -a6-transmit-device/)
// — keep it that way: a wrong statement here misleads a QC decision.
struct SourceFacts {
    // Terse, one line each: they sit in the Inspector's fixed-height rows.
    // Trimmed 2026-09-24 to what a QC call rests on and the live strip
    // does not already say: the meaning of the values, the meaning of the
    // fourth channel, and the one instruction. Source, transport and pixel
    // layout used to be rows too; the strip carries the format, the
    // item's name carries the host, and the ring name is nobody's business.
    QString source;      // who renders the pixels — the item name says it; kept for callers
    QString colour;      // what the values mean
    QString alpha;       // what the fourth channel means
    QString note;        // the one instruction for the user
};

inline SourceFacts facts(const QString &url)
{
    const QString h = hostOf(url);
    SourceFacts f;
    f.colour = QStringLiteral("Host working space · untransformed · never clamped");
    f.note   = QStringLiteral("Set QCView's OCIO input to the host's working space.");
    if (h == QLatin1String("ae")) {
        f.source = QStringLiteral("After Effects · Mercury Transmit");
        f.alpha  = QStringLiteral("Opaque · flattened over comp background");
    } else if (h == QLatin1String("premiere")) {
        f.source = QStringLiteral("Premiere Pro · Mercury Transmit");
        f.alpha  = QStringLiteral("Straight · from the sequence");
    } else if (h == QLatin1String("probe")) {
        f.source = QStringLiteral("QCBridgeAE qcbae-probe produce");
        f.colour = QStringLiteral("Synthetic · ramp runs past 1.0");
        f.alpha  = QStringLiteral("Opaque");
        f.note   = QStringLiteral("A test signal for checking QCView without an Adobe host.");
    } else {
        return {};
    }
    return f;
}

} // namespace qcv::hostbridge
