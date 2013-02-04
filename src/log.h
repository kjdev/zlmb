#ifndef __ZLMB_LOG_H__
#define __ZLMB_LOG_H__

#include <syslog.h>

#define _LOG_OPEN(_ident) if (_syslog) openlog(_ident, LOG_PID, LOG_USER)
//openlog(_ident, LOG_CONS | LOG_PID, LOG_DAEMON)
#define _LOG_CLOSE() if (_syslog) closelog()
#define _LOG_LEVEL(_ident) if (_syslog) setlogmask(LOG_UPTO(_ident))

#define _LOG(_level, ...)                               \
    if (_syslog) {                                      \
        syslog(_level, __VA_ARGS__);                    \
    } else {                                            \
        switch (_level) {                               \
            case LOG_ERR:                               \
                fprintf(stderr, "ERR: "__VA_ARGS__);    \
                break;                                  \
            case LOG_NOTICE:                            \
                fprintf(stderr, "NOTICE: "__VA_ARGS__); \
                break;                                  \
            case LOG_INFO:                              \
                fprintf(stderr, "INFO: "__VA_ARGS__);   \
                break;                                  \
            case LOG_DEBUG:                             \
                fprintf(stderr, "DEBUG: "__VA_ARGS__);  \
                break;                                  \
            default:                                    \
                fprintf(stderr, __VA_ARGS__);           \
                break;                                  \
        }                                               \
    }

#ifndef NDEBUG
#define _ERR(...) _LOG(LOG_ERR, __VA_ARGS__); _LOG(LOG_ERR, "Trace: %s@%s: %d\n", __FILE__, __FUNCTION__, __LINE__)
#else
#define _ERR(...) _LOG(LOG_ERR, __VA_ARGS__)
#endif
#define _NOTICE(...) _LOG(LOG_NOTICE, __VA_ARGS__)
#define _INFO(...) _LOG(LOG_INFO, __VA_ARGS__)
#define _VERBOSE(...) if (_verbose) { _LOG(LOG_INFO, __VA_ARGS__) }

#ifndef NDEBUG
#define _DEBUG(...) _LOG(LOG_DEBUG, __VA_ARGS__)
#else
#define _DEBUG(...)
#endif

#endif
