/*
 * zlmb server
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <getopt.h>
#include <syslog.h>

#include <yaml.h>

#include "config.h"
#include "zlmb.h"
#include "option.h"
#include "dump.h"
#include "log.h"
#include "utils.h"

#ifdef USE_SNAPPY
#    include <snappy-c.h>
#endif

#define ZLMB_SYSLOG_IDENT "zlmb-server"

#define ZLMB_POLL_TIMEOUT 500

#define ZLMB_SENDMSG            1
#define ZLMB_SENDMSG_DUMP       2
#define ZLMB_SENDMSG_COMPRESS   3
#define ZLMB_SENDMSG_UNCOMPRESS 4

#define ZLMB_CLIENT_BACKEND_INPROC_SOCKET  "inproc://zlmb.client.backend"
#define ZLMB_CLIENT_PUBLISH_MONITOR_SOCKET "inproc://zlmb.publish.monitor"
#define ZLMB_SUBSCRIBE_MONITOR_SOCKET      "inproc://zlmb.subscribe.monitor"

#define _MODE(_l, ...) _##_l("%s: "__VA_ARGS__)
#define _CLIENT(_l, ...) _##_l(ZLMB_OPTION_MODE_CLIENT": "__VA_ARGS__)
#define _PUBLISH(_l, ...) _##_l(ZLMB_OPTION_MODE_PUBLISH": "__VA_ARGS__)
#define _SUBSCRIBE(_l, ...) _##_l(ZLMB_OPTION_MODE_SUBSCRIBE": "__VA_ARGS__)
#define _CLI_PUB(_l, ...) _##_l(ZLMB_OPTION_MODE_CLIENT_PUBLISH": "__VA_ARGS__)
#define _PUB_SUB(_l, ...) _##_l(ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE": "__VA_ARGS__)
#define _CLI_SUB(_l, ...) _##_l(ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE": "__VA_ARGS__)
#define _ALONE(_l, ...) _##_l(ZLMB_OPTION_MODE_STAND_ALONE": "__VA_ARGS__)

static int _interrupted = 0;
static int _syslog = 0;
static int _verbose = 0;
static pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _mutex_monitor = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    pthread_t thread;
    void *context;
    char *endpoint;
    int event;
    char *mode;
} zlmb_socket_monitor_t;

typedef struct {
    void *socket;
    char *endpoint;
    zlmb_socket_monitor_t *monitor;
} zlmb_client_inproc_socket_t;

typedef struct {
    void *context;
    int count;
    zlmb_client_inproc_socket_t **sockets;
    char *mode;
} zlmb_client_publish_t;

typedef struct {
    pthread_t thread;
    void *context;
    void *socket;
    char *endpoints;
    char *dumpfile;
    int dumptype;
    char *mode;
} zlmb_client_backend_t;

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
_socket_monitor_destroy(zlmb_socket_monitor_t **self)
{
    if (*self) {
        if ((*self)->endpoint) {
            free((*self)->endpoint);
            (*self)->endpoint = NULL;
        }
        free(*self);
        *self = NULL;
    }
}

static zlmb_socket_monitor_t *
_socket_monitor_init(void *context, void *socket, char *endpoint, char *mode)
{
    zlmb_socket_monitor_t *self;

    if (!context || !socket || !endpoint || !mode) {
        return NULL;
    }

    self = (zlmb_socket_monitor_t *)malloc(sizeof(zlmb_socket_monitor_t));
    if (!self) {
        return NULL;
    }

    memset(self, 0, sizeof(zlmb_socket_monitor_t));

    self->context = context;
    self->endpoint = strdup(endpoint);
    self->event = 0;
    self->mode = mode;

    //if (zmq_socket_monitor(socket, self->endpoint, ZMQ_EVENT_ALL) == -1) {
    if (zmq_socket_monitor(socket, self->endpoint,
                           ZMQ_EVENT_CONNECTED | ZMQ_EVENT_DISCONNECTED
                           | ZMQ_EVENT_ACCEPTED) == -1) {
        _MODE(ERR, "ZeroMQ socket monitor: %s\n", mode, zmq_strerror(errno));
        _socket_monitor_destroy(&self);
        return NULL;
    }

    return self;
}

static void *
_socket_monitor_event(void *arg)
{
    zlmb_socket_monitor_t *self = (zlmb_socket_monitor_t *)arg;
    void *socket;
    zmq_event_t event;

    if (!self) {
        _MODE(ERR, "Function arguments: %s\n", self->mode, __FUNCTION__);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex_monitor);
        return NULL;
    }

    socket = zmq_socket(self->context, ZMQ_PAIR);
    if (!socket) {
        _MODE(ERR, "ZeroMQ monitor socket: %s\n",
              self->mode, zmq_strerror(errno));
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex_monitor);
        return NULL;
    }

    _MODE(VERBOSE, "ZeroMQ monitor socket: PAIR\n", self->mode);

    if (zmq_connect(socket, self->endpoint) == -1) {
        _MODE(ERR, "ZeroMQ monitor connect: %s\n",
              self->mode, zmq_strerror(errno));
        zmq_close(socket);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex_monitor);
        return NULL;
    }

    _MODE(VERBOSE, "ZeroMQ monitor connect: %s\n", self->mode, self->endpoint);

    pthread_mutex_unlock(&_mutex_monitor);

    _signals();

    while (!_interrupted) {
        zmq_msg_t zmsg;

        zmq_msg_init(&zmsg);

        if (zmq_recvmsg(socket, &zmsg, 0) == -1 && zmq_errno() == ETERM) {
            zmq_msg_close(&zmsg);
            break;
        }

        if (zmq_msg_size(&zmsg) >= sizeof(event)) {
            memcpy(&event, zmq_msg_data(&zmsg), sizeof(event));

            switch (event.event) {
                case ZMQ_EVENT_CONNECTED:
                    _MODE(DEBUG, "ZeroMQ monitor event connected: %s\n",
                          self->mode, self->endpoint);
                    pthread_mutex_lock(&_mutex);
                    self->event |= ZMQ_EVENT_CONNECTED;
                    pthread_mutex_unlock(&_mutex);
                    break;
                case ZMQ_EVENT_ACCEPTED:
                    _MODE(DEBUG, "ZeroMQ monitor event accepted: %s\n",
                          self->mode, self->endpoint);
                    pthread_mutex_lock(&_mutex);
                    self->event |= ZMQ_EVENT_ACCEPTED;
                    pthread_mutex_unlock(&_mutex);
                    break;
                case ZMQ_EVENT_DISCONNECTED:
                    _MODE(DEBUG, "ZeroMQ monitor event disconnected: %s\n",
                          self->mode, self->endpoint);
                    pthread_mutex_lock(&_mutex);
                    self->event |= ZMQ_EVENT_DISCONNECTED;
                    pthread_mutex_unlock(&_mutex);
                    break;
                /*
                case ZMQ_EVENT_DELAYED:
                case ZMQ_EVENT_RETRIED:
                case ZMQ_EVENT_LISTENING:
                case ZMQ_EVENT_BIND_FAILED:
                case ZMQ_EVENT_ACCEPT_FAILED:
                case ZMQ_EVENT_CLOSED:
                case ZMQ_EVENT_CLOSE_FAILED:
                */
            }
        }

        zmq_msg_close(&zmsg);
    }

    zmq_close(socket);

    return NULL;
}

static int
_sendmsg(int type, void *socket, zmq_msg_t *zmsg, int flags,
         zlmb_dump_t *dump, char *mode)
{
    size_t in_len, out_len;
    char *in = NULL, *out = NULL;

#ifdef USE_SNAPPY
    if (type == ZLMB_SENDMSG_COMPRESS) {
        _MODE(DEBUG, "Compress message.\n", mode);
        in_len = zmq_msg_size(zmsg);
        out_len = snappy_max_compressed_length(in_len);
        in = zmq_msg_data(zmsg);
        out = (char *)malloc(out_len);
        if (out) {
            if (snappy_compress(in, in_len, out, &out_len) == SNAPPY_OK) {
                if (zmq_send(socket, out, out_len, flags) != -1) {
                    free(out);
                    return 0;
                } else {
                    _MODE(ERR, "ZeroMQ compress send: %s\n",
                          mode, zmq_strerror(errno));
                }
            } else {
                _MODE(ERR, "Compress Snappy.\n", mode);
            }
            free(out);
        } else {
            _MODE(ERR, "Memory allocate in compress.\n", mode);
        }
        type = ZLMB_SENDMSG;
    } else if (type == ZLMB_SENDMSG_UNCOMPRESS) {
        _MODE(DEBUG, "Uncompress message.\n", mode);
        in = zmq_msg_data(zmsg);
        in_len = zmq_msg_size(zmsg);
        if (snappy_uncompressed_length(in, in_len, &out_len) == SNAPPY_OK) {
            out = (char *)malloc(out_len);
            if (out) {
                if (snappy_uncompress(in, in_len, out, &out_len) == SNAPPY_OK) {
                    if (zmq_send(socket, out, out_len, flags) != -1) {
                        free(out);
                        return 0;
                    } else {
                        _MODE(ERR, "ZeroMQ uncompress send: %s\n",
                              mode, zmq_strerror(errno));
                    }
                } else {
                    _MODE(ERR, "Uncompress Snappy.\n", mode);
                }
                free(out);
            } else {
                _MODE(ERR, "Memory allocate in compress.\n", mode);
            }
        } else {
            _MODE(ERR, "Uncompress output length.\n", mode);
        }
        type = ZLMB_SENDMSG;
    }
#endif

    if (type == ZLMB_SENDMSG) {
        if (zmq_sendmsg(socket, zmsg, flags) != -1) {
            return 0;
        } else {
            _MODE(ERR, "ZeroMQ send message: %s\n", mode, zmq_strerror(errno));
        }
    }

    if (dump) {
        _MODE(NOTICE, "Send message in dump.\n", mode);
        if (zlmb_dump_write(dump, zmsg, flags) != -1) {
            return 0;
        } else {
            zlmb_dump_close(dump);
            _MODE(ERR, "Output message dump.\n", mode);
        }
    } else {
        _MODE(ERR, "Send message.\n", mode);
    }

    return -1;
}

