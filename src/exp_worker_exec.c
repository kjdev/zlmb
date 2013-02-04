/*
 * Example worker exec
 *
 * Usage: exp-worker-exec [-f FILE]
 *
 * zlmb-worker -e tcp://127.0.0.1:5560 -c exp-worker-exec [-- -f /path/to/output]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <sys/file.h>
#include <libgen.h>

#define _ERR(...) fprintf(stderr, "ERR: "__VA_ARGS__)

static int
_stdin(int fd) {
    fd_set fdset;
    struct timeval timeout;

    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(fd + 1, &fdset, NULL, NULL, &timeout);
}

static void
_usage(char *arg)
{
    char *command = basename(arg);

    printf("Usage: %s [-f FILE]\n\n", command);

    printf("  -f, --file=FILE output file name [DEFAULT:stdout]\n");
}

int
main (int argc, char **argv)
{
    char *filename = NULL;
    char *command = "COMMAND";
    char *frame = NULL, *frame_length = NULL, *length = NULL, *buffer = NULL;
    char buf[BUFSIZ], date[20];
    time_t now;
    size_t pos, size, len;
    FILE *fp;
    int opt;

    const struct option long_options[] = {
        { "file", 1, NULL, 'f' },
        { "help", 0, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                filename = optarg;
                break;
            default:
                _usage(argv[0]);
                return -1;
        }
    }

    //Get environ
    frame = getenv("ZLMB_FRAME");
    frame_length = getenv("ZLMB_FRAME_LENGTH");
    length = getenv("ZLMB_LENGTH");

    //Read STDIN
    size = sizeof(char *) * BUFSIZ;
    buffer = malloc(size + 1);
    if (!buffer) {
        _ERR("Memory allocate.\n");
        return -1;
    }
    memset(buffer, '\0', size + 1);
    pos = 0;

    if (_stdin(0)) {
        while (1) {
            memset(buf, '\0', BUFSIZ);
            len = read(0, buf, BUFSIZ);
            if (len == 0) {
                break;
            }

            if (pos + len > size) {
                void *tmp = NULL;
                size += (sizeof(char *) * BUFSIZ) + 1;
                tmp = realloc(buffer, size);
                if (!tmp) {
                    free(buffer);
                    _ERR("Memory allocate.\n");
                    return -1;
                }
                buffer = (char *)tmp;
            }

            memcpy(buffer + pos, buf, len);
            pos += len;
            buffer[pos] = '\0';
        }
    }

    //Output file
    if (argc >= 1) {
        command = basename(argv[0]);
    }

    time(&now);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));

    if (filename) {
        fp = fopen(filename, "a");
    } else {
        fp = stdout;
    }

    if (fp) {
        if (flock(fileno(fp), LOCK_EX) == 0) {
            fprintf(fp, "%s: %s\n", command, date);
            fprintf(fp, "ZLMB_FRAME:%s\n", frame);
            fprintf(fp, "ZLMB_FRAME_LENGTH:%s\n", frame_length);
            fprintf(fp, "ZLMB_LENGTH:%s\n", length);
            fprintf(fp, "ZLMB_BUFFER:%s\n", buffer);
            fprintf(fp, "----------\n");
            flock(fileno(fp), LOCK_UN);
        } else {
            _ERR("Lock log file: %s\n", filename);
        }
        if (filename) {
            fclose(fp);
        }
    } else {
        _ERR("Open log file: %s\n", filename);
    }

    if (buffer) {
        free(buffer);
    }

    return 0;
}
