/*
 * Copyright (c) 2020 xiaomi.
 *
 * Unpublished copyright. All rights reserved. This material contains
 * proprietary information that should be used or copied only within
 * xiaomi, except with written permission of xiaomi.
 *
 * @file:    nn_nuttx.h
 * @brief:
 * @author:  xulongqiu@xiaomi.com
 * @version: 1.0
 * @date:    2020-11-23 20:40:09
 */

#ifndef __NN_NUTTX_H__
#define __NN_NUTTX_H__

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct nn_trans_data {
    const void* in;
    size_t      in_size;
    void*       out;  // out = malloc/calloc(), nn_nuttx responsible for free.
    size_t      out_size;
} nn_trans_data_t;


/**
 * @brief: server endpoint callback which impl of operation
 *
 * @param: cookie, see: nn_server_set_transaction_cb
 * @param code, enum: operation code
 * @param data see: nn_trans_data
 *
 * @return int
 */
typedef int (*on_transaction)(const void* cookie, const int code, nn_trans_data_t* data);

/**
 * @brief: subscriber listener
 *
 * @param  cookie, see nn_sub_connect
 * @param topic, bytes array
 * @param topic_len
 * @param content, private message
 * @param content_len
 *
 * @return int
 */
typedef int (*on_topic_listener)(const void* cookie, const void* topic, const size_t topic_len, \
        const void* content, const size_t content_len);


/**
 * @brief:nn_server_create
 *
 * @param name, alpha numeric underline
 *
 * @return 0 if error, otherwise server handle
 */
void* nn_server_create(const char* name);

/**
 * @brief:nn_server_set_transaction_cb
 *
 * @param nn_server_ctx: the value of nn_server_create retured.
 * @param cb
 * @param cb_priv
 */
void nn_server_set_transaction_cb(void* nn_server_ctx, on_transaction cb, void* cb_priv);

/**
 * @brief:nn_server_release
 *
 * @param nn_server_ctx
 *
 * @return
 */
int nn_server_release(void* nn_server_ctx);

/**
 * @brief:nn_client_connect
 *
 * @param server_name
 *
 * @return NULL if error, otherwise client handle
 */
void* nn_client_connect(const char* server_name);

/**
 * @brief:nn_client_disconnect
 *
 * @param nn_client_ctx
 *
 * @return 0 if suscess, otherwise error code returned
 */
int nn_client_disconnect(void* nn_client_ctx);

/**
 * @brief:nn_client_transaction
 *
 * @param nn_client_ctx
 * @param op_code
 * @param in
 * @param in_len
 * @param out: the struct data and parser is is your responsibility
 * @param out_len
 *
 * @return
 */
int nn_client_transaction(const void* nn_client_ctx, int op_code, const void* in, int in_len, void* out, int out_len);

/**
 * @brief:create publisher
 *
 * @param name alpha numeric underline
 *
 * @return
 */
void* nn_pub_create(const char* name);

/**
 * @brief:nn_pub_release
 *
 * @param nn_pub_ctx
 *
 * @return
 */
int nn_pub_release(void* nn_pub_ctx);

/**
 * @brief:nn_pub_topic_msg
 *
 * @param nn_pub_ctx
 * @param topic
 * @param topic_len
 * @param content
 * @param content_len
 *
 * @return 0 there is no error when call send function
 */
int nn_pub_topic_msg(void* nn_pub_ctx, const void* topic, size_t topic_len, const void* content, size_t content_len);

/**
 * @brief: connect to publisher
 *
 * @param name of publisher's
 * @param listener  call this when receives message
 * @param listener_priv
 *
 * @return
 */
void* nn_sub_connect(const char* name, on_topic_listener listener, void* listener_priv);

/**
 * @brief:nn_sub_disconnect
 *
 * @param nn_sub_ctx
 */
void nn_sub_disconnect(void* nn_sub_ctx);

/**
 * @brief:nn_sub_register_topic
 *
 * @param nn_sub_ctx
 * @param topic
 * @param topic_len
 *
 * @return
 */
int nn_sub_register_topic(void* nn_sub_ctx, const void* topic, size_t topic_len);

/**
 * @brief:nn_sub_unregister_topic
 *
 * @param nn_sub_ctx
 * @param topic
 * @param topic_len
 *
 * @return
 */
int nn_sub_unregister_topic(void* nn_sub_ctx, const void* topic, size_t topic_len);

#ifdef __cplusplus
}
#endif

#endif /*__NN_NUTTX_H__*/