static void
_client_publish_destroy(zlmb_client_publish_t **self)
{
    if (*self) {
        if ((*self)->sockets) {
            int i;
            for (i = 0; i < (*self)->count; i++) {
                if ((*self)->sockets[i]) {
                    if ((*self)->sockets[i]->monitor) {
                        _socket_monitor_destroy(&(*self)->sockets[i]->monitor);
                    }
                    if ((*self)->sockets[i]->socket) {
                        zmq_close((*self)->sockets[i]->socket);
                        (*self)->sockets[i]->socket = NULL;
                    }
                    if ((*self)->sockets[i]->endpoint) {
                        free((*self)->sockets[i]->endpoint);
                        (*self)->sockets[i]->endpoint = NULL;
                    }
                    free((*self)->sockets[i]);
                    (*self)->sockets[i] = NULL;
                }
            }
            free((*self)->sockets);
        }
        free(*self);
        *self = NULL;
    }
}

static zlmb_client_publish_t *
_client_publish_init(void *context, void *socket, char *endpoints, char *mode)
{
    int i, n = 0;
    char *token = NULL, *endpoint = endpoints;
    size_t size;
    zlmb_client_publish_t *self;

    if (!endpoint || strlen(endpoint) == 0) {
        return NULL;
    }

    while (1) {
        n++;
        token = strchr(endpoint, ',');
        if (token == NULL) {
            break;
        }

        endpoint = (++token);
    }

    if (n == 0) {
        return NULL;
    }

    self = (zlmb_client_publish_t *)malloc(sizeof(zlmb_client_publish_t));
    if (!self) {
        return NULL;
    }

    self->count = n;
    size = sizeof(zlmb_client_inproc_socket_t *) * self->count;

    self->sockets = (zlmb_client_inproc_socket_t **)malloc(size);
    if (!self->sockets) {
        _client_publish_destroy(&self);
        return NULL;
    }
    memset(self->sockets, 0, size);

    endpoint = strdup(endpoints);
    token = endpoint;

    for (i = 0; i < self->count; i++) {
        char *end, *mon;
        size_t size;

        end = strtok(token, ",");
        if (end == NULL) {
            _client_publish_destroy(&self);
            return NULL;
        }

        while (*end == ' ') {
            end++;
        }

        if (zlmb_utils_asprintf(&mon, "%s.%d.%d",
                                ZLMB_CLIENT_PUBLISH_MONITOR_SOCKET,
                                getpid(), i) == -1) {
            _client_publish_destroy(&self);
            return NULL;
        }

        size = sizeof(zlmb_client_inproc_socket_t);

        self->sockets[i] = (zlmb_client_inproc_socket_t *)malloc(size);
        if (!self->sockets[i]) {
            _client_publish_destroy(&self);
            return NULL;
        }

        memset(self->sockets[i], 0, size);

        self->sockets[i]->endpoint = strdup(end);

        self->sockets[i]->socket = zmq_socket(context, ZMQ_PUSH);
        if (self->sockets[i]->socket == NULL) {
            _client_publish_destroy(&self);
            return NULL;
        }

        _MODE(VERBOSE, "ZeroMQ client:publish socket: PUSH\n", mode);

        self->sockets[i]->monitor =
            _socket_monitor_init(context, self->sockets[i]->socket, mon, mode);
        if (self->sockets[i]->monitor == NULL) {
            _client_publish_destroy(&self);
            return NULL;
        }

        token = NULL;
        free(mon);
    }

    free(endpoint);

    self->mode = mode;

    return self;
}

static void
_client_publish_connect(zlmb_client_publish_t *self, void *socket, int *connect)
{
    int i;

    if (!self || !socket) {
        return;
    }

    for (i = 0; i < self->count; i++) {
        if (self->sockets[i]->monitor->event & ZMQ_EVENT_CONNECTED) {
            pthread_mutex_lock(&_mutex);
            if (zmq_connect(socket, self->sockets[i]->endpoint) != -1) {
                _MODE(VERBOSE, "ZeroMQ backend:publish connect(#%d): %s\n",
                      self->mode, i+1, self->sockets[i]->endpoint);
                (*connect)++;
                self->sockets[i]->monitor->event = 0;
            }
            pthread_mutex_unlock(&_mutex);
        }
        if (self->sockets[i]->monitor->event & ZMQ_EVENT_DISCONNECTED) {
            pthread_mutex_lock(&_mutex);
            if (zmq_disconnect(socket, self->sockets[i]->endpoint) != -1) {
                _MODE(VERBOSE, "ZeroMQ backend:publish disconnect(#%d): %s\n",
                      self->mode, i+1, self->sockets[i]->endpoint);
                (*connect)--;
                self->sockets[i]->monitor->event = 0;
            }
            pthread_mutex_unlock(&_mutex);
        }
    }
}


static void
_client_publish_monitor_stop(zlmb_client_publish_t *self)
{
    int i;

    if (!self) {
        return;
    }

    for (i = 0; i < self->count; i++) {
        if (self->sockets[i]->monitor->thread) {
            _MODE(VERBOSE, "Thread end backend:publish(#%d).\n",
                  self->mode, i+1);
            pthread_kill(self->sockets[i]->monitor->thread, SIGINT);
            pthread_join(self->sockets[i]->monitor->thread, NULL);
            self->sockets[i]->monitor->thread = 0;
        }
    }
}

static int
_client_publish_monitor_start(zlmb_client_publish_t *self)
{
    int i;

    if (!self) {
        return -1;
    }

    for (i = 0; i < self->count; i++) {
        pthread_mutex_lock(&_mutex_monitor);
        _MODE(VERBOSE, "Thread start backend:publish monitor(#%d).\n",
              self->mode, i+1);
        if (pthread_create(&(self->sockets[i]->monitor->thread), NULL,
                           _socket_monitor_event,
                           (void *)self->sockets[i]->monitor) == -1) {
            _MODE(ERR, "Thread create backend:publish monitor.\n", self->mode);
            return -1;
        }

        pthread_mutex_lock(&_mutex_monitor);
        pthread_mutex_unlock(&_mutex_monitor);

        if (_interrupted) {
            _MODE(ERR, "Thread started backend:publish monitor: %s\n",
                  self->mode, self->sockets[i]->endpoint);
            return -1;
        }

        if (zmq_connect(self->sockets[i]->socket,
                        self->sockets[i]->endpoint) == -1) {
            _MODE(ERR, "ZeroMQ backend:publish monitor connect: %s: %s\n",
                  self->mode, self->sockets[i]->endpoint, zmq_strerror(errno));
            return -1;
        }
        _MODE(VERBOSE, "ZeroMQ backend:publish monitor connect(#%d): %s\n",
              self->mode, i+1, self->sockets[i]->endpoint);
    }

    return 0;
}

