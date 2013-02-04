#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

#include "zlmb.h"
#include "option.h"
#include "dump.h"

#define _option_boolean(_self, _key, _data)                                \
    if (strcasecmp("yes", _data) == 0 || strcasecmp("true", _data) == 0 || \
        strcasecmp("on", _data) == 0 || strcasecmp("y", _data) == 0) {     \
        _self->_key = 1;                                                   \
    } else {                                                               \
        _self->_key = 0;                                                   \
    }

#define _option_strdup(_self, _key, _data) \
    if (_self->_key == NULL) {             \
        _self->_key = strdup(_data);       \
    }

#define _option_append(_self, _key, _data)                               \
    if (_self->_key) {                                                   \
        size_t size = strlen(_self->_key) + strlen(_data) + 2;           \
        char *val = (char *)malloc(size);                                \
        if (!val) {                                                      \
            return NULL;                                                 \
        }                                                                \
        size = snprintf(val, size, "%s,%s", _self->_key, (char *)_data); \
        free(_self->_key);                                               \
        _self->_key = val;                                               \
    } else {                                                             \
        _self->_key = strdup(_data);                                     \
    }

#define _option_dumptype(_self, _key, _data)                                \
    if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_BINARY) == 0) {                  \
        _self->_key = ZLMB_DUMP_TYPE_BINARY;                                \
    } else if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_BINARY) == 0) {           \
        _self->_key = ZLMB_DUMP_TYPE_BINARY;                                \
    } else if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_PLAIN) == 0) {            \
        _self->_key = ZLMB_DUMP_TYPE_PLAIN;                                 \
    } else if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_PLAIN_TIME) == 0) {       \
        _self->_key = ZLMB_DUMP_TYPE_PLAIN | ZLMB_DUMP_TYPE_PLAIN_DATETIME; \
    } else if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS) == 0) {      \
        _self->_key = ZLMB_DUMP_TYPE_PLAIN | ZLMB_DUMP_TYPE_PLAIN_FLAGS;    \
    } else if (strcmp(_data, ZLMB_OPTION_DUMPTYPE_PLAIN_TIME_FLAGS) == 0 || \
               strcmp(_data, ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS_TIME) == 0) { \
        _self->_key = ZLMB_DUMP_TYPE_PLAIN | ZLMB_DUMP_TYPE_PLAIN_DATETIME  \
            | ZLMB_DUMP_TYPE_PLAIN_FLAGS;                                   \
    }

zlmb_option_t *
zlmb_option_init(void)
{
    zlmb_option_t *self;

    self = (zlmb_option_t *)malloc(sizeof(zlmb_option_t));
    if (!self) {
        return NULL;
    }

    self->mode = 0;
    self->client_frontendpoint = NULL;
    self->client_backendpoints = NULL;
    self->client_dumpfile = NULL;
    self->client_dumptype = 0;
    self->publish_frontendpoint = NULL;
    self->publish_backendpoint = NULL;
    self->publish_key = NULL;
    self->publish_sendkey = 0;
    self->subscribe_frontendpoints = NULL;
    self->subscribe_backendpoint = NULL;
    self->subscribe_key = NULL;
    self->subscribe_dropkey = 0;
    self->subscribe_dumpfile = NULL;
    self->subscribe_dumptype = 0;
    self->syslog = -1;
    self->verbose = -1;

    return self;
}

void
zlmb_option_destroy(zlmb_option_t **self)
{
    if (*self) {
        if ((*self)->client_frontendpoint) {
            free((*self)->client_frontendpoint);
            (*self)->client_frontendpoint = NULL;
        }
        if ((*self)->client_backendpoints) {
            free((*self)->client_backendpoints);
            (*self)->client_backendpoints = NULL;
        }
        if ((*self)->client_dumpfile) {
            free((*self)->client_dumpfile);
            (*self)->client_dumpfile = NULL;
        }
        if ((*self)->publish_frontendpoint) {
            free((*self)->publish_frontendpoint);
            (*self)->publish_frontendpoint = NULL;
        }
        if ((*self)->publish_backendpoint) {
            free((*self)->publish_backendpoint);
            (*self)->publish_backendpoint = NULL;
        }
        if ((*self)->publish_key) {
            free((*self)->publish_key);
            (*self)->publish_key = NULL;
        }
        if ((*self)->subscribe_frontendpoints) {
            free((*self)->subscribe_frontendpoints);
            (*self)->subscribe_frontendpoints = NULL;
        }
        if ((*self)->subscribe_backendpoint) {
            free((*self)->subscribe_backendpoint);
            (*self)->subscribe_backendpoint = NULL;
        }
        if ((*self)->subscribe_key) {
            free((*self)->subscribe_key);
            (*self)->subscribe_key = NULL;
        }
        if ((*self)->subscribe_dumpfile) {
            free((*self)->subscribe_dumpfile);
            (*self)->subscribe_dumpfile = NULL;
        }

        free(*self);
        *self = NULL;
    }
}

