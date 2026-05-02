#include "log.hpp"
#include <iostream>
#include <string>
#include <signal.h>
#include <atomic>
#include <zmq.hpp>
#include <unistd.h>

// Define module name for logging
#define PROC_MODULE "Processor"

// ZMQ endpoints - CONNECT to these (not bind)
// These must match what the tun_forwarder has BOUND to
#define ZMQ_PROCESSOR_IN "tcp://127.0.0.1:5555"  // Connect to this to receive packets
#define ZMQ_PROCESSOR_OUT "tcp://127.0.0.1:5556" // Connect to this to send packets

// Global flag for clean shutdown
std::atomic<bool> running(true);
zmq::context_t* global_context = nullptr;
zmq::socket_t* global_receiver = nullptr;
zmq::socket_t* global_sender = nullptr;

// Signal handler for graceful termination
void signal_handler(int signum) {
    LOG_MODULE_INFO(PROC_MODULE, "Received signal " << signum << ", shutting down...");
    running = false;
}

// Function to clean up ZMQ resources
void cleanup_resources() {
    LOG_MODULE_INFO(PROC_MODULE, "Cleaning up resources...");
    
    // Close sockets first, then context
    if (global_receiver) {
        try {
            global_receiver->close();
            delete global_receiver;
            global_receiver = nullptr;
            LOG_MODULE_INFO(PROC_MODULE, "Closed receiver socket");
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(PROC_MODULE, "Error closing receiver: " << e.what());
        }
    }
    
    if (global_sender) {
        try {
            global_sender->close();
            delete global_sender;
            global_sender = nullptr;
            LOG_MODULE_INFO(PROC_MODULE, "Closed sender socket");
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(PROC_MODULE, "Error closing sender: " << e.what());
        }
    }
    
    if (global_context) {
        try {
            global_context->close();
            delete global_context;
            global_context = nullptr;
            LOG_MODULE_INFO(PROC_MODULE, "Closed ZMQ context");
        } catch (const std::exception& e) {
            LOG_MODULE_ERROR(PROC_MODULE, "Error closing context: " << e.what());
        }
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Register cleanup function
    std::atexit(cleanup_resources);
    
    // Set up logging
    LogManager::setModuleLogLevel(PROC_MODULE, DEBUG_LEVEL);
    
    LOG_MODULE_INFO(PROC_MODULE, "Starting simple packet processor...");
    
    try {
        // Add a small delay to ensure stability
        usleep(500000); // 500ms
        
        // Initialize ZMQ context
        global_context = new zmq::context_t(1);
        
        // Set socket options to prevent address reuse problems
        int linger = 0;
        
        // Socket to receive packets from TUN forwarder
        global_receiver = new zmq::socket_t(*global_context, zmq::socket_type::pull);
        global_receiver->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
        
        // Socket to send processed packets back to TUN forwarder
        global_sender = new zmq::socket_t(*global_context, zmq::socket_type::push);
        global_sender->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
        
        // IMPORTANT: CONNECT to the forwarder's sockets (don't bind)
        try {
            global_receiver->connect(ZMQ_PROCESSOR_IN);
            LOG_MODULE_INFO(PROC_MODULE, "Connected receiver to " << ZMQ_PROCESSOR_IN);
        } catch (const zmq::error_t& e) {
            LOG_MODULE_ERROR(PROC_MODULE, "Failed to connect to " << ZMQ_PROCESSOR_IN << ": " << e.what());
            throw;
        }
        
        try {
            global_sender->connect(ZMQ_PROCESSOR_OUT);
            LOG_MODULE_INFO(PROC_MODULE, "Connected sender to " << ZMQ_PROCESSOR_OUT);
        } catch (const zmq::error_t& e) {
            LOG_MODULE_ERROR(PROC_MODULE, "Failed to connect to " << ZMQ_PROCESSOR_OUT << ": " << e.what());
            throw;
        }
        
        LOG_MODULE_INFO(PROC_MODULE, "Receiving packets from " << ZMQ_PROCESSOR_IN);
        LOG_MODULE_INFO(PROC_MODULE, "Sending processed packets to " << ZMQ_PROCESSOR_OUT);
        
        // Main processing loop
        while (running) {
            zmq::message_t message;
            
            // Set up poll item for the receiver socket
            zmq::pollitem_t items[] = {
                { static_cast<void*>(*global_receiver), 0, ZMQ_POLLIN, 0 }
            };
            
            // Poll with timeout
            zmq::poll(items, 1, std::chrono::milliseconds(100));
            
            // Check for shutdown
            if (!running) break;
            
            // Check if there's a message to receive
            if (items[0].revents & ZMQ_POLLIN) {
                // Receive the packet
                auto result = global_receiver->recv(message, zmq::recv_flags::none);
                if (!result) {
                    LOG_MODULE_WARN(PROC_MODULE, "Failed to receive message");
                    continue;
                }
                
                size_t size = message.size();
                LOG_MODULE_INFO(PROC_MODULE, "Received packet of " << size << " bytes");
                
                // Here you would typically process the packet
                // For this simple example, we're just passing it through
                LOG_MODULE_DEBUG(PROC_MODULE, "Passing packet through without modification");
                
                // Send the packet back to the TUN forwarder
                global_sender->send(message, zmq::send_flags::none);
                LOG_MODULE_INFO(PROC_MODULE, "Sent packet of " << size << " bytes back to forwarder");
            }
        }
        
    } catch (const zmq::error_t& e) {
        LOG_MODULE_ERROR(PROC_MODULE, "ZMQ error: " << e.what());
        cleanup_resources();
        return 1;
    } catch (const std::exception& e) {
        LOG_MODULE_ERROR(PROC_MODULE, "Exception: " << e.what());
        cleanup_resources();
        return 1;
    }
    
    LOG_MODULE_INFO(PROC_MODULE, "Processor terminated");
    return 0;
}