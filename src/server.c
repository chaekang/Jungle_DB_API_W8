#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "server.h"

#include "engine.h"
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
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_WORKER_COUNT 8
#define SERVER_QUEUE_CAPACITY 32
#define SERVER_LISTEN_BACKLOG 64
#define SERVER_REQUEST_BUFFER_SIZE 65536
#define SERVER_RESPONSE_INITIAL_CAPACITY 1024

typedef struct {
    int client_fd;
} ServerRequest;

typedef struct {
    ServerRequest items[SERVER_QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} ServerRequestQueue;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} ServerBuffer;

typedef enum {
    SERVER_READ_OK,
    SERVER_READ_BAD_REQUEST,
    SERVER_READ_TOO_LARGE
} ServerReadStatus;

static volatile sig_atomic_t server_stop_requested = 0;
static int server_listener_fd = -1;

static void server_request_shutdown(int signal_number) {
    (void)signal_number;
    server_stop_requested = 1;
    if (server_listener_fd >= 0) {
        close(server_listener_fd);
        server_listener_fd = -1;
    }
}

static int server_queue_init(ServerRequestQueue *queue) {
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

static void server_queue_destroy(ServerRequestQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
}

static void server_queue_shutdown(ServerRequestQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutting_down = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

static int server_queue_push(ServerRequestQueue *queue, int client_fd) {
    if (queue == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock(&queue->mutex);
    if (queue->shutting_down || queue->count == SERVER_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&queue->mutex);
        return FAILURE;
    }

    queue->items[queue->tail].client_fd = client_fd;
    queue->tail = (queue->tail + 1) % SERVER_QUEUE_CAPACITY;
    queue->count++;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return SUCCESS;
}

static int server_queue_pop(ServerRequestQueue *queue, ServerRequest *out_request) {
    if (queue == NULL || out_request == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->shutting_down) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return FAILURE;
    }

    *out_request = queue->items[queue->head];
    queue->head = (queue->head + 1) % SERVER_QUEUE_CAPACITY;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return SUCCESS;
}

static int server_buffer_init(ServerBuffer *buffer) {
    if (buffer == NULL) {
        return FAILURE;
    }

    buffer->data = (char *)malloc(SERVER_RESPONSE_INITIAL_CAPACITY);
    if (buffer->data == NULL) {
        return FAILURE;
    }

    buffer->data[0] = '\0';
    buffer->length = 0;
    buffer->capacity = SERVER_RESPONSE_INITIAL_CAPACITY;
    return SUCCESS;
}

static void server_buffer_free(ServerBuffer *buffer) {
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static int server_buffer_reserve(ServerBuffer *buffer, size_t extra_length) {
    size_t required;
    size_t new_capacity;
    char *new_data;

    if (buffer == NULL) {
        return FAILURE;
    }

    required = buffer->length + extra_length + 1;
    if (required <= buffer->capacity) {
        return SUCCESS;
    }

    new_capacity = buffer->capacity;
    while (new_capacity < required) {
        new_capacity *= 2;
    }

    new_data = (char *)realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return FAILURE;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return SUCCESS;
}

static int server_buffer_append_n(ServerBuffer *buffer,
                                  const char *text,
                                  size_t length) {
    if (buffer == NULL || text == NULL) {
        return FAILURE;
    }

    if (server_buffer_reserve(buffer, length) != SUCCESS) {
        return FAILURE;
    }

    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return SUCCESS;
}

static int server_buffer_append(ServerBuffer *buffer, const char *text) {
    if (text == NULL) {
        return FAILURE;
    }

    return server_buffer_append_n(buffer, text, strlen(text));
}

static int server_buffer_append_char(ServerBuffer *buffer, char value) {
    return server_buffer_append_n(buffer, &value, 1);
}

static int server_buffer_append_json_string(ServerBuffer *buffer,
                                            const char *text) {
    const unsigned char *cursor;
    char escaped[8];

    if (buffer == NULL || text == NULL) {
        return FAILURE;
    }

    if (server_buffer_append_char(buffer, '"') != SUCCESS) {
        return FAILURE;
    }

    cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\\') {
            if (server_buffer_append_char(buffer, '\\') != SUCCESS ||
                server_buffer_append_char(buffer, (char)*cursor) != SUCCESS) {
                return FAILURE;
            }
        } else if (*cursor == '\n') {
            if (server_buffer_append(buffer, "\\n") != SUCCESS) {
                return FAILURE;
            }
        } else if (*cursor == '\r') {
            if (server_buffer_append(buffer, "\\r") != SUCCESS) {
                return FAILURE;
            }
        } else if (*cursor == '\t') {
            if (server_buffer_append(buffer, "\\t") != SUCCESS) {
                return FAILURE;
            }
        } else if (*cursor < 0x20U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            if (server_buffer_append(buffer, escaped) != SUCCESS) {
                return FAILURE;
            }
        } else if (server_buffer_append_char(buffer, (char)*cursor) != SUCCESS) {
            return FAILURE;
        }

        cursor++;
    }

    return server_buffer_append_char(buffer, '"');
}

