#include "server.h"

#include "engine.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_DEFAULT_WORKER_COUNT 8
#define SERVER_DEFAULT_QUEUE_CAPACITY 32
#define SERVER_MAX_REQUEST_SIZE 16384
#define SERVER_MAX_RESPONSE_SIZE 65536

static volatile sig_atomic_t server_signal_stop_requested = 0;

static void server_handle_signal(int signo) {
    (void)signo;
    server_signal_stop_requested = 1;
}

static int server_send_all(int fd, const char *buffer, size_t length) {
    size_t sent;
    int send_flags;

    sent = 0;
    send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags = MSG_NOSIGNAL;
#endif
    while (sent < length) {
        ssize_t written = send(fd, buffer + sent, length - sent, send_flags);
        if (written <= 0) {
            return FAILURE;
        }
        sent += (size_t)written;
    }

    return SUCCESS;
}

static int server_request_queue_init(ServerRequestQueue *queue, int capacity) {
    if (queue == NULL || capacity <= 0) {
        return FAILURE;
    }

    memset(queue, 0, sizeof(*queue));
    queue->client_fds = (int *)calloc((size_t)capacity, sizeof(int));
    if (queue->client_fds == NULL) {
        return FAILURE;
    }

    queue->capacity = capacity;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->client_fds);
        return FAILURE;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->client_fds);
        return FAILURE;
    }

    return SUCCESS;
}

static void server_request_queue_shutdown(ServerRequestQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

static void server_request_queue_destroy(ServerRequestQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->client_fds);
    queue->client_fds = NULL;
}

static int server_request_queue_try_push(ServerRequestQueue *queue, int client_fd) {
    int status;

    if (queue == NULL) {
        return FAILURE;
    }

    status = FAILURE;
    pthread_mutex_lock(&queue->mutex);
    if (!queue->shutdown && queue->count < queue->capacity) {
        queue->client_fds[queue->tail] = client_fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
        status = SUCCESS;
    }
    pthread_mutex_unlock(&queue->mutex);

    return status;
}

static int server_request_queue_pop(ServerRequestQueue *queue, int *out_client_fd) {
    if (queue == NULL || out_client_fd == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return FAILURE;
    }

    *out_client_fd = queue->client_fds[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return SUCCESS;
}

static int server_create_listen_socket(int port, int *out_actual_port) {
    int fd;
    int reuse_addr;
    struct sockaddr_in address;
    socklen_t address_length;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return FAILURE;
    }

    reuse_addr = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return FAILURE;
    }

    if (listen(fd, SERVER_DEFAULT_QUEUE_CAPACITY) != 0) {
        close(fd);
        return FAILURE;
    }

    address_length = (socklen_t)sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &address_length) != 0) {
        close(fd);
        return FAILURE;
    }

    *out_actual_port = (int)ntohs(address.sin_port);
    return fd;
}

static void server_increment_active_workers(MiniDbServer *server) {
    pthread_mutex_lock(&server->state_mutex);
    server->active_workers++;
    pthread_mutex_unlock(&server->state_mutex);
}

static void server_decrement_active_workers(MiniDbServer *server) {
    pthread_mutex_lock(&server->state_mutex);
    if (server->active_workers > 0) {
        server->active_workers--;
    }
    pthread_mutex_unlock(&server->state_mutex);
}

