#include "server.h"

#include "engine.h"
#include "query_result.h"
#include "utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_WORKER_COUNT 8
#define SERVER_QUEUE_CAPACITY 32
#define SERVER_MAX_REQUEST_SIZE 65536

typedef struct {
    int sockets[SERVER_QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    int stopped;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} ClientQueue;

static volatile sig_atomic_t server_stop_requested = 0;
static int server_listen_fd = -1;

static void server_signal_handler(int signo) {
    (void)signo;
    server_stop_requested = 1;
    if (server_listen_fd >= 0) {
        close(server_listen_fd);
        server_listen_fd = -1;
    }
}

static int server_queue_init(ClientQueue *queue) {
    if (queue == NULL) {
        return FAILURE;
    }

    memset(queue, 0, sizeof(*queue));
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        return FAILURE;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return FAILURE;
    }

    return SUCCESS;
}

static void server_queue_shutdown(ClientQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

static void server_queue_destroy(ClientQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
}

static int server_queue_push(ClientQueue *queue, int client_fd) {
    int status;

    if (queue == NULL) {
        return FAILURE;
    }

    status = FAILURE;
    pthread_mutex_lock(&queue->mutex);
    if (!queue->stopped && queue->count < SERVER_QUEUE_CAPACITY) {
        queue->sockets[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % SERVER_QUEUE_CAPACITY;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
        status = SUCCESS;
    }
    pthread_mutex_unlock(&queue->mutex);

    return status;
}

static int server_queue_pop(ClientQueue *queue, int *client_fd) {
    if (queue == NULL || client_fd == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->stopped) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->stopped) {
        pthread_mutex_unlock(&queue->mutex);
        return FAILURE;
    }

    *client_fd = queue->sockets[queue->head];
    queue->head = (queue->head + 1) % SERVER_QUEUE_CAPACITY;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return SUCCESS;
}

static int server_write_all(int fd, const char *buffer, size_t length) {
    size_t written;
    ssize_t chunk_size;

    written = 0;
    while (written < length) {
        chunk_size = send(fd, buffer + written, length - written, 0);
        if (chunk_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FAILURE;
        }
        if (chunk_size == 0) {
            return FAILURE;
        }
        written += (size_t)chunk_size;
    }

    return SUCCESS;
}

static int server_send_response(int fd, int status_code, const char *status_text,
                                const char *body) {
    char header[512];
    const char *safe_body;
    int header_length;

    safe_body = body == NULL ? "" : body;
    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code, status_text, strlen(safe_body));
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return FAILURE;
    }

    if (server_write_all(fd, header, (size_t)header_length) != SUCCESS) {
        return FAILURE;
    }

    return server_write_all(fd, safe_body, strlen(safe_body));
}

static int server_send_error_response(int fd, int status_code, const char *status_text,
                                      const char *message) {
    char *body;
    size_t length;
    size_t capacity;
    int status;

    body = NULL;
    length = 0;
    capacity = 0;
    if (utils_append_buffer(&body, &length, &capacity,
                            "{\"success\":false,\"error\":\"") != SUCCESS) {
        free(body);
        return FAILURE;
    }

    if (message != NULL) {
        size_t i;
        char escaped[7];

        for (i = 0; message[i] != '\0'; i++) {
            switch (message[i]) {
                case '\\':
                    status = utils_append_buffer(&body, &length, &capacity, "\\\\");
                    break;
                case '"':
                    status = utils_append_buffer(&body, &length, &capacity, "\\\"");
                    break;
                case '\n':
                    status = utils_append_buffer(&body, &length, &capacity, "\\n");
                    break;
                case '\r':
                    status = utils_append_buffer(&body, &length, &capacity, "\\r");
                    break;
                case '\t':
                    status = utils_append_buffer(&body, &length, &capacity, "\\t");
                    break;
                default:
                    escaped[0] = message[i];
                    escaped[1] = '\0';
                    status = utils_append_buffer(&body, &length, &capacity, escaped);
                    break;
            }

            if (status != SUCCESS) {
                free(body);
                return FAILURE;
            }
        }
    }

    if (utils_append_buffer(&body, &length, &capacity, "\"}") != SUCCESS) {
        free(body);
        return FAILURE;
    }

    status = server_send_response(fd, status_code, status_text, body);
    free(body);
    return status;
}

