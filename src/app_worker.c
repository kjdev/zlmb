/*
 * zlmb worker server
 *
 * command spawn environ:
 *  ZLMB_FRAME
 *  ZLMB_FRAME_LENGTH
 *  ZLMB_LENGTH
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <spawn.h>
#include <sys/wait.h>

#include "zlmb.h"
#include "stack.h"
#include "dump.h"
#include "log.h"
#include "utils.h"

#define ZLMB_SYSLOG_IDENT "zlmb-worker"

#ifndef ZLMB_WORKER_SOCKET
#define ZLMB_WORKER_SOCKET "tcp://127.0.0.1:5560"
#endif

#define ZLMB_WORKER_BACKEND_SOCKET "inproc://zlmb.worker"

static int _interrupted = 0;
static int _syslog = 0;
static int _verbose = 0;

typedef struct {
    pthread_t thread;
    char *context;
    char *command;
    char *endpoint;
    char **argv;
    int argc;
    int optind;
} zlmb_worker_t;

typedef struct {
    zlmb_stack_t *stack;
    char *frame;
    char *frame_length;
    char *length;
} zlmb_spawn_t;

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

static char *
_str_printf(const char *format, ...)
{
    size_t length, size = BUFSIZ;
    char *str;
    va_list arg, ap;
    void *tmp = NULL;

    str = (char *)malloc(sizeof(char *) * (size + 1));
    if (!str) {
        _ERR("Memory allocate string.\n");
        return NULL;
    }

    memset(str, '\0', size + 1);

    va_start(arg, format);
    va_copy(ap, arg);

    length = vsnprintf(str, size, format, arg);
    if (length >= size) {
        size = length + 1;
        tmp = realloc(str, size);
        if (!tmp) {
            free(str);
            va_end(arg);
            va_end(ap);
            _ERR("Memory allocate string.\n");
            return NULL;
        }
        str = (char *)tmp;
        vsnprintf(str, size, format, ap);
    }

    va_end(arg);
    va_end(ap);

    return str;
}

static void
_spawn_generate_environ(zlmb_spawn_t *self)
{
    size_t length = 0;
    zlmb_stack_item_t *item = NULL;

    if (!self) {
        return;
    }

    self->frame = _str_printf("ZLMB_FRAME=%ld", zlmb_stack_size(self->stack));

    item = zlmb_stack_first(self->stack);
    while (item) {
        zlmb_stack_item_t *next = zlmb_stack_item_next(item);
        zmq_msg_t *zmsg = zlmb_stack_item_data(item);
        if (zmsg) {
            size_t size = zmq_msg_size(zmsg);
            if (!self->frame_length) {
                self->frame_length = _str_printf("ZLMB_FRAME_LENGTH=%ld", size);
            } else {
                char *older = self->frame_length;
                self->frame_length = _str_printf("%s:%ld", older, size);
                free(older);
            }
            length += size;
        }
        item = next;
    }

    self->length = _str_printf("ZLMB_LENGTH=%ld", length);

    _DEBUG("POSIX spawn Environ: %s; %s; %s\n",
           self->frame, self->frame_length, self->length);
}

static int
_spawn_run(zlmb_spawn_t *self, char *command, int argc, char **argv, int opt)
{
    pid_t pid;
    int ret, in[2];
    int i, n = argc - opt;
    char *env[] = { NULL, NULL, NULL, NULL };
    char **arg = NULL;
    posix_spawn_file_actions_t actions;

    if (!self || !command || strlen(command) <= 0) {
        _ERR("Function arguments: %s\n", __FUNCTION__);
        return -1;
    }

    env[0] = self->frame;
    env[1] = self->frame_length;
    env[2] = self->length;

    if (n < 0) {
        n = 0;
    }

    arg = malloc(sizeof(char *) * (n + 2));
    if (arg == NULL) {
        _ERR("Memory allocate args.\n");
        return -1;
    }
    if (n > 0) {
        for (i = 0; i != n; i++) {
            arg[i+1] = argv[opt+i];
        }
    }
    arg[0] = command;
    arg[n+1] = NULL;

    if (pipe(in) == -1) {
        _ERR("Create STDIN pipe.\n");
        return -1;
    }

    if (posix_spawn_file_actions_init(&actions) != 0) {
        _ERR("POSIX spawn file action initilize.\n");
        return -1;
    }

    if (posix_spawn_file_actions_addclose(&actions, in[1]) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, in[0], 0) != 0) {
        _ERR("POSIX spawn file action add.\n");
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }

    _DEBUG("POSIX spawn run: %s\n", command);

    if (posix_spawnp(&pid, command, &actions, NULL, arg, env) != 0) {
        _ERR("POSIX spawn: %s\n", command);
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }

    close(in[0]);

    while (zlmb_stack_size(self->stack)) {
        zmq_msg_t *zmsg = zlmb_stack_shift(self->stack);
        if (zmsg) {
            write(in[1], zmq_msg_data(zmsg), zmq_msg_size(zmsg));
            zmq_msg_close(zmsg);
            free(zmsg);
        }
    }

    close(in[1]);

    _DEBUG("POSIX spawn wait(#%d).\n", pid);
    waitpid(pid, &ret, 0);

    posix_spawn_file_actions_destroy(&actions);

    _DEBUG("POSIX spawn finish(#%d).\n", pid);

    if (arg) {
        free(arg);
    }

    return 0;
}

static void
_spawn_destroy(zlmb_spawn_t *self)
{
    if (self->stack) {
        zlmb_stack_destroy(&self->stack);
    }
    if (self->frame) {
        free(self->frame);
    }
    if (self->frame_length) {
        free(self->frame_length);
    }
    if (self->length) {
        free(self->length);
    }
}

static void *
_worker_command(void *arg)
{
    zlmb_worker_t *worker = (zlmb_worker_t *)arg;
    zmq_pollitem_t pollitems[] = { { NULL, 0, ZMQ_POLLIN, 0 } };
    void *socket;

    if (!worker || !worker->context || !worker->command) {
        _ERR("Function arguments: %s\n", __FUNCTION__);
        return NULL;
    }

    socket = zmq_socket(worker->context, ZMQ_PULL);
    if (!socket) {
        _ERR("ZeroMQ socket: %s\n", zmq_strerror(errno));
        return NULL;
    }

    if (zmq_connect(socket, worker->endpoint) == -1) {
        _ERR("ZeroMQ socket connect: %s: %s\n",
             worker->endpoint, zmq_strerror(errno));
        zmq_close(socket);
        return NULL;
    }

    _VERBOSE("ZeroMQ socket connect: %s\n", worker->endpoint);

    _VERBOSE("ZeroMQ start worker command proxy.\n");

    pollitems[0].socket = socket;

    _signals();

    while (!_interrupted) {
        if (zmq_poll(pollitems, 1, -1) == -1) {
            break;
        }

        if (pollitems[0].revents & ZMQ_POLLIN) {
            int more;
            size_t moresz = sizeof(more);
            zlmb_spawn_t spawn = { NULL, NULL, NULL, NULL };

            _DEBUG("ZeroMQ receive in poll event.\n");

            spawn.stack = zlmb_stack_init();
            if (!spawn.stack) {
                _ERR("Message stack initilize.\n");
                break;
            }

            while (!_interrupted) {
                zmq_msg_t *zmsg = (zmq_msg_t *)malloc(sizeof(zmq_msg_t));
                if (!zmsg) {
                    break;
                }

                _DEBUG("ZeroMQ receive message.\n");

                if (zmq_msg_init(zmsg) != 0) {
                    break;
                }

                if (zmq_recvmsg(socket, zmsg, 0) == -1) {
                    _ERR("ZeroMQ socket receive: %s\n", zmq_strerror(errno));
                    zmq_msg_close(zmsg);
                    free(zmsg);
                    break;
                }

                if (zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moresz) == -1) {
                    _ERR("ZeroMQ socket option receive: %s\n",
                         zmq_strerror(errno));
                    more = 0;
                }

                if (zlmb_stack_push(spawn.stack, zmsg) != 0) {
                    _ERR("Message stack push.\n");
                }

                if (!more) {
                    break;
                }
            }

            //env
            _spawn_generate_environ(&spawn);

            //spawn
            _spawn_run(&spawn, worker->command,
                       worker->argc, worker->argv, worker->optind);

            _spawn_destroy(&spawn);
        }
    }

    _VERBOSE("ZeroMQ end worker command proxy.\n");

    zmq_close(socket);

    return NULL;
}

static void
_worker_destroy(zlmb_worker_t **self, int count, int wait)
{
    int i;

    if (wait > 0) {
        usleep(wait);
    }

    for (i = 0; i < count; i++) {
        if (self[i]) {
            if (self[i]->thread) {
                pthread_kill(self[i]->thread, SIGINT);
                pthread_join(self[i]->thread, NULL);
            }
            free(self[i]);
        }
    }

    free(self);
}

static void
_usage(char *arg, char *message)
{
    char *command = basename(arg);

    printf("Usage: %s [-e ENDPOINT] [-c COMMAND] [-t NUM] [ARGS ...]\n\n",
           command);

    printf("  -e, --endpoint=ENDPOINT server endpoint [DEFAULT: %s]\n",
           ZLMB_WORKER_SOCKET);
    printf("  -c, --command=COMMAND   command path\n");
    printf("  -t, --thread=NUM        command thread count\n");
    printf("  -s, --syslog            log to syslog\n");
    printf("  -v, --verbose           verbosity log\n");
    printf("  ARGS ...                command arguments\n");

    if (message) {
        printf("\nINFO: %s\n", message);
    }
}

int
main (int argc, char **argv)
{
    int i, opt, thread = 1;
    char *command = NULL;
    char *frontendpoint = ZLMB_WORKER_SOCKET, *backendpoint = NULL;
    void *context, *frontend = NULL, *backend = NULL;
    zlmb_worker_t **worker = NULL;
    size_t size;

    const struct option long_options[] = {
        { "endpoint", 1, NULL, 'e' },
        { "command", 1, NULL, 'c' },
        { "thread", 1, NULL, 't' },
        { "syslog", 0, NULL, 's' },
        { "verbose", 0, NULL, 'v' },
        { "help", 0, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv,
                              "e:c:t:svh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'e':
                frontendpoint = optarg;
                break;
            case 'c':
                command = optarg;
                break;
            case 't':
                thread = atoi(optarg);
                break;
            case 's':
                _syslog = 1;
                break;
            case 'v':
                _verbose = 1;
                break;
            default:
                _usage(argv[0], NULL);
                return -1;
        }
    }

    _LOG_OPEN(ZLMB_SYSLOG_IDENT);

    _INFO("Connect endpoint: %s\n", frontendpoint);
    _INFO("Execute command: %s\n", command);
    _INFO("Thread count: %d\n", thread);

    context = zmq_ctx_new();
    if (!context) {
        _ERR("ZeroMQ context: %s\n", zmq_strerror(errno));
        _LOG_CLOSE();
        return -1;
    }

    /*
    if (zmq_ctx_set(context, ZMQ_IO_THREADS, 1) == -1) {
        _ERR("%s", zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }

    if (zmq_ctx_set(context, ZMQ_MAX_SOCKETS, 1024) == -1) {
        _ERR("%s",  zmq_strerror(errno));
        zmq_ctx_destroy(context);
        return -1;
    }
    */

    /* backend: command */
    if (command) {
        backend = zmq_socket(context, ZMQ_PUSH);
        if (!backend) {
            _ERR("ZeroMQ backend socket: %s\n", zmq_strerror(errno));
            zmq_ctx_destroy(context);
            _LOG_CLOSE();
            return -1;
        }

        if (zlmb_utils_asprintf(&backendpoint, "%s.%d",
                                ZLMB_WORKER_BACKEND_SOCKET, getpid()) == -1) {
            _ERR("Allocate string backend point.\n");
            zmq_ctx_destroy(context);
            _LOG_CLOSE();
            return -1;
        }

        if (zmq_bind(backend, backendpoint) == -1) {
            _ERR("ZeroMQ backend bind: %s: %s\n",
                 backendpoint, zmq_strerror(errno));
            zmq_close(backend);
            zmq_ctx_destroy(context);
            _LOG_CLOSE();
            return -1;
        }

        _VERBOSE("ZeroMQ backend bind: %s\n", backendpoint);

        /* backend: command thread */
        if (thread <= 0) {
            thread = 1;
        }

        size = sizeof(zlmb_worker_t *) * thread;
        worker = (zlmb_worker_t **)malloc(size);
        if (!worker) {
            _ERR("Memory allocate worker command.\n");
            zmq_close(backend);
            zmq_ctx_destroy(context);
            free(backendpoint);
            _LOG_CLOSE();
            return -1;
        }

        memset(worker, 0, size);

        for (i = 0; i != thread; i++) {
            worker[i] = (zlmb_worker_t *)malloc(sizeof(zlmb_worker_t));
            if (!worker[i]) {
                _ERR("Memory allocate worker command.\n");
                _worker_destroy(worker, thread, 500);
                zmq_close(backend);
                zmq_ctx_destroy(context);
                free(backendpoint);
                return -1;
            }

            worker[i]->thread = 0;
            worker[i]->context = context;
            worker[i]->command = command;
            worker[i]->endpoint = backendpoint;
            worker[i]->argv = argv;
            worker[i]->argc = argc;
            worker[i]->optind = optind;

            if (pthread_create(&(worker[i]->thread), NULL,
                               _worker_command, (void *)worker[i]) == -1) {
                _ERR("Create command worker thread(#%d).\n", i+1);
                _worker_destroy(worker, thread, 500);
                zmq_close(backend);
                zmq_ctx_destroy(context);
                free(backendpoint);
                _LOG_CLOSE();
                return -1;
            }
        }
    }

    /* frontend */
    frontend = zmq_socket(context, ZMQ_PULL);
    if (!frontend) {
        _ERR("ZeroMQ frontend socket: %s\n", zmq_strerror(errno));
        if (worker) {
            _worker_destroy(worker, thread, 500);
            zmq_close(backend);
            free(backendpoint);
        }
        zmq_ctx_destroy(context);
        _LOG_CLOSE();
        return -1;
    }

    if (zmq_connect(frontend, frontendpoint) == -1) {
        _ERR("ZeroMQ frontend connect: %s: %s\n",
             frontendpoint, zmq_strerror(errno));
        if (worker) {
            _worker_destroy(worker, thread, 500);
            zmq_close(backend);
            free(backendpoint);
        }
        zmq_close(frontend);
        zmq_ctx_destroy(context);
        _LOG_CLOSE();
        return -1;
    }

    _VERBOSE("ZeroMQ frontend connect: %s\n", frontendpoint);

    _signals();

    _VERBOSE("ZeroMQ start proxy.\n");

    if (backend) {
        zmq_proxy(frontend, backend, NULL);
    } else {
        zmq_pollitem_t pollitems[] = { { frontend, 0, ZMQ_POLLIN, 0 } };

        _NOTICE("default receive process.\n");

        while (!_interrupted) {
            if (zmq_poll(pollitems, 1, -1) == -1) {
                break;
            }

            if (pollitems[0].revents & ZMQ_POLLIN) {
                int more;
                size_t moresz = sizeof(more);

                _DEBUG("ZeroMQ receive in poll event.\n");

                while (!_interrupted) {
                    zmq_msg_t zmsg;

                    if (zmq_msg_init(&zmsg) != 0) {
                        break;
                    }

                    _DEBUG("ZeroMQ receive message.\n");

                    if (zmq_recvmsg(frontend, &zmsg, 0) == -1) {
                        _ERR("ZeroMQ frontend socket receive: %s\n",
                             zmq_strerror(errno));
                        zmq_msg_close(&zmsg);
                        break;
                    }

                    if (zmq_getsockopt(frontend, ZMQ_RCVMORE,
                                       &more, &moresz) == -1) {
                        _ERR("ZeroMQ frontend socket option receive: %s\n",
                             zmq_strerror(errno));
                        //zmq_msg_close(&zmsg);
                        //break;
                        more = 0;
                    }
#ifndef NDEBUG
                    zlmb_dump_printmsg(stderr, &zmsg);
#endif
                    zmq_msg_close(&zmsg);

                    if (!more) {
                        break;
                    }
                }
            }
        }
    }

    _VERBOSE("ZeroMQ end proxy.\n");

    _VERBOSE("ZeroMQ close sockets.\n");

    zmq_close(frontend);

    if (worker) {
        _worker_destroy(worker, thread, 0);
        zmq_close(backend);
        free(backendpoint);
    }

    _VERBOSE("ZeroMQ destory context.\n");

    zmq_ctx_destroy(context);

    _LOG_CLOSE();

    return 0;
}
