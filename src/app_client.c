/*
 * zlmb client
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <getopt.h>

#include "zlmb.h"
#include "dump.h"
#include "log.h"

#define ZLMB_SYSLOG_IDENT "zlmb-cli"

#ifndef ZLMB_CLIENT_SOCKET
#define ZLMB_CLIENT_SOCKET "tcp://127.0.0.1:5557"
#endif

static int _syslog = 0;
static int _verbose = 0;

static void
_usage(char *arg, char *message)
{
    char *command = basename(arg);

    printf("Usage: %s [-e ENDPOINT] [-f FILE] [-m NUM] [ARGS ...]\n\n",
           command);

    printf("  -e, --endpoint=ENDPOINT server endpoint [DEFAULT: %s]\n",
           ZLMB_CLIENT_SOCKET);
    printf("  -f, --filename=FILE     input file name or 'stdin'\n");
    printf("  -m, --multipart=NUM     send multi-part message size\n");
    printf("  -s, --syslog            log to syslog\n");
    printf("  -v, --verbose           verbosity log\n");
#ifndef NDEBUG
    printf("  -q, --silent            quit debug message\n");
#endif
    printf("  ARGS ...                messages\n");

    if (message) {
        printf("\nINFO: %s\n", message);
    }
}

int
main (int argc, char **argv)
{
    int i, part, opt, multipart = 0;
    char *endpoint = ZLMB_CLIENT_SOCKET;
    char *filename = NULL;
    void *context, *socket;
#ifndef NDEBUG
    int silent = 0;
#endif

    const struct option long_options[] = {
        { "endpoint", 1, NULL, 'e' },
        { "filename", 1, NULL, 'f' },
        { "multipart", 1, NULL, 'm' },
        { "syslog", 0, NULL, 's' },
        { "verbose", 0, NULL, 'v' },
#ifndef NDEBUG
        { "silent", 0, NULL, 'q' },
#endif
        { "help", 0, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "e:f:m:svqh",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'e':
                endpoint = optarg;
                break;
            case 'f':
                filename = optarg;
                break;
            case 'm':
                multipart = atoi(optarg);
                if (multipart < 0) {
                    multipart = 0;
                }
                break;
            case 's':
                _syslog = 1;
                break;
            case 'v':
                _verbose = 1;
                break;
            case 'q':
#ifndef NDEBUG
                silent = 1;
                break;
#endif
            default:
                _usage(argv[0], NULL);
                return -1;
        }
    }

    if (filename == NULL && argc <= optind) {
        _usage(argv[0], "required args to send messages.");
        return -1;
    }

    _LOG_OPEN(ZLMB_SYSLOG_IDENT);

    _INFO("Connection endpoint: %s\n", endpoint);

    context = zmq_ctx_new();
    if (!context) {
        _ERR("ZeroMQ context: %s\n", zmq_strerror(errno));
        _LOG_CLOSE();
        return -1;
    }

    /*
    if (zmq_ctx_set(context, ZMQ_IO_THREADS, 1) == -1) {
        _ERR("%s",  zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    if (zmq_ctx_set(context, ZMQ_MAX_SOCKETS, 1024) == -1) {
        _ERR("%s",  zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }
    */

    socket = zmq_socket(context, ZMQ_PUSH);
    if (!socket) {
        _ERR("ZeroMQ socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        _LOG_CLOSE();
        return -1;
    }

    if (zmq_connect(socket, endpoint) == -1) {
        _ERR("ZeroMQ connect: %s: %s\n", endpoint, zmq_strerror(errno));
        zmq_close(socket);
        zmq_ctx_destroy(context);
        _LOG_CLOSE();
        return -1;
    }

    _VERBOSE("ZeroMQ socket connect: %s\n", endpoint);

    if (filename) {
        FILE *fp;
        char buf[BUFSIZ];

        if (strcasecmp(filename, "stdin") == 0) {
            fp = stdin;
        } else {
            fp = fopen(filename, "r");
            if (!fp) {
                _ERR("Open read file: %s\n", filename);
                zmq_close(socket);
                zmq_ctx_destroy(context);
                _LOG_CLOSE();
                return -1;
            }
        }

        while (fgets(buf, sizeof(buf), fp) != NULL) {
            char *p;

            if ((p = strrchr(buf, '\n')) != NULL) {
                *p = '\0';
            }

            part = 1;
            if (multipart > 0 && argc > optind) {
                if ((argc - optind) < multipart) {
                    part = argc;
                } else {
                    part = multipart + optind - 1;
                }
                for (i = optind; i < part; i++) {
#ifndef NDEBUG
                    if (!silent) {
                        zlmb_dump_print(stderr, argv[i], strlen(argv[i]));
                    }
#endif
                    _DEBUG("ZeroMQ send(#%d).\n", i - optind + 1);
                    if (zmq_send(socket, argv[i], strlen(argv[i]),
                                 ZMQ_SNDMORE) == -1) {
                        _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
                    }
                }
            }
#ifndef NDEBUG
            if (!silent) {
                zlmb_dump_print(stderr, buf, strlen(buf));
            }
#endif
            _DEBUG("ZeroMQ send(#%d).\n", part);
            if (zmq_send(socket, buf, strlen(buf), 0) == -1) {
                _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
            }
        }

        fclose(fp);
    } else {
        part = 1;
        for (i = optind; i != (argc - 1); i++) {
            int flags = ZMQ_SNDMORE;
            if (part >= multipart) {
                flags = 0;
                part = 0;
            }
#ifndef NDEBUG
            if (!silent) {
                zlmb_dump_print(stderr, argv[i], strlen(argv[i]));
            }
#endif
            _DEBUG("ZeroMQ send(#%d).\n", i - optind + 1);
            if (zmq_send(socket, argv[i], strlen(argv[i]), flags) == -1) {
                _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
            }
            part++;
        }
        part = argc - 1;
#ifndef NDEBUG
        if (!silent) {
            zlmb_dump_print(stderr, argv[part], strlen(argv[part]));
        }
#endif
        _DEBUG("ZeroMQ send(#%d).\n", part - optind + 1);
        if (zmq_send(socket, argv[part], strlen(argv[part]), 0) == -1) {
            _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
        }
    }

    _VERBOSE("ZeroMQ socket close.\n");
    zmq_close(socket);

    _VERBOSE("ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    _LOG_CLOSE();

    return 0;
}