static int server_append_json_string(char **buffer, size_t *length, size_t *capacity,
                                     const char *value) {
    size_t i;
    char one_char[2];

    if (utils_append_buffer(buffer, length, capacity, "\"") != SUCCESS) {
        return FAILURE;
    }

    if (value != NULL) {
        for (i = 0; value[i] != '\0'; i++) {
            switch (value[i]) {
                case '\\':
                    if (utils_append_buffer(buffer, length, capacity, "\\\\") != SUCCESS) {
                        return FAILURE;
                    }
                    break;
                case '"':
                    if (utils_append_buffer(buffer, length, capacity, "\\\"") != SUCCESS) {
                        return FAILURE;
                    }
                    break;
                case '\n':
                    if (utils_append_buffer(buffer, length, capacity, "\\n") != SUCCESS) {
                        return FAILURE;
                    }
                    break;
                case '\r':
                    if (utils_append_buffer(buffer, length, capacity, "\\r") != SUCCESS) {
                        return FAILURE;
                    }
                    break;
                case '\t':
                    if (utils_append_buffer(buffer, length, capacity, "\\t") != SUCCESS) {
                        return FAILURE;
                    }
                    break;
                default:
                    one_char[0] = value[i];
                    one_char[1] = '\0';
                    if (utils_append_buffer(buffer, length, capacity, one_char) != SUCCESS) {
                        return FAILURE;
                    }
                    break;
            }
        }
    }

    return utils_append_buffer(buffer, length, capacity, "\"");
}

