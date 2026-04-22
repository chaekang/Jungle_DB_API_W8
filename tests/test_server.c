#include "server.h"
#include "utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int port;
    const char *body;
    char response[8192];
    int status;
} RequestThreadArgs;

typedef struct {
    MiniDbServer *server;
    int expected_value;
} ServerWaitArgs;

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static int open_client_connection(int port) {
    int client_fd;
    struct sockaddr_in address;
    int attempt;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    for (attempt = 0; attempt < 50; attempt++) {
        client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) {
            return FAILURE;
        }
        if (connect(client_fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            return client_fd;
        }
        close(client_fd);
        usleep(10000);
    }

    return FAILURE;
}

static int send_http_request(int port, const char *body,
                             char *response, size_t response_size) {
    int client_fd;
    char request[4096];
    size_t total;

    snprintf(request, sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             strlen(body), body);

    client_fd = open_client_connection(port);
    if (client_fd < 0) {
        return FAILURE;
    }

    if (send(client_fd, request, strlen(request), 0) < 0) {
        close(client_fd);
        return FAILURE;
    }

    total = 0;
    while (total + 1 < response_size) {
        ssize_t received = recv(client_fd, response + total,
                                response_size - total - 1, 0);
        if (received <= 0) {
            break;
        }
        total += (size_t)received;
    }
    response[total] = '\0';
    close(client_fd);
    return SUCCESS;
}

static int send_raw_request(int port, const char *request,
                            char *response, size_t response_size) {
    int client_fd;
    size_t total;

    client_fd = open_client_connection(port);
    if (client_fd < 0) {
        return FAILURE;
    }

    if (send(client_fd, request, strlen(request), 0) < 0) {
        close(client_fd);
        return FAILURE;
    }

    total = 0;
    while (total + 1 < response_size) {
        ssize_t received = recv(client_fd, response + total,
                                response_size - total - 1, 0);
        if (received <= 0) {
            break;
        }
        total += (size_t)received;
    }
    response[total] = '\0';
    close(client_fd);
    return SUCCESS;
}

static void *request_thread_main(void *arg) {
    RequestThreadArgs *args;

    args = (RequestThreadArgs *)arg;
    args->status = send_http_request(args->port, args->body,
                                     args->response, sizeof(args->response));
    return NULL;
}

static int wait_for_condition(int (*predicate)(void *), void *arg, int timeout_ms) {
    int elapsed_ms;

    for (elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms += 10) {
        if (predicate(arg)) {
            return SUCCESS;
        }
        usleep(10000);
    }

    return FAILURE;
}

static int active_workers_reached(void *arg) {
    ServerWaitArgs *wait_args;

    wait_args = (ServerWaitArgs *)arg;
    return server_get_active_worker_count(wait_args->server) >= wait_args->expected_value;
}

static int queue_depth_reached(void *arg) {
    ServerWaitArgs *wait_args;

    wait_args = (ServerWaitArgs *)arg;
    return server_get_queue_depth(wait_args->server) >= wait_args->expected_value;
}

static int test_basic_api_flow(void) {
    MiniDbServer server;
    char response[8192];

    if (server_init(&server, 0, 2, 8) != SUCCESS) {
        return FAILURE;
    }
    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"INSERT INTO api_users (name, age) VALUES ('Alice', 30);\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "insert request should succeed") != SUCCESS ||
        assert_true(strstr(response, "200 OK") != NULL,
                    "insert request should return 200") != SUCCESS ||
        assert_true(strstr(response, "\"message\":\"1 row inserted into api_users.\"") != NULL,
                    "insert response should include the engine message") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"SELECT name FROM api_users WHERE id = 1;\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "select request should succeed") != SUCCESS ||
        assert_true(strstr(response, "200 OK") != NULL,
                    "select request should return 200") != SUCCESS ||
        assert_true(strstr(response, "\"rows\":[[\"Alice\"]]") != NULL,
                    "select response should include the result row") != SUCCESS ||
        assert_true(strstr(response, "\"index_used\":true") != NULL,
                    "id equality select should report index usage") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"SELECT * FROM missing_api_users;\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "missing table request should return a response") != SUCCESS ||
        assert_true(strstr(response, "404 Not Found") != NULL,
                    "missing table request should return 404") != SUCCESS ||
        assert_true(strstr(response, "\"success\":false") != NULL,
                    "missing table request should return an error payload") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"bad\":true}",
                                      response, sizeof(response)) == SUCCESS,
                    "bad JSON shape should return a response") != SUCCESS ||
        assert_true(strstr(response, "400 Bad Request") != NULL,
                    "bad JSON shape should return 400") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}