char *
zlmb_option_set(zlmb_option_t *self, char *key, void *data,
                int array, int depth, int clear)
{
    if (!data) {
        return NULL;
    }

    if (key == NULL) {
        if ((strcmp(data, ZLMB_OPTION_KEY_CLIENT_BACKENDPOINTS) == 0
             && self->client_backendpoints) ||
            (strcmp(data, ZLMB_OPTION_KEY_SUBSCRIBE_FRONTENDPOINTS) == 0
             && self->subscribe_frontendpoints)) {
            return NULL;
        }
        return strdup(data);
    }

    if (strcmp(key, ZLMB_OPTION_KEY_MODE) == 0) {
        if (self->mode != 0) {
            if (clear && key) {
                free(key);
            }
            return NULL;
        }
        else if (strcmp(data, ZLMB_OPTION_MODE_CLIENT) == 0) {
            self->mode = ZLMB_MODE_CLIENT;
        } else if (strcmp(data, ZLMB_OPTION_MODE_PUBLISH) == 0) {
            self->mode = ZLMB_MODE_PUBLISH;
        } else if (strcmp(data, ZLMB_OPTION_MODE_SUBSCRIBE) == 0) {
            self->mode = ZLMB_MODE_SUBSCRIBE;
        } else if (strcmp(data, ZLMB_OPTION_MODE_CLIENT_PUBLISH) == 0
                   || strcmp(data, ZLMB_OPTION_MODE_PUBLISH_CLIENT) == 0) {
            self->mode = ZLMB_MODE_CLIENT_PUBLISH;
        } else if (strcmp(data, ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE) == 0
                   || strcmp(data, ZLMB_OPTION_MODE_SUBSCRIBE_PUBLISH) == 0) {
            self->mode = ZLMB_MODE_PUBLISH_SUBSCRIBE;
        } else if (strcmp(data, ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE) == 0
                   || strcmp(data, ZLMB_OPTION_MODE_SUBSCRIBE_CLIENT) == 0) {
            self->mode = ZLMB_MODE_CLIENT_SUBSCRIBE;
        } else if (strcmp(data, ZLMB_OPTION_MODE_STAND_ALONE) == 0) {
            self->mode = ZLMB_MODE_STAND_ALONE;
        } else {
            self->mode = -1;
        }
    } else if (strcmp(key, ZLMB_OPTION_KEY_CLIENT_FRONTENDPOINT) == 0) {
        _option_strdup(self, client_frontendpoint, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_CLIENT_BACKENDPOINTS) == 0
               && depth == 1) {
        _option_append(self, client_backendpoints, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_CLIENT_DUMPFILE) == 0) {
        _option_strdup(self, client_dumpfile, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_CLIENT_DUMPTYPE) == 0) {
        if (self->client_dumptype != 0) {
            if (clear && key) {
                free(key);
            }
            return NULL;
        }
        _option_dumptype(self, client_dumptype, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_PUBLISH_FRONTENDPOINT) == 0) {
        _option_strdup(self, publish_frontendpoint, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_PUBLISH_BACKENDPOINT) == 0) {
        _option_strdup(self, publish_backendpoint, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_PUBLISH_KEY) == 0) {
        _option_strdup(self, publish_key, data);
    } else if (strcmp(key,ZLMB_OPTION_KEY_PUBLISH_SENDKEY) == 0) {
        if (self->publish_sendkey != 1) {
            _option_boolean(self, publish_sendkey, data);
        }
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_FRONTENDPOINTS) == 0
               && depth == 1) {
        _option_append(self, subscribe_frontendpoints, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_BACKENDPOINT) == 0) {
        _option_strdup(self, subscribe_backendpoint, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_KEY) == 0) {
        _option_strdup(self, subscribe_key, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_DROPKEY) == 0) {
        if (self->subscribe_dropkey != 1) {
            _option_boolean(self, subscribe_dropkey, data);
        }
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_DUMPFILE) == 0) {
        _option_strdup(self, subscribe_dumpfile, data);
    } else if (strcmp(key, ZLMB_OPTION_KEY_SUBSCRIBE_DUMPTYPE) == 0) {
        if (self->subscribe_dumptype != 0) {
            if (clear && key) {
                free(key);
            }
            return NULL;
        }
        _option_dumptype(self, subscribe_dumptype, data);
    } else if (strcmp(key,ZLMB_OPTION_KEY_SYSLOG) == 0) {
        if (self->syslog != 1) {
            _option_boolean(self, syslog, data);
        }
    } else if (strcmp(key,ZLMB_OPTION_KEY_VERBOSE) == 0) {
        if (self->verbose != 1) {
            _option_boolean(self, verbose, data);
        }
    }

    if (!array) {
        if (clear && key) {
            free(key);
        }
        return NULL;
    }

    return key;
}

