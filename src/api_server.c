#include "api_server.h"

#include "engine.h"
#include "server_queue.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define API_SERVER_MAX_REQUEST_SIZE 65536
#define API_SERVER_MAX_RESPONSE_SIZE 131072

typedef struct {
    ServerQueue queue;
    int worker_count;
    pthread_mutex_t stats_lock;
    unsigned long total_requests;
    unsigned long successful_requests;
    unsigned long failed_requests;
    unsigned long rejected_requests;
    unsigned long active_workers;
} ApiServerContext;

static volatile sig_atomic_t api_server_stop_requested = 0;

static void api_server_handle_signal(int signum) {
    (void)signum;
    api_server_stop_requested = 1;
}

static void api_server_stats_increment(unsigned long *counter,
                                       ApiServerContext *context) {
    pthread_mutex_lock(&context->stats_lock);
    (*counter)++;
    pthread_mutex_unlock(&context->stats_lock);
}

static void api_server_stats_add_worker(ApiServerContext *context, int delta) {
    pthread_mutex_lock(&context->stats_lock);
    context->active_workers = (unsigned long)((long)context->active_workers + delta);
    pthread_mutex_unlock(&context->stats_lock);
}

static void api_server_build_health_json(ApiServerContext *context,
                                         char *body, size_t body_size) {
    int queue_depth;

    queue_depth = server_queue_current_count(&context->queue);
    snprintf(body, body_size,
             "{\"success\":true,\"status\":\"ok\","
             "\"worker_count\":%d,\"queue_capacity\":%d,\"queue_depth\":%d}",
             context->worker_count, API_SERVER_QUEUE_CAPACITY, queue_depth);
}

static void api_server_build_stats_json(ApiServerContext *context,
                                        char *body, size_t body_size) {
    unsigned long total_requests;
    unsigned long successful_requests;
    unsigned long failed_requests;
    unsigned long rejected_requests;
    unsigned long active_workers;
    int queue_depth;

    pthread_mutex_lock(&context->stats_lock);
    total_requests = context->total_requests;
    successful_requests = context->successful_requests;
    failed_requests = context->failed_requests;
    rejected_requests = context->rejected_requests;
    active_workers = context->active_workers;
    pthread_mutex_unlock(&context->stats_lock);

    queue_depth = server_queue_current_count(&context->queue);
    snprintf(body, body_size,
             "{\"success\":true,"
             "\"stats\":{\"total_requests\":%lu,"
             "\"successful_requests\":%lu,"
             "\"failed_requests\":%lu,"
             "\"rejected_requests\":%lu,"
             "\"active_workers\":%lu,"
             "\"queue_depth\":%d,"
             "\"queue_capacity\":%d}}",
             total_requests, successful_requests, failed_requests,
             rejected_requests, active_workers, queue_depth,
             API_SERVER_QUEUE_CAPACITY);
}

static int api_server_send_all(int client_fd, const char *buffer, size_t length) {
    size_t sent;
    ssize_t written;

    sent = 0;
    while (sent < length) {
        written = send(client_fd, buffer + sent, length - sent, 0);
        if (written <= 0) {
            return FAILURE;
        }
        sent += (size_t)written;
    }

    return SUCCESS;
}

static int api_server_send_json_response(int client_fd, int status_code,
                                         const char *status_text,
                                         const char *body) {
    char response[API_SERVER_MAX_RESPONSE_SIZE];
    size_t body_length;
    int written;

    body_length = strlen(body);
    written = snprintf(response, sizeof(response),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       status_code, status_text, body_length, body);
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return FAILURE;
    }

    return api_server_send_all(client_fd, response, (size_t)written);
}

static void api_server_send_simple_error(int client_fd, int status_code,
                                         const char *status_text,
                                         const char *message) {
    char body[512];

    snprintf(body, sizeof(body), "{\"success\":false,\"error\":\"%s\"}", message);
    api_server_send_json_response(client_fd, status_code, status_text, body);
}

