#include "log.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <thread>
#include <atomic>
#include <signal.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>  // For htonl, ntohl
#include <zmq.hpp>       // ZeroMQ for user processing module

#define BUFFER_SIZE 2048
#define SOCKET_PATH "/tmp/tun_forwarder_socket"
#define ZMQ_PROCESSOR_REQ "tcp://127.0.0.1:5555"
#define ZMQ_PROCESSOR_REP "tcp://127.0.0.1:5556"

// Module names for logging
#define HOST_MODULE "HostFWD"
#define NS_MODULE "NsFWD"
#define PROC_MODULE "Processor"

// Global flag for clean shutdown
std::atomic<bool> running(true);

// Signal handler for graceful termination
void signal_handler(int signum) {
    LOG_MODULE_INFO(HOST_MODULE, "Received signal " << signum << ", shutting down...");
    running = false;
}

// Open a TUN device
int tun_open(const char* dev_name, const std::string& module) {
    struct ifreq ifr;
    int fd, err;

    // Open the TUN/TAP device
    if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
        LOG_MODULE_ERROR(module, "Cannot open /dev/net/tun: " << strerror(errno));
        return fd;
    }

    // Set up the device
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN device without packet info
    if (dev_name) {
        strncpy(ifr.ifr_name, dev_name, IFNAMSIZ);
    }

    // Create the TUN device
    if ((err = ioctl(fd, TUNSETIFF, (void*)&ifr)) < 0) {
        LOG_MODULE_ERROR(module, "Could not create TUN device: " << strerror(errno));
        close(fd);
        return err;
    }

    LOG_MODULE_INFO(module, "Opened TUN device: " << ifr.ifr_name);
    return fd;
}

// Create a Unix domain socket server
int create_socket_server() {
    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        LOG_MODULE_ERROR(HOST_MODULE, "Socket creation error: " << strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    unlink(SOCKET_PATH);  // Remove if exists

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        LOG_MODULE_ERROR(HOST_MODULE, "Bind error: " << strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) == -1) {
        LOG_MODULE_ERROR(HOST_MODULE, "Listen error: " << strerror(errno));
        close(fd);
        return -1;
    }

    LOG_MODULE_INFO(HOST_MODULE, "Socket server created at " << SOCKET_PATH);
    return fd;
}

// Connect to Unix domain socket server
int connect_socket_client() {
    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        LOG_MODULE_ERROR(NS_MODULE, "Socket creation error: " << strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    // Try to connect multiple times
    int retries = 5;
    while (retries > 0) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            LOG_MODULE_WARN(NS_MODULE, "Connect error: " << strerror(errno));
            retries--;
            LOG_MODULE_INFO(NS_MODULE, "Retrying connection... " << retries << " attempts left");
            sleep(1);
        } else {
            LOG_MODULE_INFO(NS_MODULE, "Connected to socket server");
            return fd;
        }
    }

    close(fd);
    return -1;
}

// Run the user processing module
void run_processor_module() {
    LOG_MODULE_INFO(PROC_MODULE, "Starting packet processor module...");
    
    // Initialize ZMQ context
    zmq::context_t context(1);
    
    // Socket to receive packets from host process
    zmq::socket_t receiver(context, zmq::socket_type::pull);
    receiver.bind(ZMQ_PROCESSOR_REQ);
    
    // Socket to send processed packets back to host process
    zmq::socket_t sender(context, zmq::socket_type::push);
    sender.bind(ZMQ_PROCESSOR_REP);
    
    LOG_MODULE_INFO(PROC_MODULE, "Listening on " << ZMQ_PROCESSOR_REQ);
    LOG_MODULE_INFO(PROC_MODULE, "Sending on " << ZMQ_PROCESSOR_REP);
    
    // Main processing loop
    while (running) {
        zmq::message_t message;
        
        // Try to receive a message
        zmq::pollitem_t items[] = {
            { static_cast<void*>(receiver), 0, ZMQ_POLLIN, 0 }
        };
        
        zmq::poll(items, 1, std::chrono::milliseconds(100));
        
        if (items[0].revents & ZMQ_POLLIN) {
            // Receive the packet
            auto result = receiver.recv(message, zmq::recv_flags::none);
            if (!result) {
                continue;
            }
            
            size_t size = message.size();
            LOG_MODULE_INFO(PROC_MODULE, "Received packet of " << size << " bytes");
            
            // Here you can add your custom packet processing
            // For example, modifying headers, payload, etc.
            // For this simple example, we just pass through the packet
            
            // Just add a simple processing indicator for demonstration
            char* data = static_cast<char*>(message.data());
            if (size > 0) {
                // This is just a demonstration - in a real application, 
                // you would process the packet according to your requirements
                LOG_MODULE_DEBUG(PROC_MODULE, "Processing packet...");
                
                // Don't modify actual packet as it could break IP connectivity
                // Just log that we would do processing here
            }
            
            // Send the processed packet back
            sender.send(message, zmq::send_flags::none);
            LOG_MODULE_INFO(PROC_MODULE, "Sent processed packet of " << size << " bytes");
        }
    }
    
    LOG_MODULE_INFO(PROC_MODULE, "Processor module terminated");
}

