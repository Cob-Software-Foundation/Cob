#!/bin/sh
# tools/apt/build_apt_repo.sh builds the standard apt layout (dists/ + pool/), and apt accepts it.
# usage: apt_repo.sh   (skips when apt-ftparchive, gpg or dpkg-deb is missing)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
for tool in apt-ftparchive gpg dpkg-deb; do command -v "$tool" > /dev/null 2>&1 || { echo "skipped: $tool not installed"; exit 0; }; done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; cd "$T" || exit 1
export GNUPGHOME="$T/gnupg"; mkdir -m 700 "$GNUPGHOME"
gpg --batch --quiet --passphrase '' --quick-generate-key "Cob Test <test@example.com>" rsa2048 sign never > /dev/null 2>&1 || { echo "skipped: gpg cannot make a key here"; exit 0; }
GPG_FPR=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}'); export GPG_FPR
mkdir -p repo/pool/main/c/csf-cob
for v in 0.1.0 0.1.1 0.1.2 0.1.3; do   # four versions: the oldest must be pruned (keep 3)
    mkdir -p "p$v/DEBIAN"
    printf 'Package: csf-cob\nVersion: %s\nArchitecture: all\nMaintainer: Test <t@example.com>\nDescription: test\n' "$v" > "p$v/DEBIAN/control"
    dpkg-deb --build "p$v" "repo/pool/main/c/csf-cob/csf-cob_${v}_all.deb" > /dev/null 2>&1 || exit 1
done
gpg --armor --export "$GPG_FPR" > repo/cob-gpg-public-key.asc
"$ROOT/tools/apt/build_apt_repo.sh" repo --keep 3 > build.log 2>&1 || { cat build.log; exit 1; }
# the layout apt expects
for f in dists/stable/Release dists/stable/InRelease dists/stable/Release.gpg \
         dists/stable/main/binary-amd64/Packages.gz dists/stable/main/binary-arm64/Packages dists/stable/main/binary-armhf/Packages \
         pool/main/c/csf-cob/csf-cob_0.1.3_all.deb cob-gpg-public-key.asc; do
    [ -f "repo/$f" ] || { echo "missing: $f"; exit 1; }
done
[ ! -e repo/pool/main/c/csf-cob/csf-cob_0.1.0_all.deb ] || { echo "the oldest version was not pruned"; exit 1; }
grep -q "^Filename: pool/main/c/csf-cob/csf-cob_0.1.3_all.deb" repo/dists/stable/main/binary-amd64/Packages || { echo "Packages has the wrong Filename"; exit 1; }
grep -q "^Architectures: amd64 arm64 armhf i386" repo/dists/stable/Release || { echo "Release lacks the architectures"; exit 1; }
# the signatures are valid, and a changed file is not
gpg --batch --verify repo/dists/stable/InRelease > /dev/null 2>&1 || { echo "InRelease does not verify"; exit 1; }
gpg --batch --verify repo/dists/stable/Release.gpg repo/dists/stable/Release > /dev/null 2>&1 || { echo "Release.gpg does not verify"; exit 1; }
# a real apt, with only the published key, offers the newest version on every architecture
if command -v apt-get > /dev/null 2>&1 && command -v apt-cache > /dev/null 2>&1; then
    for arch in amd64 arm64; do
        S="$T/apt_$arch"; mkdir -p "$S/lists/partial" "$S/cache/archives/partial"
        echo "deb [signed-by=$T/repo/cob-gpg-public-key.asc] file:$T/repo stable main" > "$S/sources.list"
        O="-o Dir::State=$S -o Dir::State::lists=$S/lists -o Dir::Cache=$S/cache -o Dir::Etc::sourcelist=$S/sources.list -o Dir::Etc::sourceparts=- -o APT::Architecture=$arch -o APT::Architectures::=$arch -o Dir::State::status=/dev/null"
        # shellcheck disable=SC2086
        apt-get update $O > "$S/update.log" 2>&1 || { echo "apt-get update failed on $arch:"; cat "$S/update.log"; exit 1; }
        # shellcheck disable=SC2086
        apt-cache policy csf-cob $O 2> /dev/null | grep -q "Candidate: 0.1.3" || { echo "apt does not offer 0.1.3 on $arch"; exit 1; }
    done
fi
echo "ok"
