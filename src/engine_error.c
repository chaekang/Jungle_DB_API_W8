#include "engine_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ENGINE_ERROR_BUFFER_SIZE 512

static __thread char engine_error_buffer[ENGINE_ERROR_BUFFER_SIZE];

void engine_error_clear(void) {
    engine_error_buffer[0] = '\0';
}

void engine_error_set(const char *format, ...) {
    va_list args;

    if (format == NULL) {
        engine_error_clear();
        return;
    }

    va_start(args, format);
    vsnprintf(engine_error_buffer, sizeof(engine_error_buffer), format, args);
    va_end(args);
}

const char *engine_error_get(void) {
    return engine_error_buffer;
}
