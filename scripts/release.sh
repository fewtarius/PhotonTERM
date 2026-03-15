#!/bin/bash
# scripts/release.sh - PhotonTERM release helper
#
# Computes or accepts a YYYYMMDD.N version, updates source files,
# commits, and creates the annotated tag. Does NOT push - do that
# manually after review.
#
# Usage:
#   ./scripts/release.sh            # auto-compute next version for today
#   ./scripts/release.sh 20260309.3 # explicit version

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# ── helpers ────────────────────────────────────────────────────────────
die() { echo "ERROR: $*" >&2; exit 1; }

today() { date -u +%Y%m%d; }

# Find the next available N for today's date
next_version() {
    local date_str
    date_str=$(today)
    local n=1
    while git rev-parse "${date_str}.${n}" >/dev/null 2>&1; do
        n=$(( n + 1 ))
    done
    echo "${date_str}.${n}"
}

# ── argument handling ──────────────────────────────────────────────────
if [ $# -eq 0 ]; then
    VERSION=$(next_version)
    echo "Auto-computed version: $VERSION"
elif [ $# -eq 1 ]; then
    VERSION="$1"
    if ! echo "$VERSION" | grep -qE '^[0-9]{8}\.[0-9]+$'; then
        die "Invalid version format: $VERSION (expected YYYYMMDD.N)"
    fi
else
    die "Usage: $0 [YYYYMMDD.N]"
fi

# ── pre-flight checks ──────────────────────────────────────────────────
git rev-parse --git-dir >/dev/null 2>&1 || die "Not in a git repository"

if ! git diff-index --quiet HEAD --; then
    die "Uncommitted changes present - commit or stash first"
fi

if git rev-parse "$VERSION" >/dev/null 2>&1; then
    die "Tag $VERSION already exists"
fi

# ── update source files ────────────────────────────────────────────────
echo "Updating photon_main.c ..."
sed -i.bak \
    's|const char \*photonterm_version = "PhotonTERM [^"]*";|const char *photonterm_version = "PhotonTERM '"$VERSION"'";|' \
    src/photonterm/photon_main.c
rm -f src/photonterm/photon_main.c.bak

echo "Updating Info.plist ..."
# Use Python - portable across macOS BSD sed and Linux GNU sed.
python3 -c "
import sys, re
version = sys.argv[1]
path = sys.argv[2]
with open(path) as f:
    text = f.read()
for key in ['CFBundleShortVersionString', 'CFBundleVersion']:
    text = re.sub(
        r'(<key>' + key + r'</key>\s*\n\s*)<string>[^<]*</string>',
        r'\g<1><string>' + version + '</string>',
        text)
with open(path, 'w') as f:
    f.write(text)
" "$VERSION" src/photonterm/Info.plist

# ── verify ─────────────────────────────────────────────────────────────
echo ""
echo "Verification:"
echo "  photon_main.c: $(grep 'photonterm_version' src/photonterm/photon_main.c | head -1 | xargs)"
echo "  Info.plist:    $(grep -A1 'CFBundleShortVersionString' src/photonterm/Info.plist | tail -1 | xargs)"
echo ""

# ── commit + tag ───────────────────────────────────────────────────────
git add src/photonterm/photon_main.c src/photonterm/Info.plist
git commit -m "chore(release): $VERSION"
git tag -a "$VERSION" -m "PhotonTERM $VERSION"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Release $VERSION prepared."
echo ""
echo "Review:  git show $VERSION"
echo "Push:    git push origin main --tags"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
