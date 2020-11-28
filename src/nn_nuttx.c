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

#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/prctl.h>

#include "nn.h"
#include "reqrep.h"
#include "pubsub.h"

#include "nn_nuttx.h"

#define NN_NUTTX_TOPIC_NAME_LEN 15

typedef struct nn_trans_hdr {
    int seq;
    int op_code;
    size_t len;
    unsigned char* data[];
} nn_trans_hdr_t;

typedef enum {
    NN_NUTTX_MODE_REQREP = 0,
    NN_NUTTX_MODE_PIPELINE,
    NN_NUTTX_MODE_PAIR,

    NN_NUTTX_MODE_MAX
} nn_nuttx_mode_t;

typedef enum {
    NN_NUTTX_TRANS_TYPE_INPROC = 0,
    NN_NUTTX_TRANS_TYPE_IPC,
    NN_NUTTX_TRANS_TYPE_TCP,

    NN_NUTTX_TRANS_TYPE_MAX
} nn_nuttx_trans_type_t;

typedef struct nn_server_ctx {
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

typedef struct nn_pub_ctx {
    char* name;
    int fd;
} nn_pub_ctx_t;

typedef struct nn_sub_ctx {
    char* name;
    int fd;
    on_topic_listener listener;
    pthread_t tid;
    bool actived;
    void* priv;
} nn_sub_ctx_t;

typedef struct nn_nuttx_topic {
    uint8_t topic[NN_NUTTX_TOPIC_NAME_LEN + 1];
    size_t content_len;
    uint8_t content[];
} nn_nuttx_topic_t;

const char* const nn_nuttx_trans_prefix_str[] = {
    "inproc://", "ipc://", "tcp://"
};

static int nn_socket_bind(int* fd, const char* name, int af, int protocol)
{
    *fd = nn_socket(af, protocol);

    if (*fd < 0) {
        return nn_errno();
    }

    if (nn_bind(*fd, name) < 0) {
        return nn_errno();
    }

    return 0;
}

static int nn_socket_connect(int* fd, const char* name, int af, int protocol)
{
    *fd = nn_socket(af, protocol);

    if (*fd < 0) {
        return nn_errno();
    }

    if (nn_connect(*fd, name) < 0) {
        return nn_errno();
    }

    return 0;
}

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

void* nn_server_create(const char* name)
{
    nn_server_ctx_t* ctx = NULL;
    const nn_nuttx_trans_type_t trans_type = NN_NUTTX_TRANS_TYPE_INPROC;
    int name_len = strlen(name);
    int ret = 0;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->fd = -1;
    ctx->name = calloc(1, strlen(nn_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    ret = nn_socket_bind(&ctx->fd, ctx->name, AF_SP_RAW, NN_REP);

    if (ret < 0) {
        goto err;
    }

    ctx->actived = true;
    ret = pthread_create(&ctx->tid, NULL, nn_server_worker, (void*)ctx);

    if (0 == ret) {
        return ctx;
    }

err:
    fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));

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

void nn_server_set_transaction_cb(void* nn_server_ctx, on_transaction cb, void* cb_priv)
{
    if (nn_server_ctx != NULL && cb != NULL) {
        nn_server_ctx_t* ctx = (nn_server_ctx_t*)nn_server_ctx;
        ctx->on_trans_cb = cb;
        ctx->priv = cb_priv;
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


void* nn_client_connect(const char* server_name)
{
    int ret = 0;
    nn_client_ctx_t* ctx = NULL;
    int name_len = strlen(server_name);
    const nn_nuttx_trans_type_t trans_type = NN_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nn_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, server_name);

    ret = nn_socket_connect(&ctx->fd, ctx->name, AF_SP, NN_REQ);

    if (ret < 0) {
        goto err;
    }

    return ctx;

err:
    fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));

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

int nn_client_transaction(const void* nn_client_ctx, int op_code, const void* in, int in_len, void* out, int out_len)
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
    trans_hdr->op_code = op_code;

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
    if (control != NULL) {
        nn_freemsg(control);
    }
    nn_freemsg(body);

    return ret;
}

void* nn_pub_create(const char* name)
{
    int ret = 0;
    nn_pub_ctx_t* ctx = NULL;
    int name_len = strlen(name);
    const nn_nuttx_trans_type_t trans_type = NN_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->fd = -1;
    ctx->name = calloc(1, strlen(nn_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    ret = nn_socket_bind(&ctx->fd, ctx->name, AF_SP, NN_PUB);

    if (ret < 0) {
        goto err;
    }

    return ctx;
err:
    fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));

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

int nn_pub_release(void* nn_pub_ctx)
{
    if (nn_pub_ctx != NULL) {
        nn_pub_ctx_t* ctx = (nn_pub_ctx_t*)nn_pub_ctx;

        if (ctx->fd >= 0) {
            nn_close(ctx->fd);
            ctx->fd = -1;
        }

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        free(ctx);
    }
}



int nn_pub_topic_msg(void* nn_pub_ctx, const void* topic, size_t topic_len, const void* content, size_t content_len)
{
    int ret = 0;
    int send_len = 0;
    nn_pub_ctx_t* ctx = (nn_pub_ctx_t*)nn_pub_ctx;
    nn_nuttx_topic_t* topic_data;

    if(topic == NULL || ctx == NULL || ctx->fd < 0 /*|| ctx->proto != NN_PUB*/) {
        return -EINVAL;
    }

    send_len = sizeof(nn_nuttx_topic_t) + content_len;
    topic_data = (nn_nuttx_topic_t*)calloc(1, send_len);
    if (topic_data == NULL) {
        return -ENOMEM;
    }
    memcpy(topic_data->topic, topic, topic_len > NN_NUTTX_TOPIC_NAME_LEN? NN_NUTTX_TOPIC_NAME_LEN : topic_len);
    topic_data->content_len = content_len;
    memcpy(topic_data->content, content, content_len);

    ret = nn_send(ctx->fd, topic_data, send_len, 0);
    free(topic_data);

    if (ret != send_len) {
        fprintf(stderr, "%s.ret=%d, send_len=%d\n", __func__, ret, send_len);
        return -1;
    }

    return 0;
}

static void* nn_sub_worker(void* arg)
{
    nn_nuttx_topic_t* topic;
    nn_sub_ctx_t* ctx = (nn_sub_ctx_t*)arg;
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

        topic = (nn_nuttx_topic_t*)body;

        if (sizeof(nn_nuttx_topic_t) + topic->content_len != rc) {
            fprintf(stderr, "%s.content_len=%lu, rc=%d\n", __func__, topic->content_len, rc);
        } else {
            if (ctx->listener != NULL) {
                void* content = body + sizeof(nn_nuttx_topic_t);
                ctx->listener(ctx->priv, topic->topic, NN_NUTTX_TOPIC_NAME_LEN, content, topic->content_len);
            }
        }
        nn_freemsg(body);
        nn_freemsg(control);
    }

    return NULL;
}

void* nn_sub_connect(const char* name, on_topic_listener listener, void* listener_priv)
{
    int ret = 0;
    nn_sub_ctx_t* ctx = NULL;
    int name_len = strlen(name);
    const nn_nuttx_trans_type_t trans_type = NN_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nn_sub_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nn_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nn_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    ret = nn_socket_connect(&ctx->fd, ctx->name, AF_SP, NN_SUB);

    if (ret < 0) {
        goto err;
    }

    ctx->listener = listener;
    ctx->priv = listener_priv;

    ret = pthread_create(&ctx->tid, NULL, nn_sub_worker, (void*)ctx);

    if (0 == ret) {
        return ctx;
    }

err:
    fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));

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

