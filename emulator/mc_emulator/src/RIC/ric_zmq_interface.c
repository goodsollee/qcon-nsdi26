/**
 * ric_zmq_interface.c
 * C implementation of ZeroMQ interface for RAN components to communicate with RIC
 * Using the zmq.h low-level API
 */

#include "RIC/ric_zmq_interface.h"
#include <zmq.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <json-c/json.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

/* Log levels */
typedef enum {
    RIC_LOG_DEBUG,
    RIC_LOG_INFO,
    RIC_LOG_WARN,  // Added WARN level between INFO and ERROR
    RIC_LOG_ERROR
} ric_log_level_t;

/* Logging macros */
#define LOG_DEBUG(fmt, ...) ric_zmq_log(RIC_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ric_zmq_log(RIC_LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ric_zmq_log(RIC_LOG_WARN, fmt, ##__VA_ARGS__)  // Fixed WARN macro
#define LOG_ERROR(fmt, ...) ric_zmq_log(RIC_LOG_ERROR, fmt, ##__VA_ARGS__)

/* Convert preprocessor log level to runtime level */
/*
#if defined(LOGLEVEL) && (LOGLEVEL == DEBUG)
  #define RIC_LOG_LEVEL RIC_LOG_DEBUG
#elif defined(LOGLEVEL) && (LOGLEVEL == INFO)  // Added WARN level option
  #define RIC_LOG_LEVEL RIC_LOG_INFO
#elif defined(LOGLEVEL) && (LOGLEVEL == WARN)  // Added WARN level option
  #define RIC_LOG_LEVEL RIC_LOG_WARN
#elif defined(LOGLEVEL) && (LOGLEVEL == ERROR)
  #define RIC_LOG_LEVEL RIC_LOG_ERROR
#else
  #define RIC_LOG_LEVEL RIC_LOG_INFO
#endif
*/

#define RIC_LOG_LEVEL RIC_LOG_ERROR