static int server_build_result_body(const QueryResult *result, char **out_body) {
    char *body;
    size_t length;
    size_t capacity;
    char number_buffer[32];
    int i;
    int j;

    if (result == NULL || out_body == NULL) {
        return FAILURE;
    }

    body = NULL;
    length = 0;
    capacity = 0;
    if (!result->success) {
        if (utils_append_buffer(&body, &length, &capacity,
                                "{\"success\":false,\"error\":") != SUCCESS ||
            server_append_json_string(&body, &length, &capacity, result->error) != SUCCESS ||
            utils_append_buffer(&body, &length, &capacity, "}") != SUCCESS) {
            free(body);
            return FAILURE;
        }

        *out_body = body;
        return SUCCESS;
    }

    if (result->kind == QUERY_RESULT_MESSAGE) {
        if (utils_append_buffer(&body, &length, &capacity,
                                "{\"success\":true,\"message\":") != SUCCESS ||
            server_append_json_string(&body, &length, &capacity, result->message) != SUCCESS ||
            utils_append_buffer(&body, &length, &capacity, "}") != SUCCESS) {
            free(body);
            return FAILURE;
        }

        *out_body = body;
        return SUCCESS;
    }

    if (result->kind == QUERY_RESULT_TABLE) {
        if (utils_append_buffer(&body, &length, &capacity,
                                "{\"success\":true,\"columns\":[") != SUCCESS) {
            free(body);
            return FAILURE;
        }

        for (i = 0; i < result->column_count; i++) {
            if (i > 0 &&
                utils_append_buffer(&body, &length, &capacity, ",") != SUCCESS) {
                free(body);
                return FAILURE;
            }
            if (server_append_json_string(&body, &length, &capacity,
                                          result->columns[i]) != SUCCESS) {
                free(body);
                return FAILURE;
            }
        }

        if (utils_append_buffer(&body, &length, &capacity, "],\"rows\":[") != SUCCESS) {
            free(body);
            return FAILURE;
        }

        for (i = 0; i < result->row_count; i++) {
            if (i > 0 &&
                utils_append_buffer(&body, &length, &capacity, ",") != SUCCESS) {
                free(body);
                return FAILURE;
            }
            if (utils_append_buffer(&body, &length, &capacity, "[") != SUCCESS) {
                free(body);
                return FAILURE;
            }
            for (j = 0; j < result->column_count; j++) {
                if (j > 0 &&
                    utils_append_buffer(&body, &length, &capacity, ",") != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
                if (server_append_json_string(&body, &length, &capacity,
                                              result->rows[i][j]) != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
            }
            if (utils_append_buffer(&body, &length, &capacity, "]") != SUCCESS) {
                free(body);
                return FAILURE;
            }
        }

        snprintf(number_buffer, sizeof(number_buffer), "%d", result->row_count);
        if (utils_append_buffer(&body, &length, &capacity, "],\"rowCount\":") != SUCCESS ||
            utils_append_buffer(&body, &length, &capacity, number_buffer) != SUCCESS ||
            utils_append_buffer(&body, &length, &capacity, "}") != SUCCESS) {
            free(body);
            return FAILURE;
        }

        *out_body = body;
        return SUCCESS;
    }

    if (utils_append_buffer(&body, &length, &capacity, "{\"success\":true}") != SUCCESS) {
        free(body);
        return FAILURE;
    }

    *out_body = body;
    return SUCCESS;
}

static int server_find_header_end(const char *buffer, size_t length) {
    size_t i;

    if (buffer == NULL || length < 4) {
        return FAILURE;
    }

    for (i = 0; i + 3 < length; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }

    return FAILURE;
}

static int server_parse_content_length(const char *request, int header_end,
                                       size_t *out_content_length) {
    const char *cursor;
    const char *line_end;
    char line_buffer[256];
    size_t content_length;

    if (request == NULL || header_end == FAILURE || out_content_length == NULL) {
        return FAILURE;
    }

    content_length = 0;
    cursor = strstr(request, "\r\n");
    if (cursor == NULL) {
        return FAILURE;
    }
    cursor += 2;

    while (cursor < request + header_end - 2) {
        line_end = strstr(cursor, "\r\n");
        if (line_end == NULL || line_end > request + header_end) {
            return FAILURE;
        }
        if (line_end == cursor) {
            break;
        }

        if ((size_t)(line_end - cursor) >= sizeof(line_buffer)) {
            return FAILURE;
        }

        memcpy(line_buffer, cursor, (size_t)(line_end - cursor));
        line_buffer[line_end - cursor] = '\0';
        if (strncasecmp(line_buffer, "Content-Length:", 15) == 0) {
            long long parsed_length;
            const char *value_cursor;

            value_cursor = line_buffer + 15;
            while (*value_cursor != '\0' &&
                   isspace((unsigned char)*value_cursor)) {
                value_cursor++;
            }

            if (!utils_is_integer(value_cursor)) {
                return FAILURE;
            }

            parsed_length = utils_parse_integer(value_cursor);
            if (parsed_length < 0) {
                return FAILURE;
            }

            content_length = (size_t)parsed_length;
            break;
        }

        cursor = line_end + 2;
    }

    *out_content_length = content_length;
    return SUCCESS;
}

static int server_read_request(int client_fd, char **out_request,
                               size_t *out_request_length, int *out_header_end) {
    char *buffer;
    size_t length;
    size_t capacity;
    int header_end;
    size_t content_length;
    ssize_t chunk_size;
    size_t required_length;

    if (out_request == NULL || out_request_length == NULL || out_header_end == NULL) {
        return FAILURE;
    }

    buffer = (char *)malloc(4096);
    if (buffer == NULL) {
        return FAILURE;
    }

    length = 0;
    capacity = 4096;
    header_end = FAILURE;
    content_length = 0;
    required_length = 0;

    while (1) {
        if (length + 1 >= capacity) {
            char *grown_buffer;
            size_t new_capacity;

            new_capacity = capacity * 2;
            if (new_capacity > SERVER_MAX_REQUEST_SIZE) {
                free(buffer);
                return FAILURE;
            }
            grown_buffer = (char *)realloc(buffer, new_capacity);
            if (grown_buffer == NULL) {
                free(buffer);
                return FAILURE;
            }
            buffer = grown_buffer;
            capacity = new_capacity;
        }

        chunk_size = recv(client_fd, buffer + length, capacity - length - 1, 0);
        if (chunk_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return FAILURE;
        }
        if (chunk_size == 0) {
            break;
        }

        length += (size_t)chunk_size;
        buffer[length] = '\0';
        if (length > SERVER_MAX_REQUEST_SIZE) {
            free(buffer);
            return FAILURE;
        }

        if (header_end == FAILURE) {
            header_end = server_find_header_end(buffer, length);
            if (header_end != FAILURE) {
                if (server_parse_content_length(buffer, header_end,
                                                &content_length) != SUCCESS) {
                    free(buffer);
                    return FAILURE;
                }
                required_length = (size_t)header_end + content_length;
                if (required_length > SERVER_MAX_REQUEST_SIZE) {
                    free(buffer);
                    return FAILURE;
                }
            }
        }

        if (header_end != FAILURE && length >= required_length) {
            *out_request = buffer;
            *out_request_length = length;
            *out_header_end = header_end;
            return SUCCESS;
        }
    }

    free(buffer);
    return FAILURE;
}

static int server_parse_request_line(const char *request,
                                     char *method, size_t method_size,
                                     char *path, size_t path_size) {
    char version[16];

    if (request == NULL || method == NULL || path == NULL) {
        return FAILURE;
    }

    if (sscanf(request, "%7s %63s %15s", method, path, version) != 3) {
        return FAILURE;
    }

    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        return FAILURE;
    }

    if (strlen(method) + 1 > method_size || strlen(path) + 1 > path_size) {
        return FAILURE;
    }

    return SUCCESS;
}

static const char *server_skip_json_whitespace(const char *cursor) {
    while (cursor != NULL && *cursor != '\0' &&
           isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int server_parse_json_string(const char **cursor, char **out_string) {
    const char *current;
    char *buffer;
    size_t length;

    if (cursor == NULL || *cursor == NULL || out_string == NULL) {
        return FAILURE;
    }

    current = *cursor;
    if (*current != '"') {
        return FAILURE;
    }

    current++;
    buffer = (char *)malloc(strlen(current) + 1);
    if (buffer == NULL) {
        return FAILURE;
    }

    length = 0;
    while (*current != '\0') {
        if (*current == '"') {
            buffer[length] = '\0';
            *out_string = buffer;
            *cursor = current + 1;
            return SUCCESS;
        }

        if (*current == '\\') {
            current++;
            switch (*current) {
                case '"':
                    buffer[length++] = '"';
                    break;
                case '\\':
                    buffer[length++] = '\\';
                    break;
                case '/':
                    buffer[length++] = '/';
                    break;
                case 'b':
                    buffer[length++] = '\b';
                    break;
                case 'f':
                    buffer[length++] = '\f';
                    break;
                case 'n':
                    buffer[length++] = '\n';
                    break;
                case 'r':
                    buffer[length++] = '\r';
                    break;
                case 't':
                    buffer[length++] = '\t';
                    break;
                default:
                    free(buffer);
                    return FAILURE;
            }
            current++;
            continue;
        }

        if ((unsigned char)*current < 0x20) {
            free(buffer);
            return FAILURE;
        }

        buffer[length++] = *current;
        current++;
    }

    free(buffer);
    return FAILURE;
}

static int server_extract_sql_from_json(const char *body, char **out_sql) {
    const char *cursor;
    char *key;
    char *value;

    if (body == NULL || out_sql == NULL) {
        return FAILURE;
    }

    cursor = server_skip_json_whitespace(body);
    if (*cursor != '{') {
        return FAILURE;
    }

    cursor = server_skip_json_whitespace(cursor + 1);
    key = NULL;
    value = NULL;
    if (server_parse_json_string(&cursor, &key) != SUCCESS) {
        return FAILURE;
    }

    cursor = server_skip_json_whitespace(cursor);
    if (*cursor != ':') {
        free(key);
        return FAILURE;
    }

    if (strcmp(key, "sql") != 0) {
        free(key);
        return FAILURE;
    }
    free(key);

    cursor = server_skip_json_whitespace(cursor + 1);
    if (server_parse_json_string(&cursor, &value) != SUCCESS) {
        return FAILURE;
    }

    cursor = server_skip_json_whitespace(cursor);
    if (*cursor != '}') {
        free(value);
        return FAILURE;
    }

    cursor = server_skip_json_whitespace(cursor + 1);
    if (*cursor != '\0' || value[0] == '\0') {
        free(value);
        return FAILURE;
    }

    *out_sql = value;
    return SUCCESS;
}

static void server_handle_client(int client_fd) {
    char *request;
    size_t request_length;
    int header_end;
    char method[8];
    char path[64];
    char *body;
    char *sql;
    QueryResult result;
    char *response_body;
    int status;
    int http_status;
    const char *status_text;

    request = NULL;
    request_length = 0;
    header_end = FAILURE;
    method[0] = '\0';
    path[0] = '\0';
    sql = NULL;
    response_body = NULL;
    query_result_init(&result);

    if (server_read_request(client_fd, &request, &request_length, &header_end) != SUCCESS) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Malformed HTTP request.");
        goto cleanup;
    }

    if (server_parse_request_line(request, method, sizeof(method),
                                  path, sizeof(path)) != SUCCESS) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Invalid HTTP request line.");
        goto cleanup;
    }

    if (!utils_equals_ignore_case(method, "POST")) {
        server_send_error_response(client_fd, 405, "Method Not Allowed",
                                   "Only POST /query is supported.");
        goto cleanup;
    }

    if (strcmp(path, "/query") != 0) {
        server_send_error_response(client_fd, 404, "Not Found",
                                   "Only /query endpoint is supported.");
        goto cleanup;
    }

    if ((size_t)header_end > request_length) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Malformed HTTP request.");
        goto cleanup;
    }

    body = request + header_end;
    if (server_extract_sql_from_json(body, &sql) != SUCCESS) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Request body must be {\"sql\":\"...\"}.");
        goto cleanup;
    }

    status = engine_execute_sql(sql, &result);
    http_status = status == SUCCESS ? 200 : 400;
    status_text = status == SUCCESS ? "OK" : "Bad Request";

    if (server_build_result_body(&result, &response_body) != SUCCESS) {
        server_send_error_response(client_fd, 500, "Internal Server Error",
                                   "Failed to build response body.");
        goto cleanup;
    }

    if (server_send_response(client_fd, http_status, status_text,
                             response_body) != SUCCESS) {
        goto cleanup;
    }