void nn_sub_disconnect(void* nn_sub_ctx)
{
    if (nn_sub_ctx != NULL) {
        nn_sub_ctx_t* ctx = (nn_sub_ctx_t*)nn_sub_ctx;
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


int nn_sub_register_topic(void* nn_sub_ctx, const void* topic, size_t topic_len)
{
    int ret = 0;
    nn_sub_ctx_t* ctx = (nn_sub_ctx_t*)nn_sub_ctx;

    if (topic == NULL || ctx == NULL || ctx->fd < 0 /* || ctx->proto != NN_SUB*/) {
        return -EINVAL;
    }

    if (topic_len > NN_NUTTX_TOPIC_NAME_LEN) {
        fprintf(stderr, "%s.topic len(%lu) > max(%d)\n", __func__, topic_len, NN_NUTTX_TOPIC_NAME_LEN);
        topic_len = NN_NUTTX_TOPIC_NAME_LEN;
    }

    ret = nn_setsockopt(ctx->fd, NN_SUB, NN_SUB_SUBSCRIBE, topic, topic_len);
    if (ret < 0) {
        fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));
    }

    return ret;
}

int nn_sub_unregister_topic(void* nn_sub_ctx, const void* topic, size_t topic_len)
{
    int ret = 0;
    nn_sub_ctx_t* ctx = (nn_sub_ctx_t*)nn_sub_ctx;

    if (topic == NULL || ctx == NULL || ctx->fd < 0 /* || ctx->proto != NN_SUB*/) {
        return -EINVAL;
    }

    if (topic_len > NN_NUTTX_TOPIC_NAME_LEN) {
        fprintf(stderr, "%s.topic len(%lu) > max(%d)\n", __func__, topic_len, NN_NUTTX_TOPIC_NAME_LEN);
        topic_len = NN_NUTTX_TOPIC_NAME_LEN;
    }

    ret = nn_setsockopt(ctx->fd, NN_SUB, NN_SUB_UNSUBSCRIBE, topic, topic_len);
    if (ret < 0) {
        fprintf(stderr, "%s.ret=%d(%s)\n", __func__, ret, nn_strerror(ret));
    }

    return ret;
}