static const char *api_server_find_header_end(const char *buffer) {
    const char *header_end;

    header_end = strstr(buffer, "\r\n\r\n");
    if (header_end != NULL) {
        return header_end;
    }

    return strstr(buffer, "\n\n");
}

static size_t api_server_header_separator_length(const char *header_end) {
    if (header_end == NULL) {
        return 0;
    }

    if (header_end[0] == '\r' && header_end[1] == '\n' &&
        header_end[2] == '\r' && header_end[3] == '\n') {
        return 4;
    }

    return 2;
}

static const char *api_server_find_header_value(const char *buffer,
                                                const char *header_name) {
    size_t header_name_length;
    const char *line_start;

    if (buffer == NULL || header_name == NULL) {
        return NULL;
    }

    header_name_length = strlen(header_name);
    line_start = buffer;
    while (*line_start != '\0') {
        const char *line_end;
        size_t candidate_length;

        if ((line_start[0] == '\r' && line_start[1] == '\n') ||
            line_start[0] == '\n') {
            break;
        }

        line_end = strstr(line_start, "\r\n");
        if (line_end == NULL) {
            line_end = strchr(line_start, '\n');
        }
        if (line_end == NULL) {
            line_end = line_start + strlen(line_start);
        }

        candidate_length = (size_t)(line_end - line_start);
        if (candidate_length > header_name_length &&
            line_start[header_name_length] == ':' &&
            strncasecmp(line_start, header_name, header_name_length) == 0) {
            const char *value = line_start + header_name_length + 1;
            while (*value != '\0' && isspace((unsigned char)*value)) {
                value++;
            }
            return value;
        }

        line_start = line_end;
        if (*line_start == '\r') {
            line_start++;
        }
        if (*line_start == '\n') {
            line_start++;
        }
    }

    return NULL;
}

static int api_server_parse_request_line(const char *request,
                                         char *method, size_t method_size,
                                         char *path, size_t path_size) {
    const char *line_end;
    char request_line[256];
    size_t line_length;
    int matched;

    (void)method_size;
    (void)path_size;

    if (request == NULL || method == NULL || path == NULL) {
        return FAILURE;
    }

    line_end = strstr(request, "\r\n");
    if (line_end == NULL) {
        line_end = strchr(request, '\n');
    }
    if (line_end == NULL) {
        return FAILURE;
    }

    line_length = (size_t)(line_end - request);
    if (line_length == 0 || line_length >= sizeof(request_line)) {
        return FAILURE;
    }

    memcpy(request_line, request, line_length);
    request_line[line_length] = '\0';
    matched = sscanf(request_line, "%15s %127s", method, path);
    return matched == 2 ? SUCCESS : FAILURE;
}

static int api_server_read_request(int client_fd, char **out_buffer, size_t *out_length) {
    char *buffer;
    size_t capacity;
    size_t length;
    ssize_t read_size;
    const char *header_end;
    size_t header_length;
    const char *content_length_header;
    size_t expected_body_length;

    if (out_buffer == NULL || out_length == NULL) {
        return FAILURE;
    }

    buffer = (char *)malloc(API_SERVER_MAX_REQUEST_SIZE + 1);
    if (buffer == NULL) {
        return FAILURE;
    }

    capacity = API_SERVER_MAX_REQUEST_SIZE;
    length = 0;
    header_end = NULL;
    header_length = 0;
    expected_body_length = 0;

    while (length < capacity) {
        read_size = recv(client_fd, buffer + length, capacity - length, 0);
        if (read_size <= 0) {
            free(buffer);
            return FAILURE;
        }

        length += (size_t)read_size;
        buffer[length] = '\0';
        header_end = api_server_find_header_end(buffer);
        if (header_end == NULL) {
            continue;
        }

        header_length = (size_t)(header_end - buffer);
        content_length_header = api_server_find_header_value(buffer, "Content-Length");
        if (content_length_header == NULL) {
            *out_buffer = buffer;
            *out_length = length;
            return SUCCESS;
        }

        expected_body_length = (size_t)strtoul(content_length_header, NULL, 10);
        if (length >= header_length +
                      api_server_header_separator_length(header_end) +
                      expected_body_length) {
            *out_buffer = buffer;
            *out_length = length;
            return SUCCESS;
        }
    }

    free(buffer);
    return FAILURE;
}

