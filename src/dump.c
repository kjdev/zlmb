#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <time.h>
#include <limits.h>
#include <syslog.h>

#include "config.h"
#include "dump.h"

#ifdef USE_SNAPPY
#    include <snappy-c.h>
#endif

const char zlmb_dump_header[5] = { 0x00, 0x7a, 0x6c, 0x6d, 0x62 };

zlmb_dump_t *
zlmb_dump_init(const char *filename, int type)
{
    zlmb_dump_t *self;

    if (!filename || strlen(filename) <= 0) {
        return NULL;
    }

    self = (zlmb_dump_t *)malloc(sizeof(zlmb_dump_t));
    if (!self) {
        return NULL;
    }

    self->type = type;
    self->filename = filename;
    self->size = 0;
    self->fp = NULL;

    return self;
}

void
zlmb_dump_destroy(zlmb_dump_t **self)
{
    if (*self) {
        free(*self);
        *self = NULL;
    }
}

int
zlmb_dump_write(zlmb_dump_t *self, zmq_msg_t *zmsg, int flags)
{
    int closed = 0;
    void *buf;
    size_t len;

    if (!self || !zmsg) {
        return -1;
    }

    buf = zmq_msg_data(zmsg);
    len = zmq_msg_size(zmsg);

    if (flags == 0) {
        closed = 1;
    }

    if (!self->fp) {
        self->fp = fopen(self->filename, "a");
        if (!self->fp) {
            return -1;
        }
    }

    if (flock(fileno(self->fp), LOCK_EX) != 0) {
        if (closed) {
            fclose(self->fp);
            self->fp = NULL;
        }
        return -1;
    }

    if (self->type & ZLMB_DUMP_TYPE_PLAIN) {
        if (self->type & ZLMB_DUMP_TYPE_PLAIN_DATETIME) {
            //datetime
            time_t now;
            char tbuf[32];
            time(&now);
            strftime(tbuf, sizeof(tbuf), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
            fprintf(self->fp, "%s ", tbuf);
        }
        if (self->type & ZLMB_DUMP_TYPE_PLAIN_FLAGS) {
            //flags
            char *fbuf = NULL;
            size_t length, fsize = 8;
            void *tmp = NULL;
            fbuf = (char *)malloc(sizeof(char *) * (fsize + 1));
            if (fbuf) {
                length = snprintf(fbuf, fsize, "[%d]", flags);
                if (length >= fsize) {
                    fsize = length + 1;
                    tmp = realloc(fbuf, fsize);
                    if (!tmp) {
                        fbuf = (char *)tmp;
                        snprintf(fbuf, fsize, "[%d]", flags);
                    } else {
                        free(fbuf);
                        fbuf = NULL;
                    }
                }
            }
            if (fbuf) {
                fprintf(self->fp, "%s ", fbuf);
                free(fbuf);
            }
        }
        //plain-text
#ifdef USE_SNAPPY
        if (snappy_validate_compressed_buffer(buf, len) == SNAPPY_OK) {
            char *ubuf = NULL;
            size_t ubuf_len = 0;
            if (snappy_uncompressed_length(buf, len, &ubuf_len) == SNAPPY_OK) {
                ubuf = (char *)malloc(ubuf_len);
                if (ubuf) {
                    if (snappy_uncompress(buf, len,
                                          ubuf, &ubuf_len) != SNAPPY_OK) {
                        free(ubuf);
                        ubuf = NULL;
                    }
                }
            }
            if (ubuf) {
                fprintf(self->fp, "%.*s\n", (int)ubuf_len, ubuf);
                free(ubuf);
            } else {
                fprintf(self->fp, "%.*s\n", (int)len, (char *)buf);
            }
        } else {
            fprintf(self->fp, "%.*s\n", (int)len, (char *)buf);
        }
#else
        fprintf(self->fp, "%.*s\n", (int)len, (char *)buf);
#endif
    } else {
        //binary
        fwrite(zlmb_dump_header,
               sizeof(char), sizeof(zlmb_dump_header), self->fp);
        fwrite(&flags, sizeof(int), 1, self->fp);
        fwrite(&len, sizeof(size_t), 1, self->fp);
        fprintf(self->fp, "%.*s", (int)len, (char *)buf);
    }

    flock(fileno(self->fp), LOCK_UN);

    if (closed) {
        fclose(self->fp);
        self->fp = NULL;
    }

    return 0;
}

int
zlmb_dump_close(zlmb_dump_t *self)
{
    if (!self) {
        return -1;
    }

    if (self->fp) {
        fclose(self->fp);
        self->fp = NULL;
    }

    return 0;
}

int
zlmb_dump_read_open(zlmb_dump_t *self)
{
    if (!self) {
        return -1;
    }

    self->fp = fopen(self->filename, "r");
    if (!self->fp) {
        return -1;
    }

    return 0;
}

int
zlmb_dump_read(zlmb_dump_t *self, zmq_msg_t *zmsg)
{
    void *data = NULL;
    int more = 0;
    size_t size = 0;
    ssize_t len = 0;
    int closed = 0;
    char header[5];

    if (!self) {
        return -1;
    }

    if (!self->fp) {
        self->fp = fopen(self->filename, "r");
        if (!self->fp) {
            return -1;
        }
        closed = 1;
    }

    if (feof(self->fp) != 0) {
        if (closed) {
            fclose(self->fp);
            self->fp = NULL;
        }
        return 0;
    }

    memset(header, 0, sizeof(header));
    len = fread(header, sizeof(char), sizeof(header), self->fp);
    if (len <= 0 || memcmp(zlmb_dump_header, header, sizeof(header)) != 0) {
        return -1;
    }
    self->size += (len * sizeof(char));

    len = fread(&more, sizeof(int), 1, self->fp);
    if (len <= 0) {
        return -1;
    }
    self->size += (len * sizeof(int));

    len = fread(&size, sizeof(size_t), 1, self->fp);
    if (len <= 0) {
        return -1;
    }
    self->size += (len * sizeof(size_t));

    if (size == 0) {
        return -1;
    }

    data = malloc(size + 1);
    if (!data) {
        return -1;
    }

    memset(data, 0, size + 1);

    len = fread(data, sizeof(void), size, self->fp);
    if (len <= 0) {
        free(data);
        return -1;
    }
    self->size += (len * sizeof(void));

    if (zmq_msg_init_size(zmsg, size) != 0) {
        free(data);
        return -1;
    }

    if (data) {
        memcpy(zmq_msg_data(zmsg), data, size);
    }

    free(data);

    if (closed) {
        fclose(self->fp);
        self->fp = NULL;
    }

    return more;
}

int
zlmb_dump_truncate(zlmb_dump_t *self)
{
    int tmp;
    char *filepath, path[PATH_MAX+1], tmpfile[PATH_MAX+1];
    unsigned char buf[BUFSIZ];
    size_t len;
    FILE *fp;

    if (!self) {
        return -1;
    }

    filepath = realpath(self->filename, path);
    if (filepath == NULL) {
        return -1;
    }

    snprintf(tmpfile, PATH_MAX, "%s.trunc.XXXXXX", filepath);

    fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }

    tmp = mkstemp(tmpfile);
    if (tmp == -1) {
        fclose(fp);
        return -1;
    }

    if (self->size > 0) {
        if (fseek(fp, self->size, SEEK_SET) == -1) {
            fclose(fp);
            close(tmp);
            unlink(tmpfile);
            return -1;
        }
    }

    if (flock(fileno(fp), LOCK_EX) != 0) {
        fclose(fp);
        close(tmp);
        unlink(tmpfile);
        return -1;
    }

    while (1) {
        memset(buf, 0, BUFSIZ);
        len = fread(buf, sizeof(unsigned char), BUFSIZ, fp);
        if (len <= 0) {
            break;
        }
        write(tmp, buf, len);
    }

    flock(fileno(fp), LOCK_UN);

    fclose(fp);
    close(tmp);

    return rename(tmpfile, filepath);
}

void
zlmb_dump_print(FILE *out, char *buf, size_t len)
{
    char *ubuf = NULL;
    size_t i, ubuf_len;

    if (!buf || len <= 0) {
        return;
    }

    if (out == NULL) {
        out = stderr;
    }

#ifdef USE_SNAPPY
    if (snappy_validate_compressed_buffer(buf, len) == SNAPPY_OK) {
        fprintf(out, "[compress:%03ld]", len);
        if (snappy_uncompressed_length(buf, len, &ubuf_len) == SNAPPY_OK) {
            ubuf = (char *)malloc(ubuf_len);
            if (ubuf) {
                if (snappy_uncompress(buf, len, ubuf, &ubuf_len) == SNAPPY_OK) {
                    buf = ubuf;
                    len = ubuf_len;
                } else {
                    free(ubuf);
                    ubuf = NULL;
                }
            }
        }
    }
#endif

    fprintf(out, "[%03ld] ", len);

    for (i = 0; i < len; i++) {
        fprintf(out, "%c", buf[i]);
    }

    fprintf(out, "\n");

    if (ubuf) {
        free(ubuf);
    }
}
