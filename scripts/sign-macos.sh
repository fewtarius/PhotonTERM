#!/usr/bin/env bash
# PhotonTERM macOS sign, notarize, and staple
#
# Works in three modes:
#
#   --sign-only   Sign with Developer ID, no notarization (fast, for dev builds)
#   (default)     Sign + notarize + staple (full release)
#
# Credential sources (in priority order):
#   1. Keychain profile "PhotonTERM" (stored via xcrun notarytool store-credentials)
#   2. Env vars: APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD
#
# Environment:
#   APPLE_DEVELOPER_ID  e.g. "Developer ID Application: Your Name (TEAMID)"
#
# Usage:
#   ./scripts/sign-macos.sh [--sign-only] [path/to/PhotonTERM.app]

set -e

SIGN_ONLY=0

# Parse flags
while [[ "$1" == --* ]]; do
    case "$1" in
        --sign-only) SIGN_ONLY=1 ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
    shift
done

APP_PATH="${1}"

# Locate app if not specified
if [ -z "${APP_PATH}" ]; then
    APP_PATH="$(find src/photonterm -name 'PhotonTERM.app' -maxdepth 2 | head -1)"
fi

if [ -z "${APP_PATH}" ] || [ ! -d "${APP_PATH}" ]; then
    echo "ERROR: PhotonTERM.app not found."
    echo "  Build first, or specify: $0 path/to/PhotonTERM.app"
    exit 1
fi

SIGNING_IDENTITY="${APPLE_DEVELOPER_ID}"
DIST_DIR="dist"

if [ -z "${SIGNING_IDENTITY}" ]; then
    echo "ERROR: APPLE_DEVELOPER_ID not set."
    exit 1
fi

echo "PhotonTERM macOS signing"
echo "  App:       ${APP_PATH}"
echo "  Identity:  ${SIGNING_IDENTITY}"
echo "  Mode:      $([ $SIGN_ONLY -eq 1 ] && echo 'sign-only' || echo 'sign+notarize')"

# --- 1. Sign ---
echo "==> Signing..."
codesign --force \
    --sign "${SIGNING_IDENTITY}" \
    --entitlements "${APP_PATH}/../../../src/photonterm/PhotonTERM.entitlements" \
    --options runtime \
    --timestamp \
    --deep \
    "${APP_PATH}"
echo "  OK: signed"

# --- 2. Verify ---
echo "==> Verifying signature..."
codesign --verify --deep --strict "${APP_PATH}"
echo "  OK: signature valid"

if [ $SIGN_ONLY -eq 1 ]; then
    echo "  Skipping notarization (sign-only mode)."
    echo ""
    echo "SUCCESS: PhotonTERM signed with Developer ID."
    exit 0
fi

# --- 3. Archive ---
mkdir -p "${DIST_DIR}"
VERSION=$(defaults read "$(cd "${APP_PATH}" && pwd)/Contents/Info" \
    CFBundleShortVersionString 2>/dev/null || echo "dev")
echo "  Version: ${VERSION}"

ARCHIVE="${DIST_DIR}/PhotonTERM-${VERSION}.zip"
echo "==> Creating archive for notarization..."
ditto -c -k --keepParent "${APP_PATH}" "${ARCHIVE}"
echo "  OK: ${ARCHIVE}"

# --- 4. Notarize ---
# Prefer keychain profile; fall back to env vars
if xcrun notarytool history --keychain-profile "PhotonTERM" > /dev/null 2>&1; then
    echo "==> Submitting for notarization (keychain profile)..."
    NOTARIZE_OUT=$(xcrun notarytool submit "${ARCHIVE}" \
        --keychain-profile "PhotonTERM" \
        --wait 2>&1) || true
elif [ -n "${APPLE_ID}" ] && [ -n "${APPLE_TEAM_ID}" ] && [ -n "${APPLE_APP_PASSWORD}" ]; then
    echo "==> Submitting for notarization (env-var credentials)..."
    NOTARIZE_OUT=$(xcrun notarytool submit "${ARCHIVE}" \
        --apple-id "${APPLE_ID}" \
        --team-id  "${APPLE_TEAM_ID}" \
        --password "${APPLE_APP_PASSWORD}" \
        --wait 2>&1) || true
else
    echo "ERROR: No notarization credentials found."
    echo "  Run: xcrun notarytool store-credentials PhotonTERM --apple-id <email> --team-id <TEAMID> --password <app-pw>"
    echo "  Or set: APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD"
    exit 1
fi

echo "${NOTARIZE_OUT}"

if echo "${NOTARIZE_OUT}" | grep -q "status: Accepted"; then
    echo "==> Stapling notarization ticket..."
    xcrun stapler staple "${APP_PATH}"
    echo "  OK: stapled"

    echo "==> Gatekeeper check..."
    spctl --assess --type execute --verbose=2 "${APP_PATH}" 2>&1 || true

    echo ""
    echo "SUCCESS: PhotonTERM ${VERSION} signed, notarized, and stapled."
    echo "  Archive: ${ARCHIVE}"
else
    echo "ERROR: Notarization failed or was rejected."
    SUBMISSION_ID=$(echo "${NOTARIZE_OUT}" | grep "id:" | head -1 | awk '{print $2}')
    if [ -n "${SUBMISSION_ID}" ]; then
        echo "Detailed log:"
        if xcrun notarytool history --keychain-profile "PhotonTERM" > /dev/null 2>&1; then
            xcrun notarytool log "${SUBMISSION_ID}" \
                --keychain-profile "PhotonTERM" \
                2>&1 | sed 's/^/  /'
        elif [ -n "${APPLE_ID}" ]; then
            xcrun notarytool log "${SUBMISSION_ID}" \
                --apple-id "${APPLE_ID}" \
                --team-id  "${APPLE_TEAM_ID}" \
                --password "${APPLE_APP_PASSWORD}" \
                2>&1 | sed 's/^/  /'
        fi
    fi
    exit 1
fi
