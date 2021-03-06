// file.c
//
// The MIT License (MIT)
//
// Copyright (c) 2016 Zewo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDINbG BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libvenice.h"
#include "utils.h"

#ifndef MILL_FILE_BUFLEN
#define MILL_FILE_BUFLEN (4096)
#endif

struct mill_file {
    int fd;
    size_t ifirst;
    size_t ilen;
    size_t olen;
    char ibuf[MILL_FILE_BUFLEN];
    char obuf[MILL_FILE_BUFLEN];
    int eof;
    mode_t mode;
};

static void mill_filetune(int fd) {
    /* Make the file descriptor non-blocking. */
    int opt = fcntl(fd, F_GETFL, 0);
    if (opt == -1)
        opt = 0;
    int rc = fcntl(fd, F_SETFL, opt | O_NONBLOCK);
    mill_assert(rc != -1);
}


mfile fileopen(const char *pathname, int flags, mode_t mode) {
    /* Open the file. */
    int fd = open(pathname, flags | O_NONBLOCK, mode);
    if (fd == -1)
        return NULL;

    /* Create the object. */
    struct mill_file *f = malloc(sizeof(struct mill_file));
    if(!f) {
        fdclean(fd);
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    f->fd = fd;
    f->ifirst = 0;
    f->ilen = 0;
    f->olen = 0;
    f->eof = 0;
    struct stat sb;
    if (fstat(f->fd, &sb) == -1)
        return NULL;
    f->mode = sb.st_mode;
    errno = 0;
    return f;
}

size_t filewrite(mfile f, const void *buf, size_t len, int64_t deadline) {
    /* If it fits into the output buffer copy it there and be done. */
    if(f->olen + len <= MILL_FILE_BUFLEN) {
        memcpy(&f->obuf[f->olen], buf, len);
        f->olen += len;
        errno = 0;
        return len;
    }

    /* If it doesn't fit, flush the output buffer first. */
    fileflush(f, deadline);
    if(errno != 0)
        return 0;

    /* Try to fit it into the buffer once again. */
    if(f->olen + len <= MILL_FILE_BUFLEN) {
        memcpy(&f->obuf[f->olen], buf, len);
        f->olen += len;
        errno = 0;
        return len;
    }

    /* The data chunk to send is longer than the output buffer.
     Let's do the writing in-place. */
    char *pos = (char*)buf;
    size_t remaining = len;
    while(remaining) {
        ssize_t sz = write(f->fd, pos, remaining);
        if(sz == -1) {
            if(errno != EAGAIN && errno != EWOULDBLOCK)
                return 0;
            /* If it's a regular file skip fdwait. */
            if (S_ISREG(f->mode) || S_ISDIR(f->mode))
                continue;
            int rc = fdwait(f->fd, FDW_OUT, deadline);
            if(rc == 0) {
                errno = ETIMEDOUT;
                return len - remaining;
            }
            mill_assert(rc == FDW_OUT);
            continue;
        }
        pos += sz;
        remaining -= sz;
    }
    return len;
}

void fileflush(mfile f, int64_t deadline) {
    if(!f->olen) {
        errno = 0;
        return;
    }
    char *pos = f->obuf;
    size_t remaining = f->olen;
    while(remaining) {
        ssize_t sz = write(f->fd, pos, remaining);
        if(sz == -1) {
            if(errno != EAGAIN && errno != EWOULDBLOCK)
                return;
            /* If it's a regular file skip fdwait. */
            if (S_ISREG(f->mode) || S_ISDIR(f->mode))
                continue;
            int rc = fdwait(f->fd, FDW_OUT, deadline);
            if(rc == 0) {
                errno = ETIMEDOUT;
                return;
            }
            mill_assert(rc == FDW_OUT);
            continue;
        }
        pos += sz;
        remaining -= sz;
    }
    f->olen = 0;
    errno = 0;
}

size_t fileread(mfile f, void *buf, size_t len, int64_t deadline) {
    /* If there's enough data in the buffer it's easy. */
    if(f->ilen >= len) {
        memcpy(buf, &f->ibuf[f->ifirst], len);
        f->ifirst += len;
        f->ilen -= len;
        errno = 0;
        return len;
    }

    /* Let's move all the data from the buffer first. */
    char *pos = (char*)buf;
    size_t remaining = len;
    memcpy(pos, &f->ibuf[f->ifirst], f->ilen);
    pos += f->ilen;
    remaining -= f->ilen;
    f->ifirst = 0;
    f->ilen = 0;

    mill_assert(remaining);
    while(1) {
        if(remaining > MILL_FILE_BUFLEN) {
            /* If we still have a lot to read try to read it in one go directly
             into the destination buffer. */
            ssize_t sz = read(f->fd, pos, remaining);
            if(!sz) {
                f->eof = 1;
                return len - remaining;
            }
            if(sz == -1) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    return len - remaining;
                sz = 0;
            }
            if((size_t)sz == remaining) {
                errno = 0;
                return len;
            }
            pos += sz;
            remaining -= sz;
        }
        else {
            /* If we have just a little to read try to read the full file
             buffer to minimise the number of system calls. */
            ssize_t sz = read(f->fd, f->ibuf, MILL_FILE_BUFLEN);
            if(!sz) {
                f->eof = 1;
                return len - remaining;
            }
            if(sz == -1) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    return len - remaining;
                sz = 0;
            }
            if((size_t)sz < remaining) {
                memcpy(pos, f->ibuf, sz);
                pos += sz;
                remaining -= sz;
                f->ifirst = 0;
                f->ilen = 0;
            }
            else {
                memcpy(pos, f->ibuf, remaining);
                f->ifirst = remaining;
                f->ilen = sz - remaining;
                errno = 0;
                return len;
            }
        }
        /* If it's a regular file skip fdwait. */
        if (S_ISREG(f->mode) || S_ISDIR(f->mode))
            continue;
        /* Wait till there's more data to read. */
        int res = fdwait(f->fd, FDW_IN, deadline);
        if (!res) {
            errno = ETIMEDOUT;
            return len - remaining;
        }
    }
}

