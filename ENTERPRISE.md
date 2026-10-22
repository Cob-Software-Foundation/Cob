# Cob Enterprise build (license-gated)

The free Cob build is unchanged and needs no license. The Enterprise build is a
separate set of binaries (`*_enterprise`) that refuse to start without a valid,
signed `cob.lic`.

## One-time setup (maintainer's PC only)

    make cob_sign
    bin/cob_sign keygen            # writes cob_private.key + include/drm_pubkey.h

* `cob_private.key` is SECRET: keep it off GitHub, off servers, back it up offline.
  (`*.key`, `cob_private*` and `*.lic` are already in .gitignore.)
* `include/drm_pubkey.h` is the PUBLIC key; it is safe to commit.

## Issue a license

    bin/cob_sign sign --key cob_private.key --customer "Acme Corp" \
        --expires 2027-10-22 --features "*" --seats 50 --out cob.lic

## Build the enterprise binaries

Once `include/drm_pubkey.h` exists, plain `make` builds everything -- the free
tools AND the license-gated ones:

    make                        # free build + enterprise build
    make enterprise             # enterprise only
    make embed_enterprise       # embeddable package with the gated interpreter

Outputs: `bin/cob_interp_full_enterprise`, `bin/popcorn_comp_enterprise`,
`bin/cob-<version>-embed_enterprise.zip`. Without `drm_pubkey.h` plain `make`
just skips the enterprise step, so a fresh clone still builds the free tools.

Publish the enterprise files in `ftp/dist/ENTERPRISE/vX.Y.Z/`.

## Signing licenses by hand

`cob_sign` is optional. If you sign some other way, `include/drm_pubkey.h` must
hold the public half of the key you sign with, and the file must look like:

    COBLIC1
    customer=Acme Corp
    issued=2026-10-22
    expires=2027-10-22          (or: never)
    features=*
    seats=50
    key_id=1
    ---
    <base64 Ed25519 signature>

The signature covers every byte up to and including the newline before `---`
(plain Ed25519, not pre-hashed).

## Where Cob looks for cob.lic

`$COB_LICENSE`, `./cob.lic`, the per-user config dir, `/etc/cob/cob.lic`.
For the embed package, have the host app set `COB_LICENSE`.
