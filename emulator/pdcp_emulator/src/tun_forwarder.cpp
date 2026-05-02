#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>     // setns()
#include <cstring>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <signal.h>
#include <cstdint>
#include <sys/socket.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

// Include PDCP modules
#include "pdcp_common.hpp"
#include "pdcp_sender.hpp"
#include "pdcp_link.hpp"
#include "pdcp_receiver.hpp"
#include "UE.hpp"

#include "log.h"
#include "pdcp_config.h"

// Maximum packet size
#define MAX_PACKET_SIZE 4096
// Maximum number of links
#define MAX_LINKS 4

// Structure to hold link information
class LinkInfo {
public:
    int fd;                       // TUN file descriptor
    std::string namespace_name;   // Namespace name
    std::string interface_name;   // Interface name
    uint32_t in_packets;          // Counter for incoming packets
    uint32_t out_packets;         // Counter for outgoing packets
    uint64_t in_bytes;            // Counter for incoming bytes
    uint64_t out_bytes;           // Counter for outgoing bytes
    
    LinkInfo() : fd(-1), in_packets(0), out_packets(0), in_bytes(0), out_bytes(0) {}
};

//////////////////////////////////////////////////////////////////////////////
// Global variables
//////////////////////////////////////////////////////////////////////////////

// Global flag to handle clean shutdown
volatile sig_atomic_t running = 1;
// Global routing state for round-robin
volatile uint8_t current_link = 0;

// PDCP context - only one per forwarder instance
std::unique_ptr<PdcpContext> pdcp_ctx;
bool pdcp_enabled = true;  // Always enabled in this version

std::unique_ptr<UE> ue_instance;

//////////////////////////////////////////////////////////////////////////////
// Forward declarations
//////////////////////////////////////////////////////////////////////////////

void init_pdcp_context(const std::string& role, int link_id = 0, const std::string& folderName =".");
static int enter_namespace(const std::string& ns_name);
static int open_tun_device(const std::string& tun_name);
static int run_multi_link_forwarder(const std::string& sender_ns, const std::string& sender_tun, 
                                  uint8_t num_links, const std::vector<std::string>& link_ns,
                                  const std::vector<std::string>& link_tun, const std::string& folderName);
static ssize_t process_packet_userspace(unsigned char *packet, size_t len, const std::string& direction, bool is_outgoing);

//////////////////////////////////////////////////////////////////////////////
// Utility functions
//////////////////////////////////////////////////////////////////////////////

// Signal handler for clean shutdown
void signal_handler(int signum) {
    (void)signum; // Suppress unused parameter warning
    running = 0;
}

/**
 * Check packet for being outgoing (true) or incoming (false)
 * Modified to check specifically for CU IP (10.100.0.1)
 */
static bool is_packet_outgoing(const unsigned char *packet, size_t len) {
    if (len < 20) {
        return false; // Can't determine, assume incoming
    }
    
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *) packet;
    uint32_t saddr_host = ntohl(iph->saddr);
    uint32_t daddr_host = ntohl(iph->daddr);
    // Source IP is from the sender's interface
    if ((saddr_host & 0xFFFFFF00) == 0x0A640000) {
        LOG_DEBUG("[PDCP Link] Outgoing, length: " << len
                  << ", saddr=" << ((saddr_host >> 24) & 0xFF) << "."
                                 << ((saddr_host >> 16) & 0xFF) << "."
                                 << ((saddr_host >>  8) & 0xFF) << "."
                                 << ( saddr_host        & 0xFF )
                  << ", daddr=" << ((daddr_host >> 24) & 0xFF) << "."
                                 << ((daddr_host >> 16) & 0xFF) << "."
                                 << ((daddr_host >>  8) & 0xFF) << "."
                                 << ( daddr_host        & 0xFF ));

        return true;
    }


    LOG_DEBUG("[PDCP Link] Incoming, length: " << len
                << ", saddr=" << ((saddr_host >> 24) & 0xFF) << "."
                                << ((saddr_host >> 16) & 0xFF) << "."
                                << ((saddr_host >>  8) & 0xFF) << "."
                                << ( saddr_host        & 0xFF )
                << ", daddr=" << ((daddr_host >> 24) & 0xFF) << "."
                                << ((daddr_host >> 16) & 0xFF) << "."
                                << ((daddr_host >>  8) & 0xFF) << "."
                                << ( daddr_host        & 0xFF ));
    
    return false;
}

/**
 * Enter a network namespace
 */