int
zlmb_option_set_default(zlmb_option_t *self)
{
    if (!self) {
        return -1;
    }

    _option_strdup(self, client_dumpfile,
                   ZLMB_DEFAULT_CLIENT_DUMP_FILE);
    _option_strdup(self, subscribe_dumpfile,
                   ZLMB_DEFAULT_SUBSCRIBE_DUMP_FILE);

    return 0;
}

int
zlmb_option_load_file(zlmb_option_t *self, const char * filename)
{
    FILE *file = NULL;
    char *key = NULL;
    int end = 0;
    int depth = 0;
    int seq = 0;
    yaml_event_t event = { 0 };
    yaml_parser_t parser;

    if (!self || !filename) {
        return -1;
    }

    file = fopen(filename, "rb");
    if (!file) {
        return -1;
    }

    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, file);

    while (!end) {
        if (!yaml_parser_parse(&parser, &event)) {
            break;
        } else {
            switch (event.type) {
                case YAML_MAPPING_START_EVENT:
                    depth++;
                    break;
                case YAML_MAPPING_END_EVENT:
                    depth--;
                    break;
                case YAML_SEQUENCE_START_EVENT:
                    seq++;
                    break;
                case YAML_SEQUENCE_END_EVENT:
                    if (key && seq == 1) {
                        free(key);
                        key = NULL;
                    }
                    seq--;
                    break;
                case YAML_STREAM_END_EVENT:
                    if (key) {
                        free(key);
                        key = NULL;
                    }
                    end = 1;
                    break;
                case YAML_SCALAR_EVENT:
                    key = zlmb_option_set(self, key,
                                          (char *)event.data.scalar.value,
                                          seq, depth, 1);
                    break;
                default:
                    break;
            }
        }

        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);

    fclose(file);

    return 0;
}

char *
zlmb_option_dumptype2string(int type)
{
    switch (type) {
        case ZLMB_DUMP_TYPE_PLAIN:
            return ZLMB_OPTION_DUMPTYPE_PLAIN;
        case (ZLMB_DUMP_TYPE_PLAIN|ZLMB_DUMP_TYPE_PLAIN_DATETIME):
            return ZLMB_OPTION_DUMPTYPE_PLAIN_TIME;
        case (ZLMB_DUMP_TYPE_PLAIN|ZLMB_DUMP_TYPE_PLAIN_FLAGS):
            return ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS;
        case (ZLMB_DUMP_TYPE_PLAIN|ZLMB_DUMP_TYPE_PLAIN_DATETIME|ZLMB_DUMP_TYPE_PLAIN_FLAGS):
            return ZLMB_OPTION_DUMPTYPE_PLAIN_TIME_FLAGS;
        case ZLMB_DUMP_TYPE_BINARY:
        default:
            return ZLMB_OPTION_DUMPTYPE_BINARY;
    }
}
