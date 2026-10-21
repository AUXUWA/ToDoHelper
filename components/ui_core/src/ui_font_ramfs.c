/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_font_ramfs.c
 * @brief A read-only file served straight out of RAM through the IDF VFS.
 *
 * See ui_font_ramfs.h for why this exists. The short version: FreeType reads
 * the .ttf with fopen()/fread(), so the speed of a glyph is the speed of
 * whatever is behind the path. This module puts the bytes in RAM and hands
 * FreeType an ordinary path.
 *
 * The rules of the ESP-IDF VFS this driver has to follow:
 *   - the driver is called with the path **relative to the base path**
 *     ("/ui_cjk.ttf" for the base "/ramfont");
 *   - the file descriptors this driver returns are its own - the VFS layer
 *     translates them for the caller;
 *   - with ESP_VFS_FLAG_READONLY_FS the VFS layer already rejects any open
 *     that is not read-only, so the driver does not have to.
 */

#include "ui_font_ramfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "sys_utils.h"

static const char *TAG = "ui_font_ramfs";

/** Where the RAM filesystem is mounted. */
#define UI_FONT_RAMFS_BASE "/ramfont"

/** How many files one instance can publish. */
#define UI_FONT_RAMFS_MAX_FILES 4

/** How many files can be open at the same time. */
#define UI_FONT_RAMFS_MAX_FDS 4

/** Longest file name accepted. */
#define UI_FONT_RAMFS_MAX_NAME 48

typedef struct {
    bool used;
    char name[UI_FONT_RAMFS_MAX_NAME];
    const uint8_t *data;
    size_t size;
} ramfs_file_t;

typedef struct {
    bool used;
    int file;   /**< index into files[] */
    size_t pos; /**< read position inside the file */
} ramfs_fd_t;

typedef struct {
    ramfs_file_t files[UI_FONT_RAMFS_MAX_FILES];
    ramfs_fd_t fds[UI_FONT_RAMFS_MAX_FDS];
} ramfs_ctx_t;

static ramfs_ctx_t s_ctx;
static bool s_registered;

/* ------------------------------------------------------------------------- */
/* Driver                                                                    */
/* ------------------------------------------------------------------------- */

/** Strip the leading '/' the VFS hands us and match a published file. */
static int find_file(ramfs_ctx_t *ctx, const char *path)
{
    if (path == NULL) {
        return -1;
    }
    while (*path == '/') {
        path++;
    }
    for (int i = 0; i < UI_FONT_RAMFS_MAX_FILES; i++) {
        if (ctx->files[i].used && strcmp(ctx->files[i].name, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int ramfs_open(void *ctx, const char *path, int flags, int mode)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    (void)mode;

    /* The VFS layer already refuses writes on a read-only filesystem, but a
     * driver that silently ignored the flags would be a trap for the next
     * caller - so refuse here too. */
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
    }

    const int file = find_file(c, path);
    if (file < 0) {
        errno = ENOENT;
        return -1;
    }

    for (int fd = 0; fd < UI_FONT_RAMFS_MAX_FDS; fd++) {
        if (!c->fds[fd].used) {
            c->fds[fd].used = true;
            c->fds[fd].file = file;
            c->fds[fd].pos = 0;
            return fd;
        }
    }

    errno = EMFILE;
    return -1;
}

static int ramfs_close(void *ctx, int fd)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    if (fd < 0 || fd >= UI_FONT_RAMFS_MAX_FDS || !c->fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    c->fds[fd].used = false;
    c->fds[fd].file = -1;
    c->fds[fd].pos = 0;
    return 0;
}

static ssize_t ramfs_read(void *ctx, int fd, void *dst, size_t size)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    if (fd < 0 || fd >= UI_FONT_RAMFS_MAX_FDS || !c->fds[fd].used) {
        errno = EBADF;
        return -1;
    }

    const ramfs_file_t *file = &c->files[c->fds[fd].file];
    const size_t left = file->size - c->fds[fd].pos;
    const size_t want = size < left ? size : left;

    if (want > 0) {
        memcpy(dst, file->data + c->fds[fd].pos, want);
        c->fds[fd].pos += want;
    }
    return (ssize_t)want;
}

static ssize_t ramfs_pread(void *ctx, int fd, void *dst, size_t size, off_t offset)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    if (fd < 0 || fd >= UI_FONT_RAMFS_MAX_FDS || !c->fds[fd].used) {
        errno = EBADF;
        return -1;
    }

    const ramfs_file_t *file = &c->files[c->fds[fd].file];
    if (offset < 0 || (size_t)offset >= file->size) {
        return 0;
    }
    const size_t left = file->size - (size_t)offset;
    const size_t want = size < left ? size : left;
    memcpy(dst, file->data + offset, want);
    return (ssize_t)want;
}

static off_t ramfs_lseek(void *ctx, int fd, off_t offset, int mode)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    if (fd < 0 || fd >= UI_FONT_RAMFS_MAX_FDS || !c->fds[fd].used) {
        errno = EBADF;
        return -1;
    }

    const ramfs_file_t *file = &c->files[c->fds[fd].file];
    off_t base;

    switch (mode) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (off_t)c->fds[fd].pos;
        break;
    case SEEK_END:
        base = (off_t)file->size;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    const off_t target = base + offset;
    if (target < 0 || (size_t)target > file->size) {
        errno = EINVAL;
        return -1;
    }
    c->fds[fd].pos = (size_t)target;
    return target;
}

