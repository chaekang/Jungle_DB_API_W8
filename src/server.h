#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>

typedef struct {
    int *client_fds;
    int capacity;
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} ServerRequestQueue;

typedef struct {
    int listen_fd;
    int configured_port;
    int actual_port;
    int worker_count;
    int queue_capacity;
    int active_workers;
    volatile int stop_requested;
    pthread_t accept_thread;
    pthread_t *workers;
    ServerRequestQueue queue;
    pthread_mutex_t state_mutex;
} MiniDbServer;

int server_init(MiniDbServer *server, int port, int worker_count, int queue_capacity);
int server_start(MiniDbServer *server);
void server_stop(MiniDbServer *server);
void server_join(MiniDbServer *server);
void server_destroy(MiniDbServer *server);
int server_get_port(const MiniDbServer *server);
int server_get_queue_depth(const MiniDbServer *server);
int server_get_active_worker_count(const MiniDbServer *server);
int server_run_forever(int port, int worker_count, int queue_capacity);

#endif
