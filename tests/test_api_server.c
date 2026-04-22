#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return -1;
    }
    return 0;
}

static int connect_with_retry(const char *host, int port) {
    struct sockaddr_in addr;
    int client_fd;
    int attempt;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    for (attempt = 0; attempt < 30; attempt++) {
        struct timespec delay;

        client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) {
            return -1;
        }

        if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return client_fd;
        }

        close(client_fd);
        delay.tv_sec = 0;
        delay.tv_nsec = 100000000L;
        nanosleep(&delay, NULL);
    }

    return -1;
}

static int send_request_and_read(const char *request, char *response, size_t response_size) {
    int client_fd;
    size_t length;
    ssize_t read_size;
    size_t offset;

    client_fd = connect_with_retry("127.0.0.1", 18080);
    if (client_fd < 0) {
        return -1;
    }

    length = strlen(request);
    if (send(client_fd, request, length, 0) != (ssize_t)length) {
        close(client_fd);
        return -1;
    }

    offset = 0;
    while ((read_size = recv(client_fd, response + offset, response_size - offset - 1, 0)) > 0) {
        offset += (size_t)read_size;
        if (offset + 1 >= response_size) {
            break;
        }
    }

    response[offset] = '\0';
    close(client_fd);
    return 0;
}

int main(void) {
    pid_t child_pid;
    int status;
    char request[1024];
    char response[8192];

    child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        execl("./sql_processor", "./sql_processor", "--server=18080", (char *)NULL);
        _exit(1);
    }

    snprintf(request, sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 61\r\n"
             "\r\n"
             "{\"sql\":\"INSERT INTO api_users (name, age) VALUES ('Kim', 28);\"}");
    if (assert_true(send_request_and_read(request, response, sizeof(response)) == 0,
                    "API server should accept INSERT request") != 0 ||
        assert_true(strstr(response, "200 OK") != NULL,
                    "INSERT response should return HTTP 200") != 0 ||
        assert_true(strstr(response, "1 row inserted into api_users.") != NULL,
                    "INSERT response should contain result message") != 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        return EXIT_FAILURE;
    }

    snprintf(request, sizeof(request),
             "POST /query HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 51\r\n"
             "\r\n"
             "{\"sql\":\"SELECT name FROM api_users WHERE id = 1;\"}");
    if (assert_true(send_request_and_read(request, response, sizeof(response)) == 0,
                    "API server should accept SELECT request") != 0 ||
        assert_true(strstr(response, "200 OK") != NULL,
                    "SELECT response should return HTTP 200") != 0 ||
        assert_true(strstr(response, "\"rows\":[[\"Kim\"]]") != NULL,
                    "SELECT response should include selected row") != 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        return EXIT_FAILURE;
    }

    kill(child_pid, SIGTERM);
    waitpid(child_pid, &status, 0);
    puts("[PASS] api_server");
    return WIFEXITED(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}
