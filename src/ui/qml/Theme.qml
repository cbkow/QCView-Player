pragma Singleton

import QtQuick

// Central palette + font-family + dimension singleton. Access from
// any QML file via:
//
//   import Qcv
//   ...
//   color: Theme.bg
//
// Visual language: the family's flat look (one cobalt-blue accent,
// minimal elevation, Phosphor icons, Geist sans + Geist Mono) with
// the humanist pass minNotes took first (2026-10-21; tried here
// 2026-10-23): three corner radii applied by surface class, a short
// hover / pressed colour ramp, popups that fade in.
QtObject {
    // --- Surfaces ---
    readonly property color bg:           "#161616"
    readonly property color bgAlt:        "#181818"
    readonly property color surface:      "#1a1a1a"
    readonly property color surfaceAlt:   "#1d1d1d"
    readonly property color surfaceHover: "#252525"
    // Recessed well — DARKER than surface/bg. Used for inset list /
    // tile bodies (e.g. the color panel reels) so they read as
    // recessed against the panel by tone alone, no border needed.
    readonly property color surfaceRecess: "#141414"
    readonly property color toolbar:      "#1f1f1f"
    readonly property color toolbarAlt:   "#262626"
    readonly property color border:       "#2a2a2a"
    readonly property color borderStrong: "#333333"
    readonly property color divider:      "#2a2a2a"
    readonly property color selection:    accentMuted
    // Currently-loaded media row fill. Distinctly brighter and
    // more saturated than `selection` so a row that's BOTH
    // selected and loaded reads unambiguously as "this is what's
    // open". Roughly midway between `selection` and `accent` in
    // saturation; lightness held back so white text keeps AAA
    // contrast.
    readonly property color rowActive:    "#0d5ba8"
    // Raised content card — inspector sections render as planes on
    // the rail's darker well (aesthetics pass 1, Resolve lean). One
    // deliberate step above `bg` so the separation survives dim
    // monitors; pairs with Theme.radius corners for the product look.
    readonly property color card:         "#1e1e1e"
    // Expanded section header fill. Sits between `bg` and
    // `surfaceHover` so an open header groups visually with its
    // content below without competing with the hover state.
    readonly property color sectionOpenBg: "#1f1f1f"
    // Collapsed section header fill. A touch LIGHTER than `bg` so a
    // closed header still reads as its own bar against the rail
    // instead of dissolving into the app background.
    readonly property color sectionClosedBg: "#1a1a1a"
    // Edge affordance buttons — the rail open buttons (top-bar
    // gutters) and the rail collapse chevrons. Idle carries a subtle
    // raised-gray fill so the button reads as interactive without a
    // hover; hover steps up to a clearer but still muted gray. Both
    // sit a notch above the `toolbar` (#1f1f1f) strip they live in.
    readonly property color affordanceIdle:  "#262626"
    readonly property color affordanceHover: "#323232"
    // A ramp's idle end is never "transparent": that is black at alpha
    // 0, so the colour animation would dip dark before it lightens.
    // This is surfaceHover at alpha 0 — the ramp only fades in.
    readonly property color hoverIdle: Qt.rgba(0x25 / 255, 0x25 / 255, 0x25 / 255, 0)

    // --- Text ---
    readonly property color textPrimary:   "#dddddd"
    readonly property color textSecondary: "#888888"
    readonly property color textMuted:     "#666666"
    readonly property color textBright:    "#ffffff"
    readonly property color textInverted:  "#111111"

    // --- Semantic / accent ---
    // Cobalt blue, locked. Predictable branding > per-user OS
    // accent on a pro review tool.
    readonly property color accent:         "#0189f1"
    readonly property color accentHover:    "#1b95f1"
    readonly property color accentMuted:    "#10395b"
    readonly property color accentSelected: accent
    readonly property color success:        "#4cb050"
    // "Not 1:1" state markers (Highlight Knee pill) — amber, distinct
    // from accent (selection) and error.
    readonly property color warning:        "#d9a441"
    readonly property color warn:           "#f5a623"
    readonly property color error:          "#c04040"
    readonly property color info:           "#9cc9ff"
    // Annotation note pins on the timeline — a violet that stays clear of
    // the green in-point, amber out-point, and blue accent already on the track.
    readonly property color noteMarker:     "#b48ef0"

    // Dual view's A / B identity — the source chips, Inspector tabs and
    // timeline signifiers.
    readonly property color sideA:          "#446a90"
    readonly property color sideB:          "#a0664a"
    readonly property color sideAMuted:     "#1e3042"   // selection fills, like accentMuted
    readonly property color sideBMuted:     "#4a2f24"
    // The side colours lifted for text (the Color panel's A / B clip-tab
    // letters), legible on raised grey and on the selection fill.
    readonly property color sideAText:      "#7fa6cc"
    readonly property color sideBText:      "#d6916c"

    // --- Fonts ---
    // Family names resolved by main.cpp at startup and exposed
    // through WindowManager. Falls back to a generic family if
    // the bundled .ttf didn't load.
    readonly property string fontFamily: WindowManager
                                         ? WindowManager.interFamily
                                         : "sans-serif"
    readonly property string monoFamily: WindowManager
                                         ? WindowManager.monoFamily
                                         : "monospace"
    readonly property string iconFamily: WindowManager
                                         ? WindowManager.phosphorFamily
                                         : "Phosphor"

    readonly property int fontSizeTiny:   10
    // Mono data values render one step below their paired sans size.
    // (Set under Inter + JetBrainsMono, whose x-heights differed; Geist
    // Sans and Geist Mono share identical vertical metrics, so this step
    // is now a density choice rather than an optical correction.)
    readonly property int fontSizeMono:   10
    readonly property int fontSizeSmall:  11
    readonly property int fontSizeBase:   12
    readonly property int fontSizeMedium: 13
    readonly property int fontSizeLarge:  14
    readonly property int fontSizeXL:     16

    // --- Icon sizes ---
    readonly property int iconSizeSmall:   12
    readonly property int iconSizeToolbar: 16
    readonly property int iconSizeMedium:  18
    readonly property int iconSizeLarge:   24

    // --- Dimensions ---
    // Inspector content rules (aesthetics pass 1):
    // every label/value and label/chips row shares one label-column
    // width so values align vertically across sections AND panels;
    // KV rows sit on a fixed-height rhythm; toggle chips are one
    // uniform height (was 18 px in InspectorPanel, 22 px in
    // ImageSequenceInspector).
    readonly property int labelColWidth:    96
    readonly property int rowHeightKV:      18
    readonly property int chipHeight:       20
    readonly property int rowHeight:        24
    readonly property int rowHeightDense:   22
    readonly property int rowHeightTall:    28
    readonly property int toolStripHeight:  32
    readonly property int headerHeight:     28
    // Track-header / transport gutter — narrow inset on the
    // left of the timeline holding edit-mode toggles. Same
    // width is used as the TransportBar's left margin so the
    // transport buttons align with the start of the track
    // area below.
    readonly property int gutterWidth:      28

    // --- Padding / spacing ---
    readonly property int paddingTight:  4
    readonly property int padding:       8
    readonly property int paddingLoose: 12

    readonly property int spacingTight:  2
    readonly property int spacing:       4
    readonly property int spacingLoose:  8

    // --- Borders / radii ---
    readonly property int borderWidth:  1
    readonly property int dividerWidth: 1
    // Three radii, by surface class (the humanist pass). Hairlines,
    // drop lines, timeline clips and track fills stay square.
    readonly property int radiusSmall:  3    // buttons, segments, chips, pills, wells, fields, menu rows, handles, tooltips
    readonly property int radius:       6    // popups, menus, dialogs, toasts, cards and their headers
    readonly property int radiusLarge: 10    // the viewport notice card; a switch's pill track

    // --- Motion ---
    readonly property int hoverMs:  100      // the hover / pressed colour ramp
    readonly property int popupMs:   80      // a popup's fade-in. No exit fade: it keeps a modal
                                             // popup alive for its length and eats the next click.
}