static int enter_namespace(const std::string& ns_name) {
    std::string path = "/var/run/netns/" + ns_name;
    
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open netns");
        return -1;
    }
    
    if (setns(fd, CLONE_NEWNET) < 0) {
        perror("setns");
        close(fd);
        return -1;
    }
    
    close(fd);
    return 0;
}

/**
 * Open a TUN device
 */
static int open_tun_device(const std::string& tun_name) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI; // Layer 3 mode, no packet info
    strncpy(ifr.ifr_name, tun_name.c_str(), IFNAMSIZ - 1);
    
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }
    
    return fd;
}

//////////////////////////////////////////////////////////////////////////////
// Packet processing functions
//////////////////////////////////////////////////////////////////////////////

/**
 * Callback function to deliver PDCP ordered packets
 */
void deliver_pdcp_ordered_packets(int tunFd, PdcpReceiver* receiver)
{
    if (!receiver) return;

    unsigned char packet[MAX_PACKET_SIZE];

    bool gotOne = true;
    while (gotOne) {
        size_t delivered_len = 0;
        gotOne = receiver->deliverPacket(packet, delivered_len);
        if (gotOne && delivered_len > 0) {
            // Forward if not dropped
            ssize_t bytes_written = write(tunFd, packet, delivered_len);
            if (bytes_written < 0) {
                perror("write to TUN in deliver_processed_packets");
            } else {
                LOG_DEBUG("[PDCP Receiver] Delivered buffered packet, length: "
                          << delivered_len << " to TUN fd=" << tunFd);
            }
        }
    }
}


/**
 * Callback function to deliver RLC packets (RLC -> PDCP)
 */
void deliver_rlc_packet(int tunFd, const unsigned char * rlc_packet, size_t delivered_len) {
    if (!rlc_packet || delivered_len == 0) return;

    ssize_t bytes_written = write(tunFd, rlc_packet, delivered_len);
    if (bytes_written < 0) {
        perror("write to TUN in deliver_rlc_packet");
    } else {
        LOG_DEBUG("[PDCP Link] Delivered RLC packet, length: "
                  << delivered_len << " to TUN fd=" << tunFd);
    }
}

/**
 * Callback function to deliver PHY packets
 */
void deliver_phy_packet(int tunFd, const unsigned char * phy_packet, size_t delivered_len) {
    if (!phy_packet || delivered_len == 0) return;

    ssize_t bytes_written = write(tunFd, phy_packet, delivered_len);
    if (bytes_written < 0) {
        perror("write to TUN in deliver_phy_packet");
    } else {
        LOG_DEBUG("[PDCP Link] Delivered PHY packet, length: "
                  << delivered_len << " to TUN fd=" << tunFd);
    }
}

/**
 * User space packet processing function
 * 
 * This function processes packets based on the 5G role and traffic direction.
 * CU->DU->UE_stack->UE_PDCP is the downlink path (outgoing = true)
 * UE_PDCP->UE_stack->DU->CU is the uplink path (outgoing = false)
 * 
 * It returns:
 * - Modified packet length if packet should be forwarded
 * - 0 if packet should be dropped
 * - -1 on error
 */


static ssize_t process_packet_userspace(unsigned char *packet, size_t len, const std::string& direction, bool is_outgoing) {

    // Basic validation
    if (len < 20) {
        LOG_ERROR("[" << direction << "] Packet too short to process: " << len << " bytes");
        return 0; // Drop packet
    }
    
    // Print packet info for debugging
    pdcp::dumpIpPacket(packet, len, direction);
    is_outgoing = is_packet_outgoing(packet, len);
    
    // Only apply PDCP processing for outgoing packets
    if (is_outgoing) {
        // Apply PDCP processing if enabled
        ssize_t new_len = len;
        
        if (ue_instance) {
            // Process the packet using the initialized UE instance
            bool processed = ue_instance->processPhyPacket(packet, len);
            if (!processed) {
                LOG_WARN("[UE] Packet processing failed, dropping packet");
            } 
            new_len = 0; // Packet was consumed by UE
        } else if (pdcp_enabled && pdcp_ctx) {
            // Process the packet using the initialized PDCP context
            new_len = pdcp_ctx->processPacket(packet, len);
        }
        
        // Default processing for packets that don't match any PDCP role
        if (static_cast<size_t>(new_len) == len) {
            struct ipv4_hdr *iph = (struct ipv4_hdr *)packet;
            
            // Only process IPv4 packets
            if (iph->version == 4) {
                // Default TTL adjustment
                iph->ttl = 64;
                
                // Recalculate checksum
                iph->check = 0;
                iph->check = pdcp::calculateIpChecksum(iph, iph->ihl * 4);
            }
        }
        
        return new_len;
    } else {
        // For incoming packets, just pass them through without any processing
        struct ipv4_hdr *iph = (struct ipv4_hdr *)packet;
        
        // Basic IPv4 validations (optional)
        if (iph->version == 4) {
            // You can still do minimal processing if needed
            // For example, you might want to ensure the TTL is valid
            if (iph->ttl <= 0) {
                iph->ttl = 64;
                // Recalculate checksum
                iph->check = 0;
                iph->check = pdcp::calculateIpChecksum(iph, iph->ihl * 4);
            }
        }
        
        return len; // Return original length for incoming packets
    }
}

