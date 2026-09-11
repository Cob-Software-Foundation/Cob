/* =====================================================================
 * farmer.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob package manager: `farmer harvest <package>`
 *
 * ---------------------------------------------------------------------
 * PROTOCOL
 * ---------------------------------------------------------------------
 * farmer talks to a completely static JSON API -- no server code, just
 * plain files hosted for free on GitHub Pages (or any static host, via
 * the COB_FARMER_BASE_URL override). For `farmer harvest <package>`:
 *
 *   1. GET  {base}/api/v1/packages/<package>.json
 *        -> {
 *             "name":    "<package>",
 *             "version": "1.0.0",
 *             "zip_url": "https://.../<package>-1.0.0.zip",
 *             "sha256":  "<hex digest, optional>"
 *           }
 *      The "name" field is checked against what was requested, so a
 *      misconfigured or spoofed metadata file is caught before
 *      anything gets downloaded and unzipped onto disk.
 *   2. GET  <zip_url>  ->  cob_modules/<package>.zip
 *      If "sha256" was present in the metadata, the downloaded zip is
 *      verified against it before extraction.
 *   3. Unzip into cob_modules/<package>/ -- the exact layout
 *      cob_interp.c's `shuck <package>` already looks for
 *      (cob_modules/<package>/<package>.cob).
 *
 * ---------------------------------------------------------------------
 * WHY system() AND NOT A REAL HTTP CLIENT
 * ---------------------------------------------------------------------
 * Per the project spec, farmer shells out to curl (Unix) or PowerShell
 * (Windows) rather than linking an HTTP/TLS library -- this keeps the
 * whole toolchain dependency-free and trivially portable, at the cost
 * of going through the shell. Because of that, every single value that
 * reaches a shell command line -- package name, URLs, file paths -- is
 * validated BEFORE being interpolated into a command string. This is
 * not optional hardening; without it, `farmer harvest "; rm -rf ~"`
 * would be a real vulnerability. See cob_validate_package_name() and
 * cob_shell_quote_single() below.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

/* Must come before any system headers: on glibc, popen()/pclose() and
 * strcasecmp() aren't visible under strict -std=c99 without requesting
 * POSIX explicitly. MSVC doesn't have them under these names at all
 * (it's _popen/_pclose/_stricmp instead) -- see the cob_popen/
 * cob_pclose/cob_strcasecmp wrappers just below the includes. */
#if !defined(_MSC_VER) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "common.h"
#include "file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_MSC_VER)
#  define cob_popen     _popen
#  define cob_pclose    _pclose
#  define cob_strcasecmp _stricmp
#else
#  include <strings.h> /* strcasecmp lives here on POSIX, not string.h */
#  define cob_popen     popen
#  define cob_pclose    pclose
#  define cob_strcasecmp strcasecmp
#endif

/* ---------------------------------------------------------------------
 * INPUT VALIDATION -- everything below exists to keep untrusted input
 * (argv, and JSON fields fetched from the network) from ever reaching
 * a shell command unescaped.
 * ------------------------------------------------------------------- */

/* Package names become part of URLs, file paths, and shell command
 * lines, so they're restricted to a conservative allowlist: letters,
 * digits, '-', and '_'. Nothing that could be a path separator, shell
 * metacharacter, or URL special character is permitted. Returns 1 if
 * valid and non-empty, 0 otherwise. */
static int cob_validate_package_name(const char *name) {
    size_t i, len;
    if (!name) return 0;
    len = strlen(name);
    if (len == 0 || len > 128) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '-' && c != '_') return 0;
    }
    return 1;
}

/* Wraps `s` in single quotes for safe use inside a POSIX shell command,
 * escaping any embedded single quotes as '\'' (the standard trick).
 * Returns a newly malloc'd string the caller must free, or NULL on
 * OOM. Used for every value -- including ones we've already validated
 * with cob_validate_package_name() -- as defense in depth. */
