/* =====================================================================
 * file_io.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * Implementation of file_io.h. Pure C99, standard library only -- no
 * platform-specific APIs are needed for simple whole-file buffering,
 * so this file compiles unmodified on Windows (MSVC/MinGW), Linux, and
 * macOS. Where platform differences *do* matter (binary-mode fopen
 * flags, directory separators) common.h's COB_OS_* macros are used.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#include "file_io.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CobFileStatus cob_file_read_all(const char *path, CobFileBuffer *out) {
    FILE *fp;
    long  file_len;
    char *buffer;
    size_t read_len;

    if (!path || !out) return COB_FILE_ERR_READ;
    out->data = NULL;
    out->size = 0;

    /* "rb" (binary mode) on every platform: on Windows this disables
     * the CRLF->LF text-mode translation, so the byte offsets we
     * compute with ftell() are exact on all three target OSes. */
    fp = fopen(path, "rb");
    if (!fp) return COB_FILE_ERR_NOT_FOUND;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return COB_FILE_ERR_READ; }
    file_len = ftell(fp);
    if (file_len < 0) { fclose(fp); return COB_FILE_ERR_READ; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return COB_FILE_ERR_READ; }

    if (file_len == 0) {
        /* Legal but useless -- an empty .cob file. Give back a valid,
         * empty, NUL-terminated buffer rather than erroring hard, so
         * callers can decide how to react (cob_interp treats it as a
         * no-op program). */
        buffer = (char *)malloc(1);
        if (!buffer) { fclose(fp); return COB_FILE_ERR_OOM; }
        buffer[0] = '\0';
        fclose(fp);
        out->data = buffer;
        out->size = 0;
        return COB_FILE_OK;
    }

    /* +1 for the guaranteed NUL terminator described in file_io.h. */
    buffer = (char *)malloc((size_t)file_len + 1);
    if (!buffer) { fclose(fp); return COB_FILE_ERR_OOM; }

    read_len = fread(buffer, 1, (size_t)file_len, fp);
    fclose(fp);

    if (read_len != (size_t)file_len) {
        free(buffer);
        return COB_FILE_ERR_READ;
    }

    buffer[read_len] = '\0';
    out->data = buffer;
    out->size = read_len;
    return COB_FILE_OK;
}

CobFileStatus cob_file_write_all(const char *path, const void *data, size_t size) {
    FILE *fp;
    size_t written;

    if (!path) return COB_FILE_ERR_READ;

    fp = fopen(path, "wb");
    if (!fp) return COB_FILE_ERR_NOT_FOUND;

    if (size == 0) {
        fclose(fp);
        return COB_FILE_OK;
    }

    written = fwrite(data, 1, size, fp);
    fclose(fp);

    return (written == size) ? COB_FILE_OK : COB_FILE_ERR_READ;
}

int cob_file_exists(const char *path) {
    FILE *fp;
    if (!path) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

void cob_file_free(CobFileBuffer *buf) {
    if (!buf) return;
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
}

int cob_make_cache_path(const char *src_path, char *out_buf, size_t out_buf_size) {
    size_t src_len;
    size_t ext_len;
    size_t cache_ext_len;

    if (!src_path || !out_buf || out_buf_size == 0) return -1;
    if (!cob_str_ends_with(src_path, COB_SOURCE_EXTENSION)) return -1;

    src_len       = strlen(src_path);
    ext_len       = strlen(COB_SOURCE_EXTENSION);
    cache_ext_len = strlen(COB_CACHE_EXTENSION);

    /* stem length = everything before the trailing ".cob" */
    if ((src_len - ext_len) + cache_ext_len + 1 > out_buf_size) return -1;

    memcpy(out_buf, src_path, src_len - ext_len);
    memcpy(out_buf + (src_len - ext_len), COB_CACHE_EXTENSION, cache_ext_len + 1);
    return 0;
}