size_t filereadlh(mfile f, void *buf, size_t lowwater, size_t highwater, int64_t deadline) {
    /* The buffer is between lowwater and highwater. */
    if(f->ilen >= lowwater && f->ilen <= highwater) {
        memcpy(buf, &f->ibuf[f->ifirst], f->ilen);
        size_t len = f->ilen;
        f->ifirst = 0;
        f->ilen = 0;
        errno = 0;
        return len;
    }

    /* The buffer is above the highwater. */
    if(f->ilen >= highwater) {
        memcpy(buf, &f->ibuf[f->ifirst], highwater);
        f->ifirst += highwater;
        f->ilen -= highwater;
        errno = 0;
        return highwater;
    }

    /* The buffer is below the lowwater. */
    /* Let's move all the data from the buffer first. */
    char *pos = (char*)buf;
    size_t remaining = highwater;
    size_t received = 0;
    memcpy(pos, &f->ibuf[f->ifirst], f->ilen);
    pos += f->ilen;
    remaining -= f->ilen;
    received += f->ilen;
    f->ifirst = 0;
    f->ilen = 0;

    mill_assert(remaining);
    while(1) {
        if(remaining > MILL_FILE_BUFLEN) {
            /* If we still have a lot to read try to read it in one go directly
             into the destination buffer. */
            ssize_t sz = read(f->fd, pos, remaining);
            if(!sz) {
                f->eof = 1;
                return received;
            }
            if(sz == -1) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    return received;
                sz = 0;
            }
            if(received + (size_t)sz >= lowwater) {
                errno = 0;
                return received + (size_t)sz;
            }
            pos += sz;
            remaining -= sz;
            received += sz;
        }
        else {
            /* If we have just a little to read try to read the full file
             buffer to minimise the number of system calls. */
            ssize_t sz = read(f->fd, f->ibuf, MILL_FILE_BUFLEN);
            if(!sz) {
                f->eof = 1;
                return received;
            }
            if(sz == -1) {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                    return received;
                sz = 0;
            }
            if(received + (size_t)sz < lowwater) {
                memcpy(pos, f->ibuf, sz);
                pos += sz;
                remaining -= sz;
                received += sz;
                f->ifirst = 0;
                f->ilen = 0;
            }
            else if(received + (size_t)sz > highwater) {
                memcpy(pos, f->ibuf, remaining);
                f->ifirst = remaining;
                f->ilen = sz - remaining;
                errno = 0;
                return highwater;
            }
            else {
                memcpy(pos, f->ibuf, sz);
                f->ifirst = 0;
                f->ilen = 0;
                errno = 0;
                return received + (size_t)sz;
            }
        }
        /* If it's a regular file skip fdwait. */
        if (S_ISREG(f->mode) || S_ISDIR(f->mode))
            continue;
        /* Wait till there's more data to read. */
        int res = fdwait(f->fd, FDW_IN, deadline);
        if(!res) {
            errno = ETIMEDOUT;
            return received;
        }
    }
}


void fileclose(mfile f) {
    fdclean(f->fd);
    int rc = close(f->fd);
    mill_assert(rc == 0);
    free(f);
    return;
}

mfile fileattach(int fd) {
    struct mill_file *f = malloc(sizeof(struct mill_file));
    if(!f) {
        errno = ENOMEM;
        return NULL;
    }
    mill_filetune(fd);
    f->fd = fd;
    f->ifirst = 0;
    f->ilen = 0;
    f->olen = 0;
    errno = 0;
    return f;
}

int filedetach(mfile f) {
    int fd = f->fd;
    free(f);
    return fd;
}

off_t filetell(mfile f) {
    return lseek(f->fd, 0, SEEK_CUR) - f->ilen;
}

off_t fileseek(mfile f, off_t offset) {
    f->ifirst = 0;
    f->ilen = 0;
    f->olen = 0;
    f->eof = 0;
    return lseek(f->fd, offset, SEEK_SET);
}

off_t filesize(mfile f) {
    /* pipe or FIFO special files does not support lseek */
    if (!S_ISREG(f->mode))
        return -1;

    off_t pos = lseek(f->fd, (size_t)0, SEEK_CUR);
    size_t size = lseek(f->fd, (size_t)0, SEEK_END);
    lseek(f->fd, pos, SEEK_SET);
    return size + f->olen;
}

int fileeof(mfile f) {
    return f->eof;
}
