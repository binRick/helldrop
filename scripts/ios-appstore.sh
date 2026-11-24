#!/usr/bin/env bash
# scripts/ios-appstore.sh
#
# Archive + export + validate + upload Helldrop to App Store Connect.
# Adapted from ../uapd/scripts/ios-appstore.sh.
#
# The version and build number come from ios/project.yml (MARKETING_VERSION,
# CURRENT_PROJECT_VERSION). Apple rejects a build number it has seen before:
# bump CURRENT_PROJECT_VERSION before every upload.
#
# Auth: an App Store Connect API key with the Admin role (nobody is signed in
# to Xcode on the build Mac, so signing is cloud-managed through the key):
#   export ASC_KEY_ID=<key id> ASC_ISSUER_ID=<issuer id>
#   key file: ~/.appstoreconnect/private_keys/AuthKey_<KEY_ID>.p8
# DRY_RUN=1 stops after the .ipa is exported.
#
# Success is the line "UPLOAD SUCCEEDED with no errors" (read the output; the
# exit code alone is not proof). Then: scripts/asc-submit.py <version> <build>.

set -euo pipefail
cd "$(dirname "$0")/.."

TEAM_ID="${TEAM_ID:-4L7XW5NPCX}"
PROJECT="ios/Helldrop.xcodeproj"
SCHEME="Helldrop"
EXPORT_PLIST="ios/ExportOptions-AppStore.plist"
ARCHIVE_PATH="build/ios-appstore/Helldrop.xcarchive"
EXPORT_DIR="build/ios-appstore/export"

VERSION="$(sed -n 's/.*MARKETING_VERSION: *"\{0,1\}\([^"]*\)"\{0,1\}.*/\1/p' ios/project.yml | head -1)"
BUILD="$(sed -n 's/.*CURRENT_PROJECT_VERSION: *\([0-9][0-9]*\).*/\1/p' ios/project.yml | head -1)"
[[ -n "$VERSION" && -n "$BUILD" ]] || { echo "ERROR: version/build not found in ios/project.yml" >&2; exit 1; }
# A UIKit app without a scene manifest is killed at launch on current iOS.
grep -q UIApplicationSceneManifest ios/Info.plist || { echo "ERROR: ios/Info.plist has no UIApplicationSceneManifest" >&2; exit 1; }

AUTH=()
ALTOOL_AUTH=()
if [[ -n "${ASC_KEY_ID:-}" && -n "${ASC_ISSUER_ID:-}" ]]; then
    ASC_KEY_FILE="${ASC_KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8}"
    [[ -f "$ASC_KEY_FILE" ]] || { echo "ERROR: $ASC_KEY_FILE not found" >&2; exit 1; }
    AUTH=(-authenticationKeyPath "$ASC_KEY_FILE" -authenticationKeyID "$ASC_KEY_ID" -authenticationKeyIssuerID "$ASC_ISSUER_ID")
    ALTOOL_AUTH=(--apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID")
elif [[ "${DRY_RUN:-0}" != "1" ]]; then
    echo "ERROR: set ASC_KEY_ID and ASC_ISSUER_ID (or DRY_RUN=1)" >&2; exit 1
fi

echo "==> Helldrop $VERSION ($BUILD), team $TEAM_ID"
make ios-project > /dev/null

echo "==> Archiving"
rm -rf "$ARCHIVE_PATH"
xcodebuild archive \
    -project "$PROJECT" -scheme "$SCHEME" -configuration Release \
    -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE_PATH" -allowProvisioningUpdates \
    ${AUTH[@]+"${AUTH[@]}"} \
    DEVELOPMENT_TEAM="$TEAM_ID" CODE_SIGN_STYLE=Automatic \
    | grep -E "error:|warning: .*([Cc]ode ?[Ss]ign|[Pp]rovisioning)|ARCHIVE (SUCCEEDED|FAILED)" || true
[[ -d "$ARCHIVE_PATH" ]] || { echo "ERROR: no archive at $ARCHIVE_PATH" >&2; exit 1; }

echo "==> Exporting App Store IPA"
rm -rf "$EXPORT_DIR"; mkdir -p "$EXPORT_DIR"
xcodebuild -exportArchive \
    -archivePath "$ARCHIVE_PATH" -exportPath "$EXPORT_DIR" \
    -exportOptionsPlist "$EXPORT_PLIST" -allowProvisioningUpdates \
    ${AUTH[@]+"${AUTH[@]}"}
IPA_PATH="$(ls "$EXPORT_DIR"/*.ipa 2>/dev/null | head -1)"
[[ -f "$IPA_PATH" ]] || { echo "ERROR: no .ipa in $EXPORT_DIR" >&2; exit 1; }
echo "    IPA: $IPA_PATH ($(du -h "$IPA_PATH" | awk '{print $1}'))"

if [[ "${DRY_RUN:-0}" == "1" ]]; then echo "==> DRY_RUN=1: not uploading"; exit 0; fi

echo "==> Validating"
xcrun altool --validate-app -f "$IPA_PATH" -t ios "${ALTOOL_AUTH[@]}" 2>&1 | tail -20
echo "==> Uploading"
xcrun altool --upload-app -f "$IPA_PATH" -t ios "${ALTOOL_AUTH[@]}" 2>&1 | tail -30
echo "Next: /usr/bin/python3 scripts/asc-submit.py $VERSION $BUILD"
