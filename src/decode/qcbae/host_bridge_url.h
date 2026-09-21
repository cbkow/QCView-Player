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

inline QString label(const QString &url)
{
    const QString h = hostOf(url);
    if (h == QLatin1String("ae"))       return QStringLiteral("After Effects");
    if (h == QLatin1String("premiere")) return QStringLiteral("Premiere Pro");
    if (h == QLatin1String("probe"))    return QStringLiteral("QCBridgeAE probe");
    return {};
}

} // namespace qcv::hostbridge
