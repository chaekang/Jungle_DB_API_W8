#include "server_queue.h"

#include <stdlib.h>

int server_queue_init(ServerQueue *queue, int capacity) {
    if (queue == NULL || capacity <= 0) {
        return FAILURE;
    }

    queue->client_fds = (int *)calloc((size_t)capacity, sizeof(int));
    if (queue->client_fds == NULL) {
        return FAILURE;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->shutdown = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    return SUCCESS;
}

void server_queue_destroy(ServerQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->client_fds);
    queue->client_fds = NULL;
}

int server_queue_push(ServerQueue *queue, int client_fd) {
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

int server_queue_pop(ServerQueue *queue, int *out_client_fd) {
    if (queue == NULL || out_client_fd == NULL) {
        return FAILURE;
    }

    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutdown && queue->count == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return FAILURE;
    }

    *out_client_fd = queue->client_fds[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return SUCCESS;
}

void server_queue_shutdown(ServerQueue *queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

int server_queue_current_count(ServerQueue *queue) {
    int count;

    if (queue == NULL) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    count = queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return count;
}