static int server_append_text(char **buffer, size_t *length, size_t *capacity,
                              const char *text) {
    if (utils_append_buffer(buffer, length, capacity, text) != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

static int server_append_json_escaped(char **buffer, size_t *length, size_t *capacity,
                                      const char *text) {
    size_t i;
    char escaped[8];

    if (server_append_text(buffer, length, capacity, "\"") != SUCCESS) {
        return FAILURE;
    }

    for (i = 0; text != NULL && text[i] != '\0'; i++) {
        switch (text[i]) {
            case '\\':
                if (server_append_text(buffer, length, capacity, "\\\\") != SUCCESS) {
                    return FAILURE;
                }
                break;
            case '"':
                if (server_append_text(buffer, length, capacity, "\\\"") != SUCCESS) {
                    return FAILURE;
                }
                break;
            case '\n':
                if (server_append_text(buffer, length, capacity, "\\n") != SUCCESS) {
                    return FAILURE;
                }
                break;
            case '\r':
                if (server_append_text(buffer, length, capacity, "\\r") != SUCCESS) {
                    return FAILURE;
                }
                break;
            case '\t':
                if (server_append_text(buffer, length, capacity, "\\t") != SUCCESS) {
                    return FAILURE;
                }
                break;
            default:
                escaped[0] = text[i];
                escaped[1] = '\0';
                if (server_append_text(buffer, length, capacity, escaped) != SUCCESS) {
                    return FAILURE;
                }
                break;
        }
    }

    return server_append_text(buffer, length, capacity, "\"");
}

static int server_build_query_result_json(const QueryResult *result, char **out_body) {
    char *body;
    size_t length;
    size_t capacity;
    int i;
    int j;

    if (result == NULL || out_body == NULL) {
        return FAILURE;
    }

    body = NULL;
    length = 0;
    capacity = 0;

    if (result->success) {
        if (result->kind == QUERY_RESULT_MESSAGE) {
            if (server_append_text(&body, &length, &capacity,
                                   "{\"success\":true,\"message\":") != SUCCESS ||
                server_append_json_escaped(&body, &length, &capacity,
                                           result->message) != SUCCESS ||
                server_append_text(&body, &length, &capacity, "}") != SUCCESS) {
                free(body);
                return FAILURE;
            }
        } else {
            if (server_append_text(&body, &length, &capacity,
                                   "{\"success\":true,\"columns\":[") != SUCCESS) {
                free(body);
                return FAILURE;
            }
            for (i = 0; i < result->column_count; i++) {
                if (i > 0 &&
                    server_append_text(&body, &length, &capacity, ",") != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
                if (server_append_json_escaped(&body, &length, &capacity,
                                               result->columns[i]) != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
            }

            if (server_append_text(&body, &length, &capacity,
                                   "],\"rows\":[") != SUCCESS) {
                free(body);
                return FAILURE;
            }
            for (i = 0; i < result->row_count; i++) {
                if (i > 0 &&
                    server_append_text(&body, &length, &capacity, ",") != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
                if (server_append_text(&body, &length, &capacity, "[") != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
                for (j = 0; j < result->column_count; j++) {
                    if (j > 0 &&
                        server_append_text(&body, &length, &capacity, ",") != SUCCESS) {
                        free(body);
                        return FAILURE;
                    }
                    if (server_append_json_escaped(&body, &length, &capacity,
                                                   result->rows[i][j]) != SUCCESS) {
                        free(body);
                        return FAILURE;
                    }
                }
                if (server_append_text(&body, &length, &capacity, "]") != SUCCESS) {
                    free(body);
                    return FAILURE;
                }
            }

            if (server_append_text(&body, &length, &capacity,
                                   "],\"index_used\":") != SUCCESS ||
                server_append_text(&body, &length, &capacity,
                                   result->index_used ? "true}" : "false}") != SUCCESS) {
                free(body);
                return FAILURE;
            }
        }
    } else {
        if (server_append_text(&body, &length, &capacity,
                               "{\"success\":false,\"error\":") != SUCCESS ||
            server_append_json_escaped(&body, &length, &capacity,
                                       result->error[0] == '\0'
                                           ? "Internal server error."
                                           : result->error) != SUCCESS ||
            server_append_text(&body, &length, &capacity, "}") != SUCCESS) {
            free(body);
            return FAILURE;
        }
    }

    *out_body = body;
    return SUCCESS;
}

static int server_send_json_response(int client_fd, int status_code,
                                     const char *reason, const char *body) {
    char header[256];
    size_t body_length;

    body_length = strlen(body);
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status_code, reason, body_length);

    if (server_send_all(client_fd, header, strlen(header)) != SUCCESS ||
        server_send_all(client_fd, body, body_length) != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

static int server_send_error_response(int client_fd, int status_code,
                                      const char *reason, const char *message) {
    char *body;
    size_t length;
    size_t capacity;

    body = NULL;
    length = 0;
    capacity = 0;
    if (server_append_text(&body, &length, &capacity,
                           "{\"success\":false,\"error\":") != SUCCESS ||
        server_append_json_escaped(&body, &length, &capacity, message) != SUCCESS ||
        server_append_text(&body, &length, &capacity, "}") != SUCCESS) {
        free(body);
        return FAILURE;
    }

    server_send_json_response(client_fd, status_code, reason, body);
    free(body);
    return SUCCESS;
}

static char *server_find_header_end(char *buffer) {
    return strstr(buffer, "\r\n\r\n");
}

static int server_parse_content_length(const char *headers, size_t *out_length) {
    const char *cursor;

    cursor = headers;
    while (cursor != NULL && *cursor != '\0') {
        const char *line_end = strstr(cursor, "\r\n");
        size_t line_length;

        if (line_end == NULL) {
            break;
        }
        line_length = (size_t)(line_end - cursor);
        if (line_length >= 15 &&
            strncasecmp(cursor, "Content-Length:", 15) == 0) {
            const char *value = cursor + 15;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            *out_length = (size_t)strtoul(value, NULL, 10);
            return SUCCESS;
        }
        cursor = line_end + 2;
        if (cursor[0] == '\r' && cursor[1] == '\n') {
            break;
        }
    }

    return FAILURE;
}

static int server_read_request(int client_fd, char *buffer, size_t buffer_size,
                               size_t *out_length) {
    size_t total;
    size_t expected_total;
    size_t content_length;
    char *header_end;

    total = 0;
    expected_total = 0;
    content_length = 0;
    buffer[0] = '\0';

    while (total + 1 < buffer_size) {
        ssize_t received = recv(client_fd, buffer + total, buffer_size - total - 1, 0);
        if (received <= 0) {
            return FAILURE;
        }

        total += (size_t)received;
        buffer[total] = '\0';
        header_end = server_find_header_end(buffer);
        if (header_end != NULL && expected_total == 0) {
            size_t header_length = (size_t)(header_end - buffer) + 4;
            if (server_parse_content_length(buffer, &content_length) != SUCCESS) {
                return FAILURE;
            }
            expected_total = header_length + content_length;
            if (expected_total >= buffer_size) {
                return FAILURE;
            }
        }

        if (expected_total != 0 && total >= expected_total) {
            *out_length = total;
            return SUCCESS;
        }
    }

    return FAILURE;
}

static int server_extract_json_string(const char *body, const char *field_name,
                                      char *out_value, size_t out_size) {
    char needle[64];
    const char *cursor;
    size_t written;

    snprintf(needle, sizeof(needle), "\"%s\"", field_name);
    cursor = strstr(body, needle);
    if (cursor == NULL) {
        return FAILURE;
    }

    cursor += strlen(needle);
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    if (*cursor != ':') {
        return FAILURE;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }
    if (*cursor != '"') {
        return FAILURE;
    }
    cursor++;

    written = 0;
    while (*cursor != '\0' && *cursor != '"') {
        char ch;

        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0') {
                return FAILURE;
            }
            switch (*cursor) {
                case '"':
                    ch = '"';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    return FAILURE;
            }
        } else {
            ch = *cursor;
        }

        if (written + 1 >= out_size) {
            return FAILURE;
        }
        out_value[written++] = ch;
        cursor++;
    }

    if (*cursor != '"') {
        return FAILURE;
    }

    out_value[written] = '\0';
    return SUCCESS;
}

static int server_status_from_result(const QueryResult *result) {
    if (result == NULL || result->success) {
        return 200;
    }

    if (strstr(result->error, "not found") != NULL) {
        return 404;
    }
    if (strstr(result->error, "Unexpected") != NULL ||
        strstr(result->error, "Expected") != NULL ||
        strstr(result->error, "Unsupported SQL") != NULL ||
        strstr(result->error, "Unterminated") != NULL ||
        strstr(result->error, "Empty SQL") != NULL) {
        return 400;
    }
    if (strstr(result->error, "not supported") != NULL ||
        strstr(result->error, "schema") != NULL ||
        strstr(result->error, "Explicit id") != NULL ||
        strstr(result->error, "doesn't match") != NULL ||
        strstr(result->error, "Column '") != NULL) {
        return 409;
    }

    return 500;
}

static const char *server_reason_phrase(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 409:
            return "Conflict";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "OK";
    }
}

static void server_handle_client(int client_fd) {
    char request[SERVER_MAX_REQUEST_SIZE];
    size_t request_length;
    char *header_end;
    char *body;
    char method[16];
    char path[128];
    char version[16];
    char sql[MAX_SQL_LENGTH];
    QueryResult result;
    char *response_body;
    int status_code;

    if (server_read_request(client_fd, request, sizeof(request), &request_length) != SUCCESS) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Malformed HTTP request.");
        return;
    }

    (void)request_length;
    header_end = server_find_header_end(request);
    if (header_end == NULL) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Malformed HTTP request.");
        return;
    }

    if (sscanf(request, "%15s %127s %15s", method, path, version) != 3) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Malformed request line.");
        return;
    }

    if (strcmp(method, "POST") != 0) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "Only POST is supported.");
        return;
    }
    if (strcmp(path, "/query") != 0) {
        server_send_error_response(client_fd, 404, "Not Found",
                                   "Unknown endpoint.");
        return;
    }

    body = header_end + 4;
    if (server_extract_json_string(body, "sql", sql, sizeof(sql)) != SUCCESS) {
        server_send_error_response(client_fd, 400, "Bad Request",
                                   "JSON body must contain a string field named 'sql'.");
        return;
    }

    query_result_init(&result);
    engine_execute_sql(sql, &result);

    response_body = NULL;
    if (server_build_query_result_json(&result, &response_body) != SUCCESS) {
        query_result_free(&result);
        server_send_error_response(client_fd, 500, "Internal Server Error",
                                   "Failed to build JSON response.");
        return;
    }

    status_code = server_status_from_result(&result);
    server_send_json_response(client_fd, status_code,
                              server_reason_phrase(status_code), response_body);
    free(response_body);
    query_result_free(&result);
}

