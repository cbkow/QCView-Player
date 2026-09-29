// ColorPanel — Phase 2.5 (D18 redesign).
//
// Per Guide 05 §2 + D18: ComboBox dropdowns are unusable above a
// native QQuickWindow child (the player), so each column is an
// always-visible vertical scroll list ("reel"). The panel takes
// substantial vertical space — Guide 03 sets ~360 px when open.
//
// Layout:
//   ┌─────────────────────────────────────────────────────────────────┐
//   │ [Preset row: ◀ Standard ▶]   [Save] [Save as…] [Manage…]        │  preset bar
//   ├─────────────────────────────────────────────────────────────────┤
//   │ ┌Preset─┐┌Config─┐┌Input──┐┌Look───┐┌SceneLUT┐ → ┌Output┐┌View┐┌DispLUT┐│  reel grid
//   │ │ list  ││ list  ││ list  ││ list  ││ tile   │   │ list ││list││ tile  ││
//   │ │       ││       ││       ││       ││        │   │      ││    ││       ││
//   │ │       ││       ││       ││       ││        │   │      ││    ││       ││
//   │ └───────┘└───────┘└───────┘└───────┘└────────┘   └──────┘└────┘└───────┘│
//   ├─────────────────────────────────────────────────────────────────┤
//   │ [⏼ Bypass OCIO]                                  [Export LUT…]  │  bottom row
//   └─────────────────────────────────────────────────────────────────┘
//
// Phase 2.5 wiring:
//   ✓ Input / Look / Output / View — live reels
//   ✓ Scene LUT / Display LUT — file-picker tiles (native dialog,
//     stacks above the player correctly)
//   ✓ Bypass — toggle button
//   ◌ Preset reel — only "None (Passthrough)" entry until presets land
//   ◌ Config reel — current config name only until config-switching
//     lands (Phase 2.5 follow-up)
//   ◌ Save / Save as… / Manage… / Export LUT — visible-but-disabled
//     placeholders so the layout reads complete

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import Qcv

