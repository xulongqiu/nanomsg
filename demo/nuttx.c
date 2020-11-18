/*
    Copyright 2016 Garrett D'Amore <garrett@damore.org>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.

    "nanomsg" is a trademark of Martin Sustrik
*/

/*  This program serves as an example for how to write a threaded RPC service,
    using the RAW request/reply pattern and pthreads.  Multiple worker threads
    are spawned on a single socket, and each worker processes jobs in order.

    Our demonstration application layer protocol is simple.  The client sends
    a number of milliseconds to wait before responding.  The server just gives
    back an empty reply after waiting that long.

    To run this program, start the server as pthread_demo <url> -s
    Then connect to it with the client as pthread_client <url> <msec>.

    For example:

    % ./pthread_demo tcp://127.0.0.1:5555 -s &
    % ./pthread_demo tcp://127.0.0.1:5555 323
    Request took 324 milliseconds.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <stdbool.h>
/*  SERVER_WORKERS_MAX is a limit on the on the number of workers we will fire
    off.  Since each worker processes jobs sequentially, this is a limit
    on the concurrency of the server.  New inbound messages will queue up
    waiting for a worker to receive them. */

#define SERVER_WORKERS_MAX 5
#define CLIENT_WORKERS_MAX 10
#define CLIENT_SEND_COUND_MAX 50

/*  Return the UNIX time in milliseconds.  You'll need a working
    gettimeofday(), so this won't work on Windows.  */
uint64_t milliseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((uint64_t)tv.tv_sec * 1000) + ((uint64_t)tv.tv_usec / 1000));
}

typedef struct {
    char name[32];
    int seq;
} reqrep_msg_t;

typedef struct {
    int id;
    pthread_t tid;
    const char* url;
    reqrep_msg_t msg;
} client_ctx_t;

typedef struct {
    int id;
    pthread_t tid;
    int fd;
} server_ctx_t;

client_ctx_t client_ctx[CLIENT_WORKERS_MAX];
server_ctx_t server_ctx[SERVER_WORKERS_MAX];

typedef enum {
    CREATE = 0,
    SET_DATA_SOURCE,
    PREPARE,
    START,
    PAUSE,
    STOP,
    RELEASE
} media_trans_type_t;

int server_handle(int msg, void* in, int in_len, void* out, int out_len)
{
    int ret = 0;

    switch (msg) {
    case CREATE:
        fprintf(stdout, "media_server.create.name=%s\n", (char*)in);
        break;

    case SET_DATA_SOURCE:
        fprintf(stdout, "media_server.set_data_source.url=%s\n", (char*)in);
        break;

    case PREPARE:
        fprintf(stdout, "media_server.prepare\n");
        break;

    case START:
        fprintf(stdout, "media_server.start\n");
        break;

    case PAUSE:
        fprintf(stdout, "media_server.pause\n");
        break;

    case STOP:
        fprintf(stdout, "media_server.stop\n");
        break;

    case RELEASE:
        fprintf(stdout, "media_server.release\n");
        break;

    default:
        break;
    }

    return ret;
}

void* server_worker(void* arg)
{
    server_ctx_t* ctx = (server_ctx_t*)arg;
    int fd = ctx->fd;
    char name[32]  = {0};

    pthread_detach(pthread_self());
    snprintf(name, sizeof(name), "server-%d", ctx->id);
    prctl(PR_SET_NAME, name, NULL, NULL, NULL);
    /*  Main processing loop. */

    for (;;) {
        reqrep_msg_t msg;
        int rc;
        int timeout;
        uint8_t* body;
        void* control;
        struct nn_iovec iov;
        struct nn_msghdr hdr;

        memset(&msg, 0, sizeof(msg));
        memset(&hdr, 0, sizeof(hdr));
        control = NULL;
        iov.iov_base = &body;
        iov.iov_len = NN_MSG;
        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;
        hdr.msg_control = &control;
        hdr.msg_controllen = NN_MSG;

        rc = nn_recvmsg(fd, &hdr, 0);

        if (rc < 0) {
            if (nn_errno() == EBADF) {
                return (NULL);   /* Socket closed by another thread. */
            }

            /*  Any error here is unexpected. */
            fprintf(stderr, "server_recv: %s\n", nn_strerror(nn_errno()));
            break;
        }

        memcpy(&msg, body, sizeof(msg));

        if (rc != sizeof(reqrep_msg_t)) {
            fprintf(stderr, "server_recv: wanted %d, but got %d\n",
                    (int) sizeof(uint32_t), rc);
            nn_freemsg(body);
            nn_freemsg(control);
            continue;
        } else {
            msg.seq = server_handle(msg.seq, msg.name, strlen(msg.name), msg.name, sizeof(msg.name));
            //fprintf(stderr, "server_recv:msg.name=%s, seq=%d\n", msg.name, msg.seq);
        }

        nn_freemsg(body);

        iov.iov_base = &msg;
        iov.iov_len = sizeof(msg);
        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;

        rc = nn_sendmsg(fd, &hdr, 0);

        if (rc < 0) {
            fprintf(stderr, "server_send: %s\n", nn_strerror(nn_errno()));
            nn_freemsg(control);
        }
    }

    return (NULL);
}