static void *server_worker_main(void *arg) {
    MiniDbServer *server;
    int client_fd;

    server = (MiniDbServer *)arg;
    while (server_request_queue_pop(&server->queue, &client_fd) == SUCCESS) {
        server_increment_active_workers(server);
        server_handle_client(client_fd);
        server_decrement_active_workers(server);
        close(client_fd);
    }

    return NULL;
}

static void *server_accept_main(void *arg) {
    MiniDbServer *server;
    int listen_fd;

    server = (MiniDbServer *)arg;
    listen_fd = server->listen_fd;
    while (!server->stop_requested) {
        struct pollfd listen_pollfd;
        int poll_status;
        int client_fd;

        memset(&listen_pollfd, 0, sizeof(listen_pollfd));
        listen_pollfd.fd = listen_fd;
        listen_pollfd.events = POLLIN;

        poll_status = poll(&listen_pollfd, 1, 100);
        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_status == 0) {
            continue;
        }
        if ((listen_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if ((listen_pollfd.revents & POLLIN) == 0) {
            continue;
        }

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (server->stop_requested || errno == EBADF || errno == EINTR) {
                break;
            }
            continue;
        }

        if (server_request_queue_try_push(&server->queue, client_fd) != SUCCESS) {
            server_send_error_response(client_fd, 503, "Service Unavailable",
                                       "Server is overloaded.");
            close(client_fd);
        }
    }

    return NULL;
}

