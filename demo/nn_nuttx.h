/* Copyright (C)
 * 2020 - xulongqiu@xiaomi.com
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
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
    void*       out;
    size_t      out_size;
} nn_trans_data_t;

typedef int (*on_transaction)(const void* cookie, const int code, nn_trans_data_t* data);
typedef int (*on_topic_listener)(const void* cookie, const void* topic, const size_t topic_len, \
        const void* content, const size_t content_len);

void* nn_server_create(const char* name);
void nn_server_set_transaction_cb(void* nn_server_ctx, on_transaction cb, void* cb_priv);
int nn_server_release(void* nn_server_ctx);
void* nn_client_connect(const char* server_name);
int nn_client_disconnect(void* nn_client_ctx);
int nn_client_transaction(const void* nn_client_ctx, int op_code, const void* in, int in_len, void* out, int out_len);
void* nn_pub_create(const char* name);
int nn_pub_release(void* nn_pub_ctx);
int nn_pub_topic_msg(void* nn_pub_ctx, const void* topic, size_t topic_len, const void* content, size_t content_len);
void* nn_sub_connect(const char* name, on_topic_listener listener, void* listener_priv);
void nn_sub_disconnect(void* nn_sub_ctx);
int nn_sub_register_topic(void* nn_sub_ctx, const void* topic, size_t topic_len);
int nn_sub_unregister_topic(void* nn_sub_ctx, const void* topic, size_t topic_len);

#ifdef __cplusplus
}
#endif

#endif /*__NN_NUTTX_H__*/
