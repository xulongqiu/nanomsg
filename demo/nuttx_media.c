#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <pthread.h>

#include "nn_nuttx.h"

#define SERVER_WORKERS_MAX 1
#define CLIENT_WORKERS_MAX 1
#define CLIENT_SEND_COUND_MAX 50

typedef struct {
    int cmd;
    int arg1;
    int arg2;
} player_info_t;

typedef struct media_player_ctx {
    void* player;
    char* name;
    struct media_player_ctx* next;
}media_player_ctx_t;

typedef struct {
    void* server;
    void* notifier;
    pthread_t tid;
    media_player_ctx_t* player_list;
} media_server_ctx_t;

typedef struct {
    const char* name;
    pthread_t tid;
    void* server_proxy;
    void* cb_proxy;
    void* handle;
} player_t;

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


static void media_server_player_notify_info(media_server_ctx_t* ctx, media_player_ctx_t* player) {
    player_info_t info = {
        .cmd = 1,
        .arg1 = 2,
        .arg2 = 3
    };
    nn_pub_topic_msg(ctx->notifier, player->name, strlen(player->name), &info, sizeof(info));
}

static void* media_server_worker(void* arg)
{
    int cnt = 5;
    media_server_ctx_t* ctx = (media_server_ctx_t*)arg;
    //pthread_detach(pthread_self()); //pthread_join()
    prctl(PR_SET_NAME, "pub_worker", NULL, NULL, NULL);

    if (ctx == NULL) {
        return NULL;
    }
    sleep(1);
    while (cnt-- > 0) {
        media_player_ctx_t* player = ctx->player_list;
        while(player != NULL) {
            media_server_player_notify_info(ctx, player);
            player = player->next;
        }

        sleep(2);
    }


    return NULL;
}

static void* media_server_player_create(media_server_ctx_t* ctx, const char* name) {
    media_player_ctx_t* player = calloc(1, sizeof(media_player_ctx_t));
    media_player_ctx_t** head = &ctx->player_list;
    if (player == NULL) {
        return NULL;
    }
    player->player = calloc(1, sizeof(void*));
    player->name = strdup(name);

    while(*head != NULL) {
        head = &((*head)->next);
    }

    *head = player;

    return player;
}

static int media_server_on_transaction(const void* cookie, const int code, nn_trans_data_t* data)
{
    int ret = 0;

    switch (code) {
    case CREATE:
        if (data != NULL) {
            fprintf(stdout, "media_server.create.name=%s\n", (char*)data->in);
            void* player = media_server_player_create((media_server_ctx_t*)cookie, data->in);
            if (player != NULL) {
                data->out = malloc(sizeof(void*));
                memcpy(data->out, &player, sizeof(void*));
                data->out_size = sizeof(void*);
            }
        }

        break;

    case SET_DATA_SOURCE:
        if (data != NULL) {
            fprintf(stdout, "media_server.set_data_source.url=%s\n", (char*)data->in);
        }

        break;

    default:
        break;
    }

    return ret;
}

static int media_player_listener(const void* thiz, const void* topic, const size_t topic_len, const void* content, const size_t content_len) {
    player_t* player = (player_t*)thiz;
    player_info_t* info = (player_info_t*)content;
    fprintf(stderr, "%s.%s.topic=%s, .cmd=%d, .arg1=%d, .arg2=%d\n", __func__, player->name, (char*)topic, info->cmd, info->arg1, info->arg2);
}

static inline void* get_media_server_proxy(void) {
    return nn_client_connect("mediaserver");
}

static inline void* get_media_server_cb_proxy(player_t* player) {
    return nn_sub_connect("mediapub", media_player_listener, player);
}

static void media_player_create(player_t* player) {
    void* handle = NULL;
    player->server_proxy = get_media_server_proxy();
    player->cb_proxy = get_media_server_cb_proxy(player);
    nn_sub_register_topic(player->cb_proxy, player->name, strlen(player->name));
    fprintf(stderr, "%s.name=%s\n", __func__, player->name);
    nn_client_transaction(player->server_proxy, CREATE, player->name, strlen(player->name) + 1, &handle, sizeof(handle));
    player->handle = handle;
    return;
}

static int media_player_set_data_source(player_t *player, const char* url) {
    int rc = nn_client_transaction(player->server_proxy, SET_DATA_SOURCE, url, strlen(url), NULL, 0);
}

static void* player_worker(void* arg) {
    player_t *player = (player_t*)arg;

    media_player_create(player);

    media_player_set_data_source(player, "http://253.mp3");

    while(1) {
        sleep(5);
        break;
    }
}

static int media_client_start(player_t client[], const int cnt)
{
    int ret = 0;
    for(int i = 0; i < cnt; i++) {
        ret = pthread_create(&client[i].tid, NULL, player_worker, &client[i]);
        if (ret != 0) {
            fprintf(stderr, "%s.player.%s.failed\n", __func__, client[i].name);
            break;
        }
    }

    return ret;
}

static void inline media_player_destroy(player_t *player) {
    pthread_join(player->tid, NULL);
    nn_client_disconnect(player->server_proxy);
    nn_sub_unregister_topic(player->cb_proxy, player->name, strlen(player->name));
    nn_sub_disconnect(player->cb_proxy);
}

static int media_client_stop(player_t client[], const int cnt) {
    for (int i = 0; i < cnt; i++) {
        media_player_destroy(&client[i]);
    }
}

int media_server_start(media_server_ctx_t* ctx)
{
    ctx->server = nn_server_create("mediaserver");
    if (ctx->server == NULL) {
        return -1;
    }

    ctx->notifier = nn_pub_create("mediapub");
    if (ctx->notifier == NULL) {
        nn_server_release(ctx->server);
        ctx->server = NULL;
        return -2;
    }

    nn_server_set_transaction_cb(ctx->server, media_server_on_transaction, ctx);
    pthread_create(&ctx->tid, NULL, media_server_worker, ctx);
}

int media_server_stop(media_server_ctx_t *ctx) {
    pthread_join(ctx->tid, NULL);
    nn_server_release(ctx->server);
    nn_pub_release(ctx->notifier);
}

int main(int argc, char** argv)
{
    player_t player[3];
    player[2].name = "wakeup";
    player[0].name = "tts";
    player[1].name = "music";
    media_server_ctx_t media_server;
    memset(&media_server, 0, sizeof(media_server));
    media_server_start(&media_server);
    media_client_start(player, 3);

    media_server_stop(&media_server);
    media_client_stop(player, 3);
    exit(EXIT_SUCCESS);
}
