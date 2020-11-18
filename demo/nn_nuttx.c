/*
 * Copyright (c) 2020 xiaomi.
 *
 * Unpublished copyright. All rights reserved. This material contains
 * proprietary information that should be used or copied only within
 * xiaomi, except with written permission of xiaomi.
 *
 * @file:    nn_nuttx.c
 * @brief:
 * @author:  xulongqiu@xiaomi.com
 * @version: 1.0
 * @date:    2020-11-18 13:21:27
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

typedef struct nn_trans_hdr {
    int seq;
    int op_code;
    size_t len;
    unsigned char* data[];
} nn_trans_hdr_t;

typedef struct nn_trans_data {
    const void* in;
    size_t      in_size;
    void*       out;
    size_t      out_size;
} nn_trans_data_t;

typedef int (*on_transaction)(const void* cookie, const int code, nn_trans_data_t* data);

typedef enum {
    NN_NUTTX_MODE_REQREP = 0,
    NN_NUTTX_MODE_PIPELINE,
    NN_NUTTX_MODE_PAIR,

    NN_NUTTX_MODE_MAX
} nn_nuttx_mode_t;

typedef enum {
    NN_NUTTX_PROTOCOL_INPROC = 0,
    NN_NUTTX_PROTOCOL_IPC,
    NN_NUTTX_PROTOCOL_TCP,

    NN_NUTTX_PROTOCOL_MAX
} nn_nuttx_protocol_t;

typedef struct nn_server_ctx {
    nn_nuttx_mode_t mode;
    nn_nuttx_mode_t protocol;
    char* name;
    int fd;
    on_transaction on_trans_cb;
    pthread_t tid;
    bool actived;
    void* priv;
} nn_server_ctx_t;

typedef struct nn_client_ctx {
    char* name;
    int fd;
} nn_client_ctx_t;

const char* const nn_nuttx_protocol_str[] = {
    "inproc://", "ipc://", "tcp://"
};

static void* nn_server_worker(void* arg)
{
    nn_trans_hdr_t* trans_hdr;
    nn_trans_data_t trans_data;
    nn_server_ctx_t* ctx = (nn_server_ctx_t*)arg;
    pthread_detach(pthread_self());

    if (NULL == ctx) {
        return NULL;
    }

    prctl(PR_SET_NAME, strstr(ctx->name, "//") + 2, NULL, NULL, NULL);

    while (1) {
        int rc;
        uint8_t* body;
        void* control = NULL;
        struct nn_iovec iov;
        struct nn_msghdr hdr;

        memset(&hdr, 0, sizeof(hdr));
        iov.iov_base = &body;
        iov.iov_len = NN_MSG;
        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;
        hdr.msg_control = &control;
        hdr.msg_controllen = NN_MSG;
        rc = nn_recvmsg(ctx->fd, &hdr, 0);

        if (rc < 0) {
            fprintf(stderr, "%s: %s\n", __func__, nn_strerror(nn_errno()));

            if (nn_errno() == EBADF) {
                break;   /* Socket closed by another thread. */
            } else {
                //TODO timeout or others
                break;
            }
        }

        trans_hdr = (nn_trans_hdr_t*)body;

        if (trans_hdr->len + sizeof(nn_trans_hdr_t) != rc) {
            nn_freemsg(body);
            nn_freemsg(control);
            continue;
        } else {
            if (ctx->on_trans_cb != NULL) {
                trans_data.in = (nn_trans_data_t*)(body + sizeof(nn_trans_hdr_t));
                trans_data.in_size = trans_hdr->len;
                trans_hdr->op_code = ctx->on_trans_cb(ctx->priv, trans_hdr->op_code, &trans_data);
            } else {
                trans_hdr->op_code = 0;
            }

            trans_hdr->len = 0;
        }

        // send response
        {
            nn_trans_hdr_t* send_hdr = trans_hdr;
            void* send_data = NULL;

            if (trans_data.out_size > 0 && trans_data.out != NULL) {
                send_data = calloc(1, sizeof(nn_trans_hdr_t) + trans_data.out_size);

                if (send_data == NULL) {
                    send_hdr->op_code = -ENOMEM;
                    send_hdr->len = 0;
                } else {
                    send_hdr = (nn_trans_hdr_t*)send_data;
                    send_hdr->seq = trans_hdr->seq;
                    send_hdr->op_code = trans_hdr->op_code;
                    send_hdr->len = trans_data.out_size;
                    memcpy(send_data + sizeof(nn_trans_hdr_t), trans_data.out, trans_data.out_size);
                }
            }

            iov.iov_base = send_hdr;
            iov.iov_len = sizeof(nn_trans_hdr_t) + send_hdr->len;
            hdr.msg_iov = &iov;
            hdr.msg_iovlen = 1;
            rc = nn_sendmsg(ctx->fd, &hdr, 0);

            if (rc < 0) {
                fprintf(stderr, "%s: %s\n", __func__, nn_strerror(nn_errno()));
                nn_freemsg(control);
            }

            if (trans_data.out != NULL) {
                free(trans_data.out);
                trans_data.out = NULL;
                trans_data.out_size = 0;
            }

            if (send_data != NULL) {
                free(send_data);
            }
        }
        nn_freemsg(body);
    }

    return NULL;
}

