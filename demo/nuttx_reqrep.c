#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "nn_nuttx.h"

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


static int media_server_on_transaction(const void* cookie, const int code, nn_trans_data_t* data)
{
    int ret = 0;

    switch (code) {
    case CREATE:
        if (data != NULL) {
            fprintf(stdout, "media_server.create.name=%s\n", (char*)data->in);
            data->out = malloc(strlen("created"));
            memcpy(data->out, "created", strlen("created"));
            data->out_size = strlen("created");
        }

        break;

    case SET_DATA_SOURCE:
        if (data != NULL) {
            fprintf(stdout, "media_server.set_data_source.url=%s\n", (char*)data->in);
        }

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

        if (data != NULL) {
            data->out = malloc(sizeof(int));
            *(int*)data->out = 1;
            data->out_size = sizeof(int);
        }

        break;

    default:
        break;
    }

    return ret;
}

int client(const char* url, const char* name)
{
    int rc = 0;
    void* client = nn_client_connect(url);

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
        server = nn_server_create(name);

        if (server == NULL) {
            fprintf(stderr, "server start failed\n");
        } else {
            fprintf(stderr, "server start success\n");
            nn_server_set_transaction_cb(server, media_server_on_transaction, server);
            rc = client(name, argv[2]);

            if (rc != 0) {
                fprintf(stderr, "client start failed, ret=%d\n", rc);
            }
        }
    } else {
        if (strcmp(argv[2], "-s") == 0) {
            server = nn_server_create(name);

            if (server == NULL) {
                fprintf(stderr, "server start failed\n");
            } else {
                nn_server_set_transaction_cb(server, media_server_on_transaction, server);
            }
        } else {
            rc = client(name, argv[2]);
        }
    }

    if (server != NULL) {
        nn_server_release(server);
    }

    exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
