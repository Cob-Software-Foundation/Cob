# Cob Enterprise build (license-gated)

The free Cob build is unchanged and needs no license. The Enterprise build is a
separate set of binaries (`*_enterprise`) that refuse to start without a valid,
signed `cob.lic`. A license can also be **bound to one machine** (see below).

## One-time setup (maintainer's PC only)

    make cob_sign
    bin/cob_sign keygen            # writes cob_private.key, appends the public key to keys/drm_pubkeys.txt

* `cob_private.key` is SECRET: keep it off GitHub, off servers, back it up offline.
  (`*.key`, `cob_private*` and `*.lic` are already in .gitignore.)
* `keys/drm_pubkeys.txt` holds the PUBLIC key(s), one `<key_id> <64 hex>` per line;
  it is safe to commit. To rotate keys, run `keygen --key-id 2`: licenses signed
  with older keys keep working.

## Issue a license

    bin/cob_sign sign --key cob_private.key --customer "Acme Corp" \
        --expires 2027-10-22 --features "*" --seats 50 --out cob.lic

By default the license is **bound to the first machine that runs it** (see
"Machine binding"). Pass `--no-bind` for an unbound license.

## Machine binding

Licenses can be tied to a machine's UUID. Cob reads the operating system's own
machine id (`/etc/machine-id` on Linux, `IOPlatformUUID` on macOS, `MachineGuid`
on Windows) and never stores it raw: what is recorded is
`sha256("cob-machine-id-v1\0" + uuid)`. `bin/cob_sign machine-id` prints it.

**Bind on first run** (the default, `bind=machine` in the signed part). The first
time any enterprise tool starts with the license, it records this machine in the
license file, below the signature:

    ---
    <base64 signature>
    bound_machine=<64 hex>
    bound_on=2026-10-30

and prints one line to stderr saying so. Every later run must match. The
license file therefore has to be **writable on its first run** (the tool fails
closed with an explanation if it isn't: make it writable, or point
`$COB_LICENSE` at a writable copy). The signature is unaffected: it covers only
the part above `---`, which is why the record lives below it.

**Pre-bound** (`--machine <hash>`). If you already know the machine, have the
customer run `cob_sign machine-id` and issue:

    bin/cob_sign sign --key cob_private.key --customer "Acme Corp" \
        --machine <hash-from-customer> --out cob.lic

The machine id is then part of the *signed* payload: it cannot be removed or
changed without invalidating the signature, and the tool does not write to the
file.

### What binding does and does not do

* It stops a license from being casually copied to other computers, and it
  detects edits to the signed fields.
* The first-run record is **not signed** (only the private key can sign, and
  that never leaves your machine). Anyone who deletes or edits the
  `bound_machine=` line can bind the license to a different machine. The source
  is public, so there is no secret a client-side check could hold. If you need
  real enforcement, use pre-bound licenses (`--machine`) or an online activation
  service.
* A machine id is not available in every environment (some containers have no
  `/etc/machine-id`). A bound license then refuses to run; mount the host's
  file read-only into the container.
* A reinstalled OS usually gets a new machine id, so the license needs reissuing.

## Build the enterprise binaries

Once `keys/drm_pubkeys.txt` exists, plain `make` builds everything -- the free
tools AND the license-gated ones:

    make                        # free build + enterprise build
    make enterprise             # enterprise only
    make embed_enterprise       # embeddable package with the gated interpreter

Outputs: `bin/cob_interp_enterprise` (also copied to `cob_interp_full_enterprise`),
`bin/popcorn_comp_enterprise`, `bin/cob-<version>-embed_enterprise.zip`. Without
`keys/drm_pubkeys.txt` plain `make` just skips the enterprise step, so a fresh
clone still builds the free tools. The public key is compiled in at build time
(`crates/cob-drm/build.rs`), so changing the key file means rebuilding.

The free binaries contain no license code at all: the enterprise code is behind
a Cargo feature (`enterprise`) and built into a separate target directory.

Publish the enterprise files in `ftp/dist/ENTERPRISE/vX.Y.Z/`.

## Signing licenses by hand

`cob_sign` is optional. If you sign some other way, `keys/drm_pubkeys.txt` must
hold the public half of the key you sign with, and the file must look like:

    COBLIC1
    customer=Acme Corp
    issued=2026-10-22
    expires=2027-10-22          (or: never)
    features=*
    seats=50
    key_id=1
    bind=machine                (optional; or machine=<64 hex> to pre-bind)
    ---
    <base64 signature>

The signature covers every byte up to and including the newline before `---`.

**The signature scheme is EdDSA over Curve25519 with Blake2b-512 as the hash**
(what Monocypher's `crypto_eddsa_*` implements), **not standard Ed25519**, which
uses SHA-512. A stock Ed25519 library will not produce signatures this build
accepts. (An earlier version of this document said "plain Ed25519"; that was
inaccurate.) Use `cob_sign`, or Monocypher, or `crates/cob-drm/src/eddsa.rs`.

## Where Cob looks for cob.lic

`$COB_LICENSE`, `./cob.lic`, the per-user config dir (`~/.config/cob/cob.lic` or
`%APPDATA%\cob\cob.lic`), `/etc/cob/cob.lic`. For the embed package, have the
host app set `COB_LICENSE`.