void* nn_server_create(const char* name, nn_nuttx_mode_t mode, nn_nuttx_protocol_t protocol)
{
    nn_server_ctx_t* ctx = NULL;
    int name_len = strlen(name);
    int ret = 0;

    if (name_len <= 0
        || mode < 0 || mode > NN_NUTTX_MODE_MAX
        || protocol < 0 || protocol > NN_NUTTX_PROTOCOL_MAX) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->fd = -1;
    ctx->name = calloc(1, strlen(nn_nuttx_protocol_str[protocol] + name_len + 1));

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_protocol_str[protocol]);
    strcat(ctx->name, name);

    ctx->fd = nn_socket(AF_SP_RAW, NN_REP);

    if (ctx->fd < 0) {
        ret = nn_errno();
        goto err;
    }

    /*  Bind to the URL.  This will bind to the address and listen
        synchronously; new clients will be accepted asynchronously
        without further action from the calling program. */

    if (nn_bind(ctx->fd, ctx->name) < 0) {
        ret = nn_errno();
        goto err;
    }

    ctx->actived = true;
    ret = pthread_create(&ctx->tid, NULL, nn_server_worker, (void*)ctx);

    if (0 == ret) {
        return ctx;
    }

err:
    fprintf(stderr, "%s.ret=%d\n", __func__, ret);

    if (ctx != NULL) {
        if (ctx->fd != 0) {
            nn_close(ctx->fd);
        }

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

void nn_server_set_transaction_cb(void* nn_server_ctx, void* cb)
{
    if (nn_server_ctx != NULL && cb != NULL) {
        nn_server_ctx_t* ctx = (nn_server_ctx_t*)nn_server_ctx;
        ctx->on_trans_cb = cb;
    }
}

int nn_server_release(void* nn_server_ctx)
{
    if (nn_server_ctx != NULL) {
        nn_server_ctx_t* ctx = (nn_server_ctx_t*)nn_server_ctx;
        ctx->actived = false;

        if (ctx->fd >= 0) {
            nn_close(ctx->fd);
            ctx->fd = -1;
        }

        pthread_join(ctx->tid, NULL);

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        free(ctx);
    }
}


void* nn_client_connect(const char* server_name, nn_nuttx_mode_t mode, nn_nuttx_protocol_t protocol)
{
    nn_client_ctx_t* ctx = NULL;
    int name_len = strlen(server_name);
    int ret = 0;

    if (name_len <= 0
        || mode < 0 || mode > NN_NUTTX_MODE_MAX
        || protocol < 0 || protocol > NN_NUTTX_PROTOCOL_MAX) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nn_nuttx_protocol_str[protocol] + name_len + 1));

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_protocol_str[protocol]);
    strcat(ctx->name, server_name);

    ctx->fd = nn_socket(AF_SP, NN_REQ);

    if (ctx->fd < 0) {
        ret = nn_errno();
        goto err;
    }

    ret = nn_connect(ctx->fd, ctx->name);

    if (ret < 0) {
        fprintf(stderr, "%s: %s\n", __func__, nn_strerror(nn_errno()));
        goto err;
    }

    return ctx;

err:
    fprintf(stderr, "%s.ret=%d\n", __func__, ret);

    if (ctx != NULL) {
        if (ctx->fd != 0) {
            nn_close(ctx->fd);
        }

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

int nn_client_disconnect(void* nn_client_ctx)
{
    if (nn_client_ctx != NULL) {
        nn_client_ctx_t* ctx = (nn_client_ctx_t*)nn_client_ctx;

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        if (ctx->fd > 0) {
            nn_close(ctx->fd);
            ctx->fd = -1;
        }

        free(ctx);
    }
}

int nn_client_transaction(const void* nn_client_ctx, int op_type, const void* in, int in_len, void* out, int out_len)
{
    int ret = 0;
    nn_trans_hdr_t* trans_hdr;
    int data_len = sizeof(nn_trans_hdr_t) + in_len;
    nn_client_ctx_t* ctx = (nn_client_ctx_t*)nn_client_ctx;
    uint8_t* body = NULL;
    void* control = NULL;
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    void* data;

    if (NULL == ctx) {
        ret = -EINVAL;
        goto out;
    }

    data = calloc(1, data_len);

    if (data == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    trans_hdr = (nn_trans_hdr_t*)data;
    trans_hdr->len = in_len;
    trans_hdr->op_code = op_type;

    if (in != NULL && in_len > 0) {
        memcpy(data + sizeof(nn_trans_hdr_t), in, in_len);
    }

    ret = nn_send(ctx->fd, data, data_len, 0);

    if (ret < 0) {
        fprintf(stderr, "%s: %s\n", __func__, nn_strerror(nn_errno()));
        goto out;
    }

    memset(&hdr, 0, sizeof(hdr));
    iov.iov_base = &body;
    iov.iov_len = NN_MSG;
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &control;
    hdr.msg_controllen = NN_MSG;

    ret = nn_recvmsg(ctx->fd, &hdr, 0);

    if (ret < 0) {
        fprintf(stderr, "client_recv: %s\n", nn_strerror(nn_errno()));
        nn_freemsg(control);
        goto out;
    }


    if (ret < sizeof(nn_trans_hdr_t)) {
        ret = -EINVAL;
    } else {
        trans_hdr = (nn_trans_hdr_t*)body;
        if (out != NULL && out_len > 0 && ret > sizeof(nn_trans_hdr_t)) {
            memcpy(out, body + sizeof(nn_trans_hdr_t), out_len > trans_hdr->len ? trans_hdr->len : out_len);
        }

        ret = trans_hdr->op_code;
    }

out:

    if (data != NULL) {
        free(data);
    }

    nn_freemsg(body);

    return ret;
}

/*******************************************TEST*********************************************/

#define SERVER_WORKERS_MAX 1
#define CLIENT_WORKERS_MAX 1
#define CLIENT_SEND_COUND_MAX 50

typedef enum {
    CREATE = 0,
    SET_DATA_SOURCE,
    PREPARE,
    START,
    PAUSE,
    STOP,
    RELEASE,
    ISPLAYING
} media_trans_type_t;


static uint64_t milliseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((uint64_t)tv.tv_sec * 1000) + ((uint64_t)tv.tv_usec / 1000));
}