static int server_build_json_response(const QueryResult *result,
                                      char **out_body) {
    ServerBuffer body;
    int i;
    int j;

    if (result == NULL || out_body == NULL) {
        return FAILURE;
    }

    *out_body = NULL;
    if (server_buffer_init(&body) != SUCCESS) {
        return FAILURE;
    }

    if (server_buffer_append(&body, "{\"success\":") != SUCCESS ||
        server_buffer_append(&body, result->success ? "true" : "false") != SUCCESS) {
        server_buffer_free(&body);
        return FAILURE;
    }

    if (!result->success) {
        if (server_buffer_append(&body, ",\"error\":") != SUCCESS ||
            server_buffer_append_json_string(&body, result->error) != SUCCESS) {
            server_buffer_free(&body);
            return FAILURE;
        }
    } else if (result->kind == QUERY_RESULT_MESSAGE) {
        if (server_buffer_append(&body, ",\"message\":") != SUCCESS ||
            server_buffer_append_json_string(&body, result->message) != SUCCESS) {
            server_buffer_free(&body);
            return FAILURE;
        }
    } else {
        if (server_buffer_append(&body, ",\"columns\":[") != SUCCESS) {
            server_buffer_free(&body);
            return FAILURE;
        }

        for (i = 0; i < result->column_count; i++) {
            if (i > 0 && server_buffer_append_char(&body, ',') != SUCCESS) {
                server_buffer_free(&body);
                return FAILURE;
            }
            if (server_buffer_append_json_string(&body, result->columns[i]) != SUCCESS) {
                server_buffer_free(&body);
                return FAILURE;
            }
        }

        if (server_buffer_append(&body, "],\"rows\":[") != SUCCESS) {
            server_buffer_free(&body);
            return FAILURE;
        }

        for (i = 0; i < result->row_count; i++) {
            if (i > 0 && server_buffer_append_char(&body, ',') != SUCCESS) {
                server_buffer_free(&body);
                return FAILURE;
            }
            if (server_buffer_append_char(&body, '[') != SUCCESS) {
                server_buffer_free(&body);
                return FAILURE;
            }

            for (j = 0; j < result->column_count; j++) {
                if (j > 0 && server_buffer_append_char(&body, ',') != SUCCESS) {
                    server_buffer_free(&body);
                    return FAILURE;
                }
                if (server_buffer_append_json_string(&body,
                                                     result->rows[i][j]) != SUCCESS) {
                    server_buffer_free(&body);
                    return FAILURE;
                }
            }

            if (server_buffer_append_char(&body, ']') != SUCCESS) {
                server_buffer_free(&body);
                return FAILURE;
            }
        }

        if (server_buffer_append_char(&body, ']') != SUCCESS) {
            server_buffer_free(&body);
            return FAILURE;
        }
    }

    if (server_buffer_append_char(&body, '}') != SUCCESS) {
        server_buffer_free(&body);
        return FAILURE;
    }

    *out_body = body.data;
    return SUCCESS;
}