static int api_server_json_escape_into(const char *src, char *dest, size_t dest_size) {
    size_t out_index;
    size_t i;

    if (src == NULL || dest == NULL || dest_size == 0) {
        return FAILURE;
    }

    out_index = 0;
    for (i = 0; src[i] != '\0'; i++) {
        const char *replacement = NULL;
        char escaped[7];

        switch (src[i]) {
            case '\\':
                replacement = "\\\\";
                break;
            case '"':
                replacement = "\\\"";
                break;
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            default:
                if ((unsigned char)src[i] < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x",
                             (unsigned char)src[i]);
                    replacement = escaped;
                }
                break;
        }

        if (replacement != NULL) {
            size_t replacement_length = strlen(replacement);
            if (out_index + replacement_length + 1 >= dest_size) {
                return FAILURE;
            }
            memcpy(dest + out_index, replacement, replacement_length);
            out_index += replacement_length;
        } else {
            if (out_index + 2 >= dest_size) {
                return FAILURE;
            }
            dest[out_index++] = src[i];
        }
    }

    dest[out_index] = '\0';
    return SUCCESS;
}

static int api_server_extract_sql(const char *body, char *sql, size_t sql_size) {
    const char *key;
    const char *cursor;
    size_t out_index;

    key = strstr(body, "\"sql\"");
    if (key == NULL) {
        return FAILURE;
    }

    cursor = strchr(key, ':');
    if (cursor == NULL) {
        return FAILURE;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    if (*cursor != '"') {
        return FAILURE;
    }

    cursor++;
    out_index = 0;
    while (*cursor != '\0' && *cursor != '"') {
        char value;

        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0') {
                return FAILURE;
            }
            switch (*cursor) {
                case 'n':
                    value = '\n';
                    break;
                case 'r':
                    value = '\r';
                    break;
                case 't':
                    value = '\t';
                    break;
                case '\\':
                    value = '\\';
                    break;
                case '"':
                    value = '"';
                    break;
                default:
                    value = *cursor;
                    break;
            }
        } else {
            value = *cursor;
        }

        if (out_index + 1 >= sql_size) {
            return FAILURE;
        }
        sql[out_index++] = value;
        cursor++;
    }

    if (*cursor != '"') {
        return FAILURE;
    }

    sql[out_index] = '\0';
    return SUCCESS;
}