/**
 * Select next link in round-robin fashion
 */
static uint8_t get_next_link(uint8_t num_links) {
    //current_link = (current_link + 1) % num_links;
    return current_link;
}

//////////////////////////////////////////////////////////////////////////////
// Protocol stack initialization
//////////////////////////////////////////////////////////////////////////////

/**
 * Initialize PDCP context for the appropriate role
 * Modified to support 5G roles: CU, DU, UE_stack, UE_PDCP
 */
void init_pdcp_context(const std::string& role, int link_id, const std::string& folderName) {
    PdcpConfig cfg;
    cfg.common.logFoldername = folderName;

    if (role == "cu") {
        // CU role - initialize as sender
        cfg.common.role = PdcpRole::SENDER;
        pdcp_ctx = std::make_unique<PdcpSender>(cfg);
        LOG_INFO("[PDCP] Initialized CU (sender) context");
    } 
    else if (role.find("du") != std::string::npos) {
        // DU role - initialize as link
        cfg.common.role = PdcpRole::LINK;
        pdcp_ctx = std::make_unique<PdcpLink>(cfg);
        LOG_INFO("[PDCP] Initialized DU (link) context for DU " << link_id);
    } 
    else if (role == "ue_stack") {
        // UE_stack role - initialize UE with full protocol stack
        UEConfig ueConfig;
        ueConfig.logFolder = folderName;
        ueConfig.pdcpReorderTimeoutMs = cfg.receiver.reorderTimeoutMs;
        ueConfig.rlcBufferSize = 100;
        ueConfig.rlcReassemblyTimeoutMs = 1000; // 1 second timeout
        
        ue_instance = std::make_unique<UE>(ueConfig);
        LOG_INFO("[UE] Initialized UE stack with full PHY/MAC/RLC protocol stack");
    }
    else if (role == "ue_pdcp") {
        // UE_PDCP role - initialize PDCP receiver
        cfg.common.role = PdcpRole::RECEIVER;
        pdcp_ctx = std::make_unique<PdcpReceiver>(cfg);
        LOG_INFO("[PDCP] Initialized UE_PDCP (receiver) context");
    }
    else {
        LOG_INFO("[PDCP] Unknown role '" << role << "', no PDCP context initialized");
        pdcp_enabled = false;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Forwarder operation modes
//////////////////////////////////////////////////////////////////////////////

/**
 * Main function for the multi-link forwarder (CU to DUs)
 * This mode connects a CU to multiple DUs for multi-connectivity
 */
static int run_multi_link_forwarder(const std::string& sender_ns, const std::string& sender_tun, 
                                  uint8_t num_links, const std::vector<std::string>& link_ns,
                                  const std::vector<std::string>& link_tun, const std::string& folderName) {
    std::vector<LinkInfo> links(num_links);
    int sender_fd = -1;
    int host_fd = -1;
    
    // Save host namespace
    host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }
    
    // Open sender TUN
    LOG_INFO("[*] Entering namespace: " << sender_ns);
    if (enter_namespace(sender_ns) < 0) {
        close(host_fd);
        return 1;
    }
    
    sender_fd = open_tun_device(sender_tun);
    if (sender_fd < 0) {
        LOG_INFO("Failed to open TUN '" << sender_tun << "' in namespace '" 
                 << sender_ns << "'");
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened CU TUN: " << sender_tun << " (fd=" << sender_fd << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(sender_fd);
        close(host_fd);
        return 1;
    }
    
    // Open each link TUN
    for (uint8_t i = 0; i < num_links; i++) {
        links[i].namespace_name = link_ns[i];
        links[i].interface_name = link_tun[i];
        
        LOG_INFO("[*] Entering namespace: " << links[i].namespace_name);
        if (enter_namespace(links[i].namespace_name) < 0) {
            // Cleanup already opened FDs
            close(sender_fd);
            for (uint8_t j = 0; j < i; j++) {
                close(links[j].fd);
            }
            close(host_fd);
            return 1;
        }
        
        links[i].fd = open_tun_device(links[i].interface_name);
        if (links[i].fd < 0) {
            LOG_INFO("Failed to open TUN '" << links[i].interface_name 
                     << "' in namespace '" << links[i].namespace_name << "'");
            // Cleanup
            close(sender_fd);
            for (uint8_t j = 0; j < i; j++) {
                close(links[j].fd);
            }
            close(host_fd);
            return 1;
        }
        LOG_INFO("[*] Opened DU TUN in " << links[i].namespace_name << ": " 
                 << links[i].interface_name << " (fd=" << links[i].fd << ")");
        
        // Return to host namespace
        if (setns(host_fd, CLONE_NEWNET) < 0) {
            perror("setns(host_fd)");
            close(sender_fd);
            for (uint8_t j = 0; j <= i; j++) {
                close(links[j].fd);
            }
            close(host_fd);
            return 1;
        }
    }
    
    // Initialize PDCP context for CU role
    init_pdcp_context("cu", 0, folderName);
    
    // Start forwarding
    LOG_INFO("[*] Starting multi-link forwarding");
    LOG_INFO("[*] Forwarding " << sender_ns << ":" << sender_tun << " <-> multiple DUs");
    LOG_INFO("[*] Using round-robin routing for outgoing traffic");
    LOG_INFO("[*] Press Ctrl+C to stop");
    
    unsigned char buffer[MAX_PACKET_SIZE];
    fd_set readfds;
    
    // For statistics
    time_t last_stats_time = time(NULL);
    
    // Main forwarding loop
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(sender_fd, &readfds);
        
        int max_fd = sender_fd;
        for (uint8_t i = 0; i < num_links; i++) {
            FD_SET(links[i].fd, &readfds);
            if (links[i].fd > max_fd) {
                max_fd = links[i].fd;
            }
        }
        
        // Wait for data on any interface
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal - likely Ctrl+C
                continue;
            }
            perror("select");
            break;
        }
        
        // Handle packets from CU (outgoing traffic)
        if (FD_ISSET(sender_fd, &readfds)) {
            ssize_t n = read(sender_fd, buffer, sizeof(buffer));
            if (n > 0) {
                bool outgoing = true; // Traffic from CU is always outgoing
                
                // Round-robin load balancing for outgoing traffic
                uint8_t link_index = get_next_link(num_links);
                
                // Process the packet in userspace (outgoing traffic)
                std::string direction = "CU->DU" + std::to_string(link_index + 1);
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(links[link_index].fd, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to DU");
                    } else {
                        // Update statistics
                        links[link_index].out_packets++;
                        links[link_index].out_bytes += processed_len;
                    }
                }
            }
        }
        
        // Handle packets from each DU (incoming traffic)
        for (uint8_t i = 0; i < num_links; i++) {
            if (FD_ISSET(links[i].fd, &readfds)) {
                ssize_t n = read(links[i].fd, buffer, sizeof(buffer));
                if (n > 0) {
                    bool outgoing = false; // Traffic from DUs is always incoming
                    
                    // Process the packet
                    std::string direction = "DU" + std::to_string(i + 1) + "->CU";
                    ssize_t processed_len = process_packet_userspace(buffer, n, direction, outgoing);
                    
                    // Forward if not dropped
                    if (processed_len > 0) {
                        ssize_t bytes_written = write(sender_fd, buffer, processed_len);
                        if (bytes_written < 0) {
                            perror("write to CU");
                        } else {
                            // Update statistics
                            links[i].in_packets++;
                            links[i].in_bytes += processed_len;
                        }
                    }
                }
            }
        }
        
        // Print statistics every 10 seconds
        time_t now = time(NULL);
        if (now - last_stats_time >= 10) {
            LOG_DEBUG("===== TRAFFIC STATISTICS =====");
            LOG_DEBUG("DU      | In (pkts/bytes) | Out (pkts/bytes)");
            LOG_DEBUG("---------+-----------------+------------------");
            
            for (uint8_t i = 0; i < num_links; i++) {
                LOG_DEBUG("DU" << (i+1) << "     | " 
                         << links[i].in_packets << "/" << links[i].in_bytes << " | "
                         << links[i].out_packets << "/" << links[i].out_bytes);
            }
            
            LOG_DEBUG("===============================");
            last_stats_time = now;
        }
    }
    
    // Clean shutdown
    LOG_INFO("[*] Shutting down multi-link forwarder...");
    close(sender_fd);
    for (uint8_t i = 0; i < num_links; i++) {
        close(links[i].fd);
    }
    close(host_fd);
    
    return 0;
}