// Run the namespace process
int run_namespace_process() {
    LOG_MODULE_INFO(NS_MODULE, "Starting namespace process...");

    // Open the TUN device in namespace
    int tun_fd = tun_open("tun_app", NS_MODULE);
    if (tun_fd < 0) {
        LOG_MODULE_ERROR(NS_MODULE, "Failed to open TUN device in namespace");
        return 1;
    }

    // Connect to the socket server
    int socket_fd = connect_socket_client();
    if (socket_fd < 0) {
        LOG_MODULE_ERROR(NS_MODULE, "Failed to connect to socket server");
        close(tun_fd);
        return 1;
    }

    // Set up buffer for data transfer
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    
    // Main loop for namespace process
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);
        FD_SET(socket_fd, &readfds);
        int maxfd = (tun_fd > socket_fd) ? tun_fd : socket_fd;

        // Wait for data on either the TUN device or the socket
        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal
            LOG_MODULE_ERROR(NS_MODULE, "Select error: " << strerror(errno));
            break;
        }

        // Data available on TUN device, forward to socket
        if (FD_ISSET(tun_fd, &readfds)) {
            int nread = read(tun_fd, buffer, BUFFER_SIZE);
            if (nread < 0) {
                LOG_MODULE_ERROR(NS_MODULE, "Error reading from TUN: " << strerror(errno));
                break;
            }
            
            LOG_MODULE_INFO(NS_MODULE, "Read " << nread << " bytes from TUN");
            
            // Write packet size first, then packet data
            uint32_t size = htonl(nread);
            if (write(socket_fd, &size, sizeof(size)) < 0) {
                LOG_MODULE_ERROR(NS_MODULE, "Error writing size to socket: " << strerror(errno));
                break;
            }
            
            if (write(socket_fd, buffer, nread) < 0) {
                LOG_MODULE_ERROR(NS_MODULE, "Error writing data to socket: " << strerror(errno));
                break;
            }
            
            LOG_MODULE_INFO(NS_MODULE, "Forwarded " << nread << " bytes to host");
        }

        // Data available on socket, forward to TUN device
        if (FD_ISSET(socket_fd, &readfds)) {
            // Read packet size first
            uint32_t size;
            int nread = read(socket_fd, &size, sizeof(size));
            if (nread <= 0) {
                if (nread == 0) {
                    LOG_MODULE_INFO(NS_MODULE, "Socket closed by peer");
                } else {
                    LOG_MODULE_ERROR(NS_MODULE, "Error reading size from socket: " << strerror(errno));
                }
                break;
            }
            
            size = ntohl(size);
            if (size > BUFFER_SIZE) {
                LOG_MODULE_ERROR(NS_MODULE, "Received packet too large: " << size);
                break;
            }
            
            // Read packet data
            nread = read(socket_fd, buffer, size);
            if (nread <= 0) {
                if (nread == 0) {
                    LOG_MODULE_INFO(NS_MODULE, "Socket closed by peer");
                } else {
                    LOG_MODULE_ERROR(NS_MODULE, "Error reading data from socket: " << strerror(errno));
                }
                break;
            }
            
            LOG_MODULE_INFO(NS_MODULE, "Read " << nread << " bytes from socket");
            
            // Write to TUN device
            if (write(tun_fd, buffer, nread) < 0) {
                LOG_MODULE_ERROR(NS_MODULE, "Error writing to TUN: " << strerror(errno));
                break;
            }
            
            LOG_MODULE_INFO(NS_MODULE, "Forwarded " << nread << " bytes to TUN");
        }
    }

    // Clean up
    close(tun_fd);
    close(socket_fd);

    LOG_MODULE_INFO(NS_MODULE, "Namespace process terminated");
    return 0;
}