static int api_server_build_result_json(const QueryResult *result,
                                        char *body, size_t body_size) {
    int written;
    int offset;
    int i;
    int j;
    char escaped[2048];

    if (result == NULL || body == NULL || body_size == 0) {
        return FAILURE;
    }

    if (!result->success) {
        if (api_server_json_escape_into(result->error, escaped, sizeof(escaped)) != SUCCESS) {
            return FAILURE;
        }
        written = snprintf(body, body_size, "{\"success\":false,\"error\":\"%s\"}", escaped);
        return (written < 0 || (size_t)written >= body_size) ? FAILURE : SUCCESS;
    }

    if (result->kind == QUERY_RESULT_MESSAGE) {
        if (api_server_json_escape_into(result->message, escaped, sizeof(escaped)) != SUCCESS) {
            return FAILURE;
        }
        written = snprintf(body, body_size, "{\"success\":true,\"message\":\"%s\"}", escaped);
        return (written < 0 || (size_t)written >= body_size) ? FAILURE : SUCCESS;
    }

    offset = snprintf(body, body_size, "{\"success\":true,\"columns\":[");
    if (offset < 0 || (size_t)offset >= body_size) {
        return FAILURE;
    }

    for (i = 0; i < result->column_count; i++) {
        if (api_server_json_escape_into(result->columns[i], escaped, sizeof(escaped)) != SUCCESS) {
            return FAILURE;
        }
        written = snprintf(body + offset, body_size - (size_t)offset,
                           "%s\"%s\"", i == 0 ? "" : ",", escaped);
        if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
            return FAILURE;
        }
        offset += written;
    }

    written = snprintf(body + offset, body_size - (size_t)offset, "],\"rows\":[");
    if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
        return FAILURE;
    }
    offset += written;

    for (i = 0; i < result->row_count; i++) {
        written = snprintf(body + offset, body_size - (size_t)offset, "%s[", i == 0 ? "" : ",");
        if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
            return FAILURE;
        }
        offset += written;

        for (j = 0; j < result->column_count; j++) {
            if (api_server_json_escape_into(result->rows[i][j], escaped, sizeof(escaped)) != SUCCESS) {
                return FAILURE;
            }
            written = snprintf(body + offset, body_size - (size_t)offset,
                               "%s\"%s\"", j == 0 ? "" : ",", escaped);
            if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
                return FAILURE;
            }
            offset += written;
        }

        written = snprintf(body + offset, body_size - (size_t)offset, "]");
        if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
            return FAILURE;
        }
        offset += written;
    }

    written = snprintf(body + offset, body_size - (size_t)offset, "]}");
    if (written < 0 || (size_t)written >= body_size - (size_t)offset) {
        return FAILURE;
    }

    return SUCCESS;
}

static void api_server_process_client(ApiServerContext *context, int client_fd) {
    char *request;
    size_t request_length;
    const char *header_end;
    const char *body;
    size_t separator_length;
    char sql[MAX_SQL_LENGTH];
    char method[16];
    char path[128];
    QueryResult result;
    char response_body[API_SERVER_MAX_RESPONSE_SIZE];
    int status;

    api_server_stats_increment(&context->total_requests, context);
    request = NULL;
    request_length = 0;
    if (api_server_read_request(client_fd, &request, &request_length) != SUCCESS) {
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 400, "Bad Request", "Failed to read request.");
        return;
    }

    (void)request_length;
    if (api_server_parse_request_line(request, method, sizeof(method),
                                      path, sizeof(path)) != SUCCESS) {
        free(request);
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 400, "Bad Request", "Malformed request line.");
        return;
    }

    header_end = api_server_find_header_end(request);
    if (header_end == NULL) {
        free(request);
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 400, "Bad Request", "Malformed HTTP request.");
        return;
    }

    if (utils_equals_ignore_case(method, "GET") &&
        strcmp(path, "/health") == 0) {
        free(request);
        api_server_build_health_json(context, response_body, sizeof(response_body));
        api_server_stats_increment(&context->successful_requests, context);
        api_server_send_json_response(client_fd, 200, "OK", response_body);
        return;
    }

    if (utils_equals_ignore_case(method, "GET") &&
        strcmp(path, "/stats") == 0) {
        free(request);
        api_server_build_stats_json(context, response_body, sizeof(response_body));
        api_server_stats_increment(&context->successful_requests, context);
        api_server_send_json_response(client_fd, 200, "OK", response_body);
        return;
    }

    if (!utils_equals_ignore_case(method, "POST") || strcmp(path, "/query") != 0) {
        free(request);
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 404, "Not Found", "Unsupported endpoint.");
        return;
    }

    separator_length = api_server_header_separator_length(header_end);
    body = header_end + separator_length;
    if (body == NULL || api_server_extract_sql(body, sql, sizeof(sql)) != SUCCESS) {
        free(request);
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 400, "Bad Request", "Missing or invalid sql field.");
        return;
    }

    free(request);

    query_result_init(&result);
    status = engine_execute_sql(sql, &result);
    if (status != SUCCESS && result.error[0] == '\0') {
        utils_safe_strcpy(result.error, sizeof(result.error), "SQL execution failed.");
    }

    if (api_server_build_result_json(&result, response_body, sizeof(response_body)) != SUCCESS) {
        query_result_free(&result);
        api_server_stats_increment(&context->failed_requests, context);
        api_server_send_simple_error(client_fd, 500, "Internal Server Error",
                                     "Failed to build JSON response.");
        return;
    }

    if (result.success) {
        api_server_stats_increment(&context->successful_requests, context);
    } else {
        api_server_stats_increment(&context->failed_requests, context);
    }

    api_server_send_json_response(client_fd,
                                  result.success ? 200 : 400,
                                  result.success ? "OK" : "Bad Request",
                                  response_body);
    query_result_free(&result);
}

