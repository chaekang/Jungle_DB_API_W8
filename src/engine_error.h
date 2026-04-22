#ifndef ENGINE_ERROR_H
#define ENGINE_ERROR_H

void engine_error_clear(void);
void engine_error_set(const char *format, ...);
const char *engine_error_get(void);

#endif
