/* =====================================================================
 * cob_interp.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob interpreter binary: `cob_interp <file.cob>`
 *
 * ---------------------------------------------------------------------
 * v0.0.1 SCOPE (by design, per project decision)
 * ---------------------------------------------------------------------
 * v0.0.1 deliberately narrows to ONE fully working feature end-to-end:
 *
 *     pop("some text")      -> prints  some text
 *
 * Everything else in the eventual language spec -- `while <cond>:`
 * blocks, `set <name> = <value>`, `shuck <lib>`, and the --no-gc-gated
 * `harvest(<bytes>)` / `trash(<var>)` pair -- is intentionally left as
 * a clearly-marked, non-crashing stub below (see cob_handle_reserved_
 * keyword()). They are *recognized* by the tokenizer (so a .cob file
 * that uses them doesn't blow up with a confusing parse error) but do
 * not execute yet. This keeps the interpreter small, correct, and easy
 * to extend in v0.0.2+ rather than half-implementing five features.
 *
 * What IS fully implemented in this file:
 *   - Loading a .cob file into memory (via file_io.h)
 *   - Line-by-line scanning with leading-space (indentation) tracking,
 *     so indentation errors are caught even though no block keyword
 *     needs it yet.
 *   - A tiny, correct string-literal parser for pop("...") with \\,
 *     \", and \n escape support.
 *   - A minimal .strawberry fast-boot bytecode cache: on first run,
 *     the parsed pop statements are serialized to <file>.strawberry.
 *     On subsequent runs, if that cache file already exists, it is
 *     loaded and executed directly, skipping the text parse entirely.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#include "common.h"
#include "file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * .strawberry CACHE FORMAT (v0.0.1)
 * ---------------------------------------------------------------------
 * All integers are little-endian, written with explicit byte shifts so
 * the file is byte-identical regardless of host endianness or struct
 * padding (no raw struct dumps -- keeps this portable across MSVC,
 * MinGW, GCC, and Clang without pragma pack games).
 *
 *   offset 0   : 8 bytes   magic "COBSTRW\0"
 *   offset 8   : 1 byte    format version (currently 1)
 *   offset 9   : 4 bytes   uint32 statement_count
 *   then, statement_count times:
 *     1 byte    opcode (0x01 = OP_POP, the only opcode in v0.0.1)
 *     4 bytes   uint32 text_length
 *     N bytes   raw UTF-8 text to print (already un-escaped)
 * ------------------------------------------------------------------- */
#define STRAWBERRY_MAGIC       "COBSTRW\0"
#define STRAWBERRY_MAGIC_LEN   8
#define STRAWBERRY_FORMAT_VER  1
#define OP_POP                 0x01

/* A single parsed Cob statement. v0.0.1 only ever produces OP_POP
 * statements, but the struct is shaped so later opcodes slot in
 * without a rewrite. */
typedef struct {
    uint8_t  opcode;
    char    *text;   /* heap-owned, un-escaped, NUL-terminated */
    size_t   text_len;
} CobStatement;

typedef struct {
    CobStatement *items;
    size_t        count;
    size_t        capacity;
} CobProgram;

static void program_init(CobProgram *p) {
    p->items = NULL;
    p->count = 0;
    p->capacity = 0;
}

static void program_free(CobProgram *p) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        free(p->items[i].text);
    }
    free(p->items);
    p->items = NULL;
    p->count = 0;
    p->capacity = 0;
}

static int program_push(CobProgram *p, uint8_t opcode, char *owned_text, size_t text_len) {
    if (p->count == p->capacity) {
        size_t new_cap = (p->capacity == 0) ? 16 : p->capacity * 2;
        CobStatement *grown = (CobStatement *)realloc(p->items, new_cap * sizeof(CobStatement));
        if (!grown) return -1;
        p->items = grown;
        p->capacity = new_cap;
    }
    p->items[p->count].opcode   = opcode;
    p->items[p->count].text     = owned_text;
    p->items[p->count].text_len = text_len;
    p->count++;
    return 0;
}

/* ---------------------------------------------------------------------
 * STRING LITERAL PARSING FOR  pop("...")
 * ------------------------------------------------------------------- */