static void *api_server_worker_main(void *arg) {
    ApiServerContext *context;
    int client_fd;

    context = (ApiServerContext *)arg;
    while (server_queue_pop(&context->queue, &client_fd) == SUCCESS) {
        api_server_stats_add_worker(context, 1);
        api_server_process_client(context, client_fd);
        api_server_stats_add_worker(context, -1);
        close(client_fd);
    }

    return NULL;
}

int api_server_run(int port) {
    int listen_fd;
    int opt_value;
    struct sockaddr_in server_addr;
    ApiServerContext context;
    pthread_t workers[API_SERVER_WORKER_COUNT];
    int i;

    signal(SIGINT, api_server_handle_signal);
    signal(SIGTERM, api_server_handle_signal);

    memset(&context, 0, sizeof(context));
    pthread_mutex_init(&context.stats_lock, NULL);
    if (server_queue_init(&context.queue, API_SERVER_QUEUE_CAPACITY) != SUCCESS) {
        pthread_mutex_destroy(&context.stats_lock);
        fprintf(stderr, "Error: Failed to initialize request queue.\n");
        return FAILURE;
    }

    context.worker_count = API_SERVER_WORKER_COUNT;
    for (i = 0; i < context.worker_count; i++) {
        if (pthread_create(&workers[i], NULL, api_server_worker_main, &context) != 0) {
            fprintf(stderr, "Error: Failed to create worker thread.\n");
            context.worker_count = i;
            server_queue_shutdown(&context.queue);
            while (--i >= 0) {
                pthread_join(workers[i], NULL);
            }
            server_queue_destroy(&context.queue);
            pthread_mutex_destroy(&context.stats_lock);
            return FAILURE;
        }
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "Error: Failed to create listen socket.\n");
        server_queue_shutdown(&context.queue);
        for (i = 0; i < context.worker_count; i++) {
            pthread_join(workers[i], NULL);
        }
        server_queue_destroy(&context.queue);
        pthread_mutex_destroy(&context.stats_lock);
        return FAILURE;
    }

    opt_value = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_value, sizeof(opt_value));
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0 ||
        listen(listen_fd, 64) != 0) {
        fprintf(stderr, "Error: Failed to bind/listen on port %d.\n", port);
        close(listen_fd);
        server_queue_shutdown(&context.queue);
        for (i = 0; i < context.worker_count; i++) {
            pthread_join(workers[i], NULL);
        }
        server_queue_destroy(&context.queue);
        pthread_mutex_destroy(&context.stats_lock);
        return FAILURE;
    }

    printf("API server listening on port %d\n", port);
    fflush(stdout);

    while (!api_server_stop_requested) {
        int client_fd;

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (server_queue_push(&context.queue, client_fd) != SUCCESS) {
            api_server_stats_increment(&context.rejected_requests, &context);
            api_server_send_simple_error(client_fd, 503, "Service Unavailable",
                                         "Server queue is full.");
            close(client_fd);
        }
    }

    close(listen_fd);
    server_queue_shutdown(&context.queue);
    for (i = 0; i < context.worker_count; i++) {
        pthread_join(workers[i], NULL);
    }
    server_queue_destroy(&context.queue);
    pthread_mutex_destroy(&context.stats_lock);
    return SUCCESS;
}