/**
 * Run the DU forwarder mode
 * This mode implements the DU functionality between CU and UE_stack
 */
static int run_du_forwarder(const std::string& du_ns, const std::string& du_tun, 
                          const std::string& connected_ns, const std::string& connected_tun, 
                          const std::string& folderName) {
    LOG_INFO("[*] Running in DU mode between " << du_ns << " and " << connected_ns);
    
    // Determine if connected to CU or UE_stack based on namespace name
    bool connected_to_cu = (connected_ns == "CU" || connected_ns.find("node_sender") != std::string::npos);
    
    // Save host namespace
    int host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }
    
    // Enter DU namespace
    LOG_INFO("[*] Entering namespace: " << du_ns);
    if (enter_namespace(du_ns) < 0) {
        close(host_fd);
        return 1;
    }
    
    // Open DU TUN interface
    int fd1 = open_tun_device(du_tun);
    if (fd1 < 0) {
        LOG_ERROR("Failed to open TUN '" << du_tun << "' in namespace '" << du_ns << "'");
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened DU TUN in " << du_ns << ": " << du_tun << " (fd=" << fd1 << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    // Enter connected namespace (CU or UE_stack)
    LOG_INFO("[*] Entering namespace: " << connected_ns);
    if (enter_namespace(connected_ns) < 0) {
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    // Open connected TUN interface
    int fd2 = open_tun_device(connected_tun);
    if (fd2 < 0) {
        LOG_ERROR("Failed to open TUN '" << connected_tun << "' in namespace '" << connected_ns << "'");
        close(fd1);
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened TUN in " << connected_ns << ": " << connected_tun << " (fd=" << fd2 << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(fd2);
        close(host_fd);
        return 1;
    }
    
    // Extract DU number from namespace name
    int du_id = 0;
    if (du_ns.find("DU") != std::string::npos) {
        std::string du_num = du_ns.substr(2); // after "DU"
        try {
            du_id = std::stoi(du_num);
        } catch (...) {
            du_id = 0;
        }
    }
    
    // Initialize DU protocol stack
    init_pdcp_context("du", du_id, folderName);
    
    // Set up the PHY delivery callback for DU
    PdcpLink* link = dynamic_cast<PdcpLink*>(pdcp_ctx.get());
    if (link) {
        // The delivery callback for PHY layer
        link->getPhyModule()->setDeliveryCallback([fd1](const unsigned char* packet, size_t len) {
            deliver_phy_packet(fd1, packet, len);
        });
        LOG_INFO("[*] Set up delivery callback for PHY layer to fd1 (TUN: " << du_tun << ")");
    }
    
    // Start forwarding
    LOG_INFO("[*] Starting packet forwarding with DU functionality");
    LOG_INFO("[*] Forwarding " << du_ns << ":" << du_tun << " <-> " << connected_ns << ":" << connected_tun);
    LOG_INFO("[*] DU ID: " << du_id);
    LOG_INFO("[*] Press Ctrl+C to stop");
    
    unsigned char buffer[MAX_PACKET_SIZE];
    fd_set readfds;
    
    // Main forwarding loop
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(fd1, &readfds);
        FD_SET(fd2, &readfds);
        
        int max_fd = (fd1 > fd2) ? fd1 : fd2;
        
        // Wait for data on either TUN interface
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal - likely Ctrl+C
                continue;
            }
            perror("select");
            break;
        }
        
        // Handle packets from DU to connected endpoint
        if (FD_ISSET(fd1, &readfds)) {
            ssize_t n = read(fd1, buffer, sizeof(buffer));
            if (n > 0) {
                std::string direction = du_ns + "->" + connected_ns;
                // Direction depends on whether we're connected to CU or UE_stack
                bool is_outgoing = !connected_to_cu; // Outgoing if to UE_stack (downlink), incoming if to CU (uplink)
                
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd2, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to connected interface");
                    }
                }
            }
        }
        
        // Handle packets from connected endpoint to DU
        if (FD_ISSET(fd2, &readfds)) {
            ssize_t n = read(fd2, buffer, sizeof(buffer));
            if (n > 0) {
                std::string direction = connected_ns + "->" + du_ns;
                // Direction depends on whether we're connected to CU or UE_stack
                bool is_outgoing = connected_to_cu; // Outgoing if from CU (downlink), incoming if from UE_stack (uplink)
                
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd1, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to DU interface");
                    }
                }
            }
        }
    }
    
    // Clean shutdown
    LOG_INFO("[*] Shutting down DU forwarder...");
    close(fd1);
    close(fd2);
    close(host_fd);
    
    return 0;
}