static void
_client_publish_gc(zlmb_client_publish_t *self, void *inproc,
                   void *publish, int connect, zlmb_dump_t *dump)
{
    zmq_pollitem_t pollitems[] = { { inproc, 0, ZMQ_POLLIN, 0 } };

    if (!self || !inproc || !publish){
        _MODE(ERR, "Function arguments: %s\n", self->mode, __FUNCTION__);
        return;
    }

    _client_publish_connect(self, publish, &connect);

    while (1) {
        if (zmq_poll(pollitems, 1, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            _MODE(DEBUG, "ZeroMQ backend:inproc receive in poll event(GC).\n",
                  self->mode);

            int more, send, flags;
            size_t moresz = sizeof(more);

            if (connect > 0) {
                send = ZLMB_SENDMSG;
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _MODE(DEBUG, "ZeroMQ backend:inproc receive message(GC).\n",
                      self->mode);

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(inproc, &zmsg, 0) == -1) {
                    _MODE(ERR, "ZeroMQ backend:inproc receive(GC): %s\n",
                          self->mode, zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(inproc, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _MODE(ERR,
                          "ZeroMQ backend:inproc receive socket option(GC):"
                          " %s\n", self->mode, zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }
#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _MODE(DEBUG, "ZeroMQ backend:publish send message.\n",
                      self->mode);

                _sendmsg(send, publish, &zmsg, flags, dump, self->mode);

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        } else {
            break;
        }

        _client_publish_connect(self, publish, &connect);
    }
}

static void *
_client_backend(void *arg)
{
    int connect = 0;
    zlmb_client_backend_t *self = (zlmb_client_backend_t *)arg;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 },
                                   { NULL, 0, ZMQ_POLLIN, 0 } };
    zlmb_dump_t *dump = NULL;
    void *socket_inproc, *socket_publish;
    zlmb_client_publish_t *publish;

    if (!self || !self->context) {
        _MODE(ERR, "Function arguments: %s\n", self->mode, __FUNCTION__);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    /* backend:inproc */
    socket_inproc = zmq_socket(self->context, ZMQ_PULL);
    if (!socket_inproc) {
        _MODE(ERR, "ZeroMQ backend:inproc socket: %s\n",
              self->mode, zmq_strerror(errno));
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    _MODE(VERBOSE, "ZeroMQ backend:inproc socket: PULL\n", self->mode);

    if (zmq_connect(socket_inproc, ZLMB_CLIENT_BACKEND_INPROC_SOCKET) == -1) {
        _MODE(ERR, "ZeroMQ backend:inproc connect: %s\n",
              self->mode, zmq_strerror(errno));
        zmq_close(socket_inproc);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    _MODE(VERBOSE, "ZeroMQ backend:inproc connect: %s\n",
          self->mode, ZLMB_CLIENT_BACKEND_INPROC_SOCKET);

    /* backend:publish */
    socket_publish = zmq_socket(self->context, ZMQ_PUSH);
    if (!socket_publish) {
        _MODE(ERR, "ZeroMQ backend:publish socket: %s\n",
              self->mode, zmq_strerror(errno));
        zmq_close(socket_inproc);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    _MODE(VERBOSE, "ZeroMQ backend:publish socket: PUSH\n", self->mode);

    publish = _client_publish_init(self->context, socket_publish,
                                   self->endpoints, self->mode);
    if (!publish) {
        _MODE(ERR, "ZeroMQ backend:publish initilized.\n", self->mode);
        zmq_close(socket_inproc);
        zmq_close(socket_publish);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    /* backend:publish monitoring */
    _MODE(VERBOSE, "Monitor start backend:publish.\n", self->mode);
    if (_client_publish_monitor_start(publish) == -1) {
        _MODE(ERR, "Monitor backend:publish started.\n", self->mode);
        _MODE(VERBOSE, "Monitor stop backend:publish.\n", self->mode);
        _client_publish_monitor_stop(publish);
        _client_publish_destroy(&publish);
        zmq_close(socket_inproc);
        zmq_close(socket_publish);
        _interrupted = 1;
        pthread_mutex_unlock(&_mutex);
        return NULL;
    }

    pthread_mutex_unlock(&_mutex);

    /* dump */
    dump = zlmb_dump_init(self->dumpfile, self->dumptype);

    /* poll */
    _MODE(VERBOSE, "ZeroMQ start backend proxy.\n", self->mode);

    pollitems[0].socket = socket_inproc;
    pollitems[1].socket = socket_publish;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 2, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, flags, send;
            size_t moresz = sizeof(more);

            _MODE(DEBUG, "ZeroMQ backend:inproc receive in poll event.\n",
                  self->mode);

            if (connect > 0) {
#ifdef USE_SNAPPY
                send = ZLMB_SENDMSG_COMPRESS;
#else
                send = ZLMB_SENDMSG;
#endif
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _MODE(DEBUG, "ZeroMQ backend:inproc receive message.\n",
                      self->mode);

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(socket_inproc, &zmsg, 0) == -1) {
                    _MODE(ERR, "ZeroMQ backend:inproc receive: %s\n",
                          self->mode, zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(socket_inproc, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _MODE(ERR,
                          "ZeroMQ backend:inproc receive socket option: %s\n",
                          self->mode, zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }
#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _MODE(DEBUG, "ZeroMQ backend:publish send message.\n",
                      self->mode);

                _sendmsg(send, socket_publish, &zmsg, flags, dump, self->mode);

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        _client_publish_connect(publish, socket_publish, &connect);

        //_MODE(DEBUG, "sleep(10)", self->mode);
        //sleep(10);
    }

    _MODE(VERBOSE, "ZeroMQ end backend proxy.\n", self->mode);

    /* gc */
    _client_publish_gc(publish, socket_inproc, socket_publish, connect, dump);

    /* publish: monitoring */
    _MODE(VERBOSE, "Monitor stop backend:publish.\n", self->mode);
    _client_publish_monitor_stop(publish);
    _client_publish_destroy(&publish);

    /* socket close */
    _MODE(VERBOSE, "ZeroMQ backend sockets.\n", self->mode);
    zmq_close(socket_inproc);
    zmq_close(socket_publish);

    /* dump: cleanup */
    if (dump) {
        zlmb_dump_destroy(&dump);
    }

    return NULL;
}

static void
_subscribe_monitor_connect(zlmb_socket_monitor_t *self, int *connect)
{
    if (!self) {
        return;
    }

    if (self->event & ZMQ_EVENT_ACCEPTED) {
        pthread_mutex_lock(&_mutex);
        (*connect)++;
        self->event = 0;
        pthread_mutex_unlock(&_mutex);
    }
    if (self->event & ZMQ_EVENT_DISCONNECTED) {
        pthread_mutex_lock(&_mutex);
        (*connect)--;
        self->event = 0;
        pthread_mutex_unlock(&_mutex);
    }
}

//------------------------------------------------------------------------------

static int
_server_client(char *frontendpoint, char *backendpoints,
               char *dumpfile, int dumptype)
{
    void *context, *frontend;
    zlmb_client_backend_t backend = { 0, NULL, NULL, backendpoints,
                                      dumpfile, dumptype,
                                      ZLMB_OPTION_MODE_CLIENT };

    if (!frontendpoint || strlen(frontendpoint) == 0) {
        _CLIENT(ERR, "frontendpoint.\n");
        return -1;
    }

    if (!backendpoints || strlen(backendpoints) == 0) {
        _CLIENT(ERR, "backendpoints.\n");
        return -1;
    }

    _CLIENT(INFO, "Bind front endpoint: %s\n", frontendpoint);
    _CLIENT(INFO, "Connect back endpoint: %s\n", backendpoints);
    _CLIENT(INFO, "Dump file: %s (%s)\n",
            dumpfile, zlmb_option_dumptype2string(dumptype));

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _CLIENT(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _CLIENT(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLIENT(VERBOSE, "ZeroMQ frontend socket: PULL\n")

    if (zmq_bind(frontend, frontendpoint) == -1) {
        _CLIENT(ERR, "ZeroMQ frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLIENT(VERBOSE, "ZeroMQ frontend bind: %s\n", frontendpoint);

    /* backend */
    backend.context = context;
    backend.socket = zmq_socket(context, ZMQ_PUSH);
    if (!backend.socket) {
        _CLIENT(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLIENT(VERBOSE, "ZeroMQ backend socket: PUSH\n")

    /* backend: bind */
    if (zmq_bind(backend.socket, ZLMB_CLIENT_BACKEND_INPROC_SOCKET) == -1) {
        _CLIENT(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_close(backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLIENT(VERBOSE, "ZeroMQ backend bind: %s\n",
            ZLMB_CLIENT_BACKEND_INPROC_SOCKET);

    /* backend: thread */
    pthread_mutex_lock(&_mutex);
    _CLIENT(VERBOSE, "Thread start backend.\n");
    if (pthread_create(&(backend.thread), NULL,
                       _client_backend, (void *)&backend) == -1) {
        _CLIENT(ERR, "Thread create backend.\n");
        zmq_close(frontend);
        zmq_close(backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    pthread_mutex_lock(&_mutex);
    pthread_mutex_unlock(&_mutex);

    if (!_interrupted) {
        _CLIENT(VERBOSE, "ZeroMQ start proxy.\n");
        _signals();
        zmq_proxy(frontend, backend.socket, NULL);
        _CLIENT(VERBOSE, "ZeroMQ end proxy.\n");
    }

    /* frontend: unbind */
    _CLIENT(VERBOSE, "ZeroMQ frontend unnbind: %s", frontendpoint);
    zmq_unbind(frontend, frontendpoint);

    /* backend: cleanup */
    _CLIENT(VERBOSE, "Thread end backend.\n");
    pthread_kill(backend.thread, SIGINT);
    pthread_join(backend.thread, NULL);

    /* sockets: cleanup */
    _CLIENT(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend.socket);

    /* context: cleanup */
    _CLIENT(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    return 0;
}

static int
_server_publish(char *frontendpoint, char *backendpoint, char *key, int sendkey)
{
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    void *context, *frontend, *backend;
    size_t key_len = 0;
    char *compress_key = NULL;

    if (!frontendpoint || strlen(frontendpoint) == 0) {
        _PUBLISH(ERR, "frontendpoint.\n");
        return -1;
    }

    if (!backendpoint || strlen(backendpoint) == 0) {
        _PUBLISH(ERR, "backendpoint.\n");
        return -1;
    }

    if (!sendkey) {
        key = NULL;
    } else {
        if (!key) {
            key = "";
        }
        key_len = strlen(key);
    }

    _PUBLISH(INFO, "Bind front endpoint: %s\n", frontendpoint);
    _PUBLISH(INFO, "Bind back endpoint: %s\n", backendpoint);
    _PUBLISH(INFO, "Publish key: %s\n", key);
    if (sendkey) {
        _PUBLISH(INFO, "Send publish key: enable\n");
    } else {
        _PUBLISH(INFO, "Send publish key: disable\n");
    }

#ifdef USE_SNAPPY
    if (key) {
        size_t compress_key_len = snappy_max_compressed_length(key_len);
        compress_key = (char *)malloc(compress_key_len);
        if (compress_key) {
            if (snappy_compress(key, key_len,
                                compress_key, &compress_key_len) == SNAPPY_OK) {
                key = compress_key;
                key_len = compress_key_len;
            } else {
                free(compress_key);
                compress_key = NULL;
            }
        }
    }
#endif

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _PUBLISH(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
        if (compress_key) {
            free(compress_key);
        }
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _PUBLISH(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _PUBLISH(VERBOSE, "ZeroMQ frontend socket: PULL\n")

    if (zmq_bind(frontend, frontendpoint) == -1) {
        _PUBLISH(ERR, "ZeroMQ frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _PUBLISH(VERBOSE, "ZeroMQ frontend bind: %s\n", frontendpoint);

    /* backend */
    backend = zmq_socket(context, ZMQ_PUB);
    if (!backend) {
        _PUBLISH(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _PUBLISH(VERBOSE, "ZeroMQ backend socket: PUB\n")

    if (zmq_bind(backend, backendpoint) == -1) {
        _PUBLISH(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _PUBLISH(VERBOSE, "ZeroMQ backend bind: %s\n", frontendpoint);

    /* poll */
    _PUBLISH(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, -1) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, flags, first = 1;
            size_t moresz = sizeof(more);

            _PUBLISH(DEBUG, "ZeroMQ frontend receive in poll event.\n");

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _PUBLISH(DEBUG, "ZeroMQ frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                    _PUBLISH(ERR, "ZeroMQ frontend receive: %s\n",
                             zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _PUBLISH(ERR, "ZeroMQ frontend receive socket option: %s\n",
                             zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }

                if (key && first) {
#ifndef NDEBG
                    zlmb_dump_print(stderr, key, key_len);
#endif
                    _PUBLISH(DEBUG,
                             "ZeroMQ backend send message(publish key).\n");

                    if (zmq_send(backend, key, key_len, ZMQ_SNDMORE) == -1) {
                        _PUBLISH(ERR, "ZeroMQ backend send: %s\n",
                                 zmq_strerror(errno));
                    }
                    first = 0;
                }

#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _PUBLISH(DEBUG, "ZeroMQ backend send message.\n");

                if (zmq_sendmsg(backend, &zmsg, flags) == -1) {
                    _PUBLISH(ERR, "ZeroMQ backend send: %s\n",
                             zmq_strerror(errno));
                    //break;
                }

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }
    }

    _PUBLISH(VERBOSE, "ZeroMQ end proxy.\n");

    /* sockets: cleanup */
    _PUBLISH(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend);

    /* context: cleanup */
    _PUBLISH(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    if (compress_key) {
        free(compress_key);
    }

    return 0;
}

static int
_server_subscribe(char *frontendpoints, char *backendpoint,
                  char *key, int dropkey, char *dumpfile, int dumptype)
{
    int connect = 0;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    zlmb_dump_t *dump = NULL;
    char *endpoint, *token;
    void *context, *frontend, *backend;
    zlmb_socket_monitor_t *monitor;
    char *compress_key = NULL;
    size_t key_len = 0;

    if (!frontendpoints || strlen(frontendpoints) == 0) {
        _SUBSCRIBE(ERR, "frontendpoints.\n");
        return -1;
    }

    if (!backendpoint || strlen(backendpoint) == 0) {
        _SUBSCRIBE(ERR, "backendpoint.\n");
        return -1;
    }

    if (!key) {
        key = "";
    } else {
        key_len = strlen(key);
    }

    _SUBSCRIBE(INFO, "Connect front endpoint: %s\n", frontendpoints);
    _SUBSCRIBE(INFO, "Bind back endpoint: %s\n", backendpoint);
    _SUBSCRIBE(INFO, "Subscribe key: %s\n", key);
    if (dropkey) {
        _SUBSCRIBE(INFO, "Drop subscribe key: enable\n");
    } else {
        _SUBSCRIBE(INFO, "Drop subscribe key: disable\n");
    }
    _SUBSCRIBE(INFO, "Dump file: %s (%s)\n",
               dumpfile, zlmb_option_dumptype2string(dumptype));

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _SUBSCRIBE(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_SUB);
    if (!frontend) {
        _SUBSCRIBE(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    _SUBSCRIBE(VERBOSE, "ZeroMQ frontend socket: SUB\n")

#ifdef USE_SNAPPY
    if (key_len > 0) {
        size_t compress_key_len = snappy_max_compressed_length(key_len);
        compress_key = (char *)malloc(compress_key_len);
        if (compress_key) {
            if (snappy_compress(key, key_len,
                                compress_key, &compress_key_len) == SNAPPY_OK) {
                key = compress_key;
                key_len = compress_key_len;
            } else {
                free(compress_key);
                compress_key = NULL;
            }
        }
    }
#endif

    if (zmq_setsockopt(frontend, ZMQ_SUBSCRIBE, key, key_len) == -1) {
        _SUBSCRIBE(ERR, "ZeroMQ frontend subscribe key: %s\n",
                   zmq_strerror(errno));
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    if (compress_key) {
        free(compress_key);
    }

    /* frontend: connect */
    endpoint = strdup(frontendpoints);
    token = endpoint;

    while (1) {
        char *end;

        end = strtok(token, ",");
        if (end == NULL) {
            break;
        }

        while (*end == ' ') {
            end++;
        }

        if (zmq_connect(frontend, end) == -1) {
            _SUBSCRIBE(ERR, "ZeroMQ frontend connect: %s: %s\n",
                       end, zmq_strerror(errno));
            free(endpoint);
            zmq_close(frontend);
            zmq_ctx_destroy(context);
            return -1;
        }

        _SUBSCRIBE(VERBOSE, "ZeroMQ frontend connect: %s\n", end);

        token = NULL;
    }

    free(endpoint);

    /* backend */
    backend = zmq_socket(context, ZMQ_PUSH);
    if (!backend) {
        _SUBSCRIBE(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _SUBSCRIBE(VERBOSE, "ZeroMQ backend socket: PUSH\n")

    /* backend: monitoring */
    if (zlmb_utils_asprintf(&endpoint, "%s.%d",
                            ZLMB_SUBSCRIBE_MONITOR_SOCKET, getpid()) == -1) {
        _SUBSCRIBE(ERR, "Allocate monitor string backend point.\n");
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    monitor = _socket_monitor_init(context, backend, endpoint,
                                   ZLMB_OPTION_MODE_SUBSCRIBE);
    if (monitor == NULL) {
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    free(endpoint);

    _SUBSCRIBE(VERBOSE, "Thread start backend monitor.\n");
    if (pthread_create(&(monitor->thread), NULL,
                       _socket_monitor_event, (void *)monitor) == -1) {
        _SUBSCRIBE(ERR, "Thread create backend monitor.\n");
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    /* backend: bind */
    if (zmq_bind(backend, backendpoint) == -1) {
        _SUBSCRIBE(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        _SUBSCRIBE(VERBOSE, "Thread end backend monitor.\n");
        if (pthread_kill(monitor->thread, 0) == 0) {
            pthread_kill(monitor->thread, SIGINT);
            pthread_join(monitor->thread, NULL);
        }
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _SUBSCRIBE(VERBOSE, "ZeroMQ backend bind: %s\n", backendpoint);

    /* dump */
    dump = zlmb_dump_init(dumpfile, dumptype);

    /* poll */
    _SUBSCRIBE(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, flags, send, frames = 0;
            size_t moresz = sizeof(more);

            _SUBSCRIBE(DEBUG, "ZeroMQ fronend receive in poll event.\n");

            if (connect > 0) {
#ifdef USE_SNAPPY
                send = ZLMB_SENDMSG_UNCOMPRESS;
#else
                send = ZLMB_SENDMSG;
#endif
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _SUBSCRIBE(DEBUG, "ZeroMQ fronend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                    _SUBSCRIBE(ERR, "ZeroMQ frontend receive: %s\n",
                               zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                frames++;

                if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _SUBSCRIBE(ERR,
                               "ZeroMQ frontend receive socket option: %s\n",
                               zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }
#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                if (!dropkey || frames != 1) {
                    _SUBSCRIBE(DEBUG, "ZeroMQ backend send message.\n");
                    _sendmsg(send, backend, &zmsg, flags,
                             dump, ZLMB_OPTION_MODE_SUBSCRIBE);
                }

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        _subscribe_monitor_connect(monitor, &connect);
    }

    _SUBSCRIBE(VERBOSE, "ZeroMQ end proxy.\n");

    /* gc ? */
    //TODO

    /* backend monitoring: cleanup */
    _SUBSCRIBE(VERBOSE, "Thread end backend monitor.\n");
    pthread_kill(monitor->thread, SIGINT);
    pthread_join(monitor->thread, NULL);
    _socket_monitor_destroy(&monitor);

    /* sockets: cleanup */
    _SUBSCRIBE(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend);

    /* context: cleanup */
    _SUBSCRIBE(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    /* dump: cleanup */
    if (dump) {
        zlmb_dump_destroy(&dump);
    }

    return 0;
}

int
_server_client_publish(char *frontendpoint, char *backendpoint,
                       char *key, int sendkey)
{
    void *context, *frontend, *backend;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    size_t key_len = 0;
    char *compress_key = NULL;

    if (!frontendpoint || strlen(frontendpoint) == 0) {
        _CLI_PUB(ERR, "frontend.\n");
        return -1;
    }

    if (!backendpoint || strlen(backendpoint) == 0) {
        _CLI_PUB(ERR, "backend.\n");
        return -1;
    }

    if (!sendkey) {
        key = NULL;
    } else {
        if (!key) {
            key = "";
        }
        key_len = strlen(key);
    }

    _CLI_PUB(INFO, "Bind front endpoint: %s\n", frontendpoint);
    _CLI_PUB(INFO, "Bind back endpoint: %s\n", backendpoint);
    _CLI_PUB(INFO, "Publish key: %s\n", key);
    if (sendkey) {
        _CLI_PUB(INFO, "Send publish key: enable\n");
    } else {
        _CLI_PUB(INFO, "Send publish key: disable\n");
    }

#ifdef USE_SNAPPY
    if (key) {
        size_t compress_key_len = snappy_max_compressed_length(key_len);
        compress_key = (char *)malloc(compress_key_len);
        if (compress_key) {
            if (snappy_compress(key, key_len,
                                compress_key, &compress_key_len) == SNAPPY_OK) {
                key = compress_key;
                key_len = compress_key_len;
            } else {
                free(compress_key);
                compress_key = NULL;
            }
        }
    }
#endif

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _CLI_PUB(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
        if (compress_key) {
            free(compress_key);
        }
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _CLI_PUB(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _CLI_PUB(VERBOSE, "ZeroMQ frontend socket: PULL\n")

    if (zmq_bind(frontend, frontendpoint) == -1) {
        _CLI_PUB(ERR, "ZeroMQ frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _CLI_PUB(VERBOSE, "ZeroMQ frontend bind: %s\n", frontendpoint);

    /* backend */
    backend = zmq_socket(context, ZMQ_PUB);
    if (!backend) {
        _CLI_PUB(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _CLI_PUB(VERBOSE, "ZeroMQ backend socket: PUB\n")

    if (zmq_bind(backend, backendpoint) == -1) {
        _CLI_PUB(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    _CLI_PUB(VERBOSE, "ZeroMQ backend bind: %s\n", backendpoint);

    /* poll */
    _CLI_PUB(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, -1) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, flags, first = 1;
            size_t moresz = sizeof(more);
#ifdef USE_SNAPPY
            int send = ZLMB_SENDMSG_COMPRESS;
#else
            int send = ZLMB_SENDMSG;
#endif
            _CLI_PUB(DEBUG, "ZeroMQ frontend receive in poll event.\n");
            while (!_interrupted) {
                zmq_msg_t zmsg;

                _CLI_PUB(DEBUG, "ZeroMQ frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                    _CLI_PUB(ERR, "ZeroMQ frontend receive: %s\n",
                             zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _CLI_PUB(ERR, "ZeroMQ frontend receive socket option: %s\n",
                             zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }

                if (key && first) {
#ifndef NDEBG
                    zlmb_dump_print(stderr, key, key_len);
#endif
                    _CLI_PUB(DEBUG,
                             "ZeroMQ backend send message(publish key).\n");

                    if (zmq_send(backend, key, key_len, ZMQ_SNDMORE) == -1) {
                        _CLI_PUB(ERR, "ZeroMQ frontend send: %s\n",
                                 zmq_strerror(errno));
                    }
                    first = 0;
                }

#ifndef NDEBG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _CLI_PUB(DEBUG, "ZeroMQ backend send message.\n");

                _sendmsg(send, backend, &zmsg, flags, NULL,
                         ZLMB_OPTION_MODE_CLIENT_PUBLISH);

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }
    }

    _CLI_PUB(VERBOSE, "ZeroMQ end proxy.\n");

    /* sockets: cleanup */
    _CLI_PUB(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend);

    /* context: cleanup */
    _CLI_PUB(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    if (compress_key) {
        free(compress_key);
    }

    return 0;
}

int
_server_publish_subscribe(char *frontendpoint, char *backendpoint,
                          char *dumpfile, int dumptype)
{
    int connect = 0;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    zlmb_dump_t *dump = NULL;
    void *context, *frontend, *backend;
    char *endpoint;
    zlmb_socket_monitor_t *monitor;

    if (!frontendpoint || strlen(frontendpoint) == 0) {
        _PUB_SUB(ERR, "frontend.\n");
        return -1;
    }

    if (!backendpoint || strlen(backendpoint) == 0) {
        _PUB_SUB(ERR, "backend.\n");
        return -1;
    }

    _PUB_SUB(INFO, "Bind front endpoint: %s\n", frontendpoint);
    _PUB_SUB(INFO, "Bind back endpoint: %s\n", backendpoint);
    _PUB_SUB(INFO, "Dump file: %s (%s)\n",
             dumpfile, zlmb_option_dumptype2string(dumptype));

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _PUB_SUB(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _PUB_SUB(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    _PUB_SUB(VERBOSE, "ZeroMQ frontend socket: PULL\n")

    if (zmq_bind(frontend, frontendpoint) == -1) {
        _PUB_SUB(ERR, "ZeroMQ frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _PUB_SUB(VERBOSE, "ZeroMQ frontend bind: %s\n", frontendpoint);

    /* backend */
    backend = zmq_socket(context, ZMQ_PUSH);
    if (!backend) {
        _PUB_SUB(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _PUB_SUB(VERBOSE, "ZeroMQ backend socket: PUSH\n")

    /* backend: bind */
    if (zmq_bind(backend, backendpoint) == -1) {
        _PUB_SUB(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _PUB_SUB(VERBOSE, "ZeroMQ backend bind: %s\n", backendpoint);

    /* backend: monitoring */
    if (zlmb_utils_asprintf(&endpoint, "%s.%d",
                            ZLMB_SUBSCRIBE_MONITOR_SOCKET, getpid()) == -1) {
        _PUB_SUB(ERR, "Allocate monitor string backend point.\n");
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    monitor = _socket_monitor_init(context, backend, endpoint,
                                   ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE);
    if (monitor == NULL) {
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    free(endpoint);

    _PUB_SUB(VERBOSE, "Start client backend monitor thread.\n");
    if (pthread_create(&(monitor->thread), NULL,
                       _socket_monitor_event, (void *)monitor) == -1) {
        _PUB_SUB(ERR, "Create backend monitor thread.\n");
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    /* dump */
    dump = zlmb_dump_init(dumpfile, dumptype);

    /* poll */
    _PUB_SUB(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, send, flags;
            size_t moresz = sizeof(more);

            _PUB_SUB(DEBUG, "ZeroMQ frontend receive in poll event.\n");

            if (connect > 0) {
#ifdef USE_SNAPPY
                send = ZLMB_SENDMSG_UNCOMPRESS;
#else
                send = ZLMB_SENDMSG;
#endif
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _PUB_SUB(DEBUG, "ZeroMQ frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                    _PUB_SUB(ERR, "ZeroMQ frontend receive: %s\n",
                             zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _PUB_SUB(ERR, "ZeroMQ frontend receive socket option: %s\n",
                             zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }

#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _PUB_SUB(DEBUG, "ZeroMQ backend send message.\n");

                _sendmsg(send, backend, &zmsg, flags, dump,
                         ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE);

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        _subscribe_monitor_connect(monitor, &connect);
    }

    _PUB_SUB(VERBOSE, "ZeroMQ end proxy.\n");

    /* backend monitoring: cleanup */
    _PUB_SUB(VERBOSE, "End backend monitor thread.\n");
    pthread_kill(monitor->thread, SIGINT);
    pthread_join(monitor->thread, NULL);
    _socket_monitor_destroy(&monitor);

    /* sockets: cleanup */
    _PUB_SUB(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend);

    /* context: cleanup */
    _PUB_SUB(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    /* dump: cleanup */
    if (dump) {
        zlmb_dump_destroy(&dump);
    }

    return 0;
}

static int
_server_client_subscribe(char *client_frontendpoint,
                         char *client_backendpoints,
                         char *client_dumpfile,
                         int client_dumptype,
                         char *subscribe_frontendpoints,
                         char *subscribe_backendpoint,
                         char *subscribe_key, int subscribe_dropkey,
                         char *subscribe_dumpfile,
                         int subscribe_dumptype)
{
    int subscribe_connect = 0;
    zlmb_client_backend_t client_backend =
        { 0, NULL, NULL, client_backendpoints,
          client_dumpfile, client_dumptype,
          ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE };
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 },
                                   { NULL, 0, ZMQ_POLLIN, 0 } };
    zlmb_dump_t *subscribe_dump = NULL;
    char *endpoint, *token;
    void *context, *client_frontend;
    void *subscribe_frontend, *subscribe_backend;
    zlmb_socket_monitor_t *subscribe_monitor;
    char *compress_key = NULL;
    size_t key_len = 0;

    if (!client_frontendpoint || strlen(client_frontendpoint) == 0) {
        _CLI_SUB(ERR, "Client frontendpoint.\n");
        return -1;
    }

    if (!client_backendpoints || strlen(client_backendpoints) == 0) {
        _CLI_SUB(ERR, "Client backendpoints.\n");
        return -1;
    }

    if (!subscribe_frontendpoints || strlen(subscribe_frontendpoints) == 0) {
        _CLI_SUB(ERR, "Subscribe frontendpoints.\n");
        return -1;
    }

    if (!subscribe_backendpoint || strlen(subscribe_backendpoint) == 0) {
        _CLI_SUB(ERR, "Subscribe backendpoint.\n");
        return -1;
    }

    if (!subscribe_key) {
        subscribe_key = "";
    } else {
        key_len = strlen(subscribe_key);
    }

    _CLI_SUB(INFO, "Client Bind front endpoint: %s\n", client_frontendpoint);
    _CLI_SUB(INFO, "Client Connect back endpoint: %s\n", client_backendpoints);
    _CLI_SUB(INFO, "Client Dump file: %s (%s)\n",
             client_dumpfile, zlmb_option_dumptype2string(client_dumptype));
    _CLI_SUB(INFO, "Subscribe Connect front endpoint: %s\n",
             subscribe_frontendpoints);
    _CLI_SUB(INFO, "Subscribe Bind back endpoint: %s\n", subscribe_backendpoint);
    _CLI_SUB(INFO, "Subscribe Key: %s\n", subscribe_key);
    if (subscribe_dropkey) {
        _CLI_SUB(INFO, "Drop subscribe key: enable\n");
    } else {
        _CLI_SUB(INFO, "Drop subscribe key: disable\n");
    }
    _CLI_SUB(INFO, "Subscribe Dump file: %s (%s)",
             subscribe_dumpfile,
             zlmb_option_dumptype2string(subscribe_dumptype));

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _CLI_SUB(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
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

    /* client:frontend */
    client_frontend = zmq_socket(context, ZMQ_PULL);
    if (!client_frontend) {
        _CLI_SUB(ERR, "ZeroMQ client frontend socket: %s\n",
                 zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    _PUB_SUB(VERBOSE, "ZeroMQ client frontend socket: PULL\n")

    if (zmq_bind(client_frontend, client_frontendpoint) == -1) {
        _CLI_SUB(ERR, "ZeroMQ client frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(client_frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ client frontend bind: %s\n", client_frontendpoint);

    /* client:backend */
    client_backend.context = context;

    client_backend.socket = zmq_socket(context, ZMQ_PUSH);
    if (!client_backend.socket) {
        _CLI_SUB(ERR, "ZeroMQ client backend socket: %s\n", zmq_strerror(errno));
        zmq_close(client_frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ client backend socket: PUSH\n")

    /* client:backend: bind */
    if (zmq_bind(client_backend.socket,
                 ZLMB_CLIENT_BACKEND_INPROC_SOCKET) == -1) {
        _CLI_SUB(ERR, "ZeroMQ client backend bind: %s\n", zmq_strerror(errno));
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ client backend bind: %s\n",
             ZLMB_CLIENT_BACKEND_INPROC_SOCKET);

    /* client:backend: thread */
    pthread_mutex_lock(&_mutex);
    _CLI_SUB(INFO, "Thread start client backend.\n");
    if (pthread_create(&(client_backend.thread), NULL,
                       _client_backend, (void *)&client_backend) == -1) {
        _CLI_SUB(ERR, "Thread create client backend.\n");
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    pthread_mutex_lock(&_mutex);
    pthread_mutex_unlock(&_mutex);

    if (pthread_kill(client_backend.thread, 0) != 0 || _interrupted) {
        _CLI_SUB(ERR, "Thread end client backend.\n");
        zmq_unbind(client_frontend, client_frontendpoint);
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    /* subscribe:frontend */
    subscribe_frontend = zmq_socket(context, ZMQ_SUB);
    if (!subscribe_frontend) {
        _CLI_SUB(ERR, "ZeroMQ subscribe frontend socket: %s\n",
                 zmq_strerror(errno));
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ subscribe frontend socket: SUB\n")

#ifdef USE_SNAPPY
    if (key_len > 0) {
        size_t compress_key_len = snappy_max_compressed_length(key_len);
        compress_key = (char *)malloc(compress_key_len);
        if (compress_key) {
            if (snappy_compress(subscribe_key, key_len,
                                compress_key, &compress_key_len) == SNAPPY_OK) {
                subscribe_key = compress_key;
                key_len = compress_key_len;
            } else {
                free(compress_key);
                compress_key = NULL;
            }
        }
    }
#endif

    if (zmq_setsockopt(subscribe_frontend, ZMQ_SUBSCRIBE,
                       subscribe_key, key_len) == -1) {
        _CLI_SUB(ERR, "ZeroMQ subscribe frontend subscribe key: %s\n",
                 zmq_strerror(errno));
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_ctx_destroy(context);
        if (compress_key) {
            free(compress_key);
        }
        return -1;
    }

    if (compress_key) {
        free(compress_key);
    }

    /* subscribe:frontend: connect */
    endpoint = strdup(subscribe_frontendpoints);
    token = endpoint;

    while (1) {
        char *end;

        end = strtok(token, ",");
        if (end == NULL) {
            break;
        }

        while (*end == ' ') {
            end++;
        }

        if (zmq_connect(subscribe_frontend, end) == -1) {
            _CLI_SUB(ERR, "ZeroMQ subscribe frontend connect: %s: %s\n",
                     end, zmq_strerror(errno));
            free(endpoint);
            _CLI_SUB(VERBOSE, "Thread end client backend.\n");
            pthread_kill(client_backend.thread, SIGINT);
            pthread_join(client_backend.thread, NULL);
            zmq_close(client_frontend);
            zmq_close(client_backend.socket);
            zmq_close(subscribe_frontend);
            zmq_ctx_destroy(context);
            return -1;
        }

        _CLI_SUB(INFO, "ZeroMQ subscribe frontend connect.\n: %s", end);

        token = NULL;
    }

    free(endpoint);

    /* subscribe:backend */
    subscribe_backend = zmq_socket(context, ZMQ_PUSH);
    if (!subscribe_backend) {
        _CLI_SUB(ERR, "ZeroMQ subscribe backend socket: %s\n",
                 zmq_strerror(errno));
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ subscribe backend socket: PUSH\n")

    /* subscribe:backend: monitoring */
    if (zlmb_utils_asprintf(&endpoint, "%s.%d",
                            ZLMB_SUBSCRIBE_MONITOR_SOCKET, getpid()) == -1) {
        _CLI_SUB(ERR, "Allocate monitor string subscribe backend point.\n");
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_close(subscribe_backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    subscribe_monitor = _socket_monitor_init(context,
                                             subscribe_backend, endpoint,
                                             ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE);
    if (subscribe_monitor == NULL) {
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        _socket_monitor_destroy(&subscribe_monitor);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_close(subscribe_backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    free(endpoint);

    _CLI_SUB(VERBOSE, "Thread start subscribe backend monitor.\n");
    if (pthread_create(&(subscribe_monitor->thread), NULL,
                       _socket_monitor_event,
                       (void *)subscribe_monitor) == -1) {
        _CLI_SUB(ERR, "Thread create subscribe backend monitor.\n");
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        _socket_monitor_destroy(&subscribe_monitor);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_close(subscribe_backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    if (pthread_kill(subscribe_monitor->thread, 0) != 0) {
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        _socket_monitor_destroy(&subscribe_monitor);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_close(subscribe_backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    /* subscribe:backend: bind */
    if (zmq_bind(subscribe_backend, subscribe_backendpoint) == -1) {
        _CLI_SUB(ERR, "ZeroMQ subscribe backend bind: %s\n",
                 zmq_strerror(errno));
        _CLI_SUB(VERBOSE, "Thread end client backend.\n");
        pthread_kill(client_backend.thread, SIGINT);
        pthread_join(client_backend.thread, NULL);
        _CLI_SUB(VERBOSE, "Thread end subscribe backend monitor.\n");
        pthread_kill(subscribe_monitor->thread, SIGINT);
        pthread_join(subscribe_monitor->thread, NULL);
        _socket_monitor_destroy(&subscribe_monitor);
        zmq_close(client_frontend);
        zmq_close(client_backend.socket);
        zmq_close(subscribe_frontend);
        zmq_close(subscribe_backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _CLI_SUB(VERBOSE, "ZeroMQ subscribe backend bind: %s\n",
             subscribe_backendpoint);

    /* dump */
    subscribe_dump = zlmb_dump_init(subscribe_dumpfile, subscribe_dumptype);

    /* poll */
    _CLI_SUB(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = client_frontend;
    pollitems[1].socket = subscribe_frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 2, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            /* client */
            int more, flags;
            size_t moresz = sizeof(more);

            _CLI_SUB(DEBUG, "ZeroMQ client frontend receive in poll event.\n");

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _CLI_SUB(DEBUG, "ZeroMQ client frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(client_frontend, &zmsg, 0) == -1) {
                    _CLI_SUB(ERR, "ZeroMQ client frontend receive: %s\n",
                             zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(client_frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _CLI_SUB(ERR,
                             "ZeroMQ client frontend receive socket option:"
                             " %s\n", zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }

#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _CLI_SUB(DEBUG, "ZeroMQ client frontend send message.\n");

                if (zmq_sendmsg(client_backend.socket, &zmsg, flags) == -1) {
                    _CLI_SUB(ERR, "ZeroMQ client backend send: %s\n",
                             zmq_strerror(errno));
                    //break;
                }

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        if (pollitems[1].revents & ZMQ_POLLIN) {
            /* subscribe */
            int more, send, flags, frames = 0;
            size_t moresz = sizeof(more);

            _CLI_SUB(DEBUG,
                     "ZeroMQ subscribe frontend receive in poll event.\n");

            if (subscribe_connect > 0) {
#ifdef USE_SNAPPY
                send = ZLMB_SENDMSG_UNCOMPRESS;
#else
                send = ZLMB_SENDMSG;
#endif
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _CLI_SUB(DEBUG, "ZeroMQ subscribe frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(subscribe_frontend, &zmsg, 0) == -1) {
                    _CLI_SUB(ERR, "ZeroMQ subscribe frontend recv: %s\n",
                             zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                frames++;

                if (zmq_getsockopt(subscribe_frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _CLI_SUB(ERR,
                             "ZeroMQ subscribe frontend recv socket option:"
                             " %s\n", zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }

#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _CLI_SUB(DEBUG, "ZeroMQ subscribe frontend send message.\n");

                if (!subscribe_dropkey || frames != 1) {
                    _sendmsg(send, subscribe_backend, &zmsg, flags,
                             subscribe_dump,
                             ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE);
                }

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        _subscribe_monitor_connect(subscribe_monitor, &subscribe_connect);
    }

    _CLI_SUB(VERBOSE, "ZeroMQ end proxy.\n");

    /* client:frontend: unbind */
    _CLI_SUB(VERBOSE, "ZeroMQ client frontend unbind.\n: %s",
             client_frontendpoint);
    zmq_unbind(client_frontend, client_frontendpoint);

    /* client:backend: cleanup */
    _CLI_SUB(VERBOSE, "Thread end client backend.\n");
    pthread_kill(client_backend.thread, SIGINT);
    pthread_join(client_backend.thread, NULL);

    /* subscribe:backend monitoring: cleanup */
    _CLI_SUB(VERBOSE, "Thread end subscribe backend monitor.\n");
    pthread_kill(subscribe_monitor->thread, SIGINT);
    pthread_join(subscribe_monitor->thread, NULL);
    _socket_monitor_destroy(&subscribe_monitor);

    /* sockets: cleanup */
    _CLI_SUB(INFO, "ZeroMQ close sockets.\n");
    zmq_close(client_frontend);
    zmq_close(client_backend.socket);
    zmq_close(subscribe_frontend);
    zmq_close(subscribe_backend);

    /* context: cleanup */
    _CLI_SUB(INFO, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    /* dump: cleanup */
    if (subscribe_dump) {
        zlmb_dump_destroy(&subscribe_dump);
    }

    return 0;
}

int
_server_stand_alone(char *frontendpoint, char *backendpoint,
                    char *dumpfile, int dumptype)
{
    int connect = 0;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    zlmb_dump_t *dump = NULL;
    char *endpoint;
    void *context, *frontend, *backend;
    zlmb_socket_monitor_t *monitor;

    if (!frontendpoint || strlen(frontendpoint) == 0) {
        _ALONE(ERR, "frontendpoint.\n");
        return -1;
    }

    if (!backendpoint || strlen(backendpoint) == 0) {
        _ALONE(ERR, "backendpoint.\n");
        return -1;
    }

    _ALONE(INFO, "Bind front endpoint: %s\n", frontendpoint);
    _ALONE(INFO, "Bind back endpoint: %s\n", backendpoint);
    _ALONE(INFO, "Dump file: %s (%s)\n",
           dumpfile, zlmb_option_dumptype2string(dumptype));

    /* context */
    context = zmq_ctx_new();
    if (!context) {
        _ALONE(ERR, "ZeroMQ context: %s\n", zmq_strerror(errno));
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

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _ALONE(ERR, "ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    _ALONE(VERBOSE, "ZeroMQ frontend socket: PULL\n")

    if (zmq_bind(frontend, frontendpoint) == -1) {
        _ALONE(ERR, "ZeroMQ frontend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _ALONE(VERBOSE, "ZeroMQ frontend bind: %s\n", frontendpoint);

    /* backend */
    backend = zmq_socket(context, ZMQ_PUSH);
    if (!backend) {
        _ALONE(ERR, "ZeroMQ backend socket: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _ALONE(VERBOSE, "ZeroMQ backend socket: PUSH\n")

    /* backend: bind */
    if (zmq_bind(backend, backendpoint) == -1) {
        _ALONE(ERR, "ZeroMQ backend bind: %s\n", zmq_strerror(errno));
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    _ALONE(VERBOSE, "ZeroMQ backend bind: %s\n", backendpoint);

    /* backend: monitoring */
    if (zlmb_utils_asprintf(&endpoint, "%s.%d",
                            ZLMB_SUBSCRIBE_MONITOR_SOCKET, getpid()) == -1) {
        _ALONE(ERR, "Allocate monitor string backend point.\n");
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    monitor = _socket_monitor_init(context, backend, endpoint,
                                   ZLMB_OPTION_MODE_STAND_ALONE);
    if (monitor == NULL) {
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    free(endpoint);

    _ALONE(VERBOSE, "Thread start backend monitor.\n");
    if (pthread_create(&(monitor->thread), NULL,
                       _socket_monitor_event, (void *)monitor) == -1) {
        _ALONE(ERR, "Thread create backend monitor.\n");
        _socket_monitor_destroy(&monitor);
        zmq_close(frontend);
        zmq_close(backend);
        zmq_ctx_destroy(context);
        return -1;
    }

    /* dump */
    dump = zlmb_dump_init(dumpfile, dumptype);

    /* poll */
    _ALONE(VERBOSE, "ZeroMQ start proxy.\n");

    pollitems[0].socket = frontend;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, ZLMB_POLL_TIMEOUT) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more, send, flags;
            size_t moresz = sizeof(more);

            _ALONE(DEBUG, "ZeroMQ  frontend receive in poll event.\n");

            if (connect > 0) {
                send = ZLMB_SENDMSG;
            } else {
                send = ZLMB_SENDMSG_DUMP;
            }

            while (!_interrupted) {
                zmq_msg_t zmsg;

                _ALONE(DEBUG, "ZeroMQ  frontend receive message.\n");

                if (zmq_msg_init(&zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                    _ALONE(ERR, "ZeroMQ frontend receive: %s\n",
                           zmq_strerror(errno));
                    zmq_msg_close(&zmsg);
                    break;
                }

                if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                   &more, &moresz) == -1) {
                    _ALONE(ERR, "ZeroMQ frontend receive socket option: %s\n",
                           zmq_strerror(errno));
                    //zmq_msg_close(&zmsg);
                    //break;
                    more = 0;
                }

                if (more) {
                    flags = ZMQ_SNDMORE;
                } else {
                    flags = 0;
                }
#ifndef NDEBUG
                zlmb_dump_printmsg(stderr, &zmsg);
#endif
                _ALONE(DEBUG, "ZeroMQ  backend send message.\n");

                _sendmsg(send, backend, &zmsg, flags, dump,
                         ZLMB_OPTION_MODE_STAND_ALONE);

                zmq_msg_close(&zmsg);

                if (flags == 0) {
                    break;
                }
            }
        }

        _subscribe_monitor_connect(monitor, &connect);
    }

    _ALONE(VERBOSE, "ZeroMQ end proxy.\n");

    /* backend monitoring: cleanup */
    _ALONE(VERBOSE, "Thread end backend monitor.\n");
    pthread_kill(monitor->thread, SIGINT);
    pthread_join(monitor->thread, NULL);
    _socket_monitor_destroy(&monitor);

    /* sockets: cleanup */
    _ALONE(VERBOSE, "ZeroMQ close sockets.\n");
    zmq_close(frontend);
    zmq_close(backend);

    /* context: cleanup */
    _ALONE(VERBOSE, "ZeroMQ destroy context.\n");
    zmq_ctx_destroy(context);

    /* dump: cleanup */
    if (dump) {
        zlmb_dump_destroy(&dump);
    }

    return 0;
}

//------------------------------------------------------------------------------
static void
_info(char *arg)
{
    int major, minor, patch;
    char *command = basename(arg);

    printf("Info: %s\n", command);

    zmq_version(&major, &minor, &patch);
    printf("  ZeroMQ version: %d.%d.%d\n", major, minor, patch);

    yaml_get_version(&major, &minor, &patch);
    printf("  YAML version: %d.%d.%d\n", major, minor, patch);

#ifdef USE_SNAPPY
    printf("  Compress: enable (snappy)\n");
#else
    printf("  Compress: disable\n");
#endif

#ifndef NDEBUG
    printf("  Debug: enable\n");
#endif
}

static void
_usage(char *arg, char *message, int mode)
{
    char *command = basename(arg);
    int len = 0;

    len = (int)strlen(command);
    printf("Usage: %s --mode=MODE\n", command);

    /* client options */
    if (!mode || mode & ZLMB_CLI_FRONT || mode & ZLMB_CLI_BACK) {
        printf("%*s      [ ", len, "");
        printf("--client_frontendpoint=ENDPOINT");
        if (!mode || mode & ZLMB_CLI_BACK) {
            printf("\n%*s        --client_backendpoints=ENDPOINTS", len, "");
            printf("\n%*s        --client_dumpfile=FILE", len, "");
            printf("\n%*s        --client_dumptype=TYPE", len, "");
        }
        printf(" ]\n");
    }

    /* publish options */
    if (!mode || mode & ZLMB_PUB_FRONT || mode & ZLMB_PUB_BACK) {
        printf("%*s      [ ", len, "");
        if (!mode || mode & ZLMB_PUB_FRONT) {
            printf("--publish_frontendpoint=ENDPOINT");
        }
        if (!mode || mode & ZLMB_PUB_BACK) {
            if (!mode || mode & ZLMB_PUB_FRONT) {
                printf("\n%*s        ", len, "");
            }
            printf("--publish_backendpoint=ENDPOINT");
            printf("\n%*s        --publish_key=KEY", len, "");
            printf("\n%*s        --publish_sendkey", len, "");
        }
        printf(" ]\n");
    }

    /* subscribe options */
    if (!mode || mode & ZLMB_SUB_FRONT || mode & ZLMB_SUB_BACK) {
        printf("%*s      [ ", len, "");
        if (!mode || mode & ZLMB_SUB_FRONT) {
            printf("--subscribe_frontendpoints=ENDPOINTS");
            printf("\n%*s        ", len, "");
        }
        printf("--subscribe_backendpoint=ENDPOINT");
        if (!mode || mode & ZLMB_SUB_FRONT) {
            printf("\n%*s        --subscribe_key=KEY", len, "");
            printf("\n%*s        --subscribe_dropkey", len, "");
        }
        printf("\n%*s        --subscribe_dumpfile=FILE", len, "");
        printf("\n%*s        --subscribe_dumptype=TYPE", len, "");
        printf(" ]\n");
    }

    /* other options */
    printf("%*s      [ --config=FILE ]\n", len, "");
    printf("%*s      [ --info ]\n", len, "");
    printf("%*s      [ --syslog ]\n", len, "");
    printf("%*s      [ --verbose ]\n", len, "");

    printf("\n");

    printf("  --mode                      execute mode\n");
    printf("                               [ %s | %s | %s |\n"
           "                                 %s | %s |\n"
           "                                 %s | %s ]\n",
           ZLMB_OPTION_MODE_CLIENT, ZLMB_OPTION_MODE_PUBLISH,
           ZLMB_OPTION_MODE_SUBSCRIBE, ZLMB_OPTION_MODE_CLIENT_PUBLISH,
           ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE,
           ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE,
           ZLMB_OPTION_MODE_STAND_ALONE);
    if (!mode || mode & ZLMB_CLI_FRONT) {
        printf("  --client_frontendpoint      client fronend endpoint\n"
               "                               (ex: tcp://127.0.0.1:5557)\n");
    }
    if (!mode || mode & ZLMB_CLI_BACK) {
        printf("  --client_backendpoints      client backend endpoints\n"
               "                              "
               " (ex: tcp://127.0.0.1:5558,tcp://127.0.0.1:6668,...)\n");
        printf("  --client_dumpfile           client error file\n"
               "                               [ %s (DEFAULT) ]\n",
               ZLMB_DEFAULT_CLIENT_DUMP_FILE);
        printf("  --client_dumptype           client error type\n"
               "                               [ %s (DEFAULT) |\n"
               "                                 %s | %s | %s |\n"
               "                                 %s ]\n",
               ZLMB_OPTION_DUMPTYPE_BINARY,
               ZLMB_OPTION_DUMPTYPE_PLAIN,
               ZLMB_OPTION_DUMPTYPE_PLAIN_TIME,
               ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS,
               ZLMB_OPTION_DUMPTYPE_PLAIN_TIME_FLAGS);
    }
    if (!mode || mode & ZLMB_PUB_FRONT) {
        printf("  --publish_frontendpoint     publish frontend point\n"
               "                               (ex: tcp://127.0.0.1:5558)\n");
    }
    if (!mode || mode & ZLMB_PUB_BACK) {
        printf("  --publish_backendpoint      publish backendend point\n"
               "                               (ex: tcp://127.0.0.1:5559)\n");
        printf("  --publish_key               publish key string\n"
               "                               [ \"\" (DEFAULT:empty) ]\n");
        printf("  --publish_sendkey           enable sending publish key\n"
               "                               [ disable (DEFAULT) ]\n");
    }
    if (!mode || mode & ZLMB_SUB_FRONT) {
        printf("  --subscribe_frontendpoints  subscribe frontend points\n"
               "                              "
               " (ex: tcp://127.0.0.1:5559,tcp://127.0.0.1:6669,...)\n");
    }
    if (!mode || mode & ZLMB_SUB_BACK) {
        printf("  --subscribe_backendpoint    subscribe backendend point\n"
               "                               (ex: tcp://127.0.0.1:5560)\n");
        if (!mode || mode & ZLMB_SUB_FRONT) {
            printf("  --subscribe_key             subscribe key string\n"
                   "                               [ \"\" (DEFAULT:empty)]\n");
            printf("  --subscribe_dropkey         enable dropped subscribe key\n"
                   "                               [ disable (DEFAULT) ]\n");
        }
        printf("  --subscribe_dumpfile        subscribe error file\n"
               "                               [ %s (DEFAULT) ]\n",
               ZLMB_DEFAULT_SUBSCRIBE_DUMP_FILE);
        printf("  --subscribe_dumptype        subscribe error type\n"
               "                               [ %s (DEFAULT) |\n"
               "                                 %s | %s | %s |\n"
               "                                 %s ]\n",
               ZLMB_OPTION_DUMPTYPE_BINARY,
               ZLMB_OPTION_DUMPTYPE_PLAIN,
               ZLMB_OPTION_DUMPTYPE_PLAIN_TIME,
               ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS,
               ZLMB_OPTION_DUMPTYPE_PLAIN_TIME_FLAGS);
    }
    printf("  --config                    config file path\n");
    printf("  --info                      application information\n");
    printf("  --syslog                    log to syslog\n");
    printf("  --verbose                   verbosity log\n");

    if (!mode) {
        printf("\nEnable mode options:\n");
        printf("  %s: client_frontendpoint,client_backendpoints,\n",
               ZLMB_OPTION_MODE_CLIENT);
        printf("  %*s: client_dumpfile,client_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT), "");
        printf("  %s: publish_frontendpoint,publish_backendpoint,\n",
               ZLMB_OPTION_MODE_PUBLISH);
        printf("  %*s: publish_key,publish_sendkey\n",
               (int)strlen(ZLMB_OPTION_MODE_PUBLISH), "");
        printf("  %s: subscribe_frontendpoint,subscribe_backendpoint,\n",
               ZLMB_OPTION_MODE_SUBSCRIBE);
        printf("  %*s: subscribe_key,subscribe_dropkey,\n",
               (int)strlen(ZLMB_OPTION_MODE_SUBSCRIBE), "");
        printf("  %*s: subscribe_dumpfile,subscribe_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_SUBSCRIBE), "");
        printf("  %s: client_frontendpoint,publish_backendpoint,\n",
               ZLMB_OPTION_MODE_CLIENT_PUBLISH);
        printf("  %*s: publish_key,publish_sendkey\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT_PUBLISH), "");
        printf("  %s: publish_frontendpoint,subscribe_backendpoint,\n",
               ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE);
        printf("  %*s: subscribe_dumpfile,subscribe_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE), "");
        printf("  %s: client_frontendpoint,client_backendpoints,\n",
               ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE);
        printf("  %*s: client_dumpfile,client_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE), "");
        printf("  %*s: subscribe_frontendpoint,subscribe_backendpoint,\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE), "");
        printf("  %*s: subscribe_key,subscribe_dropkey,\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE), "");
        printf("  %*s: subscribe_dumpfile,subscribe_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE), "");
        printf("  %s: client_frontendpoint,subscribe_backendpoint,\n",
               ZLMB_OPTION_MODE_STAND_ALONE);
        printf("  %*s: subscribe_dumpfile,subscribe_dumptype\n",
               (int)strlen(ZLMB_OPTION_MODE_STAND_ALONE), "");
    }

    if (message) {
        printf("\nINFO: %s\n", message);
    }
}

#define _option_require(_arg, _opt, _key, _info) \
    if (!_opt->_key) {                           \
        _usage(_arg, _info, _opt->mode);         \
        zlmb_option_destroy(&_opt);              \
        return -1;                               \
    }

#define _option_set(_opt, _arg, _key) \
    zlmb_option_set(_opt, ZLMB_OPTION_KEY_ ## _key, _arg, 0, 0, 0)

#define _option_sets(_opt, _arg, _key) \
    zlmb_option_set(_opt, ZLMB_OPTION_KEY_ ## _key, _arg, 0, 1, 0)

//------------------------------------------------------------------------------

int
main (int argc, char **argv)
{
    int opt;
    char *config_filename = NULL;
    zlmb_option_t *option = NULL;

    const struct option long_options[] = {
        { ZLMB_OPTION_KEY_MODE, 1, NULL, 1 },
        { ZLMB_OPTION_KEY_CLIENT_FRONTENDPOINT, 1, NULL, 11 },
        { ZLMB_OPTION_KEY_CLIENT_BACKENDPOINTS, 1, NULL, 12 },
        { ZLMB_OPTION_KEY_CLIENT_DUMPFILE, 1, NULL, 13 },
        { ZLMB_OPTION_KEY_CLIENT_DUMPTYPE, 1, NULL, 14 },
        { ZLMB_OPTION_KEY_PUBLISH_FRONTENDPOINT, 1, NULL, 21 },
        { ZLMB_OPTION_KEY_PUBLISH_BACKENDPOINT, 1, NULL, 22 },
        { ZLMB_OPTION_KEY_PUBLISH_KEY, 1, NULL, 23 },
        { ZLMB_OPTION_KEY_PUBLISH_SENDKEY, 0, NULL, 24 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_FRONTENDPOINTS, 1, NULL, 31 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_BACKENDPOINT, 1, NULL, 32 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_KEY, 1, NULL, 33 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_DROPKEY, 0, NULL, 34 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_DUMPFILE, 1, NULL, 35 },
        { ZLMB_OPTION_KEY_SUBSCRIBE_DUMPTYPE, 1, NULL, 36 },
        { "config", 1, NULL, 41 },
        { "info", 0, NULL, 42 },
        { "syslog", 0, NULL, 43 },
        { "verbose", 0, NULL, 44 },
        { "help", 0, NULL, 100 },
        { NULL, 0, NULL, 0 }
    };

    option = zlmb_option_init();
    if (!option) {
        _ERR("Option initilized.\n");
        return -1;
    }

    while ((opt = getopt_long_only(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
            case 1:
                _option_set(option, optarg, MODE);
                break;
            case 11:
                _option_set(option, optarg, CLIENT_FRONTENDPOINT);
                break;
            case 12:
                _option_sets(option, optarg, CLIENT_BACKENDPOINTS);
                break;
            case 13:
                _option_set(option, optarg, CLIENT_DUMPFILE);
                break;
            case 14:
                _option_set(option, optarg, CLIENT_DUMPFILE);
                break;
            case 21:
                _option_set(option, optarg, PUBLISH_FRONTENDPOINT);
                break;
            case 22:
                _option_set(option, optarg, PUBLISH_BACKENDPOINT);
                break;
            case 23:
                _option_set(option, optarg, PUBLISH_KEY);
                break;
            case 24:
                _option_set(option, "true", PUBLISH_SENDKEY);
                break;
            case 31:
                _option_sets(option, optarg, SUBSCRIBE_FRONTENDPOINTS);
                break;
            case 32:
                _option_set(option, optarg, SUBSCRIBE_BACKENDPOINT);
                break;
            case 33:
                _option_set(option, optarg, SUBSCRIBE_KEY);
                break;
            case 34:
                _option_set(option, "true", SUBSCRIBE_DROPKEY);
                break;
            case 35:
                _option_set(option, optarg, SUBSCRIBE_DUMPFILE);
                break;
            case 36:
                _option_set(option, optarg, SUBSCRIBE_DUMPTYPE);
                break;
            case 41:
                config_filename = optarg;
                break;
            case 42:
                _info(argv[0]);
                zlmb_option_destroy(&option);
                return -1;
            case 43:
                _option_set(option, "true", SYSLOG);
                break;
            case 44:
                _option_set(option, "true", VERBOSE);
                break;
            default:
                _usage(argv[0], NULL, option->mode);
                zlmb_option_destroy(&option);
                return -1;
        }
    }

    /* load config yaml file */
    if (config_filename) {
        if (zlmb_option_load_file(option, config_filename) == -1) {
            _usage(argv[0], "failed config file", option->mode);
            zlmb_option_destroy(&option);
            return -1;
        }
    }

    zlmb_option_set_default(option);

    if (option->syslog != -1) {
        _syslog = option->syslog;
    }
    if (option->verbose != -1) {
        _verbose = option->verbose;
    }

    _LOG_OPEN(ZLMB_SYSLOG_IDENT);

    switch (option->mode) {
        case ZLMB_MODE_CLIENT:
            _option_require(argv[0], option, client_frontendpoint,
                            "required client_frontendpoint");
            _option_require(argv[0], option, client_backendpoints,
                            "required client_backendpoints");

            _server_client(option->client_frontendpoint,
                           option->client_backendpoints,
                           option->client_dumpfile,
                           option->client_dumptype);
            break;
        case ZLMB_MODE_PUBLISH:
            _option_require(argv[0], option, publish_frontendpoint,
                            "required publish_frontendpoint");
            _option_require(argv[0], option, publish_backendpoint,
                            "required publish_backendpoint");

            _server_publish(option->publish_frontendpoint,
                            option->publish_backendpoint,
                            option->publish_key,
                            option->publish_sendkey);
            break;
        case ZLMB_MODE_SUBSCRIBE:
            _option_require(argv[0], option, subscribe_frontendpoints,
                            "required subscribe_frontendpoint");
            _option_require(argv[0], option, subscribe_backendpoint,
                            "required subscribe_backendpoint");

            _server_subscribe(option->subscribe_frontendpoints,
                              option->subscribe_backendpoint,
                              option->subscribe_key,
                              option->subscribe_dropkey,
                              option->subscribe_dumpfile,
                              option->subscribe_dumptype);
            break;
        case ZLMB_MODE_CLIENT_PUBLISH:
            _option_require(argv[0], option, client_frontendpoint,
                            "required client_frontendpoint");
            _option_require(argv[0], option, publish_backendpoint,
                            "required publish_backendpoint");

            _server_client_publish(option->client_frontendpoint,
                                   option->publish_backendpoint,
                                   option->publish_key,
                                   option->publish_sendkey);
            break;
        case ZLMB_MODE_PUBLISH_SUBSCRIBE:
            _option_require(argv[0], option, publish_frontendpoint,
                            "required publish_frontendpoint");
            _option_require(argv[0], option, subscribe_backendpoint,
                            "required subscribe_backendpoint");

            _server_publish_subscribe(option->publish_frontendpoint,
                                      option->subscribe_backendpoint,
                                      option->subscribe_dumpfile,
                                      option->subscribe_dumptype);
            break;
        case ZLMB_MODE_CLIENT_SUBSCRIBE:
            _option_require(argv[0], option, client_frontendpoint,
                            "required client_frontendpoint");
            _option_require(argv[0], option, client_backendpoints,
                            "required client_backendpoints");
            _option_require(argv[0], option, subscribe_frontendpoints,
                            "required subscribe_frontendpoint");
            _option_require(argv[0], option, subscribe_backendpoint,
                            "required subscribe_backendpoint");

            _server_client_subscribe(option->client_frontendpoint,
                                     option->client_backendpoints,
                                     option->client_dumpfile,
                                     option->client_dumptype,
                                     option->subscribe_frontendpoints,
                                     option->subscribe_backendpoint,
                                     option->subscribe_key,
                                     option->subscribe_dropkey,
                                     option->subscribe_dumpfile,
                                     option->subscribe_dumptype);
            break;
        case ZLMB_MODE_STAND_ALONE:
            _option_require(argv[0], option, client_frontendpoint,
                            "required client_frontendpoint");
            _option_require(argv[0], option, subscribe_backendpoint,
                            "required subscribe_backendpoint");

            _server_stand_alone(option->client_frontendpoint,
                                option->subscribe_backendpoint,
                                option->subscribe_dumpfile,
                                option->subscribe_dumptype);
            break;
        default:
            _usage(argv[0], "invalid mode", option->mode);
            zlmb_option_destroy(&option);
            _LOG_CLOSE();
            return -1;
    }

    zlmb_option_destroy(&option);

    _LOG_CLOSE();

    return 0;
}
