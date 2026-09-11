/* =====================================================================
 * common.h
 * ---------------------------------------------------------------------
 * Project:      Cob Language Toolchain            (file ext: .cob)
 * Codename:     Project Obsidian Falcon
 * Author:       Pixel-Pulse
 * Repository:   https://github.com/pixel-pulse-labs/Cob
 *
 * This header is included by every Cob tool (cob_interp, popcorn_comp,
 * farmer). It centralizes:
 *   - the corn-themed language vocabulary (as string constants, so the
 *     lexer/parser in cob_interp.c has one place to update keywords),
 *   - version / build metadata,
 *   - the project's license notice, and
 *   - small cross-platform helper macros (OS/arch detection) used by
 *     every other file in this repository.
 *
 * License: PolyForm Noncommercial License 1.0.0
 *          <https://polyformproject.org/licenses/noncommercial/1.0.0>
 *          See LICENSE.md at the repository root for the full text.
 *          Required Notice: Copyright Pixel-Pulse (pixel-pulse.duckdns.org)
 * ===================================================================== */

#ifndef COB_COMMON_H
#define COB_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * 1. VERSION / BUILD METADATA
 * ------------------------------------------------------------------- */
#define COB_PROJECT_NAME        "Cob Language"
#define COB_PROJECT_CODENAME    "Project Obsidian Falcon"
#define COB_VERSION_MAJOR       0
#define COB_VERSION_MINOR       0
#define COB_VERSION_PATCH       1
#define COB_VERSION_STRING      "0.0.1"
#define COB_AUTHOR_HANDLE       "Pixel-Pulse"
#define COB_REPO_URL            "https://github.com/pixel-pulse-labs/Cob"

/* Hidden --version banner. Every Cob CLI tool (cob_interp, popcorn_comp,
 * farmer) should call cob_check_hidden_version_flag(argc, argv) very
 * early in main(). If the flag was present, it prints the banner below
 * and the caller should exit(0) immediately. This is intentionally NOT
 * advertised in --help output -- it's an easter egg for the toolchain
 * maintainers and curious users who read the source. */
#define COB_VERSION_BANNER \
    "Cob Language v" COB_VERSION_STRING "  - Handcrafted by Pixel-Pulse"

/* Scans argv for a literal "--version" token. Returns 1 (and prints the
 * banner to stdout) if found, otherwise returns 0 and prints nothing. */