static int ramfs_fstat(void *ctx, int fd, struct stat *st)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    if (fd < 0 || fd >= UI_FONT_RAMFS_MAX_FDS || !c->fds[fd].used) {
        errno = EBADF;
        return -1;
    }
    const ramfs_file_t *file = &c->files[c->fds[fd].file];
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)file->size;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_blksize = 512;
    st->st_blocks = (blkcnt_t)((file->size + 511) / 512);
    return 0;
}

static int ramfs_write(void *ctx, int fd, const void *data, size_t size)
{
    (void)ctx;
    (void)fd;
    (void)data;
    (void)size;
    errno = EROFS;
    return -1;
}

static int ramfs_fsync(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}

#if CONFIG_VFS_SUPPORT_DIR
static int ramfs_stat(void *ctx, const char *path, struct stat *st)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    const int file = find_file(c, path);
    if (file < 0) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)c->files[file].size;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_blksize = 512;
    st->st_blocks = (blkcnt_t)((c->files[file].size + 511) / 512);
    return 0;
}

static int ramfs_access(void *ctx, const char *path, int amode)
{
    ramfs_ctx_t *c = (ramfs_ctx_t *)ctx;
    (void)amode;
    return find_file(c, path) < 0 ? -1 : 0;
}

static const esp_vfs_dir_ops_t s_ramfs_dir_ops = {
    .stat_p = &ramfs_stat,
    .access_p = &ramfs_access,
};
#endif /* CONFIG_VFS_SUPPORT_DIR */

static const esp_vfs_fs_ops_t s_ramfs_ops = {
    .write_p = &ramfs_write,
    .lseek_p = &ramfs_lseek,
    .read_p = &ramfs_read,
    .pread_p = &ramfs_pread,
    .open_p = &ramfs_open,
    .close_p = &ramfs_close,
    .fstat_p = &ramfs_fstat,
    .fsync_p = &ramfs_fsync,
#if CONFIG_VFS_SUPPORT_DIR
    .dir = &s_ramfs_dir_ops,
#endif
};

/* ------------------------------------------------------------------------- */
/* Public                                                                    */
/* ------------------------------------------------------------------------- */

esp_err_t ui_font_ramfs_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));

    const esp_err_t err = esp_vfs_register_fs(UI_FONT_RAMFS_BASE, &s_ramfs_ops,
                                              ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR |
                                                  ESP_VFS_FLAG_READONLY_FS,
                                              &s_ctx);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "cannot register %s: %s", UI_FONT_RAMFS_BASE, esp_err_to_name(err));
        return err;
    }

    s_registered = true;
    SYS_LOGI(TAG, "RAM filesystem mounted at %s", UI_FONT_RAMFS_BASE);
    return ESP_OK;
}

esp_err_t ui_font_ramfs_add(const char *name, const void *data, size_t size)
{
    if (name == NULL || data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(name) >= UI_FONT_RAMFS_MAX_NAME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registered) {
        const esp_err_t err = ui_font_ramfs_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    for (int i = 0; i < UI_FONT_RAMFS_MAX_FILES; i++) {
        if (s_ctx.files[i].used && strcmp(s_ctx.files[i].name, name) == 0) {
            /* Re-publishing a name replaces it. Any descriptor still open on
             * the old bytes is dropped - the caller is about to free them. */
            for (int fd = 0; fd < UI_FONT_RAMFS_MAX_FDS; fd++) {
                if (s_ctx.fds[fd].used && s_ctx.fds[fd].file == i) {
                    s_ctx.fds[fd].used = false;
                    s_ctx.fds[fd].file = -1;
                    s_ctx.fds[fd].pos = 0;
                }
            }
            s_ctx.files[i].data = (const uint8_t *)data;
            s_ctx.files[i].size = size;
            SYS_LOGI(TAG, "republished %s (%u bytes) at %s/%s", name, (unsigned)size,
                     UI_FONT_RAMFS_BASE, name);
            return ESP_OK;
        }
    }

    for (int i = 0; i < UI_FONT_RAMFS_MAX_FILES; i++) {
        if (!s_ctx.files[i].used) {
            s_ctx.files[i].used = true;
            snprintf(s_ctx.files[i].name, sizeof(s_ctx.files[i].name), "%s", name);
            s_ctx.files[i].data = (const uint8_t *)data;
            s_ctx.files[i].size = size;
            SYS_LOGI(TAG, "published %s (%u bytes) at %s/%s", name, (unsigned)size,
                     UI_FONT_RAMFS_BASE, name);
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

const char *ui_font_ramfs_path(const char *name)
{
    static char path[UI_FONT_RAMFS_MAX_NAME + 16];

    if (name == NULL || !s_registered) {
        return NULL;
    }
    if (find_file(&s_ctx, name) < 0) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s", UI_FONT_RAMFS_BASE, name);
    return path;
}
