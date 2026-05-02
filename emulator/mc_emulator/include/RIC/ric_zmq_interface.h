/**
 * ric_zmq_interface.h
 * C/C++ compatible ZeroMQ interface for RAN components to communicate with RIC
 */

#ifndef RIC_ZMQ_INTERFACE_H
#define RIC_ZMQ_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * Handle to ZMQ interface instance
 */
typedef struct RICZmqContext* RICZmqHandle;

/**
 * RAN component types
 */
typedef enum {
    RIC_COMPONENT_CU = 0,
    RIC_COMPONENT_DU = 1
} RicComponentType;

/**
 * Message types
 */
typedef enum {
    RIC_MSG_KPM_METRIC = 0,   /* Key Performance Metrics message */
    RIC_MSG_RC_COMMAND = 1,   /* RAN Control command message */
    RIC_MSG_RC_RESPONSE = 2   /* RAN Control response message */
} RicMessageType;

/**
 * Callback function type for receiving RC commands
 * @param command_type String identifying the command type
 * @param command_params JSON string containing command parameters
 * @param user_data User-provided data pointer
 * @return Response string (will be freed by the library)
 */
typedef char* (*RicCommandCallback)(const char* command_type, const char* command_params, void* user_data);

/**
 * Initialize the ZMQ interface
 * @param component_type Type of this RAN component
 * @param component_id Unique identifier for this component
 * @param kpm_port Port for KPM metrics publication
 * @param rc_port Port for RC command reception
 * @param ric_ip IP address of the RIC
 * @param ric_kpm_port RIC's port for receiving KPM metrics
 * @param ric_rc_port RIC's port for sending RC commands
 * @return Handle to the ZMQ interface or NULL on failure
 */
RICZmqHandle RIC_ZMQ_Init(
    RicComponentType component_type,
    const char* component_id,
    int kpm_port,
    int rc_port,
    const char* ric_ip,
    int ric_kpm_port,
    int ric_rc_port
);

/**
 * Set callback for receiving RC commands
 * @param handle ZMQ interface handle
 * @param callback Function to call when an RC command is received
 * @param user_data Pointer to pass to the callback
 * @return true on success, false on failure
 */
bool RIC_ZMQ_SetRcCallback(RICZmqHandle handle, RicCommandCallback callback, void* user_data);

/**
 * Send a KPM metric to the RIC
 * @param handle ZMQ interface handle
 * @param metric_type String identifying the metric type
 * @param metric_value String containing the metric value
 * @return true on success, false on failure
 */
bool RIC_ZMQ_SendKpmMetric(RICZmqHandle handle, const char* metric_type, const char* metric_value);

/**
 * Start the ZMQ interface
 * This will start a background thread to listen for RC commands
 * @param handle ZMQ interface handle
 * @return true on success, false on failure
 */
bool RIC_ZMQ_Start(RICZmqHandle handle);

/**
 * Stop the ZMQ interface
 * This will stop the background thread and clean up resources
 * @param handle ZMQ interface handle
 * @return true on success, false on failure
 */
bool RIC_ZMQ_Stop(RICZmqHandle handle);

/**
 * Free the ZMQ interface
 * This will stop the interface if it's running and free all resources
 * @param handle ZMQ interface handle
 */
void RIC_ZMQ_Free(RICZmqHandle handle);

/**
 * Get the last error message
 * @return Error message string
 */
const char* RIC_ZMQ_GetLastError(void);

#ifdef __cplusplus
}
#endif

#endif /* RIC_ZMQ_INTERFACE_H */