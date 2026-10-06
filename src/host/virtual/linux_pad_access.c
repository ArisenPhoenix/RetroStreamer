#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Session children inherit open pad fds from host_runner. The host /dev/input
 * node is also visible to Prism/Minecraft; after it is mode-000 those apps fail
 * open(), while we rewrite matching paths to the inherited fd.
 *
 * ARCHSTREAMER_PAD_ACCESS=/dev/input/event21=3:/dev/input/js1=4
 */

static int (*real_open)(const char*, int, ...);
static int (*real_openat)(int, const char*, int, ...);
static int (*real_open64)(const char*, int, ...);
static int (*real_openat64)(int, const char*, int, ...);
static int (*real_access)(const char*, int);
static int (*real_faccessat)(int, const char*, int, int);

static void resolve_reals(void) {
    if (real_open == NULL) {
        real_open = dlsym(RTLD_NEXT, "open");
    }
    if (real_openat == NULL) {
        real_openat = dlsym(RTLD_NEXT, "openat");
    }
    if (real_open64 == NULL) {
        real_open64 = dlsym(RTLD_NEXT, "open64");
    }
    if (real_openat64 == NULL) {
        real_openat64 = dlsym(RTLD_NEXT, "openat64");
    }
    if (real_access == NULL) {
        real_access = dlsym(RTLD_NEXT, "access");
    }
    if (real_faccessat == NULL) {
        real_faccessat = dlsym(RTLD_NEXT, "faccessat");
    }
}

static int inherited_fd_for_path(const char* path) {
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    const char* spec = getenv("ARCHSTREAMER_PAD_ACCESS");
    if (spec == NULL || spec[0] == '\0') {
        return -1;
    }
    const size_t path_len = strlen(path);
    const char* cursor = spec;
    while (*cursor != '\0') {
        const char* eq = strchr(cursor, '=');
        const char* sep = strchr(cursor, ':');
        if (eq == NULL) {
            break;
        }
        const size_t key_len = (size_t)(eq - cursor);
        if (key_len == path_len && strncmp(cursor, path, path_len) == 0) {
            return atoi(eq + 1);
        }
        if (sep == NULL) {
            break;
        }
        cursor = sep + 1;
    }
    return -1;
}

static int dup_inherited(int inherited, int oflag) {
    if (inherited < 0) {
        errno = EACCES;
        return -1;
    }
    const int fd = dup(inherited);
    if (fd < 0) {
        return -1;
    }
    if (oflag & O_CLOEXEC) {
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

int open(const char* path, int oflag, ...) {
    resolve_reals();
    const int inherited = inherited_fd_for_path(path);
    if (inherited >= 0) {
        return dup_inherited(inherited, oflag);
    }
    if (oflag & O_CREAT) {
        va_list args;
        va_start(args, oflag);
        const mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return real_open(path, oflag, mode);
    }
    return real_open(path, oflag);
}

int open64(const char* path, int oflag, ...) {
    resolve_reals();
    const int inherited = inherited_fd_for_path(path);
    if (inherited >= 0) {
        return dup_inherited(inherited, oflag);
    }
    if (real_open64 == NULL) {
        return open(path, oflag);
    }
    if (oflag & O_CREAT) {
        va_list args;
        va_start(args, oflag);
        const mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return real_open64(path, oflag, mode);
    }
    return real_open64(path, oflag);
}

int openat(int dirfd, const char* path, int oflag, ...) {
    resolve_reals();
    const int inherited = inherited_fd_for_path(path);
    if (inherited >= 0) {
        return dup_inherited(inherited, oflag);
    }
    if (real_openat == NULL) {
        if (oflag & O_CREAT) {
            va_list args;
            va_start(args, oflag);
            const mode_t mode = (mode_t)va_arg(args, int);
            va_end(args);
            return open(path, oflag, mode);
        }
        return open(path, oflag);
    }
    if (oflag & O_CREAT) {
        va_list args;
        va_start(args, oflag);
        const mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return real_openat(dirfd, path, oflag, mode);
    }
    return real_openat(dirfd, path, oflag);
}

int openat64(int dirfd, const char* path, int oflag, ...) {
    resolve_reals();
    const int inherited = inherited_fd_for_path(path);
    if (inherited >= 0) {
        return dup_inherited(inherited, oflag);
    }
    if (real_openat64 == NULL) {
        if (oflag & O_CREAT) {
            va_list args;
            va_start(args, oflag);
            const mode_t mode = (mode_t)va_arg(args, int);
            va_end(args);
            return openat(dirfd, path, oflag, mode);
        }
        return openat(dirfd, path, oflag);
    }
    if (oflag & O_CREAT) {
        va_list args;
        va_start(args, oflag);
        const mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return real_openat64(dirfd, path, oflag, mode);
    }
    return real_openat64(dirfd, path, oflag);
}

int access(const char* path, int amode) {
    resolve_reals();
    if (inherited_fd_for_path(path) >= 0) {
        return 0;
    }
    if (real_access == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_access(path, amode);
}

int faccessat(int dirfd, const char* path, int amode, int flags) {
    resolve_reals();
    if (inherited_fd_for_path(path) >= 0) {
        return 0;
    }
    if (real_faccessat == NULL) {
        return access(path, amode);
    }
    return real_faccessat(dirfd, path, amode, flags);
}
