/*
 * Example client
 *
 * Usage: exp-client ENDPOINT MESSAGE ...
 *
 * % exp-client tcp://127.0.0.1:5557 message
 * % exp-client tcp://127.0.0.1:5557 message1 message2 message3
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <getopt.h>

#include <zmq.h>

#define _ERR(...) fprintf(stderr, "ERR: "__VA_ARGS__)

static void
_usage(char *arg)
{
    char *command = basename(arg);

    printf("Usage: %s ENDPOINT MESSAGE ...\n\n", command);

    printf("  ENDPOINT    connect server endpoint\n");
    printf("  MESSAGE ... send messages\n");
}

int
main (int argc, char **argv)
{
    int i;
    char *endpoint = NULL;
    void *context, *socket;

    if (argc <= 2) {
        _usage(argv[0]);
        return -1;
    }

    endpoint = argv[1];

    context = zmq_ctx_new();
    if (!context) {
        _ERR("ZeroMQ context: %s\n", zmq_strerror(errno));
        return -1;
    }

    socket = zmq_socket(context, ZMQ_PUSH);
    if (!socket) {
        _ERR("ZeroMQ socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    if (zmq_connect(socket, endpoint) == -1) {
        _ERR("ZeroMQ connect: %s: %s\n", endpoint, zmq_strerror(errno));
        zmq_close(socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    for (i = 2; i != (argc - 1); i++) {
        if (zmq_send(socket, argv[i], strlen(argv[i]), ZMQ_SNDMORE) == -1) {
            _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
        }
    }
    if (zmq_send(socket, argv[i], strlen(argv[i]), 0) == -1) {
        _ERR("ZeroMQ send: %s\n", zmq_strerror(errno));
    }

    zmq_close(socket);
    zmq_ctx_destroy(context);

    return 0;
}
