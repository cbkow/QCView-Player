# Releasing QCView (macOS)

The scripts in this folder are the release flow. They were rewritten on
2026-09-22 after the originals were lost with the old dev machine — they had
been gitignored, which is why. **They are committed now; keep them that way.**
They hold no secrets: the Sparkle key lives in the login keychain, the
notarization credentials in a keychain profile.

## What has to be on the machine

| Thing | Check it | If it's missing |
|---|---|---|
| Developer ID Application cert | `security find-identity -v -p codesigning` | Apple Developer account → Certificates |
| Sparkle private key | `external/Sparkle/bin/generate_keys -p` prints the key that is in `packaging/macos/Info.plist.in` | restore from backup with `generate_keys -f <file>`. **Without it, no existing install can ever be updated again** — a new key means every user reinstalls by hand |
| notarytool profile | `xcrun notarytool history --keychain-profile QCView` | `xcrun notarytool store-credentials "QCView" --apple-id <id> --team-id 5Z4S9VHV56` (prompts for an app-specific password) |
| codesign can use the key unprompted | signing runs without password dialogs | `security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k <login-password> ~/Library/Keychains/login.keychain-db`, or tick "Allow all applications" on the private key in Keychain Access. Without it codesign fails with `errSecInternalComponent` after prompting for each of ~90 items |
| Qt 6.11.1 | `ls ~/Qt/6.11.1/macos/bin/macdeployqt` | Qt online installer |
| Vendored deps | `ls external/install/lib` | `dependencies.md` §2 — codec libs, FFmpeg, OCIO/OpenEXR/Imath, libpng/libjpeg/libtiff |

Everything QCView ships is built at the **macOS 13.0** floor, which is what
the appcast promises (`minimumSystemVersion`). Don't relax that by picking up
Homebrew libraries: they are built for a much newer minimum, and
`bundle_dylibs.sh` refuses them on purpose.

## The flow

```bash
# 1. version bump: CMakeLists.txt  project(qcview VERSION x.y.z)
#    (Info.plist takes CFBundleShortVersionString from it)

# 2. clean-ish build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.1/macos" \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
cmake --build build -j"$(sysctl -n hw.ncpu)"

# 3. run the app once and do the media matrix (see below)

# 4. stage a copy, deploy Qt, bundle dylibs, prune, sign, DMG, notarize, staple
scripts/sign-and-notarize.sh              # --skip-notarize for a dry run
# → build/dist/QCView-MacOS.dmg  (~124 MB; it was 179 before pruning)
#
# It works on a COPY in build/dist, never on what CMake built: macdeployqt
# and install_name_tool rewrite a bundle in place, so deploying twice over
# the same one starts from the previous run's output.

# 5. GitHub release v<version>, attach that DMG (the name is fixed: the
#    appcast URL points at .../releases/download/v<version>/QCView-MacOS.dmg)

# 6. appcast — docs/ IS the qcview.app site
scripts/update_appcast.sh <version> build/dist/QCView-MacOS.dmg
git add docs/appcast.xml && git commit -m "appcast: publish <version>" && git push
```

A Pages deploy sometimes sits in "queued"; an empty commit nudges it.

## Before you sign

The media matrix, in the real app, per QCView's standing rule — every media
type against every view:

- video (h264 / ProRes / DNxHR), image sequence (EXR, PNG, JPEG, TIFF),
  playlist, live (`qcbae://ae`, `srt://`), audio-only
- single view and dual view (SBS, wipe, difference), scrubbing, loop with
  in/out, screenshots, annotations
- switch between all of them, in both directions

Per-frame decoder warnings are a failure, not noise. The log is at
`~/Library/Logs/QCView/qcview-log.txt` (the previous session is kept as
`qcview-log.prev.txt`); `QCV_LOG_DIR` moves it.

## Things that have bitten

- **Sparkle's nested code** (`Autoupdate`, the two XPC services, `Updater.app`)
  arrives signed by the Sparkle project. Notarization rejects it unless it is
  re-signed with this Developer ID, innermost first. It is silent until Apple
  answers — `xcrun notarytool log <id> --keychain-profile QCView` names the
  paths.
- **macdeployqt is generous.** One Qt QML plugin linking QtMultimedia dragged
  Qt's whole media stack, and a second FFmpeg, into the DMG.
  `prune_bundle.sh` removes what nothing imports and then garbage-collects
  unreferenced frameworks; plugins count as roots because QML loads them
  dynamically.
- **Never let the app write inside its own bundle.** An unsealed file under
  `Contents/` breaks the signature, and Sparkle can then refuse to update.
  The log lives in `~/Library/Logs/QCView/`.

## Checking a build after the fact

```bash
codesign --verify --deep --strict --verbose=2 build/src/app/qcview.app
spctl --assess --type open --context context:primary-signature -vv build/dist/QCView-MacOS.dmg
xcrun stapler validate build/dist/QCView-MacOS.dmg
otool -L build/src/app/qcview.app/Contents/MacOS/qcview | grep -c homebrew   # must be 0
```

To prove the update chain end to end, install the previous release, put the
new DMG on the release, publish the appcast, and let Sparkle find it. A
signature the key can't verify shows up as "update is improperly signed".