/* Parses a double-quoted Cob string literal starting at `src[0] == '"'`.
 * Supports \\  \"  \n  \t escapes. Writes a newly malloc'd, un-escaped,
 * NUL-terminated string to *out_text (caller frees) and its length to
 * *out_len. Returns a pointer to the character immediately AFTER the
 * closing quote on success, or NULL if the literal was malformed
 * (unterminated string, etc). */
static const char *parse_string_literal(const char *src, char **out_text, size_t *out_len) {
    const char *p;
    char *buf;
    size_t buf_cap;
    size_t buf_len;

    if (*src != '"') return NULL;
    p = src + 1;

    buf_cap = 32;
    buf_len = 0;
    buf = (char *)malloc(buf_cap);
    if (!buf) return NULL;

    while (*p != '\0' && *p != '"') {
        char c = *p;
        if (c == '\\' && *(p + 1) != '\0') {
            char esc = *(p + 1);
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                default:   c = esc;  break; /* unknown escape: keep literal char */
            }
            p += 2;
        } else {
            p += 1;
        }

        if (buf_len + 1 >= buf_cap) {
            size_t new_cap = buf_cap * 2;
            char *grown = (char *)realloc(buf, new_cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
            buf_cap = new_cap;
        }
        buf[buf_len++] = c;
    }

    if (*p != '"') { /* unterminated string literal */
        free(buf);
        return NULL;
    }

    buf[buf_len] = '\0';
    *out_text = buf;
    *out_len  = buf_len;
    return p + 1; /* skip the closing quote */
}

