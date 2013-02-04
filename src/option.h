#ifndef __ZLMB_OPTION_H__
#define __ZLMB_OPTION_H__

#define ZLMB_OPTION_KEY_MODE                     "mode"
#define ZLMB_OPTION_KEY_CLIENT_FRONTENDPOINT     "client_frontendpoint"
#define ZLMB_OPTION_KEY_CLIENT_BACKENDPOINTS     "client_backendpoints"
#define ZLMB_OPTION_KEY_CLIENT_DUMPFILE          "client_dumpfile"
#define ZLMB_OPTION_KEY_CLIENT_DUMPTYPE          "client_dumptype"
#define ZLMB_OPTION_KEY_PUBLISH_FRONTENDPOINT    "publish_frontendpoint"
#define ZLMB_OPTION_KEY_PUBLISH_BACKENDPOINT     "publish_backendpoint"
#define ZLMB_OPTION_KEY_PUBLISH_KEY              "publish_key"
#define ZLMB_OPTION_KEY_PUBLISH_SENDKEY          "publish_sendkey"
#define ZLMB_OPTION_KEY_SUBSCRIBE_FRONTENDPOINTS "subscribe_frontendpoints"
#define ZLMB_OPTION_KEY_SUBSCRIBE_BACKENDPOINT   "subscribe_backendpoint"
#define ZLMB_OPTION_KEY_SUBSCRIBE_KEY            "subscribe_key"
#define ZLMB_OPTION_KEY_SUBSCRIBE_DROPKEY        "subscribe_dropkey"
#define ZLMB_OPTION_KEY_SUBSCRIBE_DUMPFILE       "subscribe_dumpfile"
#define ZLMB_OPTION_KEY_SUBSCRIBE_DUMPTYPE       "subscribe_dumptype"

#define ZLMB_OPTION_KEY_SYSLOG                   "syslog"
#define ZLMB_OPTION_KEY_VERBOSE                  "verbose"

#define ZLMB_OPTION_MODE_CLIENT            "client"
#define ZLMB_OPTION_MODE_PUBLISH           "publish"
#define ZLMB_OPTION_MODE_SUBSCRIBE         "subscribe"
#define ZLMB_OPTION_MODE_CLIENT_PUBLISH    "client-publish"
#define ZLMB_OPTION_MODE_PUBLISH_CLIENT    "publish-client"
#define ZLMB_OPTION_MODE_PUBLISH_SUBSCRIBE "publish-subscribe"
#define ZLMB_OPTION_MODE_SUBSCRIBE_PUBLISH "subscribe-publish"
#define ZLMB_OPTION_MODE_CLIENT_SUBSCRIBE  "client-subscribe"
#define ZLMB_OPTION_MODE_SUBSCRIBE_CLIENT  "subscribe-client"
#define ZLMB_OPTION_MODE_STAND_ALONE       "stand-alone"

#define ZLMB_OPTION_DUMPTYPE_BINARY           "binary"
#define ZLMB_OPTION_DUMPTYPE_PLAIN            "plain-text"
#define ZLMB_OPTION_DUMPTYPE_PLAIN_TIME       "plain-time"
#define ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS      "plain-flags"
#define ZLMB_OPTION_DUMPTYPE_PLAIN_TIME_FLAGS "plain-time-flags"
#define ZLMB_OPTION_DUMPTYPE_PLAIN_FLAGS_TIME "plain-flags-time"

typedef struct zlmb_option {
    int mode;
    char *client_frontendpoint;
    char *client_backendpoints;
    char *client_dumpfile;
    int client_dumptype;
    char *publish_frontendpoint;
    char *publish_backendpoint;
    char *publish_key;
    int publish_sendkey;
    char *subscribe_frontendpoints;
    char *subscribe_backendpoint;
    char *subscribe_key;
    int subscribe_dropkey;
    char *subscribe_dumpfile;
    int subscribe_dumptype;
    int syslog;
    int verbose;
} zlmb_option_t;

zlmb_option_t * zlmb_option_init(void);
void zlmb_option_destroy(zlmb_option_t **self);
char * zlmb_option_set(zlmb_option_t *self, char *key, void *data, int array, int depth, int clear);
int zlmb_option_set_default(zlmb_option_t *self);
int zlmb_option_load_file(zlmb_option_t *self, const char * filename);
char * zlmb_option_dumptype2string(int type);

#endif
