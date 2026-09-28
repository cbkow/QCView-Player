#include "ocio_chain_builder.h"

#include "ocio_config_manager.h"

#include <QtLogging>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// The active chain's parts, resolved once so the unsplit and split
// builds cannot drift apart. Phase 2.5d / Guide 05 D7 chain shape
// (preserved verbatim from old app's src/color/ocio_pipeline.cpp:179-262):
//
//   inputCs
//     → [LookTransform inputCs → lookResultCs]   (if Look set)
//     → [Scene LUT FileTransform]                (if Scene LUT set)
//     → DisplayViewTransform(srcCs → display+view)
//         where srcCs = lookResultCs if Look set, else inputCs
//     → [Display LUT FileTransform]              (if Display LUT set)
//
// setLooksBypass on the DVT stays at the default (false) so the view's
// built-in looks chain still applies. If overlap with the user's look
// happens, that's the colorist's choice — matches old-app behavior.
struct ChainParts {
    OCIO::LookTransformRcPtr look;         // null if no Look
    OCIO::FileTransformRcPtr sceneLut;     // null if no Scene LUT
    QByteArray               dvtSrcCs;     // colourspace entering the Display/View
    QByteArray               display;
    QByteArray               view;
    OCIO::FileTransformRcPtr displayLut;   // null if none (or overridden)
};

bool resolveParts(OCIOConfigManager *ocio, OCIO::ConstConfigRcPtr cfg,
                  const DisplayViewOverride *override,
                  ChainParts &out, QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };

    if (!ocio) return fail(QStringLiteral("OcioChainBuilder: null OCIOConfigManager"));
    if (!cfg)  return fail(QStringLiteral("OcioChainBuilder: null OCIO config"));

    const QString inputCs = ocio->activeInput();
    const QString display = override ? override->display : ocio->activeDisplay();
    const QString view    = override ? override->view    : ocio->activeView();
    const QString look    = ocio->activeLook();
    if (inputCs.isEmpty() || display.isEmpty() || view.isEmpty()) {
        return fail(QStringLiteral("OcioChainBuilder: active chain incomplete"));
    }

    const QString sceneLutPath   = ocio->activeSceneLutPath();
    const QString displayLutPath =
        override ? QString() : ocio->activeDisplayLutPath();

    out.dvtSrcCs = inputCs.toUtf8();
    out.display  = display.toUtf8();
    out.view     = view.toUtf8();

    if (!look.isEmpty()) {
        const char *lookResultCs =
            OCIO::LookTransform::GetLooksResultColorSpace(
                cfg, cfg->getCurrentContext(),
                look.toUtf8().constData());
        if (lookResultCs && *lookResultCs) {
            out.look = OCIO::LookTransform::Create();
            out.look->setSrc(inputCs.toUtf8().constData());
            out.look->setDst(lookResultCs);
            out.look->setLooks(look.toUtf8().constData());
            out.dvtSrcCs = QByteArray(lookResultCs);
        } else {
            qWarning("OcioChainBuilder: GetLooksResultColorSpace "
                     "returned null for '%s' — skipping Look",
                     qPrintable(look));
        }
    }

    if (!sceneLutPath.isEmpty()) {
        out.sceneLut = OCIO::FileTransform::Create();
        out.sceneLut->setSrc(sceneLutPath.toUtf8().constData());
        // CDL collections: which correction (empty = the first).
        out.sceneLut->setCCCId(ocio->activeSceneLutCccId().toUtf8().constData());
        out.sceneLut->setInterpolation(OCIO::INTERP_BEST);
        out.sceneLut->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
    }

    if (!displayLutPath.isEmpty()) {
        out.displayLut = OCIO::FileTransform::Create();
        out.displayLut->setSrc(displayLutPath.toUtf8().constData());
        out.displayLut->setInterpolation(OCIO::INTERP_BEST);
        out.displayLut->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
    }
    return true;
}

OCIO::DisplayViewTransformRcPtr makeDvt(const char *src, const ChainParts &p)
{
    OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
    dvt->setSrc(src);
    dvt->setDisplay(p.display.constData());
    dvt->setView(p.view.constData());
    return dvt;
}

// What the Display/View writes: its encoding (for the viewer aids and
// the knee's SDR target) and primaries family (luma weights). Uses the
// colourspace `encoding` attribute; configs without it fall back to the
// name (the bundled configs all set it).
struct DisplayOutput {
    OutputEncoding encoding = OutputEncoding::Sdr;
    int            primaries = 0;   // 0 = Rec.709, 1 = P3, 2 = Rec.2020
};

