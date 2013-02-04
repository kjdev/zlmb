/*
 * zlmb dump
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <getopt.h>

#include "zlmb.h"
#include "dump.h"
#include "log.h"

#define ZLMB_SYSLOG_IDENT "zlmb-cli"

static int _syslog = 0;
static int _verbose = 0;

static void
_usage(char *arg, char *message)
{
    char *command = basename(arg);

    printf("Usage: %s [-e ENDPOINT] [-c] [-s] FILE\n\n", command);

    printf("  -e, --endpoint=ENDPOINT send server endpoint\n");
    printf("                           (client or publish frontentpoint)\n");
    printf("  -c, --continue          continue end of file\n");
    printf("  -s, --syslog            log to syslog\n");
    printf("  -v, --verbose           verbosity log\n");
#ifndef NDEBUG
    printf("  -q, --silent            quit debug message\n");
#endif

    if (message) {
        printf("\nINFO: %s\n", message);
    }
}

int
main (int argc, char **argv)
{
    int opt, continued = 0;
    char *endpoint = NULL;
    zlmb_dump_t *dump;
#ifndef NDEBUG
    int silent = 0;
#endif

    const struct option long_options[] = {
        { "endpoint", 1, NULL, 'e' },
        { "continue", 0, NULL, 'c' },
        { "syslog", 0, NULL, 's' },
        { "verbose", 0, NULL, 'v' },
#ifndef NDEBUG
        { "silent", 0, NULL, 'q' },
#endif
        { "help", 0, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv,
                              "e:csvqh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'e':
                endpoint = optarg;
                break;
            case 'c':
                continued = 1;
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

    if (argc <= optind) {
        _usage(argv[0], NULL);
        return -1;
    }

    _LOG_OPEN(ZLMB_SYSLOG_IDENT);

    _VERBOSE("File: %s\n", argv[optind]);
    _VERBOSE("EndPoint: %s\n", endpoint);

    void *context = NULL, *socket = NULL;

    if (endpoint) {
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

        _VERBOSE("ZeroMQ connect: %s\n", endpoint);
    }

    dump = zlmb_dump_init(argv[optind], 0);
    if (!dump) {
        _ERR("Dump initilized: %s\n", argv[optind]);
        if (socket)  {
            zmq_close(socket);
            zmq_ctx_destroy(context);
        }
        _LOG_CLOSE();
        return -1;
    }

    if (zlmb_dump_read_open(dump) != 0) {
        _ERR("Read in dump file: %s\n", argv[optind]);
    }

    _VERBOSE("Read start dump.\n");

    while (1) {
        int flags;
        zmq_msg_t zmsg;

        flags = zlmb_dump_read(dump, &zmsg);
        if (flags == 0) {
            break;
        } else if (flags == -1) {
            _ERR("Read format dump file: %s\n", argv[optind]);
            break;
        }

        if (zmq_msg_size(&zmsg) == 0) {
            zmq_msg_close(&zmsg);
            break;
        }

#ifndef NDEBUG
        if (!silent) {
            zlmb_dump_printmsg(stderr, &zmsg);
        }
#endif

        if (socket)  {
            _DEBUG("ZeroMQ send.\n");
            if (zmq_sendmsg(socket, &zmsg, flags) == -1) {
                _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
            }
        }

        zmq_msg_close(&zmsg);

        if (!continued && flags == 0) {
            break;
        }
    }

    _VERBOSE("Read end dump.\n");

    zlmb_dump_close(dump);

    //truncate
    if (zlmb_dump_truncate(dump) != 0) {
        _ERR("Truncate dump file: %s\n", argv[optind]);
    }

    zlmb_dump_destroy(&dump);

    if (socket)  {
        _VERBOSE("ZeroMQ socket close.");
        zmq_close(socket);
        _VERBOSE("ZeroMQ destroy context.");
        zmq_ctx_destroy(context);
    }

    _LOG_CLOSE();

    return 0;
}