static int count_occurrences(const char *text, const char *pattern) {
    int count;
    const char *cursor;

    count = 0;
    cursor = text;
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        count++;
        cursor += strlen(pattern);
    }

    return count;
}

static int test_http_error_mapping(void) {
    MiniDbServer server;
    char response[8192];

    if (server_init(&server, 0, 2, 8) != SUCCESS) {
        return FAILURE;
    }
    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_raw_request(server_get_port(&server),
                                     "GET /query HTTP/1.1\r\n"
                                     "Host: 127.0.0.1\r\n"
                                     "Connection: close\r\n"
                                     "\r\n",
                                     response, sizeof(response)) == SUCCESS,
                    "GET request should receive a response") != SUCCESS ||
        assert_true(strstr(response, "400 Bad Request") != NULL,
                    "GET request should be rejected with 400") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_raw_request(server_get_port(&server),
                                     "POST /unknown HTTP/1.1\r\n"
                                     "Host: 127.0.0.1\r\n"
                                     "Content-Length: 2\r\n"
                                     "Connection: close\r\n"
                                     "\r\n"
                                     "{}",
                                     response, sizeof(response)) == SUCCESS,
                    "unknown endpoint should receive a response") != SUCCESS ||
        assert_true(strstr(response, "404 Not Found") != NULL,
                    "unknown endpoint should return 404") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"INSERT INTO api_conflict (id, name, age) VALUES (5, 'Alice', 30);\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "explicit id API request should receive a response") != SUCCESS ||
        assert_true(strstr(response, "409 Conflict") != NULL,
                    "explicit id API request should map to 409") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"DELETE FROM api_conflict WHERE name = 'Alice';\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "DELETE API request should receive a response") != SUCCESS ||
        assert_true(strstr(response, "409 Conflict") != NULL,
                    "DELETE API request should map to 409") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}