/**
 * Run the UE stack forwarder (--receiver mode)
 * This mode implements the PHY/MAC/RLC protocol stack between DU and UE_stack
 */
static int run_ue_stack_forwarder(const std::string& ue_stack_ns, const std::string& ue_stack_du_tun, 
                                 const std::string& du_ns, const std::string& du_stack_tun, 
                                 const std::string& folderName) {
    LOG_INFO("[*] Running in UE stack mode between " << ue_stack_ns << " and " << du_ns);
    
    // Save host namespace
    int host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }
    
    // Enter UE stack namespace
    LOG_INFO("[*] Entering namespace: " << ue_stack_ns);
    if (enter_namespace(ue_stack_ns) < 0) {
        close(host_fd);
        return 1;
    }
    
    // Open UE stack TUN interface
    int fd1 = open_tun_device(ue_stack_du_tun);
    if (fd1 < 0) {
        LOG_ERROR("Failed to open TUN '" << ue_stack_du_tun << "' in namespace '" << ue_stack_ns << "'");
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened UE stack TUN in " << ue_stack_ns << ": " << ue_stack_du_tun << " (fd=" << fd1 << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    // Enter DU namespace
    LOG_INFO("[*] Entering namespace: " << du_ns);
    if (enter_namespace(du_ns) < 0) {
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    // Open DU TUN interface
    int fd2 = open_tun_device(du_stack_tun);
    if (fd2 < 0) {
        LOG_ERROR("Failed to open TUN '" << du_stack_tun << "' in namespace '" << du_ns << "'");
        close(fd1);
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened DU TUN in " << du_ns << ": " << du_stack_tun << " (fd=" << fd2 << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(fd2);
        close(host_fd);
        return 1;
    }
    
    // Initialize UE stack protocol stack
    init_pdcp_context("ue_stack", 0, folderName);
    
    // Set up the delivery callback for the UE instance
    ue_instance->setDeliveryCallback([fd1](const unsigned char* packet, size_t len) {
        deliver_rlc_packet(fd1, packet, len);
    });
    
    // Start forwarding
    LOG_INFO("[*] Starting packet forwarding with UE protocol stack (PHY/MAC/RLC)");
    LOG_INFO("[*] Forwarding " << ue_stack_ns << ":" << ue_stack_du_tun << " <-> " << du_ns << ":" << du_stack_tun);
    LOG_INFO("[*] Protocol stack active between interfaces");
    LOG_INFO("[*] Press Ctrl+C to stop");
    
    unsigned char buffer[MAX_PACKET_SIZE];
    fd_set readfds;
    
    // Main forwarding loop
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(fd1, &readfds);
        FD_SET(fd2, &readfds);
        
        int max_fd = (fd1 > fd2) ? fd1 : fd2;
        
        // Wait for data on either TUN interface
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal - likely Ctrl+C
                continue;
            }
            perror("select");
            break;
        }
        
        // Handle packets from UE stack to DU
        if (FD_ISSET(fd1, &readfds)) {
            ssize_t n = read(fd1, buffer, sizeof(buffer));
            if (n > 0) {
                std::string direction = ue_stack_ns + "->" + du_ns;
                bool is_outgoing = false; // UE stack → DU is uplink
                
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd2, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to DU interface");
                    }
                }
            }
        }
        
        // Handle packets from DU to UE stack
        if (FD_ISSET(fd2, &readfds)) {
            ssize_t n = read(fd2, buffer, sizeof(buffer));
            if (n > 0) {
                std::string direction = du_ns + "->" + ue_stack_ns;
                bool is_outgoing = true; // DU → UE stack is downlink
                
                // This is where the UE stack is applied
                // The process_packet_userspace will use the UE instance to handle 
                // the PHY/MAC/RLC and deliver packets via the callback
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // No need to forward here - the UE stack will handle delivery via callback
                // The callback is called from within process_packet_userspace when packets are ready
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd2, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to DU interface");
                    }
                }
            }
        }
    }
    
    // Clean shutdown
    LOG_INFO("[*] Shutting down UE stack forwarder...");
    close(fd1);
    close(fd2);
    close(host_fd);
    
    return 0;
}

