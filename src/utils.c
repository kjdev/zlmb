#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define ZLMB_UTILS_BUFSIZ 128

int
zlmb_utils_asprintf(char **str, const char *format, ...)
{
    int ret, size = ZLMB_UTILS_BUFSIZ;
    char *string = NULL;
    va_list arg, ap;
    void *tmp = NULL;

    string = (char *)malloc(size);
    if (!string) {
        return -1;
    }

    va_start(arg, format);
    va_copy(ap, arg);

    ret = vsnprintf(string, size, format, arg);

    if (ret >= size) {
        size = ret + 1;
        tmp = realloc(string, size);
        if (!tmp) {
            free(string);
            va_end(arg);
            va_end(ap);
            return -1;
        }
        string = (char *)tmp;
        ret = vsnprintf(string, size, format, ap);
        if (ret >= 0) {
            *str = string;
        } else {
            free(string);
        }
    } else if (ret >= 0) {
        *str = string;
    } else {
        free(string);
    }

    va_end(arg);
    va_end(ap);

    return ret;
}