static int server_send_all(int fd, const char *data, size_t length) {
    size_t sent_total;

    sent_total = 0;
    while (sent_total < length) {
        ssize_t sent = send(fd, data + sent_total, length - sent_total, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FAILURE;
        }
        if (sent == 0) {
            return FAILURE;
        }
        sent_total += (size_t)sent;
    }

    return SUCCESS;
}

static int server_send_response(int fd, int status_code,
                                const char *reason, const char *body) {
    char header[512];
    size_t body_length;
    int header_length;

    if (reason == NULL || body == NULL) {
        return FAILURE;
    }

    body_length = strlen(body);
    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code, reason, body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return FAILURE;
    }

    if (server_send_all(fd, header, (size_t)header_length) != SUCCESS) {
        return FAILURE;
    }

    return server_send_all(fd, body, body_length);
}

static int server_send_json_error(int fd, int status_code,
                                  const char *reason, const char *error) {
    QueryResult result;
    char *body;
    int status;

    query_result_init(&result);
    query_result_set_error(&result, error);
    body = NULL;
    status = server_build_json_response(&result, &body);
    if (status == SUCCESS) {
        status = server_send_response(fd, status_code, reason, body);
    }
    free(body);
    query_result_free(&result);
    return status;
}

static size_t server_find_header_end(const char *buffer, size_t length) {
    size_t i;

    if (buffer == NULL || length < 4) {
        return (size_t)-1;
    }

    for (i = 3; i < length; i++) {
        if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' &&
            buffer[i - 1] == '\r' && buffer[i] == '\n') {
            return i + 1;
        }
    }

    return (size_t)-1;
}

static int server_header_name_matches(const char *line, size_t line_length,
                                      const char *name) {
    size_t name_length;
    size_t i;

    if (line == NULL || name == NULL) {
        return 0;
    }

    name_length = strlen(name);
    if (line_length <= name_length || line[name_length] != ':') {
        return 0;
    }

    for (i = 0; i < name_length; i++) {
        if (tolower((unsigned char)line[i]) !=
            tolower((unsigned char)name[i])) {
            return 0;
        }
    }

    return 1;
}

static int server_parse_content_length(const char *buffer,
                                       size_t header_end,
                                       size_t *out_length) {
    size_t line_start;
    size_t line_end;
    const char *value;
    char *parse_end;
    long parsed;

    if (buffer == NULL || out_length == NULL) {
        return FAILURE;
    }

    *out_length = 0;
    line_start = 0;
    while (line_start + 1 < header_end) {
        line_end = line_start;
        while (line_end + 1 < header_end &&
               !(buffer[line_end] == '\r' && buffer[line_end + 1] == '\n')) {
            line_end++;
        }

        if (server_header_name_matches(buffer + line_start,
                                       line_end - line_start,
                                       "content-length")) {
            value = buffer + line_start + strlen("content-length") + 1;
            while (value < buffer + line_end &&
                   isspace((unsigned char)*value)) {
                value++;
            }

            errno = 0;
            parsed = strtol(value, &parse_end, 10);
            if (errno != 0 || parse_end == value || parsed < 0) {
                return FAILURE;
            }

            *out_length = (size_t)parsed;
            return SUCCESS;
        }

        line_start = line_end + 2;
    }

    return SUCCESS;
}

static ServerReadStatus server_read_http_request(int fd,
                                                 char *buffer,
                                                 size_t buffer_size,
                                                 size_t *out_length,
                                                 size_t *out_body_offset,
                                                 size_t *out_body_length) {
    size_t length;
    size_t header_end;
    size_t content_length;
    int have_header;

    if (buffer == NULL || out_length == NULL || out_body_offset == NULL ||
        out_body_length == NULL || buffer_size == 0) {
        return SERVER_READ_BAD_REQUEST;
    }

    length = 0;
    header_end = (size_t)-1;
    content_length = 0;
    have_header = 0;

    while (length + 1 < buffer_size) {
        ssize_t received = recv(fd, buffer + length, buffer_size - length - 1, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SERVER_READ_BAD_REQUEST;
        }
        if (received == 0) {
            return SERVER_READ_BAD_REQUEST;
        }

        length += (size_t)received;
        buffer[length] = '\0';

        if (!have_header) {
            header_end = server_find_header_end(buffer, length);
            if (header_end != (size_t)-1) {
                have_header = 1;
                if (server_parse_content_length(buffer, header_end,
                                                &content_length) != SUCCESS) {
                    return SERVER_READ_BAD_REQUEST;
                }
                if (content_length > buffer_size - header_end - 1) {
                    return SERVER_READ_TOO_LARGE;
                }
            }
        }

        if (have_header && length >= header_end + content_length) {
            *out_length = length;
            *out_body_offset = header_end;
            *out_body_length = content_length;
            return SERVER_READ_OK;
        }
    }

    return SERVER_READ_TOO_LARGE;
}

