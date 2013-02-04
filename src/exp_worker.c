/*
 * Example worker
 *
 * Usage: exp-worker ENDPOINT
 *
 * % exp-client tcp://127.0.0.1:5560
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <getopt.h>
#include <signal.h>

#include <zmq.h>

#define _ERR(...) fprintf(stderr, "ERR: "__VA_ARGS__)

static int _interrupted = 0;

static void
_signal_handler(int sig)
{
    _interrupted = 1;
}

static void
_signals(void)
{
    struct sigaction sa;

    sa.sa_handler = _signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void
_usage(char *arg)
{
    char *command = basename(arg);

    printf("Usage: %s ENDPOINT\n\n", command);

    printf("  ENDPOINT  connect server endpoint\n");
}

int
main (int argc, char **argv)
{
    char *endpoint = NULL;
    void *context, *socket;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };

    if (argc <= 1) {
        _usage(argv[0]);
        return -1;
    }

    endpoint = argv[1];

    context = zmq_ctx_new();
    if (!context) {
        _ERR("ZeroMQ context: %s\n", zmq_strerror(errno));
        return -1;
    }

    socket = zmq_socket(context, ZMQ_PULL);
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

    pollitems[0].socket = socket;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, -1) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more;
            size_t moresz = sizeof(more);

            while (!_interrupted) {
                zmq_msg_t zmsg;

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(socket, &zmsg, 0) == -1) {
                    _ERR("ZeroMQ receive: %s\n", zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moresz) == -1) {
                    _ERR("ZeroMQ option receive: %s\n", zmq_strerror(errno));
                    more = 0;
                }

                fprintf(stdout, "%.*s\n",
                        (int)zmq_msg_size(&zmsg), (char *)zmq_msg_data(&zmsg));

                zmq_msg_close(&zmsg);

                if (!more) {
                    break;
                }
            }
        }
    }

    zmq_close(socket);
    zmq_ctx_destroy(context);

    return 0;
}