static inline int cob_check_hidden_version_flag(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i] != NULL && strcmp(argv[i], "--version") == 0) {
            printf("%s\n", COB_VERSION_BANNER);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * 2. LICENSE NOTICE (short form)
 *    The authoritative, full license text lives in LICENSE.md at the
 *    repository root -- per the PolyForm terms, that file (or the URL
 *    below) MUST travel with any redistributed copy of this software.
 *    This macro is just a short, embeddable notice tools may print via
 *    `--license`.
 * ------------------------------------------------------------------- */
#define COB_LICENSE_NAME  "PolyForm Noncommercial License 1.0.0"
#define COB_LICENSE_URL   "https://polyformproject.org/licenses/noncommercial/1.0.0"
#define COB_LICENSE_NOTICE \
    COB_PROJECT_NAME " is licensed under the " COB_LICENSE_NAME ".\n" \
    "Full terms: " COB_LICENSE_URL "\n" \
    "Required Notice: Copyright Pixel-Pulse (pixel-pulse.duckdns.org)\n" \
    "This is a noncommercial license: permitted uses are personal,\n" \
    "educational, charitable, and governmental. See LICENSE.md.\n"

static inline void cob_print_license(void) {
    fputs(COB_LICENSE_NOTICE, stdout);
}

/* ---------------------------------------------------------------------
 * 3. CROSS-PLATFORM OS / ARCH DETECTION
 *    Used by file_io, popcorn_comp (for CobOS/CobArch cross-compiling)
 *    and farmer (to pick curl vs PowerShell).
 * ------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#   define COB_OS_WINDOWS 1
#   define COB_OS_NAME "windows"
#   include <direct.h>
#   define COB_PATH_SEP '\\'
#   define COB_MKDIR(path) _mkdir(path)
#elif defined(__APPLE__) && defined(__MACH__)
#   define COB_OS_DARWIN 1
#   define COB_OS_NAME "darwin"
#   include <sys/stat.h>
#   define COB_PATH_SEP '/'
#   define COB_MKDIR(path) mkdir(path, 0755)
#elif defined(__linux__)
#   define COB_OS_LINUX 1
#   define COB_OS_NAME "linux"
#   include <sys/stat.h>
#   define COB_PATH_SEP '/'
#   define COB_MKDIR(path) mkdir(path, 0755)
#else
#   define COB_OS_UNKNOWN 1
#   define COB_OS_NAME "unknown"
#   define COB_PATH_SEP '/'
#   define COB_MKDIR(path) (0)
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#   define COB_ARCH_NAME "amd64"
#elif defined(__i386__) || defined(_M_IX86)
#   define COB_ARCH_NAME "386"
#elif defined(__aarch64__) || defined(_M_ARM64)
#   define COB_ARCH_NAME "arm64"
#elif defined(__arm__) || defined(_M_ARM)
#   define COB_ARCH_NAME "arm"
#else
#   define COB_ARCH_NAME "unknown"
#endif

/* ---------------------------------------------------------------------
 * 4. COB VOCABULARY (v0.0.1 SPEC)
 *    The full corn-themed keyword set recognized by cob_interp.c and
 *    popcorn_comp.c. Declared here as string literals so every tool
 *    (and the shuck-time library scanner in the interpreter) agrees on
 *    exact spelling. NOTE: these are the raw token spellings; the
 *    interpreter's tokenizer does the whitespace/indent-sensitive
 *    parsing around them.
 * ------------------------------------------------------------------- */
#define COB_KW_WHILE        "while"      /* while <condition>:            */
#define COB_KW_SET           "set"       /* set <name> = <value>          */
#define COB_KW_POP            "pop"      /* pop("text") -> stdout output   */
#define COB_KW_SHUCK          "shuck"    /* shuck <library_name>          */
#define COB_KW_HARVEST        "harvest"  /* harvest(<bytes>) [--no-gc]     */
#define COB_KW_TRASH          "trash"    /* trash(<variable>) [--no-gc]    */
#define COB_BLOCK_COLON       ':'        /* block opener terminator        */

/* CLI flag recognized by popcorn_comp.c / cob_interp.c that unlocks the
 * low-level manual-memory keywords (harvest/trash). Off by default --
 * Cob is garbage-collected unless the farmer/dev explicitly opts out. */
#define COB_FLAG_NO_GC        "--no-gc"

/* Fast-boot bytecode cache file extension written by cob_interp.c next
 * to the source .cob file, e.g. "myprogram.cob" -> "myprogram.strawberry" */
#define COB_CACHE_EXTENSION   ".strawberry"

/* Source file extension recognized by all Cob tools. */
#define COB_SOURCE_EXTENSION  ".cob"

/* ---------------------------------------------------------------------
 * 5. SMALL SHARED UTILITIES
 * ------------------------------------------------------------------- */

/* Returns 1 if `str` ends with `suffix`, else 0. Used to detect .cob /
 * .strawberry files and to build cache file names. */
static inline int cob_str_ends_with(const char *str, const char *suffix) {
    size_t len_str, len_suffix;
    if (!str || !suffix) return 0;
    len_str = strlen(str);
    len_suffix = strlen(suffix);
    if (len_suffix > len_str) return 0;
    return strcmp(str + (len_str - len_suffix), suffix) == 0;
}

/* Counts leading ASCII space characters on a line (tabs are treated as
 * a hard error upstream by the caller -- Cob v0.0.1 is spaces-only, in
 * the spirit of Python's recommended style). Returns the count. */
static inline int cob_count_leading_spaces(const char *line) {
    int n = 0;
    while (line[n] == ' ') n++;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* COB_COMMON_H */