static char *cob_shell_quote_single(const char *s) {
    size_t len = strlen(s);
    size_t cap = len * 4 + 3; /* worst case: every char is a quote */
    char *out = (char *)malloc(cap);
    size_t i, j = 0;
    if (!out) return NULL;
    out[j++] = '\'';
    for (i = 0; i < len; i++) {
        if (s[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

/* Windows PowerShell equivalent: wraps in single quotes and doubles
 * any embedded single quotes (PowerShell's own escaping rule inside
 * single-quoted strings). */
static char *cob_powershell_quote_single(const char *s) {
    size_t len = strlen(s);
    size_t cap = len * 2 + 3;
    char *out = (char *)malloc(cap);
    size_t i, j = 0;
    if (!out) return NULL;
    out[j++] = '\'';
    for (i = 0; i < len; i++) {
        if (s[i] == '\'') { out[j++] = '\''; out[j++] = '\''; }
        else out[j++] = s[i];
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

/* URLs we build ourselves from a validated package name and a base
 * URL read from either a compiled-in constant or an environment
 * variable -- so in practice these components are already safe, but
 * we still quote everything at the shell layer as defense in depth. */

/* ---------------------------------------------------------------------
 * MINIMAL JSON FIELD EXTRACTION
 * ---------------------------------------------------------------------
 * farmer's metadata files are small, flat, single-level JSON objects
 * with string values -- {"name": "...", "version": "...", ...}. Rather
 * than vendor a full JSON parser for that, this looks for
 * "<field>"[:space:]*:[:space:]*"<value>" anywhere in the text and
 * unescapes \" \\ \n \t in the value. Returns a newly malloc'd string,
 * or NULL if the field wasn't found or was malformed.
 * ------------------------------------------------------------------- */
static char *json_extract_string_field(const char *json, const char *field) {
    char needle[160];
    const char *p;
    const char *val_start;
    char *out;
    size_t out_cap, out_len;

    snprintf(needle, sizeof(needle), "\"%s\"", field);
    p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    val_start = p;

    out_cap = 64; out_len = 0;
    out = (char *)malloc(out_cap);
    if (!out) return NULL;

    while (*p != '\0' && *p != '"') {
        char c = *p;
        if (c == '\\' && *(p + 1) != '\0') {
            char esc = *(p + 1);
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                default: c = esc; break;
            }
            p += 2;
        } else {
            p += 1;
        }
        if (out_len + 1 >= out_cap) {
            size_t new_cap = out_cap * 2;
            char *grown = (char *)realloc(out, new_cap);
            if (!grown) { free(out); return NULL; }
            out = grown; out_cap = new_cap;
        }
        out[out_len++] = c;
    }
    if (*p != '"') { free(out); return NULL; } /* unterminated */
    out[out_len] = '\0';
    (void)val_start;
    return out;
}

/* ---------------------------------------------------------------------
 * PLATFORM-SPECIFIC DOWNLOAD (via curl on Unix, PowerShell on Windows,
 * exactly as the project spec calls for)
 * ------------------------------------------------------------------- */

/* Downloads `url` to `out_path`. Returns 0 on success. Both the URL
 * and the output path are shell-quoted before being interpolated.
 * Success is verified by checking the output file actually exists and
 * is non-empty afterward -- system()'s return code alone isn't a
 * reliable signal across shells/platforms for network failures. */
static int cob_download(const char *url, const char *out_path) {
    char *cmd;
    int rc;

#if defined(COB_OS_WINDOWS)
    char *q_url = cob_powershell_quote_single(url);
    char *q_out = cob_powershell_quote_single(out_path);
    if (!q_url || !q_out) { free(q_url); free(q_out); return -1; }
    size_t cmd_cap = strlen(q_url) + strlen(q_out) + 256;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_url); free(q_out); return -1; }
    snprintf(cmd, cmd_cap,
        "powershell -NoProfile -NonInteractive -Command "
        "\"$ProgressPreference='SilentlyContinue'; "
        "Invoke-WebRequest -Uri %s -OutFile %s -UseBasicParsing\"",
        q_url, q_out);
    free(q_url); free(q_out);
#else
    char *q_url = cob_shell_quote_single(url);
    char *q_out = cob_shell_quote_single(out_path);
    if (!q_url || !q_out) { free(q_url); free(q_out); return -1; }
    size_t cmd_cap = strlen(q_url) + strlen(q_out) + 128;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_url); free(q_out); return -1; }
    snprintf(cmd, cmd_cap, "curl -fsSL -o %s %s", q_out, q_url);
    free(q_url); free(q_out);
#endif

    rc = system(cmd);
    free(cmd);
    if (rc != 0) return -1;
    if (!cob_file_exists(out_path)) return -1;
    return 0;
}

/* Extracts the zip at `zip_path` into directory `dest_dir` (created if
 * needed). Returns 0 on success. */
static int cob_unzip(const char *zip_path, const char *dest_dir) {
    char *cmd;
    int rc;

    COB_MKDIR(dest_dir);

#if defined(COB_OS_WINDOWS)
    char *q_zip = cob_powershell_quote_single(zip_path);
    char *q_dest = cob_powershell_quote_single(dest_dir);
    if (!q_zip || !q_dest) { free(q_zip); free(q_dest); return -1; }
    size_t cmd_cap = strlen(q_zip) + strlen(q_dest) + 128;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_zip); free(q_dest); return -1; }
    snprintf(cmd, cmd_cap,
        "powershell -NoProfile -NonInteractive -Command "
        "\"Expand-Archive -Path %s -DestinationPath %s -Force\"",
        q_zip, q_dest);
    free(q_zip); free(q_dest);