cleanup:
    free(response_body);
    free(sql);
    free(request);
    query_result_free(&result);
    close(client_fd);
}

static void *server_worker_main(void *arg) {
    ClientQueue *queue;
    int client_fd;

    queue = (ClientQueue *)arg;
    while (server_queue_pop(queue, &client_fd) == SUCCESS) {
        server_handle_client(client_fd);
    }

    return NULL;
}

static int server_create_listen_socket(int port) {
    int fd;
    int reuse_address;
    struct sockaddr_in address;

    if (port <= 0 || port > 65535) {
        return FAILURE;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return FAILURE;
    }

    reuse_address = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) != 0) {
        close(fd);
        return FAILURE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return FAILURE;
    }

    if (listen(fd, SERVER_QUEUE_CAPACITY) != 0) {
        close(fd);
        return FAILURE;
    }

    return fd;
}

int server_run(int port) {
    ClientQueue queue;
    pthread_t workers[SERVER_WORKER_COUNT];
    int worker_count;
    int client_fd;
    int status;

    if (server_queue_init(&queue) != SUCCESS) {
        return FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, server_signal_handler);
    signal(SIGTERM, server_signal_handler);

    server_stop_requested = 0;
    server_listen_fd = server_create_listen_socket(port);
    if (server_listen_fd < 0) {
        server_queue_destroy(&queue);
        return FAILURE;
    }

    worker_count = 0;
    while (worker_count < SERVER_WORKER_COUNT) {
        if (pthread_create(&workers[worker_count], NULL,
                           server_worker_main, &queue) != 0) {
            status = FAILURE;
            goto shutdown;
        }
        worker_count++;
    }

    printf("Server listening on port %d\n", port);
    fflush(stdout);

    status = SUCCESS;
    while (!server_stop_requested) {
        client_fd = accept(server_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (server_stop_requested) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            status = FAILURE;
            break;
        }

        if (server_queue_push(&queue, client_fd) != SUCCESS) {
            server_send_error_response(client_fd, 503, "Service Unavailable",
                                       "Request queue is full.");
            close(client_fd);
        }
    }

shutdown:
    if (server_listen_fd >= 0) {
        close(server_listen_fd);
        server_listen_fd = -1;
    }
    server_queue_shutdown(&queue);

    while (worker_count > 0) {
        worker_count--;
        pthread_join(workers[worker_count], NULL);
    }

    server_queue_destroy(&queue);
    return status;
}