static int server_parse_request_line(const char *request,
                                     char *method,
                                     size_t method_size,
                                     char *path,
                                     size_t path_size) {
    int scanned;

    if (request == NULL || method == NULL || path == NULL ||
        method_size == 0 || path_size == 0) {
        return FAILURE;
    }

    scanned = sscanf(request, "%15s %127s", method, path);
    if (scanned != 2) {
        return FAILURE;
    }

    if (strlen(method) >= method_size || strlen(path) >= path_size) {
        return FAILURE;
    }

    return SUCCESS;
}

static void server_skip_json_space(const char **cursor, const char *end) {
    while (*cursor < end && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static int server_read_json_string(const char **cursor,
                                   const char *end,
                                   char *out,
                                   size_t out_size) {
    const char *p;
    size_t length;

    if (cursor == NULL || *cursor == NULL || out == NULL || out_size == 0 ||
        *cursor >= end || **cursor != '"') {
        return FAILURE;
    }

    p = *cursor + 1;
    length = 0;
    while (p < end) {
        char ch = *p++;

        if (ch == '"') {
            out[length] = '\0';
            *cursor = p;
            return SUCCESS;
        }

        if (ch == '\\') {
            if (p >= end) {
                return FAILURE;
            }

            ch = *p++;
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'u') {
                if (end - p < 4) {
                    return FAILURE;
                }
                p += 4;
                ch = '?';
            }
        }

        if (length + 1 >= out_size) {
            return FAILURE;
        }

        out[length++] = ch;
    }

    return FAILURE;
}

static int server_extract_sql_from_json(const char *body,
                                        size_t body_length,
                                        char *out_sql,
                                        size_t out_sql_size) {
    const char *cursor;
    const char *end;
    char key[64];

    if (body == NULL || out_sql == NULL || out_sql_size == 0) {
        return FAILURE;
    }

    cursor = body;
    end = body + body_length;
    while (cursor < end) {
        while (cursor < end && *cursor != '"') {
            cursor++;
        }
        if (cursor >= end) {
            break;
        }

        if (server_read_json_string(&cursor, end, key, sizeof(key)) != SUCCESS) {
            return FAILURE;
        }
        server_skip_json_space(&cursor, end);
        if (cursor >= end || *cursor != ':') {
            continue;
        }
        cursor++;
        server_skip_json_space(&cursor, end);

        if (strcmp(key, "sql") == 0) {
            return server_read_json_string(&cursor, end, out_sql, out_sql_size);
        }
    }

    return FAILURE;
}

static int server_status_for_result(const QueryResult *result) {
    if (result == NULL || result->success) {
        return 200;
    }

    if (strncmp(result->error, "Table '", 7) == 0 &&
        strstr(result->error, "not found") != NULL) {
        return 404;
    }

    return 400;
}

static const char *server_reason_phrase(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Internal Server Error";
    }
}