Pane {
    id: root
    padding: 0

    // Two groups, read the same in single and dual view:
    //   CLIP — the selected clip's own chain (Input, Look, Scene LUT,
    //          knee). Every edit stays with that clip; highlights take the
    //          side's colour (A's, or B's on the B tab).
    //   VIEW — Output, View, Display LUT: shared by everything; muted
    //          teal highlights.
    //   SETUP (far left) — presets and the config, the vocabulary every
    //          column uses; muted ochre. A preset splits like the panel:
    //          its clip half → the clip, its view half → View.
    readonly property bool  editingB: !!WindowManager.ocio && WindowManager.ocio.dualView
                                      && WindowManager.ocio.activeTab === 1
    readonly property color clipAccent:    editingB ? Theme.sideB : Theme.sideA
    readonly property color clipSelection: editingB ? Theme.sideBMuted : Theme.sideAMuted
    // Each group's surfaces lean toward its colour — wells 8 %, the header
    // plate 12 % — below the highlights, so they stay legible.
    function washWell(c)  { return Qt.tint(Theme.surfaceRecess, Qt.rgba(c.r, c.g, c.b, 0.08)); }
    function washPlate(c) { return Qt.tint(Theme.surface, Qt.rgba(c.r, c.g, c.b, 0.12)); }
    readonly property color clipWell:   washWell(clipAccent)
    readonly property color clipPlate:  washPlate(clipAccent)
    readonly property color setupWell:  washWell(Theme.setupAccent)
    readonly property color setupPlate: washPlate(Theme.setupAccent)
    readonly property color viewWell:   washWell(Theme.viewAccent)
    readonly property color viewPlate:  washPlate(Theme.viewAccent)
    // Hover fills lean the same way, a little stronger than the wells.
    function washHover(c) { return Qt.tint(Theme.surfaceHover, Qt.rgba(c.r, c.g, c.b, 0.16)); }
    readonly property color clipHover:  washHover(clipAccent)
    readonly property color setupHover: washHover(Theme.setupAccent)
    readonly property color viewHover:  washHover(Theme.viewAccent)
    // Icon highlights (a collapsed strip or LUT tile under the mouse):
    // the group colour, lifted so a muted side colour still reads.
    function iconHighlight(c) { return Qt.lighter(c, 1.35); }
    readonly property color viewAccent:    Theme.viewAccent
    readonly property color viewSelection: Theme.viewSelection

    background: Rectangle {
        // Surface is one shade up from Theme.bg so the inner reels
        // (which use Theme.bg) read as recessed against the panel.
        // No top divider — the VResizeHandle above is itself a 1-px
        // Theme.divider line; doubling it would render as 2 px.
        color: Theme.surface
    }

    // Persistent expand/collapse for LUT tile columns. The two LUT
    // pickers eat horizontal space but are used less often than the
    // OCIO reels; default both to collapsed so the OCIO chain gets
    // the room. The collapsed strip is still a clear affordance —
    // rotated title + identity icon + status border.
    Settings {
        id: lutTileSettings
        category: "color"
        property bool sceneLutExpanded: false
        property bool displayLutExpanded: false
        property bool lookExpanded: false        // Phase 2.5 polish: Look collapsed by default
        property bool kneeExpanded: false
    }

    // Shared LUT picker, target-routed.
    property string lutPickerTarget: ""

    FileDialog {
        id: lutPicker
        title: qsTr("Choose LUT")
        // CDL (.cc / .ccc / .cdl) only for the Scene LUT slot.
        nameFilters: root.lutPickerTarget === "scene"
            ? [qsTr("LUT / CDL files (*.cube *.3dl *.csp *.cc *.ccc *.cdl)"),
               qsTr("All files (*)")]
            : [qsTr("LUT files (*.cube *.3dl *.csp)"),
               qsTr("All files (*)")]
        fileMode: FileDialog.OpenFile
        onAccepted: {
            const path = WindowManager.urlToOsPath(selectedFile);
            if (root.lutPickerTarget === "scene") {
                WindowManager.ocio.activeSceneLutPath = path;
            } else if (root.lutPickerTarget === "display") {
                WindowManager.ocio.activeDisplayLutPath = path;
            }
        }
    }

    function pickLut(target) {
        root.lutPickerTarget = target;
        lutPicker.open();
    }

    // Save-as for "Export LUT…" — bakes the active OCIO chain into
    // a 65³ .cube via OCIOConfigManager::exportLut. Independent of
    // engaged state; only requires a configured chain.
    FileDialog {
        id: exportLutDialog
        title: qsTr("Export LUT…")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "cube"
        nameFilters: [qsTr("3D LUT (*.cube)")]
        onAccepted: {
            const path = WindowManager.urlToOsPath(selectedFile);
            const err = WindowManager.ocio.exportLut(path, 65);
            if (err && err.length > 0) {
                console.warn("Export LUT failed:", err);
                WindowManager.toast(qsTr("Export LUT failed — %1").arg(err), 2);
            } else {
                WindowManager.toast(
                    qsTr("LUT exported: %1").arg(root.basenameOf(path)), 0);
            }
        }
    }
    function basenameOf(path) {
        if (!path) return "";
        const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return idx >= 0 ? path.substring(idx + 1) : path;
    }


    // Phase 2.5e.2 — Save as… as a top-level Window, not a Popup.
    // QML Popups live inside the UI window's scene graph and end up
    // BEHIND the native player child window on macOS (same root
    // cause as Guide 05 D18 — child windows stack above QML popups).
    // A Window element is its own top-level OS window and stacks
    // correctly above the player.
    Window {
        id: saveAsDialog
        flags: Qt.Dialog
        modality: Qt.ApplicationModal
        title: qsTr("Save preset as…")
        width: 380
        height: 160
        color: Theme.bg

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                text: qsTr("Save preset as…")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
            }
            TextField {
                id: saveAsField
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name")
                onAccepted: saveAsDialog.commit()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingLoose
                Item { Layout.fillWidth: true }
                FlatButton {
                    variant: "raised"
                    text: qsTr("Cancel")
                    onClicked: saveAsDialog.close()
                }
                FlatButton {
                    text: qsTr("Save")
                    variant: "primary"
                    iconName: "floppy-disk"
                    enabled: saveAsField.text.trim().length > 0
                    onClicked: saveAsDialog.commit()
                }
            }
        }

        function open() {
            saveAsField.text = WindowManager.presets
                && WindowManager.presets.activePresetName.length > 0
                ? WindowManager.presets.activePresetName + " (copy)"
                : "";
            show();
            requestActivate();
            saveAsField.forceActiveFocus();
        }
        function commit() {
            const name = saveAsField.text.trim();
            if (!name) return;
            if (WindowManager.presets.saveAs(name)) {
                close();
                WindowManager.toast(qsTr("Preset saved: %1").arg(name), 0);
            } else {
                // Dialog stays open so the user can pick another name.
                WindowManager.toast(
                    qsTr("Couldn't save preset “%1” — try another name")
                        .arg(name), 2);
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        // Toolbars flush to the panel top/bottom edges (no gap
        // above the preset bar or below the engage row). The reel
        // grid in the middle owns its own vertical breathing via
        // Layout.topMargin / bottomMargin.
        anchors.margins: 0
        spacing: 0

        // ---- Preset bar (top) — toolbar strip (aesthetics pass 3
        // tone offset): matches the rail header treatment so the
        // panel reads as three bands — toolbar / reel field /
        // toolbar — with the recessed wells keeping their contrast
        // against the middle surface.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: Theme.toolbar

            Rectangle {
                anchors.left:   parent.left
                anchors.right:  parent.right
                anchors.bottom: parent.bottom
                height: Theme.dividerWidth
                color:  Theme.divider
            }

            RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingLoose
            anchors.rightMargin: Theme.spacingLoose
            spacing: Theme.spacingLoose

            Text {
                text: qsTr("Color")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
            }
            Text {
                text: WindowManager.ocio
                      ? WindowManager.ocio.configDescription : ""
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // Preset stepper — wired to PresetManager (Phase 2.5e.1).
            FlatButton {
                variant: "raised"
                iconName: "caret-left"
                tooltipText: qsTr("Previous preset")
                enabled: WindowManager.presets
                onClicked: WindowManager.presets.stepPreset(-1)
            }
            Rectangle {
                Layout.preferredWidth: 280
                Layout.preferredHeight: Theme.toolStripHeight
                // Read-only value = well vocabulary; Theme.surface
                // was invisible against the panel's own surface.
                color: Theme.surfaceRecess
                radius: Theme.radiusSmall
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLoose
                    anchors.rightMargin: Theme.spacingLoose
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: {
                        if (!WindowManager.presets) return qsTr("(no preset)");
                        const name = WindowManager.presets.activePresetName;
                        if (!name) return qsTr("(no preset)");
                        return WindowManager.presets.modified
                               ? name + qsTr(" — modified")
                               : name;
                    }
                    color: WindowManager.presets
                           && WindowManager.presets.activePresetName.length > 0
                           ? (WindowManager.presets.modified
                              ? Theme.warn : Theme.textPrimary)
                           : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: !WindowManager.presets
                                 || WindowManager.presets.activePresetName.length === 0
                                 || WindowManager.presets.modified
                    elide: Text.ElideMiddle
                }
            }
            FlatButton {
                variant: "raised"
                iconName: "caret-right"
                tooltipText: qsTr("Next preset")
                enabled: WindowManager.presets
                onClicked: WindowManager.presets.stepPreset(1)
            }
            Item { width: Theme.spacingLoose }
            FlatButton {
                variant: "raised"
                text: qsTr("Save")
                iconName: "floppy-disk"
                tooltipText: qsTr("Overwrite the active user preset")
                // Save overwrites the active user preset's slot tuple.
                // Built-ins are read-only; an attempt routes the user
                // to Save as instead. Save is also disabled when the
                // active preset isn't modified (no work to do).
                enabled: WindowManager.presets
                         && WindowManager.presets.activePresetName.length > 0
                         && !WindowManager.presets.activeIsBuiltIn()
                         && WindowManager.presets.modified
                onClicked: {
                    const name = WindowManager.presets.activePresetName;
                    WindowManager.presets.saveCurrent();
                    WindowManager.toast(qsTr("Preset saved: %1").arg(name), 0);
                }
            }
            FlatButton {
                variant: "raised"
                text: qsTr("Save as…")
                iconName: "floppy-disk-back"
                enabled: WindowManager.presets
                onClicked: saveAsDialog.open()
            }
            // Delete the active user preset. Built-ins (and the
            // empty/no-preset state) keep this disabled. The
            // alert-red fill is reserved for an actually-actionable
            // destructive operation — when disabled, the button
            // falls back to the default greyed-out look so a quiet
            // "no user preset selected" state isn't shouting in red.
            FlatButton {
                text: qsTr("Delete")
                iconName: "trash"
                enabled: WindowManager.presets
                         && WindowManager.presets.activePresetName.length > 0
                         && !WindowManager.presets.activeIsBuiltIn()
                variant: enabled ? "danger" : "raised"
                tooltipText: enabled
                             ? qsTr("Delete the active user preset")
                             : qsTr("Select a user preset to delete")
                onClicked: {
                    const name = WindowManager.presets.activePresetName;
                    if (WindowManager.presets.deleteCurrent()) {
                        WindowManager.toastAction(
                            qsTr("Preset deleted: %1").arg(name), 1,
                            qsTr("Undo"), "undo-preset-delete");
                    }
                }
            }
            }
        }

        // ---- Reel grid (middle, fills available height)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin:   Theme.spacingLoose
            Layout.rightMargin:  Theme.spacingLoose
            Layout.topMargin:    Theme.spacingLoose
            Layout.bottomMargin: Theme.spacingLoose
            spacing: Theme.spacingLoose

            // Input-referred group
            //
            // Preset reel is a custom column (not the generic ReelColumn)
            // because its ListView uses section.property to render
            // grouped headers (Built-in / Blender 5.2 / Blender 5.1 /
            // ACES 2.0 / ACES 1.3 / Custom). The model is QVariantList of
            // {name, section} maps from PresetManager.
            // Phase 2.5 polish: the Presets column is the entry point
            // into the chain. The body fills with the app's default
            // background but draws a 1-px border so the list reads as
            // a contained surface against the reels on its right.
            // ---- SETUP — presets and the config, one column, two tabs.
            ColumnLayout {
                id: presetColumn
                property string filterText: ""
                property int    setupTab: 0   // 0 Presets, 1 Config
                Layout.minimumWidth: 180
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                spacing: Theme.spacing

                // Same header height as the Clip / View groups so the
                // lists line up.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.preferredHeight: Theme.toolStripHeight
                    color: root.setupPlate
                    radius: Theme.radiusSmall
                RowLayout {
                    id: setupHeader
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing
                    anchors.rightMargin: Theme.spacing
                    spacing: Theme.spacing
                    Text {
                        id: setupLabel
                        text: qsTr("Setup")
                        color: Theme.setupAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    FlatButton {
                        checkable: true
                        checked: presetColumn.setupTab === 0
                        checkedFill: Theme.setupTab
                        uncheckedFill: Theme.setupFaded
                        id: presetsTab
                        text: qsTr("Presets")
                        tooltipText: qsTr("A preset sets both groups: its clip half on the selected clip, its view half on the View")
                        onClicked: presetColumn.setupTab = 0
                    }
                    FlatButton {
                        checkable: true
                        checked: presetColumn.setupTab === 1
                        checkedFill: Theme.setupTab
                        uncheckedFill: Theme.setupFaded
                        text: qsTr("Config · %1").arg(WindowManager.ocio
                                                      ? WindowManager.ocio.activeConfigName : "")
                        tooltipText: qsTr("The OCIO config — the colourspace, display and view names every column uses")
                        elideMode: Text.ElideRight
                        // From the column's width, not the header's: the
                        // header's own width follows its children.
                        Layout.preferredWidth: Math.max(0, Math.min(implicitWidth,
                            presetColumn.width - setupLabel.width - presetsTab.width
                            - 5 * setupHeader.spacing))
                        onClicked: presetColumn.setupTab = 1
                    }
                    Item { Layout.fillWidth: true }
                }
                }
                GroupRule { color: Theme.setupAccent }

                // Caption in the reels' title row, so the lists line up.
                Text {
                    text: presetColumn.setupTab === 0 ? qsTr("Sets clip + view")
                                                      : qsTr("Names every column uses")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                }

                ReelColumn {
                    visible: presetColumn.setupTab === 1
                    showTitle: false
                    Layout.fillWidth: true
                    accentColor: Theme.setupAccent
                    selectionColor: Theme.setupSelection
                    wellColor: root.setupWell
                    model: WindowManager.ocio
                           ? WindowManager.ocio.availableConfigs : []
                    currentText: WindowManager.ocio
                                 ? WindowManager.ocio.activeConfigName : ""
                    onSelected: (entry) => WindowManager.ocio.setActiveConfig(entry)
                }

                FlatTextField {
                    id: presetFilterField
                    visible: presetColumn.setupTab === 0
                    Layout.fillWidth: true
                    placeholderText: qsTr("Filter…")
                    onTextChanged: presetColumn.filterText = text
                    // Borderless recessed slot — recess tone at rest
                    // (matches the list well below), hover/focus lift
                    // it, accent bottom-rule on focus.
                    background: Rectangle {
                        color: presetFilterField.activeFocus
                               ? root.setupHover
                               : (presetFilterField.hovered
                                  ? Qt.lighter(root.setupWell, 1.25) : root.setupWell)
                        Rectangle {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: presetFilterField.activeFocus
                                   ? Theme.setupAccent : "transparent"
                        }
                    }
                }

                Rectangle {
                    visible: presetColumn.setupTab === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // Recessed well — darker than the panel so the
                    // list reads as inset by tone alone (no border).
                    color: root.setupWell
                    radius: Theme.radiusSmall
                    clip: true

                    ListView {
                        id: presetList
                        anchors.fill: parent
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: WindowManager.presets
                               ? WindowManager.presets.availablePresetEntries : []
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        section.property: "section"
                        section.criteria: ViewSection.FullString
                        section.delegate: Rectangle {
                            width: ListView.view.width
                            height: 18
                            // Match the list bg so the section header
                            // reads as a label on the same surface as
                            // the items, not a contrasting stripe.
                            color: root.setupWell
                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                verticalAlignment: Text.AlignVCenter
                                text: section
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                font.bold: true
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.5
                            }
                        }

                        delegate: Item {
                            id: presetRow
                            width: ListView.view.width
                            height: visible ? Theme.rowHeightDense : 0
                            property string entryName: modelData.name
                            // PresetManager::Preset::Kind:
                            //   0 Universal, 1 SdrSrgb, 2 HdrEdrSrgb,
                            //   3 HdrPq, 4 HdrEdrP3, 5 SdrP3.
                            property int entryKind: modelData.kind || 0
                            property bool isCurrent:
                                WindowManager.presets
                                && entryName === WindowManager.presets.activePresetName

                            readonly property bool isMacOS:
                                Qt.platform.os === "osx"
                                || Qt.platform.os === "macos"
                            readonly property bool isWindows:
                                Qt.platform.os === "windows"
                            readonly property int activeMode:
                                WindowManager ? WindowManager.hdrMode : 0
                            // Enable rule:
                            //   Universal  — always
                            //   SdrSrgb    — in SDR sRGB (mode 0), both OSes
                            //   SdrP3      — in SDR Display P3 (mode 1), both OSes
                            //   HdrEdrSrgb — cross-platform: in EDR
                            //                modes (2/3) on macOS OR
                            //                in Windows scRGB (mode 2)
                            //   HdrPq      — in HDR10 PQ mode (4),
                            //                Windows/Linux only
                            //   HdrEdrP3   — in EDR modes (2/3), macOS only
                            readonly property bool entryEnabled: {
                                if (entryKind === 0) return true;
                                if (entryKind === 1) return activeMode === 0;
                                if (entryKind === 5) return activeMode === 1;
                                if (entryKind === 2) {
                                    if (presetRow.isMacOS)
                                        return activeMode === 2 || activeMode === 3;
                                    if (presetRow.isWindows)
                                        return activeMode === 2;
                                    return false;
                                }
                                if (entryKind === 3)
                                    return activeMode === 4
                                           && !presetRow.isMacOS;
                                if (entryKind === 4)
                                    return (activeMode === 2 || activeMode === 3)
                                           && presetRow.isMacOS;
                                return true;
                            }
                            // Badge text — soft grey, right-aligned.
                            readonly property string badgeText: {
                                if (entryKind === 1 || entryKind === 5)
                                    return qsTr("SDR");
                                if (entryKind === 2 || entryKind === 3
                                    || entryKind === 4)
                                    return qsTr("HDR");
                                return "";
                            }
                            // Platform-logo decoration. HdrEdrSrgb is
                            // cross-platform so it shows BOTH logos.
                            readonly property bool showAppleIcon:
                                entryKind === 2 || entryKind === 4
                            readonly property bool showWindowsIcon:
                                entryKind === 2 || entryKind === 3

                            visible: presetColumn.filterText.length === 0
                                     || entryName.toLowerCase().indexOf(
                                          presetColumn.filterText.toLowerCase()) >= 0

                            Rectangle {
                                anchors.fill: parent
                                color: presetRow.isCurrent ? Theme.setupSelection
                                     : (presetMa.containsMouse && presetRow.entryEnabled
                                        ? root.setupHover : "transparent")
                            }
                            // Current-preset accent rule — rail-row
                            // selected vocabulary (was a green ★).
                            Rectangle {
                                visible: presetRow.isCurrent
                                anchors.left:   parent.left
                                anchors.top:    parent.top
                                anchors.bottom: parent.bottom
                                width: 2
                                color: Theme.setupAccent
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: Theme.spacing
                                opacity: presetRow.entryEnabled ? 1.0 : 0.45

                                Text {
                                    Layout.fillWidth: true
                                    text: presetRow.entryName
                                    color: presetRow.isCurrent
                                           ? Theme.textBright : Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    elide: Text.ElideRight
                                }
                                // Platform-logo decoration. Cross-
                                // platform HdrEdrSrgb presets render
                                // BOTH icons side by side.
                                Icon {
                                    visible: presetRow.showAppleIcon
                                    name: "apple-logo"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                    Layout.preferredWidth: visible
                                                           ? Theme.iconSizeSmall : 0
                                }
                                Icon {
                                    visible: presetRow.showWindowsIcon
                                    name: "windows-logo"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                    Layout.preferredWidth: visible
                                                           ? Theme.iconSizeSmall : 0
                                }
                                // SDR / HDR badge in soft grey.
                                Text {
                                    visible: presetRow.badgeText.length > 0
                                    text: presetRow.badgeText
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeTiny
                                    font.bold: true
                                    Layout.preferredWidth: visible ? 22 : 0
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                            MouseArea {
                                id: presetMa
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: presetRow.entryEnabled
                                cursorShape: presetRow.entryEnabled
                                              ? Qt.PointingHandCursor
                                              : Qt.ArrowCursor
                                onClicked: WindowManager.presets.applyPreset(entryName)
                            }
                        }
                    }
                }
            }
            // Group divider — Setup feeds the Clip chain.
            Text {
                text: "→"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 22
                Layout.alignment: Qt.AlignVCenter
            }

            // ---- CLIP — the selected clip's own chain.
            ColumnLayout {
                id: clipGroup
                readonly property var ocio: WindowManager.ocio
                readonly property var project: WindowManager.project
                readonly property bool dual: !!ocio && ocio.dualView
                readonly property string nameA: ocio && project && ocio.clipIdA.length > 0
                                                ? (project.mediaItemMap(ocio.clipIdA).name || "") : ""
                readonly property string nameB: dual && project && ocio.clipIdB.length > 0
                                                ? (project.mediaItemMap(ocio.clipIdB).name || "") : ""
                Layout.fillHeight: true
                spacing: Theme.spacing

                Rectangle {
                    // Takes the width the columns below give the group,
                    // never widens it (a long clip name would).
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.preferredHeight: Theme.toolStripHeight
                    color: root.clipPlate
                    radius: Theme.radiusSmall
                RowLayout {
                    id: clipHeader
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing
                    anchors.rightMargin: Theme.spacing
                    spacing: Theme.spacing
                    // Room for the side tabs; a short name keeps its full
                    // width and the other tab gets the rest.
                    readonly property real tabRoom: Math.max(0, width - clipLabel.width
                        - (tabB.visible ? spacing : 0) - spacing)
                    function tabWidth(own, other) {
                        if (!tabB.visible) return Math.min(own, tabRoom);
                        const half = tabRoom / 2;
                        if (own <= half) return own;
                        return Math.min(own, Math.max(half, tabRoom - other));
                    }

                    Text {
                        id: clipLabel
                        text: qsTr("Clip")
                        color: root.clipAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    FlatButton {
                        checkable: true
                        checked: !clipGroup.ocio || clipGroup.ocio.activeTab === 0
                                 || !clipGroup.dual
                        checkedFill: Theme.sideA
                        uncheckedFill: Theme.sideAFaded
                        text: qsTr("A · %1").arg(clipGroup.nameA || qsTr("no clip"))
                        tooltipText: clipGroup.dual
                                     ? qsTr("%1\nA's clip — changes stay with this clip")
                                           .arg(clipGroup.nameA || qsTr("no clip"))
                                     : qsTr("%1\nChanges here stay with this clip")
                                           .arg(clipGroup.nameA || qsTr("no clip"))
                        id: tabA
                        elideMode: Text.ElideMiddle
                        Layout.preferredWidth: clipHeader.tabWidth(implicitWidth, tabB.implicitWidth)
                        onClicked: if (clipGroup.ocio) clipGroup.ocio.activeTab = 0
                    }
                    FlatButton {
                        visible: clipGroup.dual
                        checkable: true
                        checked: !!clipGroup.ocio && clipGroup.ocio.activeTab === 1
                        checkedFill: Theme.sideB
                        uncheckedFill: Theme.sideBFaded
                        text: qsTr("B · %1").arg(clipGroup.nameB || qsTr("no clip"))
                        tooltipText: qsTr("%1\nB's clip — changes stay with this clip")
                                         .arg(clipGroup.nameB || qsTr("no clip"))
                        id: tabB
                        elideMode: Text.ElideMiddle
                        Layout.preferredWidth: clipHeader.tabWidth(implicitWidth, tabA.implicitWidth)
                        onClicked: clipGroup.ocio.activeTab = 1
                    }
                    Item { Layout.fillWidth: true }
                }
                }
                GroupRule { color: root.clipAccent }

                RowLayout {
                    Layout.fillHeight: true
                    spacing: Theme.spacingLoose

                    ReelColumn {
                        title: qsTr("Input")
                        model: WindowManager.ocio ? WindowManager.ocio.colorspaces : []
                        currentText: WindowManager.ocio
                                     ? WindowManager.ocio.activeInput : ""
                        pinSlot: "input"
                        pinned: !!WindowManager.ocio && WindowManager.ocio.inputPinned
                        onSelected: (entry) => WindowManager.ocio.activeInput = entry
                    }
                    ReelColumn {
                        title: qsTr("Look")
                        expandable: true
                        expanded: lutTileSettings.lookExpanded
                        onExpandedChanged: lutTileSettings.lookExpanded = expanded
                        collapsedIconName: "magic-wand"
                        model: WindowManager.ocio
                               ? [qsTr("(none)")].concat(WindowManager.ocio.looks)
                               : [qsTr("(none)")]
                        currentText: WindowManager.ocio
                                     && WindowManager.ocio.activeLook.length > 0
                                     ? WindowManager.ocio.activeLook
                                     : qsTr("(none)")
                        pinSlot: "look"
                        pinned: !!WindowManager.ocio && WindowManager.ocio.lookPinned
                        onSelected: (entry) => WindowManager.ocio.activeLook =
                            (entry === qsTr("(none)") ? "" : entry)
                    }

                    // Scene LUT — picker tile, full column height
                    LutTileColumn {
                        title: qsTr("Scene LUT")
                        iconName: "cube"
                        path: WindowManager.ocio
                              ? WindowManager.ocio.activeSceneLutPath : ""
                        expanded: lutTileSettings.sceneLutExpanded
                        onExpandedChanged: lutTileSettings.sceneLutExpanded = expanded
                        onPickRequested: root.pickLut("scene")
                        onClearRequested: WindowManager.ocio.activeSceneLutPath = ""
                        // CDL collections: pick the correction (empty = first).
                        showCccId: /\.(ccc|cdl)$/i.test(path)
                        cccId: WindowManager.ocio ? WindowManager.ocio.activeSceneLutCccId : ""
                        onCccIdEdited: (id) => WindowManager.ocio.activeSceneLutCccId = id
                        pinSlot: "sceneLut"
                        pinned: !!WindowManager.ocio && WindowManager.ocio.sceneLutPinned
                    }

                    // Highlight Knee — the chain step between the scene side and
                    // the Display/View (see color/linear_stage.h).
                    KneeColumn {
                        expanded: lutTileSettings.kneeExpanded
                        onExpandedChanged: lutTileSettings.kneeExpanded = expanded
                    }
                }
            }

            // Group divider
            Text {
                text: "→"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 22
                Layout.alignment: Qt.AlignVCenter
            }

            // ---- VIEW — shared by everything on screen.
            ColumnLayout {
                Layout.fillHeight: true
                spacing: Theme.spacing

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.preferredHeight: Theme.toolStripHeight
                    color: root.viewPlate
                    radius: Theme.radiusSmall
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing
                    anchors.rightMargin: Theme.spacing
                    spacing: Theme.spacing
                    Text {
                        text: qsTr("View")
                        color: Theme.viewAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    Text {
                        text: clipGroup.dual ? qsTr("both sides") : qsTr("everything")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    Item { Layout.fillWidth: true }
                }
                }
                GroupRule { color: Theme.viewAccent }

                RowLayout {
                    Layout.fillHeight: true
                    spacing: Theme.spacingLoose

                    ReelColumn {
                        title: qsTr("Output")
                        model: WindowManager.ocio ? WindowManager.ocio.displays : []
                        currentText: WindowManager.ocio
                                     ? WindowManager.ocio.activeDisplay : ""
                        onSelected: (entry) => WindowManager.ocio.activeDisplay = entry
                    }
                    ReelColumn {
                        title: qsTr("View")
                        // Recompute when display changes — bind to activeDisplay
                        // so the view list refreshes after Output selection.
                        model: WindowManager.ocio
                            ? WindowManager.ocio.viewsForDisplay(
                                WindowManager.ocio.activeDisplay) : []
                        currentText: WindowManager.ocio
                                     ? WindowManager.ocio.activeView : ""
                        onSelected: (entry) => WindowManager.ocio.activeView = entry
                    }

                    // Display LUT — picker tile, full column height
                    LutTileColumn {
                        title: qsTr("Display LUT")
                        iconName: "monitor"
                        path: WindowManager.ocio
                              ? WindowManager.ocio.activeDisplayLutPath : ""
                        expanded: lutTileSettings.displayLutExpanded
                        onExpandedChanged: lutTileSettings.displayLutExpanded = expanded
                        onPickRequested: root.pickLut("display")
                        onClearRequested: WindowManager.ocio.activeDisplayLutPath = ""
                    }
                }
            }
        }

        // ---- Bottom row: Engage | Export LUT
        // Engage is the explicit "apply this chain" action. Default
        // off, so the app boots in passthrough. Once engaged, slot
        // changes update live (the chain rebuild is cheap — typically
        // < a few ms). Disengage to return to raw without losing
        // slot selections. See Guide 05 D9 (and the engage-vs-bypass
        // pivot in Phase 2.5b).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: Theme.toolbar

            Rectangle {
                anchors.left:  parent.left
                anchors.right: parent.right
                anchors.top:   parent.top
                height: Theme.dividerWidth
                color: Theme.divider
            }

            RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingLoose
            anchors.rightMargin: Theme.spacingLoose
            spacing: Theme.spacingLoose

            RowLayout {
                spacing: Theme.spacingLoose
                FlatSwitch {
                    id: engageSwitch
                    tint: Theme.viewAccent
                    // Attention styling on both states — blue "engage
                    // me" while off, green "engaged" when on. Was
                    // `!checked` previously which dropped attention
                    // mode the moment the user toggled on, missing
                    // the green confirmation.
                    attention: true
                    checked: WindowManager.ocio
                             ? WindowManager.ocio.engaged : false
                    onToggled: WindowManager.ocio.engaged = checked
                }
                Item { Layout.preferredWidth: Theme.padding }
                Text {
                    text: engageSwitch.checked ? qsTr("OCIO On") : qsTr("OCIO Off")
                    color: engageSwitch.checked ? Theme.success : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: engageSwitch.checked
                }
            }

            Item { Layout.fillWidth: true }

            // Viewer aids (color/linear_stage.h) — inspection only: they
            // need the OCIO chain, are never saved in presets or baked into
            // exports, and are captured (and tagged) in screenshots / note
            // thumbnails. Exposure is the linear stage's gain before the
            // View; Gamma and Channel apply after the whole chain.
            RowLayout {
                id: viewerAids
                spacing: Theme.spacing
                enabled: WindowManager.ocio ? WindowManager.ocio.engaged : false
                opacity: enabled ? 1.0 : 0.45

                Text {
                    text: qsTr("Exposure")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                FlatSlider {
                    id: exposureSlider
                    tint: Theme.viewAccent
                    from: -4.0
                    to: 4.0
                    stepSize: 0.1
                    value: WindowManager ? WindowManager.exposure : 0.0
                    implicitWidth: 110
                    onMoved: WindowManager.exposure = value
                    // Double-click resets to 0 stops.
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        propagateComposedEvents: true
                        onPressed: (mouse) => { mouse.accepted = false }
                        onDoubleClicked: WindowManager.exposure = 0.0
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 48
                    Layout.preferredHeight: 16
                    radius: Theme.radiusSmall
                    color: Theme.surfaceRecess
                    Text {
                        anchors.centerIn: parent
                        text: {
                            const ev = WindowManager ? WindowManager.exposure : 0.0;
                            return (ev > 0.005 ? "+" : (ev < -0.005 ? "−" : ""))
                                   + Math.abs(ev).toFixed(1) + " st";
                        }
                        color: Theme.textPrimary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                    }
                }

                Item { Layout.preferredWidth: Theme.spacing }

                Text {
                    text: qsTr("Gamma")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                FlatSlider {
                    tint: Theme.viewAccent
                    from: 0.25
                    to: 4.0
                    stepSize: 0.05
                    value: WindowManager ? WindowManager.viewerGamma : 1.0
                    implicitWidth: 90
                    onMoved: WindowManager.viewerGamma = value
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        propagateComposedEvents: true
                        onPressed: (mouse) => { mouse.accepted = false }
                        onDoubleClicked: WindowManager.viewerGamma = 1.0
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 16
                    radius: Theme.radiusSmall
                    color: root.viewWell
                    Text {
                        anchors.centerIn: parent
                        text: (WindowManager ? WindowManager.viewerGamma : 1.0).toFixed(2)
                        color: Theme.textPrimary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                    }
                }

                Item { Layout.preferredWidth: Theme.spacing }

                // Channel view — RGB / R / G / B / A / luma, shown as grey.
                Repeater {
                    model: [qsTr("RGB"), "R", "G", "B", "A", "Y"]
                    FlatChip {
                        required property int index
                        required property string modelData
                        tint: Theme.viewAccent
                        label: modelData
                        active: WindowManager && WindowManager.channelView === index
                        minWidth: index === 0 ? 30 : 20
                        tooltip: [qsTr("All channels"), qsTr("Red"), qsTr("Green"),
                                  qsTr("Blue"), qsTr("Alpha matte"),
                                  qsTr("Luma (output primaries)")][index]
                        onClicked: WindowManager.channelView = index
                    }
                }

                // Reset all viewer aids — only while any is active.
                FlatButton {
                    iconName: "arrow-counter-clockwise"
                    tooltipText: qsTr("Reset exposure, gamma and channel")
                    enabled: WindowManager && WindowManager.viewerAdjusted
                    opacity: enabled ? 1 : 0
                    onClicked: WindowManager.resetViewerAids()
                }
            }

            // Phase 2.6.1: HDR mode picker. Lives in the panel's
            // bottom row temporarily; Guide 06 D3 puts the final
            // chip in the menu bar. The dropdown items map to
            // WindowManager.HdrMode enum values.
            Text {
                text: qsTr("Display")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }
            FlatComboBox {
                id: hdrModeCombo
                tint: Theme.viewAccent
                implicitHeight: 26
                // Pin a width that fits the longest entry so the
                // shorter "SDR" selection doesn't shrink the box and
                // hide the menu's other items behind a narrow header.
                implicitWidth: 200

                // Per Guide 06 D2 + Phase F.2.9: macOS gets the EDR
                // variants via CAMetalLayer extendedDynamicRange;
                // Windows D3D11 exposes scRGB-linear AND HDR10 PQ —
                // both are first-class HDR options. PQ uses an
                // R10G10B10A2_UNORM swapchain tagged G2084_NONE_P2020
                // (BT.2020 + ST.2084) with HDR10 mastering metadata;
                // scRGB uses R16G16B16A16_FLOAT + G10_NONE_P709 and
                // leans on the OS compositor to tone-map. Linux falls
                // through to PQ on Vulkan.
                //
                // Show ALL modes always; flag platform-unavailable
                // ones as `supported: false` so they grey out
                // instead of vanishing.
                readonly property bool isMacOS:
                    Qt.platform.os === "osx" || Qt.platform.os === "macos"
                readonly property bool isWindows:
                    Qt.platform.os === "windows"
                readonly property var entries: [
                    { name: qsTr("SDR — sRGB"),           value: 0, supported: true },
                    { name: qsTr("SDR — Display P3"),     value: 1, supported: true },
                    { name: qsTr("EDR — Linear sRGB"),    value: 2, supported: isMacOS },
                    { name: qsTr("EDR — Linear P3"),      value: 3, supported: isMacOS },
                    { name: qsTr("HDR10 PQ"),             value: 4, supported: !isMacOS },
                    { name: qsTr("Extended Linear sRGB"), value: 2, supported: !isMacOS },
                ]
                textRole: "name"
                valueRole: "value"
                model: entries
                currentIndex: {
                    // Tie-break on `supported` so value-2 picks the
                    // correctly-labeled row for the active platform.
                    for (let i = 0; i < entries.length; ++i) {
                        if (entries[i].value === WindowManager.hdrMode
                            && entries[i].supported) return i;
                    }
                    for (let i = 0; i < entries.length; ++i) {
                        if (entries[i].value === WindowManager.hdrMode) return i;
                    }
                    return 0;
                }
                onActivated: (index) => {
                    if (!entries[index].supported) return;
                    WindowManager.hdrMode = entries[index].value;
                }

                // Custom delegate so unsupported rows render greyed
                // out and reject clicks — defaults from
                // FlatComboBox use string `modelData` and don't know
                // about per-item enabled.
                delegate: ItemDelegate {
                    width: hdrModeCombo.width
                    height: 24
                    enabled: modelData.supported
                    contentItem: Text {
                        text: modelData.name
                        color: modelData.supported
                               ? Theme.textPrimary : Theme.textMuted
                        font: hdrModeCombo.font
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: Theme.spacing
                    }
                    background: Rectangle {
                        color: (parent.hovered || parent.highlighted)
                               && modelData.supported
                               ? root.viewHover : "transparent"
                    }
                }
            }

            FlatButton {
                variant: "raised"
                text: qsTr("Export LUT…")
                iconName: "export"
                // Bakes the configured OCIO chain to a .cube — works
                // whether or not the chain is engaged (engage is for
                // live viewport apply; export is independent).
                enabled: WindowManager.ocio
                         && WindowManager.ocio.activeInput.length > 0
                         && WindowManager.ocio.activeDisplay.length > 0
                         && WindowManager.ocio.activeView.length > 0
                tooltipText: enabled
                             ? qsTr("Bake the active OCIO chain to a 65³ .cube LUT")
                             : qsTr("Configure input / display / view first")
                onClicked: exportLutDialog.open()
            }
            }
        }
    }

    // ---- Reusable: a vertical scroll-list reel.
    // Always-visible options list with current-selection star and
    // optional filter field. Click a row to select.
    //
    // Phase 2.5 polish: optional collapsible mode (mirrors LutTileColumn).
    // Set expandable=true and the parent owns `expanded`. Collapsed
    // state renders a slim strip with rotated title — saves horizontal
    // space for reels that aren't typically active (e.g. Look).
    // A clip column the selected clip has set: ↺ returns it to the default
    // (what an untouched clip shows). Hidden while the column shows the
    // default.
    component RevertButton: Item {
        id: revert
        property string slot: ""
        implicitWidth: 18
        implicitHeight: 18

        Icon {
            anchors.centerIn: parent
            name: "arrow-counter-clockwise"
            size: Theme.iconSizeSmall
            color: revertMa.containsMouse ? Theme.textBright : root.clipAccent
        }
        MouseArea {
            id: revertMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: WindowManager.ocio.setSlotPinned(revert.slot, false)
            FlatToolTip {
                visible: revertMa.containsMouse
                text: qsTr("Set on this clip — click to go back to the default")
            }
        }
    }

    // Collapsed-strip mark for a clip column the clip has set.
    component ClipSetDot: Rectangle {
        Layout.alignment: Qt.AlignHCenter
        width: 6
        height: 6
        radius: 3
        color: root.clipAccent
    }

    // Header over each group: its name, then (clip group) the side tabs.
    component GroupRule: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 2
        radius: 1
    }

    component ReelColumn: ColumnLayout {
        id: reel
        property string title: ""
        property var    model: []
        property string currentText: ""
        property bool   showFilter: true
        property string filterText: ""
        property bool   expandable: false
        property bool   expanded: true
        property string collapsedIconName: "list-bullets"
        // False when the column sits under a header that names it.
        property bool   showTitle: true
        // Clip column: its slot name ("" = a View column), and whether
        // the selected clip has set it.
        property string pinSlot: ""
        property bool   pinned: false
        // Clip columns highlight in the side's colour, View columns grey.
        property color  accentColor:    pinSlot.length > 0 ? root.clipAccent : root.viewAccent
        property color  selectionColor: pinSlot.length > 0 ? root.clipSelection : root.viewSelection
        property color  wellColor:      pinSlot.length > 0 ? root.clipWell : root.viewWell
        property color  hoverColor:     pinSlot.length > 0 ? root.clipHover : root.viewHover
        signal selected(string entry)

        Layout.minimumWidth: (expandable && !expanded) ? 32 : 130
        Layout.preferredWidth: (expandable && !expanded) ? 32 : 160
        Layout.fillHeight: true
        spacing: Theme.spacing

        // Title slot — always reserves its height (matches LutTileColumn)
        // so collapsed and expanded columns line up at the same body Y.
        // The inner Text + chevron are what toggle on collapse.
        Item {
            visible: reel.showTitle
            Layout.fillWidth: true
            Layout.preferredHeight: titleProbe.implicitHeight

            Text {
                id: titleProbe
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: !reel.expandable || reel.expanded
                text: reel.title
                // Shared card-title voice (aesthetics pass 3).
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
            }
            RevertButton {
                anchors.left: titleProbe.right
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: reel.pinned && titleProbe.visible
                slot: reel.pinSlot
            }
            Icon {
                visible: reel.expandable && reel.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                name: "caret-double-left"
                size: Theme.iconSizeSmall
                color: reelCollapseMa.containsMouse
                       ? Theme.textPrimary : Theme.textSecondary
            }
            MouseArea {
                id: reelCollapseMa
                visible: reel.expandable && reel.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: reel.expanded = false
                FlatToolTip {
                    visible: reelCollapseMa.containsMouse
                    text: qsTr("Collapse")
                }
            }
        }

        // Optional filter (shown when list could be long)
        FlatTextField {
            id: reelFilterField
            Layout.fillWidth: true
            visible: reel.showFilter && (!reel.expandable || reel.expanded)
            placeholderText: qsTr("Filter…")
            onTextChanged: reel.filterText = text
            // Recessed fill matching the column's list well below;
            // borderless, hover/focus lift, accent bottom-rule on focus.
            background: Rectangle {
                color: reelFilterField.activeFocus
                       ? reel.hoverColor
                       : (reelFilterField.hovered
                          ? Qt.lighter(reel.wellColor, 1.25) : reel.wellColor)
                Rectangle {
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: reelFilterField.activeFocus
                           ? reel.accentColor : "transparent"
                }
            }
        }

        // Expanded list body.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !reel.expandable || reel.expanded
            color: reel.enabled ? reel.wellColor : Qt.darker(reel.wellColor, 1.15)
            radius: Theme.radiusSmall
            clip: true
            opacity: reel.enabled ? 1.0 : 0.55

            ListView {
                id: list
                anchors.fill: parent
                model: reel.model
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                // Filter is a simple substring match on the original
                // model. If filtering causes complexity later (grouped
                // entries / categories), promote model to a real
                // QSortFilterProxyModel.
                delegate: Item {
                    width: ListView.view.width
                    height: visible ? Theme.rowHeightDense : 0
                    visible: reel.filterText.length === 0
                             || modelData.toLowerCase().indexOf(
                                  reel.filterText.toLowerCase()) >= 0

                    Rectangle {
                        anchors.fill: parent
                        color: modelData === reel.currentText ? reel.selectionColor
                             : (mouseArea.containsMouse ? reel.hoverColor : "transparent")
                    }
                    // Current-entry accent rule — same selected
                    // vocabulary as the rail rows (the old green ★
                    // glyph was a one-off).
                    Rectangle {
                        visible: modelData === reel.currentText
                        anchors.left:   parent.left
                        anchors.top:    parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: reel.accentColor
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: Theme.spacing
                        Text {
                            Layout.fillWidth: true
                            text: modelData
                            color: modelData === reel.currentText
                                   ? Theme.textBright : Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: reel.enabled
                        onClicked: reel.selected(modelData)
                    }
                }
            }
        }

        // Collapsed strip — same shape as LutTileColumn's collapsed body.
        Rectangle {
            id: reelCollapsedStrip
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: reel.expandable && !reel.expanded
            color: reelExpandMa.containsMouse ? reel.hoverColor : reel.wellColor
            border.width: 0
            radius: Theme.radiusSmall

            MouseArea {
                id: reelExpandMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: reel.expanded = true
                FlatToolTip {
                    visible: reelExpandMa.containsMouse
                    text: qsTr("%1 — click to expand").arg(reel.title)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingLoose
                anchors.bottomMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        text: reel.title
                        // Always-soft grey for the rotated label — the
                        // collapsed strip's job is "here's a column you
                        // can expand," not a loaded-state signal.
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                }

                ClipSetDot { visible: reel.pinned }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: reel.collapsedIconName
                    size: 20
                    // Grey icon (no green/success cue) — keeps the
                    // collapsed strip neutral; the group colour on hover.
                    color: reelExpandMa.containsMouse
                           ? root.iconHighlight(reel.accentColor) : Theme.textMuted
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "caret-double-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textMuted
                }
            }
        }
    }

    // ---- Reusable: a LUT picker tile shaped like a reel column.
    //
    // Two states:
    //   - Expanded: full ~160-px column with a big centered CTA
    //     tile (cube/monitor identity icon, "+ Add LUT" / "Replace
    //     LUT" title, basename subtitle, corner ✕ to clear).
    //   - Collapsed (default): slim ~32-px strip with a rotated
    //     vertical title, the identity icon, and a caret-right
    //     hint. Border / icon color still signal whether a LUT is
    //     loaded (Theme.success when present).
    //
    // Clicking the section header chevron toggles the state. In
    // collapsed state, clicking anywhere on the strip expands.
    // Highlight Knee chain step — a BT.2390-style shoulder in PQ that
    // compresses [knee start, source peak] onto [knee start, target
    // peak] before the Display/View. Deliberate: off by default, every
    // value set by hand ("Use file MaxCLL" reads the file only when
    // clicked). Saved in presets and baked into LUT export.
    component KneeColumn: ColumnLayout {
        id: knee
        property bool expanded: false
        readonly property var ocio: WindowManager.ocio
        readonly property bool on: !!ocio && ocio.kneeEnabled
        readonly property bool available: !!ocio && ocio.kneeAvailable
        // The clip whose chain the panel shows (dual view: the A/B tab).
        readonly property var video: {
            const p = WindowManager.project;
            if (!p) return null;
            const id = ocio ? ocio.focusClipId : "";
            const item = id.length > 0 ? p.mediaItemMap(id) : p.activeItem;
            return item ? item.video : null;
        }
        readonly property int fileMaxCll: video ? (video.maxCll || 0) : 0
        readonly property real fileMastering: video ? (video.masteringMaxNits || 0) : 0

        Layout.minimumWidth:   knee.expanded ? 180 : 32
        Layout.preferredWidth: knee.expanded ? 200 : 32
        Layout.fillHeight: true
        spacing: Theme.spacing

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: kneeTitle.implicitHeight
            Text {
                id: kneeTitle
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: knee.expanded
                text: qsTr("Highlight Knee")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
            }
            RevertButton {
                anchors.left: kneeTitle.right
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: knee.expanded && !!knee.ocio && knee.ocio.kneePinned
                slot: "knee"
            }
            Icon {
                visible: knee.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                name: "caret-double-left"
                size: Theme.iconSizeSmall
                color: kneeCollapseMa.containsMouse ? Theme.textPrimary : Theme.textSecondary
            }
            MouseArea {
                id: kneeCollapseMa
                visible: knee.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: knee.expanded = false
                FlatToolTip { visible: kneeCollapseMa.containsMouse; text: qsTr("Collapse") }
            }
        }

        // ---- Expanded body
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: knee.expanded
            color: root.clipWell
            radius: Theme.radiusSmall

            Flickable {
                anchors.fill: parent
                anchors.margins: Theme.spacingLoose
                contentHeight: kneeBody.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: kneeBody
                    width: parent.width
                    spacing: Theme.spacing
                    enabled: knee.available

                    RowLayout {
                        spacing: Theme.spacing
                        FlatSwitch {
                            tint: root.clipAccent
                            checked: knee.on
                            onToggled: knee.ocio.kneeEnabled = checked
                        }
                        Text {
                            text: knee.on ? qsTr("On") : qsTr("Off")
                            color: knee.on ? Theme.warning : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            font.bold: knee.on
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        visible: !knee.available
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Not available for this chain (data view, or the "
                                   + "config has no interchange role).")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.italic: true
                    }

                    // Source peak
                    Text {
                        text: qsTr("Source peak")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    RowLayout {
                        spacing: Theme.spacing
                        FlatSpinBox {
                            tint: root.clipAccent
                            id: srcSpin
                            Layout.fillWidth: true
                            from: 100
                            to: 10000
                            stepSize: 100
                            editable: true
                            value: knee.ocio ? Math.round(knee.ocio.kneeSourceNits) : 1000
                            onValueModified: knee.ocio.kneeSourceNits = value
                        }
                        Text {
                            text: qsTr("nits")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }
                    Text {
                        text: knee.ocio
                              ? qsTr("+%1 stops over SDR white")
                                    .arg((Math.log(knee.ocio.kneeSourceNits / 100)
                                          / Math.LN2).toFixed(1))
                              : ""
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                    }
                    FlatButton {
                        Layout.fillWidth: true
                        variant: "subtle"
                        text: knee.fileMaxCll > 0
                              ? qsTr("Use file MaxCLL (%1)").arg(knee.fileMaxCll)
                              : (knee.fileMastering > 0
                                 ? qsTr("Use mastering peak (%1)").arg(Math.round(knee.fileMastering))
                                 : qsTr("No MaxCLL in file"))
                        enabled: knee.fileMaxCll > 0 || knee.fileMastering > 0
                        tooltipText: qsTr("Set the source peak from the file's HDR10 metadata")
                        onClicked: knee.ocio.kneeSourceNits =
                            knee.fileMaxCll > 0 ? knee.fileMaxCll : knee.fileMastering
                    }

                    // Target peak
                    Text {
                        text: qsTr("Target peak")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    Text {
                        visible: knee.ocio && knee.ocio.displayIsSdr
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("100 nits · SDR display")
                        color: Theme.textPrimary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                    }
                    RowLayout {
                        visible: knee.ocio && !knee.ocio.displayIsSdr
                        spacing: Theme.spacing
                        FlatSpinBox {
                            tint: root.clipAccent
                            Layout.fillWidth: true
                            from: 100
                            to: 10000
                            stepSize: 50
                            editable: true
                            value: knee.ocio ? Math.round(knee.ocio.kneeTargetNits) : 1000
                            onValueModified: knee.ocio.kneeTargetNits = value
                        }
                        Text {
                            text: qsTr("nits")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }

                    // Knee start
                    Text {
                        text: qsTr("Knee start")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    FlatSlider {
                        tint: root.clipAccent
                        Layout.fillWidth: true
                        from: 0.0
                        to: 0.99
                        stepSize: 0.01
                        value: knee.ocio ? knee.ocio.kneeStartEffective : 0.5
                        onMoved: knee.ocio.kneeStart = value
                        // Double-click returns to BT.2390's default.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            propagateComposedEvents: true
                            onPressed: (mouse) => { mouse.accepted = false }
                            onDoubleClicked: knee.ocio.kneeStart = -1
                        }
                    }
                    RowLayout {
                        spacing: Theme.spacing
                        Text {
                            text: knee.ocio
                                  ? qsTr("%1 nits").arg(Math.round(knee.ocio.kneeStartNits))
                                  : ""
                            color: Theme.textPrimary
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeMono
                        }
                        Text {
                            visible: knee.ocio && knee.ocio.kneeStart < 0
                            text: qsTr("BT.2390")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                        Item { Layout.fillWidth: true }
                        FlatButton {
                            iconName: "arrow-counter-clockwise"
                            tooltipText: qsTr("Reset knee start to BT.2390")
                            enabled: knee.ocio && knee.ocio.kneeStart >= 0
                            opacity: enabled ? 1 : 0
                            onClicked: knee.ocio.kneeStart = -1
                        }
                    }
                }
            }
        }

        // ---- Collapsed body — slim vertical strip (matches LutTileColumn)
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !knee.expanded
            color: kneeStripMa.containsMouse ? root.clipHover : root.clipWell
            radius: Theme.radiusSmall

            MouseArea {
                id: kneeStripMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: knee.expanded = true
                FlatToolTip {
                    visible: kneeStripMa.containsMouse
                    text: knee.on
                          ? qsTr("Highlight Knee — on, %1 → %2 nits")
                                .arg(Math.round(knee.ocio.kneeSourceNits))
                                .arg(knee.ocio.displayIsSdr
                                     ? 100 : Math.round(knee.ocio.kneeTargetNits))
                          : qsTr("Highlight Knee — off, click to expand")
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingLoose
                anchors.bottomMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        text: qsTr("Highlight Knee")
                        color: knee.on ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                }
                ClipSetDot { visible: !!knee.ocio && knee.ocio.kneePinned }
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "sun-horizon"
                    size: 20
                    color: knee.on ? Theme.warning
                                   : (kneeStripMa.containsMouse ? root.iconHighlight(root.clipAccent)
                                                                : Theme.textMuted)
                }
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "caret-double-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textMuted
                }
            }
        }
    }

    component LutTileColumn: ColumnLayout {
        id: tile
        property string title: ""
        property string iconName: "cube"
        property string path: ""
        property bool   expanded: false
        property bool   showCccId: false
        property string cccId: ""
        // Clip column: its slot name ("" = the View's Display LUT), and
        // whether the selected clip has set it.
        property string pinSlot: ""
        property bool   pinned: false
        property color  wellColor: pinSlot.length > 0 ? root.clipWell : root.viewWell
        property color  hoverColor: pinSlot.length > 0 ? root.clipHover : root.viewHover
        property color  accentColor: pinSlot.length > 0 ? root.clipAccent : root.viewAccent
        signal pickRequested()
        signal clearRequested()
        signal cccIdEdited(string id)

        Layout.minimumWidth:   tile.expanded ? 130 : 32
        Layout.preferredWidth: tile.expanded ? 160 : 32
        Layout.fillHeight: true
        spacing: Theme.spacing

        // Title slot — same line height as ReelColumn's title text,
        // so list bodies / tile bodies / collapsed strips all start
        // at the same Y. Section header (title + collapse chevron)
        // is rendered inside when expanded; collapsed leaves this
        // bar empty.
        Item {
            id: titleSlot
            Layout.fillWidth: true
            Layout.preferredHeight: titleProbe.implicitHeight

            Text {
                id: titleProbe
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: tile.expanded
                text: tile.title
                // Shared card-title voice (aesthetics pass 3).
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
            }
            RevertButton {
                anchors.left: titleProbe.right
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: tile.pinned && tile.expanded
                slot: tile.pinSlot
            }
            Icon {
                visible: tile.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // Matches the Look reel's collapse direction — the
                // column shrinks horizontally into a left-side strip,
                // so left-pointing carets read as "collapse" and
                // right-pointing as "expand" on the collapsed body.
                name: "caret-double-left"
                size: Theme.iconSizeSmall
                color: collapseMa.containsMouse
                       ? Theme.textPrimary : Theme.textSecondary
            }
            MouseArea {
                id: collapseMa
                visible: tile.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: tile.expanded = false
                FlatToolTip {
                    visible: collapseMa.containsMouse
                    text: qsTr("Collapse")
                }
            }
        }

        // ---- Expanded body — big CTA tile.
        // No filter slot here — the LUT body (big tile / collapsed
        // strip) extends up into the filter zone for extra room.
        // The title slot above remains the only reserved header
        // area, and stays empty in collapsed mode.
        Rectangle {
            id: tileBg
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: tile.expanded
            // Borderless recessed well. Hover lifts the fill; loaded-
            // state cue lives on the identity icon (Theme.success).
            color: tileMa.containsMouse ? tile.hoverColor : tile.wellColor
            border.width: 0
            radius: Theme.radiusSmall

            // Body click target. Declared first so the corner ✕
            // (declared later) wins when the click lands on it.
            MouseArea {
                id: tileMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.pickRequested()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingLoose
                spacing: Theme.spacing

                Item { Layout.fillHeight: true }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: tile.iconName
                    size: 36
                    color: tile.path
                           ? Theme.success
                           : (tileMa.containsMouse
                              ? root.iconHighlight(tile.accentColor) : Theme.textMuted)
                }

                Item { Layout.preferredHeight: Theme.spacing }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    text: tile.path
                          ? qsTr("Replace LUT")
                          : qsTr("+ Add LUT")
                    color: tileMa.containsMouse
                           ? Theme.textBright : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.padding
                    Layout.rightMargin: Theme.padding
                    visible: tile.path.length > 0
                    text: root.basenameOf(tile.path)
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                }
                FlatTextField {
                    tint: root.clipAccent
                    visible: tile.showCccId
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.padding
                    Layout.rightMargin: Theme.padding
                    placeholderText: qsTr("Correction ID (first)")
                    text: tile.cccId
                    onEditingFinished: tile.cccIdEdited(text)
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    visible: tile.path.length === 0
                    text: qsTr("click to choose")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Item { Layout.fillHeight: true }
            }

            // Top-right clear button when a LUT is loaded. Declared
            // AFTER tileMa so this MouseArea catches clicks within
            // its bounds before the body MA does.
            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Theme.spacing
                width: 22
                height: 22
                color: clearMa.containsMouse ? Theme.error : "transparent"
                radius: Theme.radiusSmall
                visible: tile.path.length > 0
                Icon {
                    anchors.centerIn: parent
                    name: "x"
                    size: Theme.iconSizeSmall
                    color: clearMa.containsMouse
                           ? Theme.textBright : Theme.textMuted
                }
                MouseArea {
                    id: clearMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tile.clearRequested()
                    FlatToolTip {
                        visible: clearMa.containsMouse
                        text: qsTr("Clear LUT")
                    }
                }
            }
        }

        // ---- Collapsed body — slim vertical strip.
        Rectangle {
            id: collapsedStrip
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !tile.expanded
            // Borderless recessed well. Hover lifts the fill; loaded-
            // state cue lives on the identity icon (Theme.success).
            color: collapsedMa.containsMouse ? tile.hoverColor : tile.wellColor
            border.width: 0
            radius: Theme.radiusSmall

            MouseArea {
                id: collapsedMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.expanded = true
                FlatToolTip {
                    visible: collapsedMa.containsMouse
                    text: tile.path
                          ? qsTr("%1 — %2").arg(tile.title)
                                            .arg(root.basenameOf(tile.path))
                          : qsTr("%1 — click to expand").arg(tile.title)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingLoose
                anchors.bottomMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose

                // Rotated vertical title — fills the upper portion
                // of the strip. Wrapped in an Item so the rotated
                // bounding box doesn't fight the layout.
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        text: tile.title
                        color: tile.path ? Theme.textPrimary
                                          : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                }

                ClipSetDot { visible: tile.pinned }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: tile.iconName
                    size: 20
                    color: tile.path
                           ? Theme.success
                           : (collapsedMa.containsMouse
                              ? root.iconHighlight(tile.accentColor) : Theme.textMuted)
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "caret-double-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textMuted
                }
            }
        }
    }
}
