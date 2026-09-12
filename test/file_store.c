/*
 * file_store.c - see file_store.h. POSIX only; this file never ships
 * to the target (the firmware binds NVS in main/nvs_store.c).
 */
#define _POSIX_C_SOURCE 200809L

#include "file_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void rec_path(const file_store *fs, uint32_t idx,
                     char *out, size_t cap)
{
    snprintf(out, cap, "%s/r%08u.bin", fs->dir, idx);
}

static bool fs_put(void *ctx, uint32_t idx, const void *rec, size_t len)
{
    file_store *fs = ctx;
    char path[512];
    rec_path(fs, idx, path, sizeof path);

    fs->puts_seen++;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;

    if (fs->torn_at != 0 && fs->puts_seen == fs->torn_at) {
        /* The power cut. Half the record lands, the rest of the "page"
         * reads back as erased flash, and the process dies inside the
         * write - no return, no cleanup, no goodbye. */
        uint8_t torn[SAF_RECORD_MAX];
        memcpy(torn, rec, len / 2);
        memset(torn + len / 2, 0xff, len - len / 2);
        (void)!write(fd, torn, len);
        fsync(fd);
        _exit(FILE_STORE_TORN_EXIT);
    }

    ssize_t n = write(fd, rec, len);
    bool ok = n == (ssize_t)len && fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool fs_get(void *ctx, uint32_t idx, void *rec, size_t *len)
{
    file_store *fs = ctx;
    char path[512];
    rec_path(fs, idx, path, sizeof path);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, rec, *len);
    close(fd);
    if (n < 0)
        return false;
    *len = (size_t)n;
    return true;
}

static bool fs_erase(void *ctx, uint32_t idx)
{
    file_store *fs = ctx;
    char path[512];
    rec_path(fs, idx, path, sizeof path);
    return unlink(path) == 0;
}

static void fs_scan(void *ctx, uint32_t *first, uint32_t *last, bool *any)
{
    file_store *fs = ctx;
    *any = false;
    DIR *d = opendir(fs->dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        unsigned idx;
        if (sscanf(e->d_name, "r%8u.bin", &idx) != 1)
            continue;
        if (!*any || idx < *first)
            *first = idx;
        if (!*any || idx > *last)
            *last = idx;
        *any = true;
    }
    closedir(d);
}

static bool fs_get_meta(void *ctx, uint32_t *v)
{
    file_store *fs = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/ceil.bin", fs->dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = fread(v, sizeof *v, 1, f) == 1;
    fclose(f);
    return ok;
}

static bool fs_put_meta(void *ctx, uint32_t v)
{
    file_store *fs = ctx;
    char path[512], tmp[512];
    snprintf(path, sizeof path, "%s/ceil.bin", fs->dir);
    snprintf(tmp, sizeof tmp, "%s/ceil.tmp", fs->dir);
    /* write-then-rename: the meta update is atomic, like an NVS entry
     * commit - a cut leaves the OLD ceiling, which is always safe
     * (sequences may skip, never repeat) */
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(&v, sizeof v, 1, f) == 1 && fflush(f) == 0 &&
              fsync(fileno(f)) == 0;
    fclose(f);
    return ok && rename(tmp, path) == 0;
}

void file_store_init(file_store *fs, const char *dir, saf_ops *ops)
{
    fs->dir = dir;
    fs->puts_seen = 0;
    const char *t = getenv("SAF_TORN_AT_PUT");
    fs->torn_at = t ? atoi(t) : 0;

    ops->store_put = fs_put;
    ops->store_get = fs_get;
    ops->store_erase = fs_erase;
    ops->store_scan = fs_scan;
    ops->store_get_meta = fs_get_meta;
    ops->store_put_meta = fs_put_meta;
}