static int media_server_on_transaction(void* cookie, int code, nn_trans_data_t* data)
{
    int ret = 0;

    switch (code) {
    case CREATE:
        fprintf(stdout, "media_server.create.name=%s\n", (char*)data->in);
        data->out = malloc(strlen("created"));
        memcpy(data->out, "created", strlen("created"));
        data->out_size = strlen("created");
        break;

    case SET_DATA_SOURCE:
        fprintf(stdout, "media_server.set_data_source.url=%s\n", (char*)data->in);
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

    case ISPLAYING:
        fprintf(stdout, "media_server.isplaying\n");
        data->out = malloc(sizeof(int));
        *(int*)data->out = 1;
        data->out_size = sizeof(int);
        break;

    default:
        break;
    }

    return ret;
}

int client(const char* url, const char* name, int protocol)
{
    int rc = 0;
    void* client = nn_client_connect(url, NN_NUTTX_MODE_REQREP, protocol);

    if (client != NULL) {
        char out[15] = {0};
        int  is_playing = 0;
        fprintf(stderr, "client connected\n");
        rc = nn_client_transaction(client, CREATE, name, strlen(name), out, sizeof(out));

        if (rc != 0) {
            fprintf(stdout, "player.name=%s, create.error=%d\n", name, rc);
        } else {
            fprintf(stdout, "player.name=%s, create.out=%s\n", name, out);
        }

        rc = nn_client_transaction(client, SET_DATA_SOURCE, "http://253.mp3", strlen("http://253.mp3"), NULL, 0);

        if (rc != 0) {
            fprintf(stdout, "player.name=%s, set_data_source.error=%d\n", name, rc);
        }

        rc = nn_client_transaction(client, PREPARE, NULL, 0, NULL, 0);

        if (rc != 0) {
            fprintf(stdout, "player.name=%s, prepare.error=%d\n", name, rc);
        }

        rc = nn_client_transaction(client, START, NULL, 0, NULL, 0);

        if (rc != 0) {
            fprintf(stdout, "player.name=%s, start.error=%d\n", name, rc);
        }

        rc = nn_client_transaction(client, ISPLAYING, NULL, 0, &is_playing, sizeof(is_playing));

        if (rc != 0) {
            fprintf(stdout, "player.name=%s, isplaying.error=%d\n", name, rc);
        } else {
            fprintf(stdout, "player.name=%s, isplaying=%d\n", name, is_playing);
        }
    }

    nn_client_disconnect(client);
    return rc;
}

int main(int argc, char** argv)
{
    int rc;
    bool inproc = false;
    void* server = NULL;
    const char* name = NULL;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <url> [-s|name]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    inproc = (strstr(argv[1], "inproc") != NULL);
    name = strstr(argv[1], "//") + 2;

    if (inproc) {
        server = nn_server_create(name, NN_NUTTX_MODE_REQREP, NN_NUTTX_PROTOCOL_INPROC);

        if (server == NULL) {
            fprintf(stderr, "server start failed\n");
        } else {
            fprintf(stderr, "server start success\n");
            nn_server_set_transaction_cb(server, media_server_on_transaction);
            rc = client(name, argv[2], NN_NUTTX_PROTOCOL_INPROC);

            if (rc != 0) {
                fprintf(stderr, "client start failed, ret=%d\n", rc);
            }
        }
    } else {
        if (strcmp(argv[2], "-s") == 0) {
            server = nn_server_create(name, NN_NUTTX_MODE_REQREP, NN_NUTTX_PROTOCOL_IPC);

            if (server == NULL) {
                fprintf(stderr, "server start failed\n");
            } else {
                nn_server_set_transaction_cb(server, media_server_on_transaction);
            }
        } else {
            rc = client(name, argv[2], NN_NUTTX_PROTOCOL_IPC);
        }
    }

    if (server != NULL) {
        nn_server_release(server);
    }

    exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
