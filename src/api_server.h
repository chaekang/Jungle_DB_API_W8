#ifndef API_SERVER_H
#define API_SERVER_H

#define API_SERVER_DEFAULT_PORT 8080
#define API_SERVER_WORKER_COUNT 8
#define API_SERVER_QUEUE_CAPACITY 32

int api_server_run(int port);

#endif
