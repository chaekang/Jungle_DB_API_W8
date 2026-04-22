#ifndef SERVER_QUEUE_H
#define SERVER_QUEUE_H

#include "utils.h"

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
} ServerQueue;

int server_queue_init(ServerQueue *queue, int capacity);
void server_queue_destroy(ServerQueue *queue);
int server_queue_push(ServerQueue *queue, int client_fd);
int server_queue_pop(ServerQueue *queue, int *out_client_fd);
void server_queue_shutdown(ServerQueue *queue);
int server_queue_current_count(ServerQueue *queue);

#endif