/*  The server runs forever. */
int server(const char* url)
{
    int fd;
    int i;
    int rc;

    /*  Create the socket. */
    fd = nn_socket(AF_SP_RAW, NN_REP);

    if (fd < 0) {
        fprintf(stderr, "nn_socket: %s\n", nn_strerror(nn_errno()));
        return (-1);
    }

    /*  Bind to the URL.  This will bind to the address and listen
        synchronously; new clients will be accepted asynchronously
        without further action from the calling program. */

    if (nn_bind(fd, url) < 0) {
        fprintf(stderr, "nn_bind: %s\n", nn_strerror(nn_errno()));
        nn_close(fd);
        return (-1);
    }

    /*  Start up the threads. */
    for (i = 0; i < sizeof(server_ctx) / sizeof(server_ctx[0]); i++) {
        server_ctx[i].id = i;
        server_ctx[i].fd = fd;
        rc = pthread_create(&server_ctx[i].tid, NULL, server_worker, (void*)(&server_ctx[i]));

        if (rc < 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            nn_close(fd);
            break;
        }
    }

    return 0;
}

int nn_reqres(int fd, int op_type, void* in, int in_len, void* out, int out_len)
{
    int rc;
    reqrep_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.seq = op_type;
    if (in != NULL && in_len > 0) {
        memcpy(msg.name, in, in_len > sizeof(msg.name) ? sizeof(msg.name) : in_len);
    }
    rc = nn_send(fd, &msg, sizeof(msg), 0);

    if (rc < 0) {
        fprintf(stderr, "client_send: %s\n", nn_strerror(nn_errno()));
        return -1;
    } else {
        //fprintf(stderr, "client_send: bytes=%lu, actual=%d\n", sizeof(msg), rc);
    }

    rc = nn_recv(fd, &msg, sizeof(msg), 0);

    if (rc < 0) {
        fprintf(stderr, "client_recv: %s\n", nn_strerror(nn_errno()));
        return -1;
    }

    rc = msg.seq;

    if (out != NULL && out_len > 0) {
        memcpy(out, msg.name, out_len > sizeof(msg.name) ? sizeof(msg.name) : out_len);
    }

    return rc;
}

void* client_worker(void* arg)
{
    int fd;
    int rc;
    uint64_t start;
    uint64_t end;
    client_ctx_t* ctx = (client_ctx_t*)arg;
    reqrep_msg_t* msg = &(ctx->msg);
    const char* url = ctx->url;

    pthread_detach(pthread_self());
    prctl(PR_SET_NAME, msg->name, NULL, NULL, NULL);

    msg->seq = 0;
    fd = nn_socket(AF_SP, NN_REQ);

    if (fd < 0) {
        fprintf(stderr, "nn_socket: %s\n", nn_strerror(nn_errno()));
        return NULL;
    }

    if (nn_connect(fd, url) < 0) {
        fprintf(stderr, "nn_socket: %s\n", nn_strerror(nn_errno()));
        nn_close(fd);
        return NULL;
    }

    while (1) {
        start = milliseconds();
        rc = nn_reqres(fd, CREATE, msg->name, strlen(msg->name), NULL, 0);
        if (rc != 0) {
            fprintf(stdout, "player.name=%s, create.error=%d\n", msg->name, rc);
            continue;
        }
        rc = nn_reqres(fd, SET_DATA_SOURCE, "http://253.mp3", strlen("http://253.mp3"), NULL, 0);
        if (rc != 0) {
            fprintf(stdout, "player.name=%s, set_data_source.error=%d\n", msg->name, rc);
            continue;
        }
        rc = nn_reqres(fd, PREPARE, NULL, 0, NULL, 0);
        if (rc != 0) {
            fprintf(stdout, "player.name=%s, prepare.error=%d\n", msg->name, rc);
        }
        rc = nn_reqres(fd, START, NULL, 0, NULL, 0);
        if (rc != 0) {
            fprintf(stdout, "player.name=%s, start.error=%d\n", msg->name, rc);
        }
        end = milliseconds();
        fprintf(stderr, "player.success=%s, use_ms=%lu\n", msg->name, end - start);
        break;
    }

    nn_close(fd);

    return NULL;
}

int client(const char* url, const char* name)
{
    int i;
    int ret;

    for (i = 0; i < sizeof(client_ctx) / sizeof(client_ctx[0]); i++) {
        snprintf(client_ctx[i].msg.name, sizeof(client_ctx[i].msg.name), "%s-%d", name, i);
        client_ctx[i].id = i;
        client_ctx[i].url = url;
        ret = pthread_create(&client_ctx[i].tid, NULL, client_worker, (void*)&client_ctx[i]);

        if (ret < 0) {
            fprintf(stderr, "client.pthread_create.fail.ret=%d\n", ret);
            break;
        }
    }

    return ret;
}


int main(int argc, char** argv)
{
    int rc;
    bool inproc = false;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <url> [-s|name]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    inproc = (strstr(argv[1], "inproc") != NULL);
    if (inproc) {
        rc = server(argv[1]);
        if (rc != 0) {
            fprintf(stderr, "server start failed, ret=%d\n", rc);
        } else {
            rc = client(argv[1], argv[2]);
            if (rc != 0) {
                fprintf(stderr, "client start failed, ret=%d\n", rc);
            }
        }
    } else {
        if (strcmp(argv[2], "-s") == 0) {
            rc = server(argv[1]);
        } else {
            rc = client(argv[1], argv[2]);
        }
    }

    for (int i = 0; i < sizeof(client_ctx) / sizeof(client_ctx[0]); i++) {
        pthread_join(client_ctx[i].tid, NULL);
    }

    nn_close(server_ctx[0].fd);
    for (int j = 0; j < sizeof(server_ctx) / sizeof(server_ctx[j]); j++) {
        pthread_join(server_ctx[j].tid, NULL);
    }

    exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
