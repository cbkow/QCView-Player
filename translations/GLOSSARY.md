# Glossary

What the words in QCView's strings mean, and which ones stay in English.
Read this before translating; the machine pass used it too.

## Never translate

Product and brand names, standards, codecs and colour-science terms that
professionals use in English everywhere:

QCView · minNotes · minColor · QCBridge · QCBridgeAE · QCBridgeN · Louper ·
After Effects · Premiere Pro · Premiere · Blender · Sparkle · OpenColorIO ·
OCIO · OpenDRT · AgX · LUT · CDL · EXR · ProRes · DNxHR · DNxHD · H.264 ·
H.265 · AAC · SRT · RTMP · NDI · AJA · HDR · HDR10 · SDR · EDR · PQ · HLG ·
Rec.709 · Rec.2020 · Rec.2100 · P3-D65 · Display P3 · sRGB · BT.2390 ·
BS.775 · MaxCLL · MaxFALL · NCLC · XMP · QT (QuickTime) · Vulkan · D3D11VA ·
Metal · GPU · fps · kbps · nits · Hz · ms · dB · UHD · 1080p · RGB · YUV ·
Y′ (luma) · A / B (the two sides of a dual view) · Markdown · HTML · PDF ·
DOCX · PNG · JPEG · TIFF.

Keyboard names and shortcuts inside parentheses stay as written:
`(Space)`, `(Ctrl+Alt+T)`, `(⌥T)`, `(⌃3)`, `(Shift+I)`.

Menu accelerators: an `&` before a letter marks the Alt key letter on
Windows (`&File`, `E&xit`). Keep one `&` in the translation on a sensible
letter, or drop it. `&&` is a literal ampersand.

Placeholders `%1`, `%2`, `%n` are filled in by the program. Keep every one,
in the order the sentence needs. HTML tags such as `<b>` stay.

## Terms, as QCView uses them

| English | Meaning here | Note for translators |
|---|---|---|
| clip | one media item on the timeline or in a dual view | the editing term, not "video clip" in general |
| playlist | an ordered list of clips played as one | |
| dual view | two sources shown together (side by side, wipe, difference) | A and B are its two sides |
| source A / B | the two sides of a dual view | keep the letters |
| rail | the collapsible side panel (left: project, right: inspector) | a UI panel |
| Inspector | the right panel showing media details | |
| Color panel | the colour-management panel | US spelling in the source |
| viewport | the picture area | |
| viewer / viewer aids | temporary exposure, gamma and channel adjustments | |
| scope | vectorscope or waveform monitor | broadcast term |
| trace | the drawn signal in a scope | |
| scrub | drag through the timeline to preview frames | editing term; many languages borrow it |
| playhead | the current-time marker on the timeline | |
| in point / out point | the start and end of a playback range | editing terms |
| head / tail (of a clip) | its start and end | trim badges on the timeline |
| trim, slip, slide | editing operations on a clip's boundaries and position | use the editing-software terms of the language |
| freeze (a live feed) | hold the current frame for review | opposite of "back to live" |
| live | a stream or bridge feed playing in real time | |
| bridge / QCBridge | the plug-in that sends frames from After Effects or Premiere | |
| host (application) | the application a bridge runs inside (After Effects, Premiere) | |
| sink / output | an installed program that receives QCView's picture (NDI, AJA) | "Output" is the section name |
| feed | the audio or picture sent to outputs | |
| stream (verb/noun) | SRT or RTMP sending; also an incoming SRT source | |
| stream key | the SRT stream id | |
| note | a review annotation on a frame | the Notes panel |
| addressed (note) | marked as dealt with | |
| stroke | one drawn mark of an annotation | |
| safety guides | overlay frames showing title-safe and platform-safe areas | |
| background | the fill behind the picture (black, grey, checkerboard) | |
| Compact Mode | the small frameless window | a mode name |
| Minimal Mode | rails collapsed, panels hidden | a mode name |
| Always on Top | the window stays above other windows | the pin |
| preset | a saved colour setup | |
| clip chain | the per-clip half of the colour chain (Input, Look, LUTs) | minColor and OCIO term |
| view (OCIO) | the display-side transform | OCIO term |
| look (OCIO) | a creative transform in the config | OCIO term |
| input (OCIO) | the source colourspace | |
| transfer | the transfer function (PQ, HLG, sRGB) | colour-science term |
| gamut / primaries | the colour primaries | |
| range: full / limited | video levels (0–255 vs 16–235) | broadcast term |
| untagged | the file carries no colour metadata | |
| knee / Highlight Knee | the highlight compression stage | keep "Knee" if the language borrows it |
| toe / shoulder | the dark and bright ends of a tone curve | photography terms |
| rendering (minColor) | the tone-mapping stage | |
| peak (luminance) | the brightest level in nits | |
| image sequence | numbered still frames played as a clip | |
| cache stride / cache window | how often and how far frames are cached | |
| timecode | HH:MM:SS:FF | keep the abbreviation TC |
| drop frame (DF) | the NTSC timecode counting mode | |
| step (a frame) | move by one frame | |
| review speed | the slowed or sped playback speed for review | |
| screenshot | a saved picture of the viewport | |

## The three macOS app-menu items

On macOS Qt moves "About QCView…" and "Settings…" into the application
menu by looking at their titles: About must start or end with Qt's own
word for *About* in that language (the ellipsis counts, so Japanese and
Korean end without one: QCViewについて, QCView 정보), and Settings must
start with Qt's word for Config / Preference / Options / Setting / Setup.
Qt's words: de Über · Einstellung — fr À propos · Réglage — es Acerca de ·
Ajuste — pt_BR Sobre · Preferências — it Informazioni · Impostazione —
pl Informacje o programie · Ustawienia — ja について · 設定 — ko 정보 ·
설정 — zh_CN 关于 · 设置 — ru О ("О QCView") · Настройк / Параметры /
Конфигурация — uk Про · Налаштування / Опції / Установки. Italian "Impostazioni…" cannot match
"Impostazione" and stays in the Tools menu on macOS; that is accepted.

## Register

Address the user the way system software in the language normally does.
Decisions already taken: German **Sie** · French **vous** · Spanish **usted**
(neutral, no regional slang) · Portuguese (Brazil) **você** · Italian **tu**
(as macOS and Windows do) · Polish impersonal / formal · Russian formal **вы**
(lower case, as system software does) · Ukrainian formal **ви** · Japanese ですます
· Korean 합니다체 · Chinese plain 您 where a pronoun is needed.

Short labels stay short: German and French run a third longer than
English and the panels are narrow. Prefer the compact common term over
a descriptive phrase; a tooltip (the longer sentences) can breathe.
