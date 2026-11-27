#!/usr/bin/env bash
# Builds the apt repository that is served from https://downloads.pixel-pulse.dev/apt, in the standard layout:
#
#   apt/
#     cob-gpg-public-key.asc
#     release_notes.md                              (optional)
#     pool/main/c/csf-cob/csf-cob_<version>_all.deb
#     dists/stable/Release  InRelease  Release.gpg
#     dists/stable/main/binary-{amd64,arm64,armhf,i386}/Packages  Packages.gz
#
# Put the .deb files in REPO/pool/main/c/csf-cob/ first (older versions that should stay installable too), then run:
#     GPG_FPR=<key id or fingerprint> tools/apt/build_apt_repo.sh REPO [--keep N] [--no-sign]
# and upload the whole REPO folder to the bucket's apt/ prefix, so that dists/stable/InRelease is reachable at
# https://downloads.pixel-pulse.dev/apt/dists/stable/InRelease. Users then add:
#     deb [signed-by=/etc/apt/keyrings/csf-cob.asc] https://downloads.pixel-pulse.dev/apt stable main
#
# The package is Architecture: all (its postinst picks the right build), so one index is written for every architecture a
# Debian machine commonly has, Raspberry Pi included.
#
# Environment: GPG_FPR (signing key; GNUPGHOME as usual), GPG_PASSFILE (file holding its passphrase, if it has one).
# Needs: apt-ftparchive (package apt-utils), gpg, gzip.
set -euo pipefail

SUITE="${COB_APT_SUITE:-stable}"
COMPONENT="main"
ARCHES="amd64 arm64 armhf i386"
KEEP=3
SIGN=1

die() { echo "build_apt_repo: $*" >&2; exit 1; }

[ $# -ge 1 ] || die "usage: $0 REPO_DIR [--keep N] [--no-sign]"
REPO="$1"; shift
while [ $# -gt 0 ]; do
    case "$1" in
        --keep) KEEP="${2:?--keep needs a number}"; shift 2 ;;
        --no-sign) SIGN=0; shift ;;
        *) die "unknown option: $1" ;;
    esac
done
case "$KEEP" in ''|*[!0-9]*) die "--keep must be a whole number" ;; esac
[ "$KEEP" -ge 1 ] || die "--keep must be at least 1"
command -v apt-ftparchive > /dev/null || die "apt-ftparchive not found (install the apt-utils package)"
[ "$SIGN" -eq 0 ] || command -v gpg > /dev/null || die "gpg not found"

POOL="pool/$COMPONENT/c/csf-cob"
cd "$REPO" || die "no such folder: $REPO"
ls "$POOL"/csf-cob_*_all.deb > /dev/null 2>&1 || die "no csf-cob_*_all.deb in $REPO/$POOL"

# keep the newest $KEEP versions installable, drop the rest
ls -1 "$POOL"/csf-cob_*_all.deb | sort -V | head -n -"$KEEP" | while read -r old; do echo "removing old version: $old"; rm -f "$old"; done
echo "versions in the repository:"; ls -1 "$POOL"/csf-cob_*_all.deb

# the indexes: Filename: paths in Packages are relative to the repository root, so run from there
rm -rf dists
BASE="dists/$SUITE"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
apt-ftparchive packages pool > "$TMP/Packages"
[ -s "$TMP/Packages" ] || die "apt-ftparchive wrote an empty Packages file"
for a in $ARCHES; do
    d="$BASE/$COMPONENT/binary-$a"
    mkdir -p "$d"
    cp "$TMP/Packages" "$d/Packages"
    gzip -9kn "$d/Packages"
done

# Release lists the hash of every index; it must not exist while apt-ftparchive hashes the folder
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin="Cob Software Foundation" \
    -o APT::FTPArchive::Release::Label="Cob" \
    -o APT::FTPArchive::Release::Suite="$SUITE" \
    -o APT::FTPArchive::Release::Codename="$SUITE" \
    -o APT::FTPArchive::Release::Architectures="$ARCHES" \
    -o APT::FTPArchive::Release::Components="$COMPONENT" \
    -o APT::FTPArchive::Release::Description="Cob apt repository" \
    release "$BASE" > "$TMP/Release"
mv "$TMP/Release" "$BASE/Release"

if [ "$SIGN" -eq 1 ]; then
    GPGARGS=(--batch --yes)
    [ -z "${GPG_PASSFILE:-}" ] || GPGARGS+=(--pinentry-mode loopback --passphrase-file "$GPG_PASSFILE")
    [ -z "${GPG_FPR:-}" ] || GPGARGS+=(--local-user "$GPG_FPR")
    gpg "${GPGARGS[@]}" --clearsign --output "$BASE/InRelease" "$BASE/Release"
    gpg "${GPGARGS[@]}" --armor --detach-sign --output "$BASE/Release.gpg" "$BASE/Release"
else
    echo "NOT SIGNED (--no-sign): apt will refuse this repository unless the source line has [trusted=yes]"
fi
echo "done:"; find dists -type f | sort