// Main function
int main(int argc, char* argv[]) {
    // Set up module log levels
    LogManager::setModuleLogLevel(HOST_MODULE, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(NS_MODULE, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(PROC_MODULE, DEBUG_LEVEL);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Check for special run modes
    if (argc > 1) {
        if (std::string(argv[1]) == "--namespace") {
            return run_namespace_process();
        } else if (std::string(argv[1]) == "--processor") {
            run_processor_module();
            return 0;
        }
    }

    // Main host process
    LOG_MODULE_INFO(HOST_MODULE, "Starting host process...");

    // Create socket server for IPC
    int server_fd = create_socket_server();
    if (server_fd < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Failed to create socket server");
        return 1;
    }

    // Open the TUN device in host
    int tun_fd = tun_open("tun_host", HOST_MODULE);
    if (tun_fd < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Failed to open TUN device in host");
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }

    // Initialize ZMQ context for processor communication
    zmq::context_t zmq_context(1);
    
    // Socket to send packets to processor
    zmq::socket_t processor_sender(zmq_context, zmq::socket_type::push);
    processor_sender.connect(ZMQ_PROCESSOR_REQ);
    
    // Socket to receive processed packets
    zmq::socket_t processor_receiver(zmq_context, zmq::socket_type::pull);
    processor_receiver.connect(ZMQ_PROCESSOR_REP);
    
    LOG_MODULE_INFO(HOST_MODULE, "Connected to processor module");

    // Fork the namespace process
    pid_t namespace_pid = fork();
    if (namespace_pid < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Fork failed for namespace process: " << strerror(errno));
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }

    if (namespace_pid == 0) {
        // Child process: exec into namespace
        close(server_fd);  // Close unnecessary descriptor
        close(tun_fd);     // Close unnecessary descriptor
        
        // Execute in namespace
        execlp("ip", "ip", "netns", "exec", "app_ns", argv[0], "--namespace", NULL);
        
        // If we get here, exec failed
        LOG_MODULE_ERROR(NS_MODULE, "Exec failed: " << strerror(errno));
        exit(1);
    }

    // Fork the processor module
    pid_t processor_pid = fork();
    if (processor_pid < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Fork failed for processor module: " << strerror(errno));
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        kill(namespace_pid, SIGTERM);
        waitpid(namespace_pid, NULL, 0);
        return 1;
    }

    if (processor_pid == 0) {
        // Child process: run processor module
        close(server_fd);  // Close unnecessary descriptor
        close(tun_fd);     // Close unnecessary descriptor
        
        // Run processor module
        execlp(argv[0], argv[0], "--processor", NULL);
        
        // If we get here, exec failed
        LOG_MODULE_ERROR(PROC_MODULE, "Exec failed: " << strerror(errno));
        exit(1);
    }

    // Parent process: accept connection from namespace process
    LOG_MODULE_INFO(HOST_MODULE, "Waiting for namespace process to connect...");
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Accept failed: " << strerror(errno));
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        kill(namespace_pid, SIGTERM);
        kill(processor_pid, SIGTERM);
        waitpid(namespace_pid, NULL, 0);
        waitpid(processor_pid, NULL, 0);
        return 1;
    }

    LOG_MODULE_INFO(HOST_MODULE, "Namespace process connected");

    // Set up buffer for data transfer
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    
    // Main loop for host process
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);
        FD_SET(client_fd, &readfds);
        int maxfd = (tun_fd > client_fd) ? tun_fd : client_fd;

        // Set timeout for select
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms
        
        // Wait for data on either the TUN device or the socket
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal
            LOG_MODULE_ERROR(HOST_MODULE, "Select error: " << strerror(errno));
            break;
        }

        // Data available on TUN device, forward to processor then to socket
        if (ready > 0 && FD_ISSET(tun_fd, &readfds)) {
            int nread = read(tun_fd, buffer, BUFFER_SIZE);
            if (nread < 0) {
                LOG_MODULE_ERROR(HOST_MODULE, "Error reading from TUN: " << strerror(errno));
                break;
            }
            
            LOG_MODULE_INFO(HOST_MODULE, "Read " << nread << " bytes from TUN");
            
            // Send to processor module
            zmq::message_t message(buffer, nread);
            processor_sender.send(message, zmq::send_flags::none);
            LOG_MODULE_INFO(HOST_MODULE, "Sent " << nread << " bytes to processor");
            
            // Receive processed packet
            zmq::message_t processed_message;
            
            // Poll with timeout to avoid blocking forever
            zmq::pollitem_t items[] = {
                { static_cast<void*>(processor_receiver), 0, ZMQ_POLLIN, 0 }
            };
            
            zmq::poll(items, 1, std::chrono::seconds(1));
            
            if (items[0].revents & ZMQ_POLLIN) {
                auto result = processor_receiver.recv(processed_message, zmq::recv_flags::none);
                if (result) {
                    nread = processed_message.size();
                    LOG_MODULE_INFO(HOST_MODULE, "Received " << nread << " bytes from processor");
                    
                    // Forward to namespace
                    // Write packet size first, then packet data
                    uint32_t size = htonl(nread);
                    if (write(client_fd, &size, sizeof(size)) < 0) {
                        LOG_MODULE_ERROR(HOST_MODULE, "Error writing size to socket: " << strerror(errno));
                        break;
                    }
                    
                    if (write(client_fd, processed_message.data(), nread) < 0) {
                        LOG_MODULE_ERROR(HOST_MODULE, "Error writing data to socket: " << strerror(errno));
                        break;
                    }
                    
                    LOG_MODULE_INFO(HOST_MODULE, "Forwarded " << nread << " processed bytes to namespace");
                }
            } else {
                LOG_MODULE_WARN(HOST_MODULE, "Timeout waiting for processed packet");
            }
        }

        // Data available on socket (from namespace), forward to TUN device
        if (ready > 0 && FD_ISSET(client_fd, &readfds)) {
            // Read packet size first
            uint32_t size;
            int nread = read(client_fd, &size, sizeof(size));
            if (nread <= 0) {
                if (nread == 0) {
                    LOG_MODULE_INFO(HOST_MODULE, "Socket closed by peer");
                } else {
                    LOG_MODULE_ERROR(HOST_MODULE, "Error reading size from socket: " << strerror(errno));
                }
                break;
            }
            
            size = ntohl(size);
            if (size > BUFFER_SIZE) {
                LOG_MODULE_ERROR(HOST_MODULE, "Received packet too large: " << size);
                break;
            }
            
            // Read packet data
            nread = read(client_fd, buffer, size);
            if (nread <= 0) {
                if (nread == 0) {
                    LOG_MODULE_INFO(HOST_MODULE, "Socket closed by peer");
                } else {
                    LOG_MODULE_ERROR(HOST_MODULE, "Error reading data from socket: " << strerror(errno));
                }
                break;
            }
            
            LOG_MODULE_INFO(HOST_MODULE, "Read " << nread << " bytes from socket (namespace)");
            
            // For uplink, we're not processing - directly write to TUN
            // In the future, you could add processing here too
            if (write(tun_fd, buffer, nread) < 0) {
                LOG_MODULE_ERROR(HOST_MODULE, "Error writing to TUN: " << strerror(errno));
                //break;
            }
            
            LOG_MODULE_INFO(HOST_MODULE, "Forwarded " << nread << " bytes to TUN");
        }
    }

    // Clean up
    close(tun_fd);
    close(client_fd);
    close(server_fd);
    unlink(SOCKET_PATH);

    // Terminate child processes
    LOG_MODULE_INFO(HOST_MODULE, "Terminating child processes...");
    kill(namespace_pid, SIGTERM);
    kill(processor_pid, SIGTERM);
    waitpid(namespace_pid, NULL, 0);
    waitpid(processor_pid, NULL, 0);

    LOG_MODULE_INFO(HOST_MODULE, "Host process terminated");
    return 0;
}