#else
    char *q_zip = cob_shell_quote_single(zip_path);
    char *q_dest = cob_shell_quote_single(dest_dir);
    if (!q_zip || !q_dest) { free(q_zip); free(q_dest); return -1; }
    size_t cmd_cap = strlen(q_zip) + strlen(q_dest) + 128;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_zip); free(q_dest); return -1; }
    snprintf(cmd, cmd_cap, "unzip -o -q %s -d %s", q_zip, q_dest);
    free(q_zip); free(q_dest);
#endif

    rc = system(cmd);
    free(cmd);
    return rc == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------
 * SHA-256 VERIFICATION (optional -- only if the metadata JSON supplied
 * a "sha256" field). Shells out to `sha256sum`/`shasum` (Unix) or
 * PowerShell's Get-FileHash (Windows) rather than vendoring a crypto
 * implementation, consistent with the rest of farmer's design.
 * ------------------------------------------------------------------- */
static int cob_verify_sha256(const char *file_path, const char *expected_hex) {
    char *cmd;
    char actual[256];
    FILE *pipe_fp;
    int match;

#if defined(COB_OS_WINDOWS)
    char *q_path = cob_powershell_quote_single(file_path);
    if (!q_path) return -1;
    size_t cmd_cap = strlen(q_path) + 128;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_path); return -1; }
    snprintf(cmd, cmd_cap,
        "powershell -NoProfile -NonInteractive -Command "
        "\"(Get-FileHash -Algorithm SHA256 -Path %s).Hash.ToLower()\"",
        q_path);
    free(q_path);
#else
    char *q_path = cob_shell_quote_single(file_path);
    if (!q_path) return -1;
    size_t cmd_cap = strlen(q_path) + 256;
    cmd = (char *)malloc(cmd_cap);
    if (!cmd) { free(q_path); return -1; }
    /* sha256sum on Linux, shasum -a 256 on macOS -- try sha256sum
     * first and fall back if it's not found. */
    snprintf(cmd, cmd_cap,
        "sha256sum %s 2>/dev/null | cut -d' ' -f1 || shasum -a 256 %s 2>/dev/null | cut -d' ' -f1",
        q_path, q_path);
    free(q_path);
#endif

    pipe_fp = cob_popen(cmd, "r");
    free(cmd);
    if (!pipe_fp) return -1;

    actual[0] = '\0';
    if (!fgets(actual, sizeof(actual), pipe_fp)) { cob_pclose(pipe_fp); return -1; }
    cob_pclose(pipe_fp);

    /* Trim trailing whitespace/newline. */
    size_t len = strlen(actual);
    while (len > 0 && (actual[len - 1] == '\n' || actual[len - 1] == '\r' || actual[len - 1] == ' ')) {
        actual[--len] = '\0';
    }

    match = (cob_strcasecmp(actual, expected_hex) == 0);
    return match ? 0 : -1;
}

/* ---------------------------------------------------------------------
 * `farmer harvest <package>`
 * ------------------------------------------------------------------- */
