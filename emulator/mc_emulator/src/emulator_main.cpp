#include "../utils/log.hpp"
#include "../include/pdcp_processor.hpp"
#include "pdcp_config.h"
#include "config_parser.hpp"
#include "pdcp_common.hpp"
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
#include <netinet/in.h>

// Define module names for logging
#define HOST_MODULE "HostFWD"
#define NS_MODULE "NsFWD"
#define PROC_MODULE "Processor"

#define BUFFER_SIZE 2048
#define SOCKET_PATH "/tmp/tun_forwarder_socket"

// Global flag for clean shutdown
std::atomic<bool> running(true);

// Pointer to PDCP processor
std::unique_ptr<PdcpProcessor> pdcp_processor;

// Add this near the top of your file
namespace pdcp {
    // Dumps packet in hex format for debugging
    void dumpPacketHex(const unsigned char* packet, size_t len) {
        std::stringstream ss;
        ss << "Packet dump (" << len << " bytes):\n";
        
        for (size_t i = 0; i < len; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", packet[i]);
            ss << buf;
            
            if ((i + 1) % 16 == 0) {
                ss << "\n";
            } else if ((i + 1) % 8 == 0) {
                ss << " ";
            }
        }
        
        LOG_MODULE_INFO(NS_MODULE, ss.str());
    }
}

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
            
            // Validate packet before writing to TUN device
            if (nread < 20) {
                LOG_MODULE_ERROR(NS_MODULE, "Packet too small to be a valid IP packet: " << nread << " bytes");
                continue;
            }
            
            // Examine the first byte to ensure it's an IPv4 packet
            unsigned char version = buffer[0] >> 4;
            if (version != 4) {
                LOG_MODULE_ERROR(NS_MODULE, "Not an IPv4 packet (version=" << (int)version << ")");
                pdcp::dumpPacketHex((unsigned char*)buffer, nread < 64 ? nread : 64);
                continue;
            }
            
            // Write to TUN device
            if (write(tun_fd, buffer, nread) < 0) {
                LOG_MODULE_ERROR(NS_MODULE, "Error writing to TUN: " << strerror(errno) 
                            << " (errno=" << errno << ") packet size=" << nread);
                
                // Dump first 64 bytes of the packet for debugging
                pdcp::dumpPacketHex((unsigned char*)buffer, nread < 64 ? nread : 64);
                continue;
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
    // [Optionally parse a config file name from CLI, or use a default]
    std::string configFile = "config.json";  // default
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            configFile = argv[i + 1];
            break;
        }
    }

    // [Load the JSON config]
    ConfigParser configParser;
    if (!configParser.loadConfig(configFile)) {
        std::cerr << "[ERROR] Failed to load config from " << configFile 
                  << ". Using built-in defaults.\n";
    }

    // Set up module log levels
    /*
    LogManager::setModuleLogLevel(HOST_MODULE, ERROR_LEVEL);
    LogManager::setModuleLogLevel(NS_MODULE, ERROR_LEVEL);
    LogManager::setModuleLogLevel(PROC_MODULE, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(MODULE_SENDER, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(MODULE_DU, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(MODULE_UE, DEBUG_LEVEL);
    LogManager::setModuleLogLevel(MODULE_RECEIVER, DEBUG_LEVEL);
    */
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // [Read PDCP config from JSON. For example, we pass linkId=0 or 1, etc. Customize as needed.]
    //  If you have only one link in your scenario, you might just pick linkId=1:
    PdcpConfig pdcpConfig = configParser.getPdcpConfig(/* linkId = */ 1);

    // Check for special run modes
    if (argc > 1) {
        if (std::string(argv[1]) == "--namespace") {
            return run_namespace_process();
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

    // Initialize PDCP processor
    pdcp_processor = PdcpProcessor::create();
    pdcp_processor->configure(&configParser);
    if (!pdcp_processor->initialize()) {
        LOG_MODULE_ERROR(HOST_MODULE, "Failed to initialize PDCP processor");
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }
    
    if (!pdcp_processor->start()) {
        LOG_MODULE_ERROR(HOST_MODULE, "Failed to start PDCP processor");
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        return 1;
    }
    
    LOG_MODULE_INFO(HOST_MODULE, "PDCP processor started");

    // Fork the namespace process
    pid_t namespace_pid = fork();
    if (namespace_pid < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Fork failed for namespace process: " << strerror(errno));
        pdcp_processor->stop();
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

    // Parent process: accept connection from namespace process
    LOG_MODULE_INFO(HOST_MODULE, "Waiting for namespace process to connect...");
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        LOG_MODULE_ERROR(HOST_MODULE, "Accept failed: " << strerror(errno));
        pdcp_processor->stop();
        close(tun_fd);
        close(server_fd);
        unlink(SOCKET_PATH);
        kill(namespace_pid, SIGTERM);
        waitpid(namespace_pid, NULL, 0);
        return 1;
    }

    LOG_MODULE_INFO(HOST_MODULE, "Namespace process connected");

    // Register socket handlers with the PDCP processor
    pdcp_processor->registerDownlinkSendHandler(client_fd);
    pdcp_processor->registerUplinkSendHandler(tun_fd);
    
    // Set up buffer for data transfer
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    
    // Main loop for host process
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);
        FD_SET(client_fd, &readfds);
        int maxfd = (tun_fd > client_fd) ? tun_fd : client_fd;


        // Wait for data on either the TUN device or the socket
        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal
            LOG_MODULE_ERROR(HOST_MODULE, "Select error: " << strerror(errno));
            break;
        }

        // Data available on TUN device, process and forward to socket
        if (ready > 0 && FD_ISSET(tun_fd, &readfds)) {
            int nread = read(tun_fd, buffer, BUFFER_SIZE);
            if (nread < 0) {
                LOG_MODULE_ERROR(HOST_MODULE, "Error reading from TUN: " << strerror(errno));
                break;
            }
            
            LOG_MODULE_INFO(HOST_MODULE, "Read " << nread << " bytes from TUN");
            
            // Process with PDCP modules (downlink)
            size_t processed_size = pdcp_processor->processDownlink(
                reinterpret_cast<unsigned char*>(buffer), nread);
            
            // If processDownlink returns > 0, it means we should forward the packet immediately
            // (This would only happen in pass-through mode without actual PDCP processing)
            if (processed_size > 0) {
                // Forward to namespace
                // Write packet size first, then packet data
                uint32_t size = htonl(processed_size);
                if (write(client_fd, &size, sizeof(size)) < 0) {
                    LOG_MODULE_ERROR(HOST_MODULE, "Error writing size to socket: " << strerror(errno));
                    break;
                }
                
                if (write(client_fd, buffer, processed_size) < 0) {
                    LOG_MODULE_ERROR(HOST_MODULE, "Error writing data to socket: " << strerror(errno));
                    break;
                }
                
                LOG_MODULE_INFO(HOST_MODULE, "Forwarded " << processed_size << " bytes to namespace (direct)");
            }
            // If processDownlink returns 0, the cellular stack is handling it
            // The packet will be sent when processing is complete via the registered send handler
        }

        // Data available on socket (from namespace), process and forward to TUN
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
            
            // Process with PDCP modules (uplink)
            size_t processed_size = pdcp_processor->processUplink(
                reinterpret_cast<unsigned char*>(buffer), nread);
            
            // If processUplink returns > 0, it means we should forward the packet immediately
            // (This would only happen in pass-through mode without actual PDCP processing)
            if (processed_size > 0) {
                // Write to TUN device
                if (write(tun_fd, buffer, processed_size) < 0) {
                    LOG_MODULE_ERROR(HOST_MODULE, "Error writing to TUN: " << strerror(errno));
                    break;
                }
                
                LOG_MODULE_INFO(HOST_MODULE, "Forwarded " << processed_size << " bytes to TUN (direct)");
            }
            // If processUplink returns 0, the cellular stack is handling it
            // The packet will be sent when processing is complete via the registered send handler
        }
    }

    // Clean up
    pdcp_processor->stop();
    close(tun_fd);
    close(client_fd);
    close(server_fd);
    unlink(SOCKET_PATH);

    // Terminate namespace process
    LOG_MODULE_INFO(HOST_MODULE, "Terminating namespace process...");
    kill(namespace_pid, SIGTERM);
    waitpid(namespace_pid, NULL, 0);

    LOG_MODULE_INFO(HOST_MODULE, "Host process terminated");
    return 0;
}