int server_init(MiniDbServer *server, int port, int worker_count, int queue_capacity) {
    if (server == NULL) {
        return FAILURE;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->configured_port = port;
    server->actual_port = 0;
    server->worker_count = worker_count > 0 ? worker_count : SERVER_DEFAULT_WORKER_COUNT;
    server->queue_capacity = queue_capacity > 0
                                 ? queue_capacity
                                 : SERVER_DEFAULT_QUEUE_CAPACITY;

    server->workers =
        (pthread_t *)calloc((size_t)server->worker_count, sizeof(pthread_t));
    if (server->workers == NULL) {
        return FAILURE;
    }

    if (server_request_queue_init(&server->queue, server->queue_capacity) != SUCCESS) {
        free(server->workers);
        server->workers = NULL;
        return FAILURE;
    }
    if (pthread_mutex_init(&server->state_mutex, NULL) != 0) {
        server_request_queue_destroy(&server->queue);
        free(server->workers);
        server->workers = NULL;
        return FAILURE;
    }

    return SUCCESS;
}

int server_start(MiniDbServer *server) {
    int i;

    if (server == NULL) {
        return FAILURE;
    }

    server->listen_fd = server_create_listen_socket(server->configured_port,
                                                    &server->actual_port);
    if (server->listen_fd < 0) {
        return FAILURE;
    }

    for (i = 0; i < server->worker_count; i++) {
        if (pthread_create(&server->workers[i], NULL, server_worker_main, server) != 0) {
            server_stop(server);
            server_join(server);
            return FAILURE;
        }
    }

    if (pthread_create(&server->accept_thread, NULL, server_accept_main, server) != 0) {
        server_stop(server);
        server_join(server);
        return FAILURE;
    }

    return SUCCESS;
}

void server_stop(MiniDbServer *server) {
    if (server == NULL) {
        return;
    }

    server->stop_requested = 1;
    server_request_queue_shutdown(&server->queue);
}

void server_join(MiniDbServer *server) {
    int i;

    if (server == NULL) {
        return;
    }

    if (server->accept_thread) {
        pthread_join(server->accept_thread, NULL);
        server->accept_thread = 0;
    }

    for (i = 0; i < server->worker_count; i++) {
        if (server->workers != NULL && server->workers[i]) {
            pthread_join(server->workers[i], NULL);
            server->workers[i] = 0;
        }
    }
}

void server_destroy(MiniDbServer *server) {
    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    server_request_queue_destroy(&server->queue);
    pthread_mutex_destroy(&server->state_mutex);
    free(server->workers);
    server->workers = NULL;
}

int server_get_port(const MiniDbServer *server) {
    if (server == NULL) {
        return FAILURE;
    }

    return server->actual_port;
}

int server_get_queue_depth(const MiniDbServer *server) {
    int count;

    if (server == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock((pthread_mutex_t *)&server->queue.mutex);
    count = server->queue.count;
    pthread_mutex_unlock((pthread_mutex_t *)&server->queue.mutex);
    return count;
}

int server_get_active_worker_count(const MiniDbServer *server) {
    int count;

    if (server == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock((pthread_mutex_t *)&server->state_mutex);
    count = server->active_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&server->state_mutex);
    return count;
}

int server_run_forever(int port, int worker_count, int queue_capacity) {
    MiniDbServer server;
    struct sigaction action;

    server_signal_stop_requested = 0;
    if (server_init(&server, port, worker_count, queue_capacity) != SUCCESS) {
        return FAILURE;
    }

    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = server_handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    printf("Server listening on port %d\n", server_get_port(&server));
    fflush(stdout);

    while (!server_signal_stop_requested) {
        pause();
    }

    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}