static int cmd_harvest(const char *package) {
    const char *base_url;
    char meta_url[1024];
    char meta_path[256];
    char zip_path[256];
    char dest_dir[256];
    CobFileBuffer meta_buf;
    char *name_field = NULL, *version_field = NULL, *zip_url_field = NULL, *sha256_field = NULL;
    int rc = 1;

    if (!cob_validate_package_name(package)) {
        fprintf(stderr,
            "farmer: error: '%s' is not a valid package name "
            "(letters, digits, '-', '_' only)\n", package);
        return 1;
    }

    base_url = getenv(COB_FARMER_BASE_URL_ENV);
    if (!base_url || base_url[0] == '\0') base_url = COB_FARMER_DEFAULT_BASE_URL;

    snprintf(meta_url, sizeof(meta_url), "%s/api/v1/packages/%s.json", base_url, package);
    snprintf(meta_path, sizeof(meta_path), "%s.meta.json.tmp", package);

    printf("farmer: fetching metadata for '%s'...\n", package);
    if (cob_download(meta_url, meta_path) != 0) {
        fprintf(stderr, "farmer: error: could not fetch %s\n", meta_url);
        return 1;
    }

    if (cob_file_read_all(meta_path, &meta_buf) != COB_FILE_OK) {
        fprintf(stderr, "farmer: error: could not read downloaded metadata\n");
        remove(meta_path);
        return 1;
    }

    name_field    = json_extract_string_field(meta_buf.data, "name");
    version_field = json_extract_string_field(meta_buf.data, "version");
    zip_url_field = json_extract_string_field(meta_buf.data, "zip_url");
    sha256_field  = json_extract_string_field(meta_buf.data, "sha256"); /* optional */
    cob_file_free(&meta_buf);
    remove(meta_path);

    if (!name_field || !version_field || !zip_url_field) {
        fprintf(stderr, "farmer: error: metadata for '%s' is missing required fields "
                         "(need name, version, zip_url)\n", package);
        goto cleanup;
    }

    /* Integrity check: the metadata's own "name" field must match what
     * was requested, catching a misconfigured or mismatched registry
     * entry before anything is downloaded and unzipped onto disk. */
    if (strcmp(name_field, package) != 0) {
        fprintf(stderr,
            "farmer: error: metadata name mismatch -- requested '%s' but "
            "server returned metadata for '%s'\n", package, name_field);
        goto cleanup;
    }

    printf("farmer: found %s v%s\n", name_field, version_field);

    snprintf(zip_path, sizeof(zip_path), "%s.zip.tmp", package);
    printf("farmer: downloading %s...\n", zip_url_field);
    if (cob_download(zip_url_field, zip_path) != 0) {
        fprintf(stderr, "farmer: error: could not fetch %s\n", zip_url_field);
        goto cleanup;
    }

    if (sha256_field) {
        printf("farmer: verifying sha256...\n");
        if (cob_verify_sha256(zip_path, sha256_field) != 0) {
            fprintf(stderr, "farmer: error: sha256 mismatch for '%s' -- refusing to install\n", package);
            remove(zip_path);
            goto cleanup;
        }
        printf("farmer: checksum OK\n");
    } else {
        printf("farmer: note: no sha256 provided in metadata, skipping checksum verification\n");
    }

    COB_MKDIR(COB_FARMER_MODULES_DIR);
    snprintf(dest_dir, sizeof(dest_dir), "%s%c%s", COB_FARMER_MODULES_DIR, COB_PATH_SEP, package);

    printf("farmer: unzipping into %s...\n", dest_dir);
    if (cob_unzip(zip_path, dest_dir) != 0) {
        fprintf(stderr, "farmer: error: failed to unzip %s\n", zip_path);
        remove(zip_path);
        goto cleanup;
    }
    remove(zip_path);

    printf("farmer: harvested %s v%s -> %s\n", name_field, version_field, dest_dir);
    printf("farmer: use `shuck %s` in your .cob file to import it\n", package);
    rc = 0;

cleanup:
    free(name_field); free(version_field); free(zip_url_field); free(sha256_field);
    return rc;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s harvest <package>\n"
        "\n"
        "  farmer harvest <package>   download and install a Cob package\n"
        "                             into %s/<package>/\n"
        "\n"
        "Environment:\n"
        "  %s   override the registry base URL (default: %s)\n",
        argv0, COB_FARMER_MODULES_DIR, COB_FARMER_BASE_URL_ENV, COB_FARMER_DEFAULT_BASE_URL);
}

int main(int argc, char **argv) {
    if (cob_check_hidden_version_flag(argc, argv)) return 0;

    if (argc < 2) { print_usage(argv[0]); return 1; }

    if (strcmp(argv[1], "harvest") == 0) {
        if (argc != 3) {
            fprintf(stderr, "farmer: error: 'harvest' needs exactly one package name\n");
            print_usage(argv[0]);
            return 1;
        }
        return cmd_harvest(argv[2]);
    }

    fprintf(stderr, "farmer: error: unknown command '%s'\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