/* Logging function */
static void ric_zmq_log(ric_log_level_t level, const char* fmt, ...) {
    /* Skip logs below the configured level */
    if (level < RIC_LOG_LEVEL) {
        return;
    }
    
    char timestamp[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    const char* level_str;
    switch (level) {
        case RIC_LOG_DEBUG: level_str = "DEBUG"; break;
        case RIC_LOG_INFO:  level_str = "INFO"; break;
        case RIC_LOG_WARN:  level_str = "WARN"; break;  // Added WARN case
        case RIC_LOG_ERROR: level_str = "ERROR"; break;
        default: level_str = "UNKNOWN";
    }
    
    fprintf(stderr, "[%s] [RIC-ZMQ] [%s] ", timestamp, level_str);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    fprintf(stderr, "\n");
}

/* Error message buffer */
static __thread char error_message[512] = {0};

/* Internal RIC ZMQ Context */
struct RICZmqContext {
    /* Component information */
    RicComponentType component_type;
    char component_type_str[16];
    char component_id[64];
    
    /* Endpoints */
    char kpm_endpoint[128];
    char rc_endpoint[128];
    char ric_kpm_endpoint[128];
    char ric_rc_endpoint[128];
    
    /* ZeroMQ context and sockets */
    void* zmq_context;   /* ZMQ context */
    void* kpm_socket;    /* PUB socket for KPM metrics */
    void* rc_socket;     /* REP socket for RC commands */
    
    /* RC command callback */
    RicCommandCallback rc_callback;
    void* user_data;
    
    /* Thread for listening to RC commands */
    pthread_t listen_thread;
    int running;
    pthread_mutex_t mutex;
};

/* Helper function to set the last error message */
static void SetLastError(const char* message) {
    strncpy(error_message, message, sizeof(error_message) - 1);
    error_message[sizeof(error_message) - 1] = '\0';
}

/* Convert component type to string */
static const char* ComponentTypeToString(RicComponentType type) {
    switch (type) {
        case RIC_COMPONENT_CU:
            return "CU";
        case RIC_COMPONENT_DU:
            return "DU";
        default:
            return "UNKNOWN";
    }
}

/* Helper function to send a string through a ZMQ socket */
static int zmq_send_string(void* socket, const char* string) {
    return zmq_send(socket, string, strlen(string), ZMQ_DONTWAIT);
}

/* Helper function to receive a string from a ZMQ socket */
static char* zmq_recv_string(void* socket) {
    zmq_msg_t message;
    zmq_msg_init(&message);
    
    int size = zmq_msg_recv(&message, socket, 0);
    if (size < 0) {
        zmq_msg_close(&message);
        return NULL;
    }
    
    char* string = (char*)malloc(size + 1);
    if (!string) {
        zmq_msg_close(&message);
        return NULL;
    }
    
    memcpy(string, zmq_msg_data(&message), size);
    string[size] = '\0';
    
    zmq_msg_close(&message);
    return string;
}

/* RC command listening thread function */
static void* ListenThreadFunc(void* arg) {
    RICZmqHandle ctx = (RICZmqHandle)arg;
    LOG_DEBUG("RC command listen loop started");
    
    while (ctx->running) {
        /* Poll with timeout */
        zmq_pollitem_t items[] = {
            { ctx->rc_socket, 0, ZMQ_POLLIN, 0 }
        };
        
        int rc = zmq_poll(items, 1, 100);
        if (rc < 0) {
            LOG_ERROR("ZMQ poll error: %s", zmq_strerror(errno));
            continue;
        }
        
        if (items[0].revents & ZMQ_POLLIN) {
            /* Receive message */
            char* msg_str = zmq_recv_string(ctx->rc_socket);
            if (!msg_str) {
                LOG_ERROR("Failed to receive message");
                continue;
            }
            
            /* Parse RC command */
            struct json_object* root = json_tokener_parse(msg_str);
            if (!root) {
                LOG_ERROR("Failed to parse JSON message");
                free(msg_str);
                continue;
            }
            
            /* Extract message type */
            struct json_object* type_obj;
            const char* type_str = NULL;
            if (json_object_object_get_ex(root, "type", &type_obj)) {
                type_str = json_object_get_string(type_obj);
            }
            
            if (type_str && strcmp(type_str, "rc_command") == 0) {
                /* Extract component ID */
                struct json_object* comp_id_obj;
                const char* cmd_component_id = NULL;
                if (json_object_object_get_ex(root, "component_id", &comp_id_obj)) {
                    cmd_component_id = json_object_get_string(comp_id_obj);
                }
                
                /* Check if command is for this component */
                LOG_DEBUG("Received RC command for: %s", cmd_component_id ? cmd_component_id : "NULL");
                if (cmd_component_id && strcmp(cmd_component_id, ctx->component_id) == 0) {
                    /* Extract command details */
                    struct json_object* cmd_type_obj;
                    struct json_object* cmd_params_obj;
                    const char* command_type = NULL;
                    const char* command_params = NULL;
                    
                    if (json_object_object_get_ex(root, "command_type", &cmd_type_obj)) {
                        command_type = json_object_get_string(cmd_type_obj);
                    }
                    
                    if (json_object_object_get_ex(root, "command_params", &cmd_params_obj)) {
                        command_params = json_object_get_string(cmd_params_obj);
                    }
                    
                    if (command_type && command_params) {
                        LOG_INFO("Received RC command: %s", command_type);
                        LOG_DEBUG("Command params: %s", command_params);
                        
                        /* Process command through callback */
                        char* response_data = NULL;
                        const char* response_code = "200"; /* Default success */
                        
                        if (ctx->rc_callback) {
                            pthread_mutex_lock(&ctx->mutex);
                            response_data = ctx->rc_callback(command_type, command_params, ctx->user_data);
                            pthread_mutex_unlock(&ctx->mutex);
                            
                            if (!response_data) {
                                response_code = "500";
                                response_data = strdup("Command handler returned NULL");
                            }
                        } else {
                            response_code = "501";
                            response_data = strdup("No command handler registered");
                        }
                        
                        /* Create response */
                        struct json_object* response = json_object_new_object();
                        json_object_object_add(response, "type", json_object_new_string("rc_response"));
                        json_object_object_add(response, "component_id", json_object_new_string(ctx->component_id));
                        json_object_object_add(response, "response_code", json_object_new_string(response_code));
                        json_object_object_add(response, "response_data", json_object_new_string(response_data ? response_data : ""));
                        
                        /* Add timestamp */
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        uint64_t timestamp = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
                        json_object_object_add(response, "timestamp", json_object_new_int64(timestamp));
                        
                        /* Send response */
                        const char* response_str = json_object_to_json_string(response);
                        zmq_send_string(ctx->rc_socket, response_str);
                        
                        LOG_INFO("Sent response: %s", response_code);
                        
                        /* Clean up */
                        json_object_put(response);
                        if (response_data) {
                            free(response_data);
                        }
                    }
                } else {
                    LOG_ERROR("Received command for different component: %s", 
                              cmd_component_id ? cmd_component_id : "NULL");
                }
            } else {
                LOG_ERROR("Invalid message format or type");
            }
            
            /* Clean up */
            json_object_put(root);
            free(msg_str);
        }
    }
    
    LOG_DEBUG("RC command listen loop ended");
    return NULL;
}

RICZmqHandle RIC_ZMQ_Init(
    RicComponentType component_type,
    const char* component_id,
    int kpm_port,
    int rc_port,
    const char* ric_ip,
    int ric_kpm_port,
    int ric_rc_port
) {
    LOG_INFO("Starting RIC_ZMQ_Init: component=%s, id=%s, kpm_port=%d, rc_port=%d, ric_ip=%s",
             ComponentTypeToString(component_type), component_id, kpm_port, rc_port, ric_ip);
    
    if (!component_id || !ric_ip) {
        SetLastError("Invalid parameters (NULL pointers)");
        LOG_ERROR("RIC_ZMQ_Init failed: Invalid parameters (NULL pointers)");
        return NULL;
    }
    
    /* Allocate context */
    RICZmqHandle ctx = (RICZmqHandle)calloc(1, sizeof(struct RICZmqContext));
    if (!ctx) {
        SetLastError("Memory allocation failed");
        LOG_ERROR("RIC_ZMQ_Init failed: Memory allocation failed");
        return NULL;
    }
    
    /* Initialize component info */
    ctx->component_type = component_type;
    strncpy(ctx->component_type_str, ComponentTypeToString(component_type), sizeof(ctx->component_type_str) - 1);
    strncpy(ctx->component_id, component_id, sizeof(ctx->component_id) - 1);
    
    /* Determine local IP (for simplicity, using localhost) */
    const char* local_ip = ric_ip;
    LOG_INFO("Using local IP: %s", local_ip);
    
    /* Setup endpoints */
    snprintf(ctx->kpm_endpoint, sizeof(ctx->kpm_endpoint), 
             "tcp://%s:%d", local_ip, kpm_port);
    snprintf(ctx->rc_endpoint, sizeof(ctx->rc_endpoint), 
             "tcp://%s:%d", local_ip, rc_port);
    
    LOG_INFO("KPM endpoint: %s", ctx->kpm_endpoint);
    LOG_INFO("RC endpoint: %s", ctx->rc_endpoint);
    
    /* Initialize mutex */
    int mutex_result = pthread_mutex_init(&ctx->mutex, NULL);
    if (mutex_result != 0) {
        SetLastError("Failed to initialize mutex");
        LOG_ERROR("RIC_ZMQ_Init failed: Mutex initialization failed with code %d", mutex_result);
        free(ctx);
        return NULL;
    }
    LOG_INFO("Mutex initialized successfully");
    
    /* Create ZMQ context */
    ctx->zmq_context = zmq_ctx_new();
    if (!ctx->zmq_context) {
        SetLastError("Failed to create ZMQ context");
        LOG_ERROR("RIC_ZMQ_Init failed: ZMQ context creation failed (errno: %d)", errno);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }
    LOG_INFO("ZMQ context created successfully");
    
    /* Create KPM socket (PUB) */
    ctx->kpm_socket = zmq_socket(ctx->zmq_context, ZMQ_PUB);
    if (!ctx->kpm_socket) {
        SetLastError("Failed to create KPM socket");
        LOG_ERROR("RIC_ZMQ_Init failed: ZMQ KPM socket creation failed (errno: %d)", errno);
        zmq_ctx_destroy(ctx->zmq_context);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }
    LOG_INFO("KPM socket created successfully");
    
    /* Set socket options */
    int linger = 1000; /* 1 second linger period */
    int result = zmq_setsockopt(ctx->kpm_socket, ZMQ_LINGER, &linger, sizeof(linger));
    if (result != 0) {
        LOG_WARN("Warning: Failed to set ZMQ_LINGER option on KPM socket (errno: %d)", errno);
    } else {
        LOG_INFO("KPM socket options set successfully");
    }
    
    char bind_str[256];
    snprintf(bind_str, sizeof(bind_str), "tcp://*:%d", kpm_port);
    LOG_INFO("Binding KPM socket to: %s", bind_str);
    
    result = zmq_bind(ctx->kpm_socket, bind_str);
    if (result != 0) {
        SetLastError("Failed to bind KPM socket");
        LOG_ERROR("RIC_ZMQ_Init failed: ZMQ KPM socket binding failed (errno: %d - %s)", 
                 errno, zmq_strerror(errno));
        zmq_close(ctx->kpm_socket);
        zmq_ctx_destroy(ctx->zmq_context);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }
    LOG_INFO("KPM socket bound successfully");
    
    /* Create RC socket (REP) */
    ctx->rc_socket = zmq_socket(ctx->zmq_context, ZMQ_REP);
    if (!ctx->rc_socket) {
        SetLastError("Failed to create RC socket");
        LOG_ERROR("RIC_ZMQ_Init failed: ZMQ RC socket creation failed (errno: %d)", errno);
        zmq_close(ctx->kpm_socket);
        zmq_ctx_destroy(ctx->zmq_context);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }
    LOG_INFO("RC socket created successfully");
    
    /* Set socket options */
    result = zmq_setsockopt(ctx->rc_socket, ZMQ_LINGER, &linger, sizeof(linger));
    if (result != 0) {
        LOG_WARN("Warning: Failed to set ZMQ_LINGER option on RC socket (errno: %d)", errno);
    }
    
    int timeout = 5000; /* 5 second receive timeout */
    result = zmq_setsockopt(ctx->rc_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    if (result != 0) {
        LOG_WARN("Warning: Failed to set ZMQ_RCVTIMEO option on RC socket (errno: %d)", errno);
    } else {
        LOG_INFO("RC socket options set successfully");
    }
    
    LOG_INFO("Binding RC socket to: %s", ctx->rc_endpoint);
    if (zmq_bind(ctx->rc_socket, ctx->rc_endpoint) != 0) {
        SetLastError("Failed to bind RC socket");
        LOG_ERROR("RIC_ZMQ_Init failed: ZMQ RC socket binding failed (errno: %d - %s)", 
                 errno, zmq_strerror(errno));
        zmq_close(ctx->rc_socket);
        zmq_close(ctx->kpm_socket);
        zmq_ctx_destroy(ctx->zmq_context);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }
    LOG_INFO("RC socket bound successfully");
    
    LOG_INFO("RIC ZMQ interface initialized successfully:");
    LOG_INFO("  Component: %s (%s)", ctx->component_type_str, ctx->component_id);
    LOG_INFO("  KPM: Publishing to %s", ctx->kpm_endpoint);
    LOG_INFO("  RC: Listening on %s", ctx->rc_endpoint);
    
    return ctx;
}

bool RIC_ZMQ_SetRcCallback(RICZmqHandle handle, RicCommandCallback callback, void* user_data) {
    if (!handle) {
        SetLastError("Invalid handle");
        return false;
    }
    
    pthread_mutex_lock(&handle->mutex);
    handle->rc_callback = callback;
    handle->user_data = user_data;
    pthread_mutex_unlock(&handle->mutex);
    
    LOG_INFO("RC command callback set");
    return true;
}

bool RIC_ZMQ_SendKpmMetric(RICZmqHandle handle, const char* metric_type, const char* metric_value) {
    if (!handle || !metric_type || !metric_value) {
        SetLastError("Invalid parameters (NULL pointers)");
        return false;
    }
    
    /* Create KPM message */
    struct json_object* root = json_object_new_object();
    json_object_object_add(root, "type", json_object_new_string("kpm_metrics"));
    json_object_object_add(root, "component_id", json_object_new_string(handle->component_id));
    json_object_object_add(root, "metric_type", json_object_new_string(metric_type));
    json_object_object_add(root, "metric_value", json_object_new_string(metric_value));
    
    /* Add timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    json_object_object_add(root, "timestamp", json_object_new_int64(timestamp));
    
    /* Get JSON string */
    const char* msg_str = json_object_to_json_string(root);
    
    /* Prefix message with component ID for filtering */
    char* prefixed_msg = (char*)malloc(strlen(handle->component_id) + strlen(msg_str) + 2);
    if (!prefixed_msg) {
        SetLastError("Memory allocation failed");
        json_object_put(root);
        return false;
    }
    
    /* Format: "component_id json_message" */
    sprintf(prefixed_msg, "%s %s", handle->component_id, msg_str);
    
    /* Send message */
    int result = zmq_send_string(handle->kpm_socket, prefixed_msg);
    
    /* Clean up */
    free(prefixed_msg);
    json_object_put(root);
    
    if (result < 0) {
        SetLastError("Failed to send KPM metric");
        return false;
    }
    
    LOG_DEBUG("Sent KPM metric: %s = %s", metric_type, metric_value);
    return true;
}

bool RIC_ZMQ_Start(RICZmqHandle handle) {
    if (!handle) {
        SetLastError("Invalid handle");
        return false;
    }
    
    if (handle->running) {
        return true;  /* Already running */
    }
    
    handle->running = 1;
    
    /* Start listening thread */
    if (pthread_create(&handle->listen_thread, NULL, ListenThreadFunc, handle) != 0) {
        SetLastError("Failed to create listening thread");
        handle->running = 0;
        return false;
    }
    
    LOG_INFO("Started listening for RC commands");
    return true;
}

bool RIC_ZMQ_Stop(RICZmqHandle handle) {
    if (!handle) {
        SetLastError("Invalid handle");
        return false;
    }
    
    if (!handle->running) {
        return true;  /* Already stopped */
    }
    
    handle->running = 0;
    
    /* Wait for thread to finish */
    pthread_join(handle->listen_thread, NULL);
    
    LOG_INFO("Stopped listening");
    return true;
}

void RIC_ZMQ_Free(RICZmqHandle handle) {
    if (!handle) {
        return;
    }
    
    /* Stop if running */
    if (handle->running) {
        RIC_ZMQ_Stop(handle);
    }
    
    /* Clean up sockets */
    zmq_close(handle->kpm_socket);
    zmq_close(handle->rc_socket);
    
    /* Clean up ZMQ context */
    zmq_ctx_destroy(handle->zmq_context);
    
    /* Clean up mutex */
    pthread_mutex_destroy(&handle->mutex);
    
    /* Free the context */
    free(handle);
}

const char* RIC_ZMQ_GetLastError(void) {
    return error_message;
}