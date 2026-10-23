# Translations

QCView's interface is written in English and translated through Qt's
Linguist catalogs. Each language is one file here, `qcview_<code>.ts`,
and the build compiles them into the app. Nothing in this folder is
edited by hand except the translation text.

## Helping with a translation

You do not need to build QCView. Work from the spreadsheet for your
language (ask for the link, or export one yourself with the script
below), fill in the **translation** column, and send the sheet back or
open a pull request with the updated `.ts`.

Rules that keep the import working:

- Change only the **translation** column. The English is the key the
  app looks strings up by; an English fix is a code change, not a
  translation.
- Keep every `%1`, `%2`, `%n` placeholder. They are replaced at run
  time (`%1` by a value, `%n` by a count). You may reorder them.
- Keep `<b>`, `</b>` and other tags, `&` before a menu accelerator
  letter, and the `…` ellipsis.
- A `%n` string has one row per plural form (`form` 0, 1, ...). Row 0
  is the singular. Languages with one plural form have one row.
- `status` tells you what is left: `unfinished` rows are untranslated or
  the English changed since they were translated.

Context strings such as `LeftRail` or `InspectorPanel` name the part of
the UI the string lives in; `developer_comment` explains a terse string
("Output state", "Trim badge: …"); `location` is the source file and
line if you want to look. `GLOSSARY.md` beside this file lists the terms
QCView uses, the names that stay in English, and the form of address
chosen for each language.

## Machine first, people second

Every language shipped first as a machine pass: each string translated
with the glossary and comments at hand, imported with its `status` left
`unfinished`, and the language marked "machine translation" in the
Language picker. That label comes off a language when a person has read
its sheet through — add the code to `kReviewed` in `src/app/i18n.cpp`.

So a correction is never a blank to fill, it is a line to improve. When
you send a sheet back, set `status` to `done` on the rows you checked
(or leave it; a row with text and status `machine` keeps asking for
review).

## Maintainer workflow

```sh
# (re)extract the English strings into every catalog after UI changes
cmake --build build --target update_translations

# spreadsheet out, spreadsheet in
scripts/i18n_csv.py export translations/qcview_de.ts de.csv
scripts/i18n_csv.py import translations/qcview_de.ts de.csv
scripts/i18n_csv.py stats  translations/qcview_de.ts
```

Import refuses a row whose placeholders differ from the English (the
usual translator slip) and never clears an existing translation with an
empty cell. `import --keep-unfinished` (or `status` = `machine` on a
row) fills the text but keeps the review flag; that is how the machine
pass went in. Commit the `.ts`; the `.qm` files are build products.

**Adding a language:** append its code to `QCV_TRANSLATED_LANGUAGES` in
the top-level `CMakeLists.txt`, configure, run `update_translations`,
commit the new `.ts`. Shipping now: zh_CN, ja, ko, de, fr, es, pt_BR,
it, pl. Codes are Qt locale names: `de`, `fr`, `ja`,
`pt_BR`, `zh_CN`. Before it ships, also list the language in
`packaging/macos/Info.plist.in` (`CFBundleLocalizations`) and
`installer/msix/AppxManifest.xml.in` (`<Resource Language=…>`).

`qcview_en.ts` is special: it holds only the English plural forms
(`%n frame` / `%n frames`), which the source strings cannot express.
Fill those in when a new `%n` string appears.

**Writing a translatable string:** one sentence per `qsTr()`, with
placeholders rather than glued fragments (`qsTr("%1 fps").arg(x)`, never
`x + " fps"`); counts go through `qsTr("%n frame(s)", "", n)`; a terse
label gets a `//:` comment on the line above so the sheet explains it.

## Testing a catalog

- `QCV_LANG=de` runs the app in that language regardless of the setting.
- `QCV_TRANSLATIONS_DIR=<dir>` loads `qcview_<code>.qm` from there
  first, so a translator can test without rebuilding: compile the `.ts`
  with `lrelease qcview_de.ts` (Qt's bin dir) and point the variable at
  the folder.
- `QCV_PSEUDO_LOC=1` shows every translatable string as
  `[Its English text ~~~~]`, about a third longer. Anything that still
  reads plainly bypasses the catalog; anything clipped needs room.

The language setting itself is Settings ▸ Interface ▸ Language and
takes effect on the next launch.
