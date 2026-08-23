/* =====================================================================
 * file_io.h
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 *
 * Cross-platform helpers for loading a source file fully into memory as
 * a NUL-terminated string buffer. Used by cob_interp.c to load .cob
 * files and by popcorn_comp.c to load .strawberry bytecode files.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#ifndef COB_FILE_IO_H
#define COB_FILE_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes returned by the loader functions below. */
typedef enum {
    COB_FILE_OK = 0,
    COB_FILE_ERR_NOT_FOUND,
    COB_FILE_ERR_READ,
    COB_FILE_ERR_OOM,
    COB_FILE_ERR_EMPTY
} CobFileStatus;

/* A loaded file, fully buffered in memory. `data` is always
 * NUL-terminated (one extra byte is allocated beyond `size`), which
 * makes it safe to treat as a C string for text sources like .cob
 * files, while `size` gives the exact byte count for binary payloads
 * like .strawberry caches. Free with cob_file_free(). */
typedef struct {
    char   *data;
    size_t  size;
} CobFileBuffer;

/* Reads the entire contents of `path` into a newly allocated buffer.
 * Works identically on Windows, Linux, and macOS (opens in binary mode
 * to avoid platform-specific newline translation surprises).
 *
 * On success returns COB_FILE_OK and populates *out. On failure, *out
 * is zeroed and an error code is returned; nothing needs to be freed. */
CobFileStatus cob_file_read_all(const char *path, CobFileBuffer *out);

/* Writes `size` bytes from `data` to `path`, creating or truncating the
 * file as needed (binary mode). Returns COB_FILE_OK on success. Used by
 * cob_interp.c to persist the .strawberry fast-boot cache. */
CobFileStatus cob_file_write_all(const char *path, const void *data, size_t size);

/* Returns 1 if a regular, readable file exists at `path`, else 0. */
int cob_file_exists(const char *path);

/* Frees a buffer previously populated by cob_file_read_all(). Safe to
 * call on an already-freed/zeroed buffer. */
void cob_file_free(CobFileBuffer *buf);

/* Given a source path like "myprogram.cob", writes the corresponding
 * fast-boot cache path "myprogram.strawberry" into `out_buf` (caller-
 * supplied buffer of size `out_buf_size`). Returns 0 on success, -1 if
 * the buffer was too small or src_path didn't look like a .cob file. */
int cob_make_cache_path(const char *src_path, char *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* COB_FILE_IO_H */