/**
 * Run host to UE_PDCP forwarder (--host-mode)
 * This mode connects the UE_PDCP to the host for internet access
 */
static int run_host_ue_pdcp_forwarder(const std::string& host_tun, const std::string& ue_pdcp_ns, 
                                    const std::string& folderName) {
    LOG_INFO("[*] Running in host to UE_PDCP mode");
    
    // Save host namespace
    int host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }
    
    // Open host TUN
    int fd1 = open_tun_device(host_tun);
    if (fd1 < 0) {
        LOG_ERROR("Failed to open TUN '" << host_tun << "' in host namespace");
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened host TUN: " << host_tun << " (fd=" << fd1 << ")");
    
    // Find the tun device name on UE_PDCP side
    std::string tun_ns = "tun_ue_host";  // Default name
    
    // Enter UE_PDCP namespace
    LOG_INFO("[*] Entering namespace: " << ue_pdcp_ns);
    if (enter_namespace(ue_pdcp_ns) < 0) {
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    // Open the TUN in the UE_PDCP namespace
    int fd2 = open_tun_device(tun_ns);
    if (fd2 < 0) {
        LOG_ERROR("Failed to open TUN '" << tun_ns << "' in namespace '" << ue_pdcp_ns << "'");
        close(fd1);
        close(host_fd);
        return 1;
    }
    LOG_INFO("[*] Opened TUN in " << ue_pdcp_ns << ": " << tun_ns << " (fd=" << fd2 << ")");
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(fd2);
        close(host_fd);
        return 1;
    }
    close(host_fd);
    
    // Initialize PDCP context for UE_PDCP
    init_pdcp_context("ue_pdcp", 0, folderName);

    // Set up the PDCP receiver delivery callback if available
    PdcpReceiver* receiver = dynamic_cast<PdcpReceiver*>(pdcp_ctx.get());
    if (receiver) {
        receiver->setDeliveryCallback([fd2, receiver]() {
            // This lambda is called from processPacket().
            // We call deliver_processed_packets() with the FD and the pointer back to the receiver.
            deliver_pdcp_ordered_packets(fd2, receiver);
        });
    }
                
    // Start forwarding
    LOG_INFO("[*] Starting packet forwarding");
    LOG_INFO("[*] Forwarding " << host_tun << " <-> " << ue_pdcp_ns << ":" << tun_ns);
    LOG_INFO("[*] Press Ctrl+C to stop");
    
    unsigned char buffer[MAX_PACKET_SIZE];
    fd_set readfds;
    
    // Main forwarding loop
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(fd1, &readfds);
        FD_SET(fd2, &readfds);
        
        int max_fd = (fd1 > fd2) ? fd1 : fd2;
        
        // Wait for data on either TUN interface
        int ret = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal - likely Ctrl+C
                continue;
            }
            perror("select");
            break;
        }
        
        // Handle packets from host to UE_PDCP
        if (FD_ISSET(fd1, &readfds)) {
            ssize_t n = read(fd1, buffer, sizeof(buffer));
            if (n > 0) {
                ssize_t processed_len = process_packet_userspace(buffer, n, "host->UE_PDCP", false);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd2, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to UE_PDCP");
                    }
                }
            }
        }
        
        // Handle packets from UE_PDCP to host
        if (FD_ISSET(fd2, &readfds)) {
            ssize_t n = read(fd2, buffer, sizeof(buffer));
            if (n > 0) {
                ssize_t processed_len = process_packet_userspace(buffer, n, "UE_PDCP->host", true);

                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd1, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to host");
                    }
                }
            }
        }
    }
    
    // Clean shutdown
    LOG_INFO("[*] Shutting down host-UE_PDCP forwarder...");
    close(fd1);
    close(fd2);
    
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Main function
//////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[]) {
    // Set up signal handlers for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Determine our role from the arguments
    std::string role;
    std::string folderName = "."; // Default to current directory

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--folder") == 0 && (i + 1 < argc)) {
            folderName = argv[i + 1];

            // Remove these two args so the rest of your existing logic sees fewer args.
            // Easiest is to shift leftover arguments:
            for (int j = i; j + 2 < argc; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--; // so we re-check the same index next iteration
        }
    }
    
    LOG_DEBUG("Folder name: \"" << folderName << "\"");

    // Multi-link mode (CU -> multiple DUs)
    if (argc >= 3 && strcmp(argv[1], "--multi-link") == 0) {
        uint8_t num_links = atoi(argv[2]);
        
        if (num_links < 1 || num_links > MAX_LINKS) {
            LOG_ERROR("Error: Number of links must be between 1 and " << MAX_LINKS);
            return 1;
        }
        
        if (argc != 5 + num_links * 2) {
            LOG_ERROR("Usage for multi-link mode:");
            LOG_ERROR("  " << argv[0] << " --multi-link <num_links> <CU_ns> <CU_tun> <DU1_ns> <DU1_tun> ... ");
            return 1;
        }
        
        std::string sender_ns = argv[3];
        std::string sender_tun = argv[4];
        
        std::vector<std::string> link_ns;
        std::vector<std::string> link_tun;
        
        for (uint8_t i = 0; i < num_links; i++) {
            link_ns.push_back(argv[5 + i*2]);
            link_tun.push_back(argv[5 + i*2 + 1]);
        }
        
        return run_multi_link_forwarder(sender_ns, sender_tun, num_links, link_ns, link_tun, folderName);
    }
    
    // Host to UE_PDCP mode
    if (argc == 4 && strcmp(argv[1], "--host-mode") == 0) {
        std::string tun_host = argv[2];
        std::string ns_name = argv[3];
        
        // UE_PDCP mode 
        if (ns_name == "UE_PDCP") {
            return run_host_ue_pdcp_forwarder(tun_host, ns_name, folderName);
        }
        else {
            LOG_ERROR("Unknown namespace type for --host-mode: " << ns_name);
            return 1;
        }
    }

    // DU mode (detects if argc == 6 but not --receiver)
    if (argc == 5) {
        // Special mode for DU between CU/sender and UE_stack/receiver
        std::string du_ns = argv[1];
        std::string du_tun = argv[2];
        std::string connected_ns = argv[3];
        std::string connected_tun = argv[4];
        
        return run_du_forwarder(du_ns, du_tun, connected_ns, connected_tun, folderName);
    }
        
    // UE stack mode (with PHY/MAC/RLC)
    if (argc == 6 && strcmp(argv[1], "--receiver") == 0) {
        // Special mode for UE stack between DU and UE_PDCP
        std::string ue_stack_ns = argv[2];
        std::string ue_stack_du_tun = argv[3];
        std::string du_ns = argv[4];
        std::string du_stack_tun = argv[5];
        
        return run_ue_stack_forwarder(ue_stack_ns, ue_stack_du_tun, du_ns, du_stack_tun, folderName);
    }

    // Regular argument checking
    if (argc != 5 && argc != 4) {
        LOG_INFO(
            "Usage:\n"
            "  Between namespaces: " << argv[0] << " <namespace1> <tun1> <namespace2> <tun2>\n"
            "  Host to namespace:  " << argv[0] << " <tun_host> <namespace> <tun_ns>\n"
            "  Host to UE_PDCP:    " << argv[0] << " --host-mode <tun_host> <UE_PDCP>\n"
            "  UE stack mode:      " << argv[0] << " --receiver <UE_stack_ns> <tun_stack_du> <DU_ns> <tun_du_stack>\n"
            "  Multi-link mode:    " << argv[0] << " --multi-link <num_links> <CU_ns> <CU_tun> <DU1_ns> <DU1_tun> ...\n"
            "\nExamples with 5G naming:\n"
            "  Host to UE_PDCP:    " << argv[0] << " --host-mode tun_host_ue UE_PDCP\n"
            "  UE_PDCP to UE_stack: " << argv[0] << " UE_PDCP tun_pdcp_stack1 UE_stack1 tun_stack1_pdcp\n"
            "  UE_stack to DU:     " << argv[0] << " --receiver UE_stack1 tun_stack1_du DU1 tun_du1_stack\n"
            "  Multi-link:         " << argv[0] << " --multi-link 2 CU tun_s_multi DU1 tun_du1_sender DU2 tun_du2_sender\n"
            "\nLegacy examples:\n"
            "  Host to receiver:   " << argv[0] << " --host-mode tun_host_recv node_receiver\n"
            "  Link to sender:     " << argv[0] << " node_link1 tun_l1_s node_sender tun_s_l1"
        );
        return 1;
    }

    return 0;
}