static int test_parallel_insert_integrity(void) {
    MiniDbServer server;
    RequestThreadArgs request_args[6];
    pthread_t request_threads[6];
    char response[32768];
    int i;

    if (server_init(&server, 0, 4, 16) != SUCCESS) {
        return FAILURE;
    }
    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    for (i = 0; i < 6; i++) {
        char *body;

        memset(&request_args[i], 0, sizeof(request_args[i]));
        request_args[i].port = server_get_port(&server);
        body = (char *)malloc(128);
        if (body == NULL) {
            server_stop(&server);
            server_join(&server);
            server_destroy(&server);
            return FAILURE;
        }
        snprintf(body, 128,
                 "{\"sql\":\"INSERT INTO api_parallel_rows (name, age) VALUES ('worker_%d', %d);\"}",
                 i + 1, 20 + i);
        request_args[i].body = body;
        if (assert_true(pthread_create(&request_threads[i], NULL, request_thread_main,
                                       &request_args[i]) == 0,
                        "parallel insert thread should start") != SUCCESS) {
            server_stop(&server);
            server_join(&server);
            server_destroy(&server);
            return FAILURE;
        }
    }

    for (i = 0; i < 6; i++) {
        if (assert_true(pthread_join(request_threads[i], NULL) == 0,
                        "parallel insert thread should join") != SUCCESS ||
            assert_true(request_args[i].status == SUCCESS,
                        "parallel insert request should succeed") != SUCCESS ||
            assert_true(strstr(request_args[i].response, "200 OK") != NULL,
                        "parallel insert response should return 200") != SUCCESS) {
            server_stop(&server);
            server_join(&server);
            server_destroy(&server);
            return FAILURE;
        }
        free((void *)request_args[i].body);
        request_args[i].body = NULL;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"SELECT name FROM api_parallel_rows;\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "parallel insert verification select should succeed") != SUCCESS ||
        assert_true(strstr(response, "200 OK") != NULL,
                    "parallel insert verification should return 200") != SUCCESS ||
        assert_true(count_occurrences(response, "worker_") == 6,
                    "parallel inserts should all be visible in the final SELECT") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}

static int test_parallel_api_processing(void) {
    MiniDbServer server;
    int holding_client_fd;
    RequestThreadArgs request_args;
    ServerWaitArgs wait_args;
    pthread_t request_thread;

    if (server_init(&server, 0, 2, 8) != SUCCESS) {
        return FAILURE;
    }
    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    holding_client_fd = open_client_connection(server_get_port(&server));
    if (assert_true(holding_client_fd >= 0,
                    "holding client should connect to the server") != SUCCESS) {
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    wait_args.server = &server;
    wait_args.expected_value = 1;
    if (assert_true(wait_for_condition(active_workers_reached, &wait_args, 1000) == SUCCESS,
                    "parallel test should wait for blocked worker") != SUCCESS) {
        close(holding_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    memset(&request_args, 0, sizeof(request_args));
    request_args.port = server_get_port(&server);
    request_args.body =
        "{\"sql\":\"INSERT INTO parallel_users (name, age) VALUES ('Alice', 30);\"}";

    if (assert_true(pthread_create(&request_thread, NULL, request_thread_main, &request_args) == 0,
                    "parallel request thread should start") != SUCCESS) {
        close(holding_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(pthread_join(request_thread, NULL) == 0,
                    "parallel request thread should join") != SUCCESS ||
        assert_true(request_args.status == SUCCESS,
                    "parallel request should complete while another worker is blocked") != SUCCESS ||
        assert_true(strstr(request_args.response, "200 OK") != NULL,
                    "parallel request should return 200") != SUCCESS) {
        close(holding_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    close(holding_client_fd);
    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}

static int test_queue_overload(void) {
    MiniDbServer server;
    int worker_client_fd;
    int queued_client_fd;
    char response[8192];
    ServerWaitArgs wait_args;

    queued_client_fd = -1;

    if (server_init(&server, 0, 1, 1) != SUCCESS) {
        return FAILURE;
    }
    if (server_start(&server) != SUCCESS) {
        server_destroy(&server);
        return FAILURE;
    }

    worker_client_fd = open_client_connection(server_get_port(&server));
    if (assert_true(worker_client_fd >= 0,
                    "overload worker client should connect") != SUCCESS) {
        if (worker_client_fd >= 0) {
            close(worker_client_fd);
        }
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    wait_args.server = &server;
    wait_args.expected_value = 1;
    if (assert_true(wait_for_condition(active_workers_reached, &wait_args, 1000) == SUCCESS,
                    "overload test should wait for blocked worker") != SUCCESS) {
        close(worker_client_fd);
        close(queued_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    queued_client_fd = open_client_connection(server_get_port(&server));
    if (assert_true(queued_client_fd >= 0,
                    "overload queued client should connect") != SUCCESS) {
        close(worker_client_fd);
        if (queued_client_fd >= 0) {
            close(queued_client_fd);
        }
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    wait_args.expected_value = 1;
    if (assert_true(wait_for_condition(queue_depth_reached, &wait_args, 1000) == SUCCESS,
                    "overload test should wait for queued request") != SUCCESS) {
        close(worker_client_fd);
        close(queued_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    if (assert_true(send_http_request(server_get_port(&server),
                                      "{\"sql\":\"INSERT INTO overload_users (name, age) VALUES ('Bob', 31);\"}",
                                      response, sizeof(response)) == SUCCESS,
                    "overload request should receive a response") != SUCCESS ||
        assert_true(strstr(response, "503 Service Unavailable") != NULL,
                    "queue overload should return 503") != SUCCESS) {
        close(worker_client_fd);
        close(queued_client_fd);
        server_stop(&server);
        server_join(&server);
        server_destroy(&server);
        return FAILURE;
    }

    close(worker_client_fd);
    close(queued_client_fd);
    server_stop(&server);
    server_join(&server);
    server_destroy(&server);
    return SUCCESS;
}

int main(void) {
    if (assert_true(test_basic_api_flow() == SUCCESS,
                    "basic API flow should pass") != SUCCESS ||
        assert_true(test_http_error_mapping() == SUCCESS,
                    "HTTP error mapping should pass") != SUCCESS ||
        assert_true(test_parallel_api_processing() == SUCCESS,
                    "parallel API processing should pass") != SUCCESS ||
        assert_true(test_parallel_insert_integrity() == SUCCESS,
                    "parallel insert integrity should pass") != SUCCESS ||
        assert_true(test_queue_overload() == SUCCESS,
                    "queue overload handling should pass") != SUCCESS) {
        return EXIT_FAILURE;
    }

    puts("[PASS] server");
    return EXIT_SUCCESS;
}