/* Skips ASCII spaces/tabs in-place, returning the first non-blank char. */
static const char *skip_blank(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ---------------------------------------------------------------------
 * RESERVED-BUT-NOT-YET-IMPLEMENTED KEYWORDS
 * ---------------------------------------------------------------------
 * Called when a trimmed line starts with one of the future keywords
 * (while / set / shuck / harvest / trash). We don't execute them in
 * v0.0.1, but we also don't want the interpreter to choke -- it prints
 * a single, clear notice (once per statement) and moves on, so authors
 * writing forward-looking .cob files aren't blindsided by a crash.
 * ------------------------------------------------------------------- */
static int cob_handle_reserved_keyword(const char *trimmed_line, int line_no, const char *matched_kw) {
    fprintf(stderr,
            "[cob_interp] note: line %d uses reserved keyword '%s' "
            "(not implemented in v0.0.1 -- only pop() runs today): %s\n",
            line_no, matched_kw, trimmed_line);
    return 0;
}

/* Returns 1 and sets *kw_out if `trimmed_line` begins with a reserved
 * future keyword followed by a word boundary (space, '(', or ':'). */
static int cob_line_is_reserved_keyword(const char *trimmed_line, const char **kw_out) {
    static const char *reserved[] = {
        COB_KW_WHILE, COB_KW_SET, COB_KW_SHUCK, COB_KW_HARVEST, COB_KW_TRASH, NULL
    };
    int i;
    for (i = 0; reserved[i] != NULL; i++) {
        size_t kw_len = strlen(reserved[i]);
        if (strncmp(trimmed_line, reserved[i], kw_len) == 0) {
            char boundary = trimmed_line[kw_len];
            if (boundary == ' ' || boundary == '(' || boundary == ':' || boundary == '\0') {
                *kw_out = reserved[i];
                return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * PARSER: .cob TEXT -> CobProgram
 * ------------------------------------------------------------------- */

/* Parses one logical line already known to start (after indentation)
 * with "pop(". Expects: pop("literal") [optional trailing whitespace].
 * On success, pushes an OP_POP statement onto `prog` and returns 0.
 * On malformed input, prints a diagnostic and returns -1. */
static int parse_pop_statement(const char *trimmed_line, int line_no, CobProgram *prog) {
    const char *p = trimmed_line + strlen(COB_KW_POP); /* just past "pop" */
    char *text = NULL;
    size_t text_len = 0;

    p = skip_blank(p);
    if (*p != '(') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '(' after pop\n", line_no);
        return -1;
    }
    p++; /* skip '(' */
    p = skip_blank(p);

    p = parse_string_literal(p, &text, &text_len);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: malformed string literal in pop()\n", line_no);
        return -1;
    }

    p = skip_blank(p);
    if (*p != ')') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ')' to close pop(\n", line_no);
        free(text);
        return -1;
    }
    /* Anything after the closing ')' on the same line is ignored in
     * v0.0.1 (no statement chaining / trailing comments handling yet
     * beyond a simple '#' check done by the caller). */

    if (program_push(prog, OP_POP, text, text_len) != 0) {
        fprintf(stderr, "[cob_interp] out of memory building program\n");
        free(text);
        return -1;
    }
    return 0;
}

/* Splits `source` into lines and builds a CobProgram. Tracks leading
 * space counts per non-blank line (indentation) purely for validation
 * right now: v0.0.1 has no block statements that need nested depth, so
 * the only rule enforced is "no tabs" (Cob is spaces-only, matching
 * the project's Python-style indentation design). Returns 0 on success
 * (even if the program is empty), -1 on a hard parse error. */
static int cob_parse_source(const char *source, CobProgram *prog) {
    const char *line_start = source;
    int line_no = 0;

    /* When we hit a reserved block-opener keyword (currently only
     * `while <cond>:`) that we can't execute yet, we must not let its
     * indented body fall through and run as top-level statements.
     * `skipping_block` tracks that we're inside such an unimplemented
     * block, and `skip_base_indent` is the indentation of the keyword
     * line itself -- any subsequent line indented MORE than that is
     * part of the unexecuted body and is skipped outright; a line
     * indented the same or less means the block has ended. */
    int skipping_block = 0;
    int skip_base_indent = 0;

    while (*line_start != '\0') {
        const char *line_end = strchr(line_start, '\n');
        size_t raw_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line_buf;
        const char *trimmed;
        int indent;

        line_no++;

        /* Copy the line into a scratch buffer (bounded, freed below) so
         * we can safely NUL-terminate it for string functions -- the
         * source buffer itself is treated as read-only. */
        line_buf = (char *)malloc(raw_len + 1);
        if (!line_buf) {
            fprintf(stderr, "[cob_interp] out of memory scanning line %d\n", line_no);
            return -1;
        }
        memcpy(line_buf, line_start, raw_len);
        line_buf[raw_len] = '\0';

        /* Strip a single trailing '\r' so CRLF source files (common on
         * Windows) don't corrupt string literals or indentation counts. */
        if (raw_len > 0 && line_buf[raw_len - 1] == '\r') {
            line_buf[raw_len - 1] = '\0';
        }

        /* Reject tabs used for indentation -- Cob v0.0.1 is spaces-only. */
        if (line_buf[0] == '\t') {
            fprintf(stderr,
                    "[cob_interp] indentation error at line %d: tabs are not allowed, use spaces\n",
                    line_no);
            free(line_buf);
            program_free(prog);
            return -1;
        }

        indent = cob_count_leading_spaces(line_buf);
        trimmed = skip_blank(line_buf);

        /* Blank line or a full-line comment ('#'): skip, and don't let
         * it count as a dedent that would end an unimplemented block
         * (blank lines carry no meaningful indentation). */
        if (*trimmed == '\0' || *trimmed == '#') {
            free(line_buf);
            line_start = line_end ? line_end + 1 : line_start + raw_len;
            continue;
        }

        /* If we're inside the body of an unimplemented block (e.g. a
         * `while ...:` we can't execute yet), skip any line indented
         * deeper than the block header -- it belongs to that block and
         * must NOT run as a stray top-level statement. Once indentation
         * returns to <= the header's level, the block has ended. */
        if (skipping_block) {
            if (indent > skip_base_indent) {
                free(line_buf);
                line_start = line_end ? line_end + 1 : line_start + raw_len;
                continue;
            }
            skipping_block = 0; /* dedent: fall through and parse this line normally */
        }

        if (strncmp(trimmed, COB_KW_POP, strlen(COB_KW_POP)) == 0) {
            const char *after_kw = trimmed + strlen(COB_KW_POP);
            if (*after_kw == '(' || *after_kw == ' ') {
                if (parse_pop_statement(trimmed, line_no, prog) != 0) {
                    free(line_buf);
                    program_free(prog);
                    return -1;
                }
                free(line_buf);
                line_start = line_end ? line_end + 1 : line_start + raw_len;
                continue;
            }
        }

        {
            const char *matched_kw = NULL;
            if (cob_line_is_reserved_keyword(trimmed, &matched_kw)) {
                size_t trimmed_len = strlen(trimmed);
                cob_handle_reserved_keyword(trimmed, line_no, matched_kw);

                /* A block-opener keyword (only `while ...:` in the
                 * v0.0.1 spec) ends its header with ':'. Since we can't
                 * execute the block yet, mark everything more-indented
                 * than this line as unexecuted body, not stray
                 * top-level statements. */
                if (trimmed_len > 0 && trimmed[trimmed_len - 1] == COB_BLOCK_COLON) {
                    skipping_block = 1;
                    skip_base_indent = indent;
                }

                free(line_buf);
                line_start = line_end ? line_end + 1 : line_start + raw_len;
                continue;
            }
        }

        fprintf(stderr, "[cob_interp] syntax error at line %d: unrecognized statement: %s\n",
                line_no, trimmed);
        free(line_buf);
        program_free(prog);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * EXECUTION
 * ------------------------------------------------------------------- */
static void cob_execute(const CobProgram *prog) {
    size_t i;
    for (i = 0; i < prog->count; i++) {
        const CobStatement *s = &prog->items[i];
        switch (s->opcode) {
            case OP_POP:
                fwrite(s->text, 1, s->text_len, stdout);
                fputc('\n', stdout);
                break;
            default:
                fprintf(stderr, "[cob_interp] internal error: unknown opcode 0x%02x\n", s->opcode);
                break;
        }
    }
}

/* ---------------------------------------------------------------------
 * .strawberry CACHE SERIALIZATION
 * ------------------------------------------------------------------- */

static void put_u32_le(unsigned char *dst, uint32_t v) {
    dst[0] = (unsigned char)(v & 0xFF);
    dst[1] = (unsigned char)((v >> 8) & 0xFF);
    dst[2] = (unsigned char)((v >> 16) & 0xFF);
    dst[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const unsigned char *src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

/* Serializes `prog` to the .strawberry binary format and writes it to
 * `cache_path`. Non-fatal on failure (a cache write failure should
 * never stop the program from having already run) -- logs a warning
 * instead of propagating an error. */
static void cob_write_strawberry_cache(const char *cache_path, const CobProgram *prog) {
    unsigned char *buf;
    size_t cap, len = 0;
    size_t i;
    CobFileStatus status;

    cap = STRAWBERRY_MAGIC_LEN + 1 + 4 + 64; /* rough starting estimate */
    for (i = 0; i < prog->count; i++) {
        cap += 1 + 4 + prog->items[i].text_len;
    }

    buf = (unsigned char *)malloc(cap);
    if (!buf) {
        fprintf(stderr, "[cob_interp] warning: could not allocate .strawberry cache buffer\n");
        return;
    }

    memcpy(buf + len, STRAWBERRY_MAGIC, STRAWBERRY_MAGIC_LEN);
    len += STRAWBERRY_MAGIC_LEN;

    buf[len++] = STRAWBERRY_FORMAT_VER;

    put_u32_le(buf + len, (uint32_t)prog->count);
    len += 4;

    for (i = 0; i < prog->count; i++) {
        const CobStatement *s = &prog->items[i];
        buf[len++] = s->opcode;
        put_u32_le(buf + len, (uint32_t)s->text_len);
        len += 4;
        memcpy(buf + len, s->text, s->text_len);
        len += s->text_len;
    }

    status = cob_file_write_all(cache_path, buf, len);
    free(buf);

    if (status != COB_FILE_OK) {
        fprintf(stderr, "[cob_interp] warning: failed to write fast-boot cache '%s'\n", cache_path);
    }
}

/* Attempts to load and validate a .strawberry cache into `prog`.
 * Returns 0 on success, -1 if the cache is missing, truncated, or has
 * a bad magic/version (any of which just means "fall back to parsing
 * the .cob source normally" -- never a fatal condition). */
static int cob_load_strawberry_cache(const char *cache_path, CobProgram *prog) {
    CobFileBuffer file;
    const unsigned char *buf;
    size_t len;
    size_t pos;
    uint32_t count, i;

    if (cob_file_read_all(cache_path, &file) != COB_FILE_OK) {
        return -1;
    }

    buf = (const unsigned char *)file.data;
    len = file.size;
    pos = 0;

    if (len < STRAWBERRY_MAGIC_LEN + 1 + 4 ||
        memcmp(buf, STRAWBERRY_MAGIC, STRAWBERRY_MAGIC_LEN) != 0) {
        cob_file_free(&file);
        return -1;
    }
    pos += STRAWBERRY_MAGIC_LEN;

    if (buf[pos] != STRAWBERRY_FORMAT_VER) {
        cob_file_free(&file);
        return -1;
    }
    pos += 1;

    count = get_u32_le(buf + pos);
    pos += 4;

    program_init(prog);

    for (i = 0; i < count; i++) {
        uint8_t opcode;
        uint32_t text_len;
        char *text_copy;

        if (pos + 1 + 4 > len) { program_free(prog); cob_file_free(&file); return -1; }
        opcode = buf[pos]; pos += 1;
        text_len = get_u32_le(buf + pos); pos += 4;

        if (pos + text_len > len) { program_free(prog); cob_file_free(&file); return -1; }

        text_copy = (char *)malloc((size_t)text_len + 1);
        if (!text_copy) { program_free(prog); cob_file_free(&file); return -1; }
        memcpy(text_copy, buf + pos, text_len);
        text_copy[text_len] = '\0';
        pos += text_len;

        if (program_push(prog, opcode, text_copy, text_len) != 0) {
            free(text_copy);
            program_free(prog);
            cob_file_free(&file);
            return -1;
        }
    }

    cob_file_free(&file);
    return 0;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <file.cob> [--no-cache]\n"
        "  --no-cache   ignore/skip writing the .strawberry fast-boot cache\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *src_path = NULL;
    int use_cache = 1;
    char cache_path[1024];
    CobProgram prog;
    int i;

    if (cob_check_hidden_version_flag(argc, argv)) {
        return 0;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Flags (e.g. --no-cache) may appear before or after the source
     * path -- scan the whole argv rather than assuming a fixed slot,
     * so `cob_interp --no-cache file.cob` and `cob_interp file.cob
     * --no-cache` both work. The first non-flag argument is the file. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            use_cache = 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[cob_interp] error: unrecognized flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (src_path == NULL) {
            src_path = argv[i];
        } else {
            fprintf(stderr, "[cob_interp] error: unexpected extra argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (src_path == NULL) {
        fprintf(stderr, "[cob_interp] error: no source file given\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!cob_str_ends_with(src_path, COB_SOURCE_EXTENSION)) {
        fprintf(stderr, "[cob_interp] error: expected a %s file, got '%s'\n",
                COB_SOURCE_EXTENSION, src_path);
        return 1;
    }

    if (cob_make_cache_path(src_path, cache_path, sizeof(cache_path)) != 0) {
        fprintf(stderr, "[cob_interp] error: source path too long to derive cache path\n");
        return 1;
    }

    program_init(&prog);

    /* Fast-boot path: if a .strawberry cache already sits next to the
     * source file, skip parsing entirely and run straight from it. */
    if (use_cache && cob_file_exists(cache_path)) {
        if (cob_load_strawberry_cache(cache_path, &prog) == 0) {
            cob_execute(&prog);
            program_free(&prog);
            return 0;
        }
        /* Corrupt/incompatible cache: fall through and reparse from
         * source, then overwrite the cache with a fresh copy below. */
        fprintf(stderr, "[cob_interp] note: cache '%s' unreadable, reparsing source\n", cache_path);
        program_init(&prog);
    }

    {
        CobFileBuffer source;
        CobFileStatus status = cob_file_read_all(src_path, &source);
        if (status != COB_FILE_OK) {
            fprintf(stderr, "[cob_interp] error: could not read source file '%s'\n", src_path);
            return 1;
        }

        if (cob_parse_source(source.data, &prog) != 0) {
            cob_file_free(&source);
            return 1;
        }
        cob_file_free(&source);
    }

    cob_execute(&prog);

    if (use_cache) {
        cob_write_strawberry_cache(cache_path, &prog);
    }

    program_free(&prog);
    return 0;
}
