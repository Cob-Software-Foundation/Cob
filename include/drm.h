/* =====================================================================
 * drm.h -- Cob Enterprise license verification
 * ---------------------------------------------------------------------
 * ONLY compiled into the enterprise build (`make ENTERPRISE=1`, which
 * defines COB_ENTERPRISE). The free build never includes this header
 * or src/drm.c, so free users carry no license code at all.
 *
 * Model: a license is a small text file (cob.lic) signed with an
 * Ed25519 private key that lives only on the maintainer's machine.
 * This code holds the matching PUBLIC key (include/drm_pubkey.h) and
 * can verify licenses but cannot create them.
 *
 * File format (LF line endings; the signature covers every byte up to
 * and including the newline before the "---" line):
 *
 *     COBLIC1
 *     customer=Acme Corp
 *     issued=2026-10-22
 *     expires=2027-10-22          (or: never)
 *     features=audit_log,sso      (or: *)
 *     seats=50
 *     key_id=1
 *     ---
 *     <base64 Ed25519 signature>
 * ===================================================================== */
#ifndef COB_DRM_H
#define COB_DRM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   customer[128];
    char   issued[16];
    char   expires[16];     /* "YYYY-MM-DD" or "never" */
    char   features[256];   /* comma list, or "*" */
    int    seats;
    int    key_id;
    time_t expires_at;      /* 0 = never */
} CobLicense;

enum {
    COB_LIC_OK = 0,
    COB_LIC_NOT_FOUND,
    COB_LIC_MALFORMED,
    COB_LIC_UNKNOWN_KEY,
    COB_LIC_BAD_SIGNATURE,
    COB_LIC_EXPIRED
};

const char *cob_lic_strerror(int code);

/* Verify license text against an explicit 32-byte public key.
 * (Used by tools/cob_sign; the products use cob_lic_load below.) */
int cob_lic_verify_text(const char *text, size_t len,
                        const uint8_t pubkey[32], CobLicense *out);

#ifndef COB_LIC_NO_EMBEDDED_KEYS
/* Look for a license (explicit path, $COB_LICENSE, ./cob.lic,
 * ~/.config/cob/cob.lic or %APPDATA%\cob\cob.lic, /etc/cob/cob.lic),
 * verify it against the embedded key(s), and fill *out. */
int cob_lic_load(const char *explicit_path, CobLicense *out);

/* Startup gate for enterprise tools: loads + verifies the license.
 * Prints a clear error and returns nonzero on failure; silent on
 * success (no banners, no nagging). */
int cob_lic_require(const char *tool_name);

/* After a successful cob_lic_require(): is this feature unlocked? */
int cob_feature_enabled(const char *feature);
const CobLicense *cob_lic_current(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* COB_DRM_H */
