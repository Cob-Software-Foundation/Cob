# Portable helpers for the CI workflows. `source` this from a bash step.
#
# GitHub's runners disagree about which tools exist: macOS has no GNU
# `sha256sum`, and the Windows Git Bash has neither `zip` nor (reliably)
# `unzip`. Each helper uses the usual tool when it is there and falls back to
# one that every runner does have.

# sha256 <file>...  -- same output format as `sha256sum`: "<hash>  <file>"
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"      # macOS
  fi
}

# mkzip <out.zip> <dir>  -- zip <dir> (a path relative to the current
# directory), keeping "<dir>/" as the archive's top-level folder.
mkzip() {
  local out=$1 src=$2
  case "$out" in /*|[A-Za-z]:*) ;; *) out="$PWD/$out" ;; esac
  rm -f "$out"
  if command -v zip >/dev/null 2>&1; then
    zip -rq "$out" "$src"
  elif command -v 7z >/dev/null 2>&1; then
    7z a -tzip -bso0 -bsp0 "$out" "$src"
  else
    echo "mkzip: neither zip nor 7z is installed" >&2
    return 1
  fi
}

# mkzip_contents <out.zip> <dir>  -- zip what is INSIDE <dir> (no top-level
# folder), as the embeddable package wants.
mkzip_contents() {
  local out=$1 src=$2
  case "$out" in /*|[A-Za-z]:*) ;; *) out="$PWD/$out" ;; esac
  rm -f "$out"
  if command -v zip >/dev/null 2>&1; then
    (cd "$src" && zip -rq "$out" .)
  elif command -v 7z >/dev/null 2>&1; then
    (cd "$src" && 7z a -tzip -bso0 -bsp0 "$out" .)
  else
    echo "mkzip_contents: neither zip nor 7z is installed" >&2
    return 1
  fi
}

# unzip_to <file.zip> <dir>
unzip_to() {
  mkdir -p "$2"
  if command -v unzip >/dev/null 2>&1; then
    unzip -q "$1" -d "$2"
  elif command -v 7z >/dev/null 2>&1; then
    7z x -y -bso0 -bsp0 "-o$2" "$1"
  else
    powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '$1' -DestinationPath '$2'"
  fi
}