static void server_handle_client(int client_fd) {
    char request_buffer[SERVER_REQUEST_BUFFER_SIZE];
    char method[16];
    char path[128];
    char sql[MAX_SQL_LENGTH];
    size_t request_length;
    size_t body_offset;
    size_t body_length;
    ServerReadStatus read_status;
    QueryResult result;
    char *body;
    int status_code;

    read_status = server_read_http_request(client_fd, request_buffer,
                                           sizeof(request_buffer),
                                           &request_length, &body_offset,
                                           &body_length);
    (void)request_length;
    if (read_status == SERVER_READ_TOO_LARGE) {
        server_send_json_error(client_fd, 413, "Payload Too Large",
                               "Request is too large.");
        return;
    }
    if (read_status != SERVER_READ_OK) {
        server_send_json_error(client_fd, 400, "Bad Request",
                               "Malformed HTTP request.");
        return;
    }

    if (server_parse_request_line(request_buffer, method, sizeof(method),
                                  path, sizeof(path)) != SUCCESS) {
        server_send_json_error(client_fd, 400, "Bad Request",
                               "Malformed request line.");
        return;
    }

    if (strcmp(method, "POST") != 0) {
        server_send_json_error(client_fd, 405, "Method Not Allowed",
                               "Only POST is supported.");
        return;
    }

    if (strcmp(path, "/query") != 0) {
        server_send_json_error(client_fd, 404, "Not Found",
                               "Only POST /query is supported.");
        return;
    }

    if (body_length == 0 ||
        server_extract_sql_from_json(request_buffer + body_offset, body_length,
                                     sql, sizeof(sql)) != SUCCESS) {
        server_send_json_error(client_fd, 400, "Bad Request",
                               "JSON body must contain a string field named sql.");
        return;
    }

    query_result_init(&result);
    engine_execute_sql(sql, &result);

    body = NULL;
    if (server_build_json_response(&result, &body) != SUCCESS) {
        query_result_free(&result);
        server_send_json_error(client_fd, 500, "Internal Server Error",
                               "Failed to build response.");
        return;
    }

    status_code = server_status_for_result(&result);
    server_send_response(client_fd, status_code,
                         server_reason_phrase(status_code), body);

    free(body);
    query_result_free(&result);
}

static void *server_worker_main(void *arg) {
    ServerRequestQueue *queue;
    ServerRequest request;

    queue = (ServerRequestQueue *)arg;
    while (server_queue_pop(queue, &request) == SUCCESS) {
        server_handle_client(request.client_fd);
        close(request.client_fd);
    }

    return NULL;
}

static int server_create_listener(int port) {
    int fd;
    int reuse;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SERVER_LISTEN_BACKLOG) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void server_install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = server_request_shutdown;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int server_run(int port) {
    ServerRequestQueue queue;
    pthread_t workers[SERVER_WORKER_COUNT];
    int worker_count;
    int listener_fd;
    int status;
    int i;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid server port.\n");
        return FAILURE;
    }

    if (server_queue_init(&queue) != SUCCESS) {
        fprintf(stderr, "Error: Failed to initialize request queue.\n");
        return FAILURE;
    }

    listener_fd = server_create_listener(port);
    if (listener_fd < 0) {
        server_queue_destroy(&queue);
        return FAILURE;
    }

    server_stop_requested = 0;
    server_listener_fd = listener_fd;
    server_install_signal_handlers();

    worker_count = 0;
    status = SUCCESS;
    for (i = 0; i < SERVER_WORKER_COUNT; i++) {
        if (pthread_create(&workers[i], NULL, server_worker_main, &queue) != 0) {
            fprintf(stderr, "Error: Failed to start worker thread.\n");
            status = FAILURE;
            break;
        }
        worker_count++;
    }

    if (status == SUCCESS) {
        printf("Server listening on http://0.0.0.0:%d\n", port);
        fflush(stdout);
    }

    while (status == SUCCESS && !server_stop_requested) {
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server_stop_requested || errno == EBADF) {
                break;
            }
            perror("accept");
            status = FAILURE;
            break;
        }

        if (server_queue_push(&queue, client_fd) != SUCCESS) {
            server_send_json_error(client_fd, 503, "Service Unavailable",
                                   "Server is busy.");
            close(client_fd);
        }
    }

    if (server_listener_fd >= 0) {
        close(server_listener_fd);
        server_listener_fd = -1;
    }

    server_queue_shutdown(&queue);
    for (i = 0; i < worker_count; i++) {
        pthread_join(workers[i], NULL);
    }
    server_queue_destroy(&queue);
    return status;
}
