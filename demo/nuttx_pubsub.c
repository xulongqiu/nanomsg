#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <pthread.h>

#include "nn_nuttx.h"

#define CLIENT_WORKERS_MAX 5

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

typedef struct {
    char name[16];
    int id;
    const char* url;
    void* handle;
} nn_nuttx_sub_test_t;

nn_nuttx_sub_test_t g_sub_test[CLIENT_WORKERS_MAX];

static uint64_t milliseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((uint64_t)tv.tv_sec * 1000) + ((uint64_t)tv.tv_usec / 1000));
}


static void* pub_worker(void* arg)
{
    int cnt = 5;
    //pthread_detach(pthread_self()); //pthread_join()
    prctl(PR_SET_NAME, "pub_worker", NULL, NULL, NULL);

    while (cnt-- > 0) {
        for(int i = 0; i < sizeof(g_sub_test) / sizeof(g_sub_test[0]); i++) {
            nn_pub_topic_msg(arg, g_sub_test[i].name, strlen(g_sub_test[i].name), g_sub_test[i].name, strlen(g_sub_test[i].name));
        }
        sleep(2);
    }


    return NULL;
}

static int local_sub_listener(const void* thiz, const void* topic, const size_t topic_len, const void* content, const size_t content_len) {
    nn_nuttx_sub_test_t* test = (nn_nuttx_sub_test_t*)thiz;
    fprintf(stderr, "%s.%s.listener.topic=%s\n", __func__, test->name, (char*)topic);
}

int client(const char* url, const char* name)
{
    int i;
    int ret;

    for (i = 0; i < sizeof(g_sub_test) / sizeof(g_sub_test[0]); i++) {
        snprintf(g_sub_test[i].name, sizeof(g_sub_test[i].name), "%s-%d", name, i);
        g_sub_test[i].handle = nn_sub_connect(url, local_sub_listener, &g_sub_test[i]);
        if (g_sub_test[i].handle != NULL) {
            if(nn_sub_register_topic(g_sub_test[i].handle, g_sub_test[i].name, strlen(g_sub_test[i].name)) < 0) {
                fprintf(stderr, "%s.%s.nn_register_topic(%s).failed\n", __func__, g_sub_test[i].name, g_sub_test[i].name);
            }
        } else {
            fprintf(stderr, "%s.%s.nn_sub_connect(%s).failed\n", __func__, g_sub_test[i].name, url);
        }
    }

    return ret;
}


int main(int argc, char** argv)
{
    int rc;
    bool inproc = false;
    void* server = NULL;
    const char* name = NULL;
    pthread_t pub_tid;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <url> [-s|name]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    inproc = (strstr(argv[1], "inproc") != NULL);
    name = strstr(argv[1], "//") + 2;

    if (inproc) {
        server = nn_pub_create(name);

        if (server == NULL) {
            fprintf(stderr, "server start failed\n");
        } else {
            fprintf(stderr, "server start success\n");
            rc = pthread_create(&pub_tid, NULL, pub_worker, server);
            if (rc != 0) {
                fprintf(stderr, "start pub_worker failed, ret=%d\n", rc);
            }
            rc = client(name, argv[2]);

            if (rc != 0) {
                fprintf(stderr, "client start failed, ret=%d\n", rc);
            }
        }
    } else {
        if (strcmp(argv[2], "-s") == 0) {
            server = nn_pub_create(name);

            if (server == NULL) {
                fprintf(stderr, "server start failed\n");
            }
        } else {
            rc = client(name, argv[2]);
        }
    }
    fprintf(stderr, "%s.wait exit!\n", __func__);
    pthread_join(pub_tid, NULL);
    fprintf(stderr, "%s.wait exit...!\n", __func__);
    if (server != NULL) {
        nn_pub_release(server);
    }
    for(int i = 0; i < sizeof(g_sub_test) / sizeof(g_sub_test[0]); i++) {
        nn_sub_unregister_topic(g_sub_test[i].handle, g_sub_test[i].name, strlen(g_sub_test[i].name));
        nn_sub_disconnect(g_sub_test[i].handle);
    }

    exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