DisplayOutput displayViewOutput(OCIO::ConstConfigRcPtr cfg, const ChainParts &p)
{
    const char *csName =
        cfg->getDisplayViewColorSpaceName(p.display.constData(), p.view.constData());
    QByteArray name = csName ? QByteArray(csName) : QByteArray();
    if (name.isEmpty() || name == "<USE_DISPLAY_NAME>") name = p.display;
    OCIO::ConstColorSpaceRcPtr cs = cfg->getColorSpace(name.constData());
    const QByteArray enc = cs ? QByteArray(cs->getEncoding()) : QByteArray();
    const QString n = QString::fromUtf8(name) + QLatin1Char(' ') + QString::fromUtf8(p.display);
    auto has = [&n](const char *s) { return n.contains(QLatin1String(s), Qt::CaseInsensitive); };

    DisplayOutput out;
    if (enc == "sdr-video") {
        out.encoding = OutputEncoding::Sdr;
    } else if (enc == "display-linear" || enc == "scene-linear") {
        out.encoding = OutputEncoding::Linear;
    } else if (enc == "hdr-video") {
        out.encoding = has("HLG") ? OutputEncoding::Hlg : OutputEncoding::Pq;
    } else if (has("HLG")) {
        out.encoding = OutputEncoding::Hlg;
    } else if (has("PQ") || has("ST2084") || has("2100")) {
        out.encoding = OutputEncoding::Pq;
    } else if (has("EDR") || has("Linear")) {
        out.encoding = OutputEncoding::Linear;
    }
    if (has("P3"))                          out.primaries = 1;
    else if (has("2020") || has("2100"))    out.primaries = 2;
    return out;
}

OcioChain extractShader(OCIO::ConstConfigRcPtr cfg,
                        OCIO::ConstTransformRcPtr transform,
                        OCIO::GpuLanguage lang,
                        const char *functionName,
                        const char *prefix)
{
    OcioChain out;
    try {
        OCIO::ConstProcessorRcPtr    proc    = cfg->getProcessor(transform);
        OCIO::ConstGPUProcessorRcPtr gpuProc = proc->getDefaultGPUProcessor();
        OCIO::GpuShaderDescRcPtr     desc    = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(lang);
        desc->setFunctionName(functionName);
        desc->setResourcePrefix(prefix);
        gpuProc->extractGpuShaderInfo(desc);
        out.desc       = desc;
        out.shaderText = QString::fromUtf8(desc->getShaderText());
        out.ok         = true;
    } catch (const OCIO::Exception &e) {
        out.errorMessage = QStringLiteral("OCIO build: %1").arg(e.what());
    }
    return out;
}

OCIO::GpuLanguage toGpuLanguage(OcioChainBuilder::Language language)
{
    switch (language) {
        case OcioChainBuilder::Language::Glsl_4_0:    return OCIO::GPU_LANGUAGE_GLSL_4_0;
        case OcioChainBuilder::Language::Msl_2_0:     return OCIO::GPU_LANGUAGE_MSL_2_0;
        case OcioChainBuilder::Language::GlslVk_4_6:  return OCIO::GPU_LANGUAGE_GLSL_VK_4_6;
        case OcioChainBuilder::Language::Hlsl_Sm_5_0: return OCIO::GPU_LANGUAGE_HLSL_SM_5_0;
    }
    return OCIO::GPU_LANGUAGE_GLSL_4_0;
}

} // namespace

OCIO::GroupTransformRcPtr OcioChainBuilder::buildGroupTransform(
    OCIOConfigManager *ocio,
    OCIO::ConstConfigRcPtr cfg,
    QString *errorOut,
    const DisplayViewOverride *override)
{
    try {
        ChainParts p;
        if (!resolveParts(ocio, cfg, override, p, errorOut)) return nullptr;

        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        if (p.look)       group->appendTransform(p.look);
        if (p.sceneLut)   group->appendTransform(p.sceneLut);
        group->appendTransform(makeDvt(p.dvtSrcCs.constData(), p));
        if (p.displayLut) group->appendTransform(p.displayLut);
        return group;
    } catch (const OCIO::Exception &e) {
        if (errorOut) *errorOut = QStringLiteral("OCIO build: %1").arg(e.what());
        return nullptr;
    }
}

bool OcioChainBuilder::buildSplitTransforms(OCIOConfigManager *ocio,
                                            OCIO::ConstConfigRcPtr cfg,
                                            OcioSplitTransforms &out,
                                            QString *errorOut,
                                            const DisplayViewOverride *override)
{
    auto fail = [&](const QString &msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };
    try {
        ChainParts p;
        if (!resolveParts(ocio, cfg, override, p, errorOut)) return false;

        OCIO::ConstColorSpaceRcPtr src = cfg->getColorSpace(p.dvtSrcCs.constData());
        if (!src) {
            return fail(QStringLiteral("OcioChainBuilder: unknown colourspace '%1'")
                            .arg(QString::fromUtf8(p.dvtSrcCs)));
        }
        if (src->isData()) {
            return fail(QStringLiteral("OcioChainBuilder: '%1' is a data colourspace")
                            .arg(QString::fromUtf8(p.dvtSrcCs)));
        }
        // A data view ("Raw") passes the source's values through untouched,
        // so there is nothing for the stage to act on — and splitting
        // would show the interchange values instead of the source's.
        {
            const char *viewCs = cfg->getDisplayViewColorSpaceName(
                p.display.constData(), p.view.constData());
            QByteArray vn = viewCs ? QByteArray(viewCs) : QByteArray();
            if (vn.isEmpty() || vn == "<USE_DISPLAY_NAME>") vn = p.display;
            OCIO::ConstColorSpaceRcPtr vcs = cfg->getColorSpace(vn.constData());
            if (vcs && vcs->isData()) {
                return fail(QStringLiteral("OcioChainBuilder: '%1' is a data view")
                                .arg(QString::fromUtf8(p.view)));
            }
        }
        const bool scene = src->getReferenceSpaceType() == OCIO::REFERENCE_SPACE_SCENE;
        const char *role = scene ? "aces_interchange" : "cie_xyz_d65_interchange";
        if (!cfg->hasRole(role)) {
            return fail(QStringLiteral("OcioChainBuilder: config has no %1 role")
                            .arg(QString::fromUtf8(role)));
        }
        const char *interchange = cfg->getRoleColorSpace(role);

        out.pre = OCIO::GroupTransform::Create();
        if (p.look)     out.pre->appendTransform(p.look);
        if (p.sceneLut) out.pre->appendTransform(p.sceneLut);
        OCIO::ColorSpaceTransformRcPtr toInterchange = OCIO::ColorSpaceTransform::Create();
        toInterchange->setSrc(p.dvtSrcCs.constData());
        toInterchange->setDst(interchange);
        out.pre->appendTransform(toInterchange);

        out.post = OCIO::GroupTransform::Create();
        out.post->appendTransform(makeDvt(interchange, p));
        if (p.displayLut) out.post->appendTransform(p.displayLut);

        out.side         = scene ? InterchangeSide::Scene : InterchangeSide::Display;
        const DisplayOutput dout = displayViewOutput(cfg, p);
        out.encoding        = dout.encoding;
        out.outputPrimaries = dout.primaries;
        out.displayIsSdr    = dout.encoding == OutputEncoding::Sdr;
        return true;
    } catch (const OCIO::Exception &e) {
        return fail(QStringLiteral("OCIO build: %1").arg(e.what()));
    }
}

OcioChain OcioChainBuilder::build(OCIOConfigManager *ocio, Language language,
                                  const DisplayViewOverride *override)
{
    OcioChain out;
    if (!ocio) {
        out.errorMessage = QStringLiteral("OcioChainBuilder: null OCIOConfigManager");
        return out;
    }
    try {
        OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromFile(
            ocio->configIdentifier().toUtf8().constData());
        OCIO::GroupTransformRcPtr group =
            buildGroupTransform(ocio, cfg, &out.errorMessage, override);
        if (!group) return out;  // errorMessage already populated
        out = extractShader(cfg, group, toGpuLanguage(language), "OCIODisplay", "ocio_");
        ChainParts p;
        if (out.ok && resolveParts(ocio, cfg, override, p, nullptr)) {
            const DisplayOutput dout = displayViewOutput(cfg, p);
            out.encoding        = dout.encoding;
            out.outputPrimaries = dout.primaries;
        }
        return out;
    } catch (const OCIO::Exception &e) {
        out.errorMessage = QStringLiteral("OCIO build: %1").arg(e.what());
        return out;
    }
}

OcioSplitChain OcioChainBuilder::buildSplit(OCIOConfigManager *ocio,
                                            Language language,
                                            const DisplayViewOverride *override)
{
    OcioSplitChain out;
    if (!ocio) {
        out.errorMessage = QStringLiteral("OcioChainBuilder: null OCIOConfigManager");
        return out;
    }
    try {
        OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromFile(
            ocio->configIdentifier().toUtf8().constData());
        OcioSplitTransforms t;
        if (!buildSplitTransforms(ocio, cfg, t, &out.errorMessage, override)) {
            return out;
        }
        const OCIO::GpuLanguage lang = toGpuLanguage(language);
        out.pre  = extractShader(cfg, t.pre,  lang, "OCIOPre",  "ocio_pre_");
        out.post = extractShader(cfg, t.post, lang, "OCIOPost", "ocio_post_");
        if (!out.pre.ok || !out.post.ok) {
            out.errorMessage = !out.pre.ok ? out.pre.errorMessage : out.post.errorMessage;
            return out;
        }
        out.side            = t.side;
        out.displayIsSdr    = t.displayIsSdr;
        out.encoding        = t.encoding;
        out.outputPrimaries = t.outputPrimaries;
        out.ok              = true;
    } catch (const OCIO::Exception &e) {
        out.errorMessage = QStringLiteral("OCIO build: %1").arg(e.what());
    }
    return out;
}

} // namespace qcv
