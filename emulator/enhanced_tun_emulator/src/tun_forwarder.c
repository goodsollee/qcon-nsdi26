#define _GNU_SOURCE  // Required for setns and CLONE_NEWNET

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>     // setns()
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

// Include PDCP modules
#include "pdcp_common.h"
#include "pdcp_sender.h"
#include "pdcp_link.h"
#include "pdcp_receiver.h"

// Maximum packet size
#define MAX_PACKET_SIZE 4096
// Maximum number of links
#define MAX_LINKS 4

// IPv4 header structure definition
struct ipv4_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    unsigned int ihl:4;
    unsigned int version:4;
#elif __BYTE_ORDER == __BIG_ENDIAN
    unsigned int version:4;
    unsigned int ihl:4;
#else
#error "Byte order not supported"
#endif
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

// Structure to hold link information
typedef struct {
    int fd;                  // TUN file descriptor
    char namespace[32];      // Namespace name
    char interface[32];      // Interface name
    uint32_t in_packets;     // Counter for incoming packets
    uint32_t out_packets;    // Counter for outgoing packets
    uint64_t in_bytes;       // Counter for incoming bytes
    uint64_t out_bytes;      // Counter for outgoing bytes
} link_info_t;

// Global flag to handle clean shutdown
volatile sig_atomic_t running = 1;
// Global routing state for round-robin
volatile uint8_t current_link = 0;

// PDCP contexts
pdcp_sender_ctx_t sender_ctx;
pdcp_link_ctx_t link_ctx[MAX_LINKS];
pdcp_receiver_ctx_t receiver_ctx;
bool pdcp_enabled = true;  // Always enabled in this version

// Signal handler for clean shutdown
void signal_handler(int signum) {
    (void)signum; // Suppress unused parameter warning
    running = 0;
}

/**
 * User space packet processing function
 * 
 * This is where you'll implement your custom packet manipulation logic.
 * The function receives a packet buffer, processes it, and returns:
 * - Modified packet length if packet should be forwarded
 * - 0 if packet should be dropped
 * - -1 on error
 */
static ssize_t process_packet_userspace(unsigned char *packet, size_t len, const char *direction, bool is_outgoing) {
    // Basic validation
    if (len < 20) {
        fprintf(stderr, "[%s] Packet too short to process: %zu bytes\n", direction, len);
        return 0; // Drop packet
    }
    
    // Print packet info for debugging
    pdcp_dump_ip_packet(packet, len, direction);
    
    // Determine which PDCP processing to apply based on direction
    ssize_t new_len = len;
    
    // Sender processing
    if (strstr(direction, "sender->link") != NULL && is_outgoing) {
        new_len = pdcp_sender_process(&sender_ctx, packet, len);
    }
    // Link processing
    else if ((strstr(direction, "ns1->ns2") != NULL || strstr(direction, "ns2->ns1") != NULL) &&
             (strstr(direction, "link") != NULL)) {
        // Determine link ID based on direction string
        int link_id = 0;
        for (int i = 1; i <= MAX_LINKS; i++) {
            char link_str[8];
            snprintf(link_str, sizeof(link_str), "link%d", i);
            if (strstr(direction, link_str) != NULL) {
                link_id = i - 1; // zero-based index
                break;
            }
        }
        
        new_len = pdcp_link_process(&link_ctx[link_id], packet, len);
    }
    // Receiver processing
    else if (strstr(direction, "link->sender") != NULL && !is_outgoing) {
        new_len = pdcp_receiver_process(&receiver_ctx, packet, len);
        
        // If packet was buffered (new_len == 0), try to deliver a packet from the reordering buffer
        if (new_len == 0) {
            size_t delivered_len = 0;
            if (pdcp_receiver_deliver(&receiver_ctx, packet, &delivered_len) > 0) {
                new_len = delivered_len;
                
                // Recalculate IPv4 header checksum if needed
                if (new_len >= 20) {
                    struct ipv4_hdr *iph = (struct ipv4_hdr *)packet;
                    if (iph->version == 4) {
                        iph->check = 0;
                        iph->check = pdcp_ip_checksum(iph, iph->ihl * 4);
                    }
                }
            }
        }
    }
    
    // Default processing for packets that don't match any PDCP role
    if (new_len == len) {
        struct ipv4_hdr *iph = (struct ipv4_hdr *)packet;
        
        // Only process IPv4 packets
        if (iph->version == 4) {
            // Default TTL adjustment
            iph->ttl = 64;
            
            // Recalculate checksum
            iph->check = 0;
            iph->check = pdcp_ip_checksum(iph, iph->ihl * 4);
        }
    }
    
    return new_len;
}

/**
 * Check packet for being outgoing (true) or incoming (false)
 */
static bool is_packet_outgoing(const unsigned char *packet, size_t len) {
    if (len < 20) {
        return true; // Default to outgoing if can't determine
    }
    
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *) packet;
    
    // Source IP is from the sender's interface
    if ((iph->saddr & 0xFFFFFF00) == 0x0A640000) { // 10.100.0.0/24
        return true;
    }
    
    return false;
}

/**
 * Select next link in round-robin fashion
 */
static uint8_t get_next_link(uint8_t num_links) {
    current_link = (current_link + 1) % num_links;
    return current_link;
}

/**
 * Initialize PDCP contexts for all roles
 */
static void init_pdcp_contexts(uint8_t num_links) {
    // Initialize sender context
    pdcp_sender_init(&sender_ctx);
    
    // Initialize link contexts
    for (uint8_t i = 0; i < num_links; i++) {
        pdcp_link_init(&link_ctx[i], i + 1);
    }
    
    // Initialize receiver context
    pdcp_receiver_init(&receiver_ctx, PDCP_DEFAULT_REORDER_TIMEOUT_MS);
    
    fprintf(stderr, "[PDCP] All contexts initialized, reordering timeout: %d ms\n", 
            PDCP_DEFAULT_REORDER_TIMEOUT_MS);
}

/**
 * Enter a network namespace
 */
static int enter_namespace(const char *ns_name) {
    char path[256];
    snprintf(path, sizeof(path), "/var/run/netns/%s", ns_name);
    
    int fd = open(path, O_RDONLY);
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
static int open_tun_device(const char *tun_name) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI; // Layer 3 mode, no packet info
    strncpy(ifr.ifr_name, tun_name, IFNAMSIZ - 1);
    
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }
    
    return fd;
}

/**
 * Check packet for being outgoing (true) or incoming (false)
 */
static bool is_packet_outgoing(const unsigned char *packet, size_t len) {
    if (len < 20) {
        return true; // Default to outgoing if can't determine
    }
    
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *) packet;
    
    // Source IP is from the sender's single interface
    if ((iph->saddr & 0xFFFFFF00) == 0x0A640000) { // 10.100.0.0/24
        return true;
    }
    
    return false;
}

/**
 * Main function for the multi-link forwarder
 */
static int run_multi_link_forwarder(const char *sender_ns, const char *sender_tun, 
                                    uint8_t num_links, const char *link_ns[],
                                    const char *link_tun[]) {
    link_info_t links[MAX_LINKS] = {0};
    int sender_fd = -1;
    int host_fd = -1;
    
    // Save host namespace
    host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }
    
    // Open sender TUN
    fprintf(stderr, "[*] Entering namespace: %s\n", sender_ns);
    if (enter_namespace(sender_ns) < 0) {
        close(host_fd);
        return 1;
    }
    
    sender_fd = open_tun_device(sender_tun);
    if (sender_fd < 0) {
        fprintf(stderr, "Failed to open TUN '%s' in namespace '%s'\n", sender_tun, sender_ns);
        close(host_fd);
        return 1;
    }
    fprintf(stderr, "[*] Opened sender TUN: %s (fd=%d)\n", sender_tun, sender_fd);
    
    // Return to host namespace
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(sender_fd);
        close(host_fd);
        return 1;
    }
    
    // Open each link TUN
    for (uint8_t i = 0; i < num_links; i++) {
        strncpy(links[i].namespace, link_ns[i], sizeof(links[i].namespace)-1);
        strncpy(links[i].interface, link_tun[i], sizeof(links[i].interface)-1);
        
        fprintf(stderr, "[*] Entering namespace: %s\n", links[i].namespace);
        if (enter_namespace(links[i].namespace) < 0) {
            // Cleanup already opened FDs
            close(sender_fd);
            for (uint8_t j = 0; j < i; j++) {
                close(links[j].fd);
            }
            close(host_fd);
            return 1;
        }
        
        links[i].fd = open_tun_device(links[i].interface);
        if (links[i].fd < 0) {
            fprintf(stderr, "Failed to open TUN '%s' in namespace '%s'\n", 
                    links[i].interface, links[i].namespace);
            // Cleanup
            close(sender_fd);
            for (uint8_t j = 0; j < i; j++) {
                close(links[j].fd);
            }
            close(host_fd);
            return 1;
        }
        fprintf(stderr, "[*] Opened link TUN in %s: %s (fd=%d)\n", 
                links[i].namespace, links[i].interface, links[i].fd);
        
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
    
    // Start forwarding
    fprintf(stderr, "[*] Starting multi-link forwarding\n");
    fprintf(stderr, "[*] Forwarding %s:%s <-> multiple links\n", sender_ns, sender_tun);
    fprintf(stderr, "[*] Using round-robin routing for outgoing traffic\n");
    fprintf(stderr, "[*] Bypassing userspace processing for incoming traffic\n");
    fprintf(stderr, "[*] Press Ctrl+C to stop\n");
    
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
        
        // Handle packets from sender (outgoing traffic)
        if (FD_ISSET(sender_fd, &readfds)) {
            ssize_t n = read(sender_fd, buffer, sizeof(buffer));
            if (n > 0) {
                bool outgoing = true; // Traffic from sender is always outgoing
                
                // Round-robin load balancing for outgoing traffic
                uint8_t link_index = get_next_link(num_links);
                
                // Process the packet in userspace (outgoing traffic)
                ssize_t processed_len = process_packet_userspace(buffer, n, "sender->link", outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(links[link_index].fd, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to link");
                    } else {
                        // Update statistics
                        links[link_index].out_packets++;
                        links[link_index].out_bytes += processed_len;
                    }
                }
            }
        }
        
        // Handle packets from each link (incoming traffic)
        for (uint8_t i = 0; i < num_links; i++) {
            if (FD_ISSET(links[i].fd, &readfds)) {
                ssize_t n = read(links[i].fd, buffer, sizeof(buffer));
                if (n > 0) {
                    bool outgoing = false; // Traffic from links is always incoming
                    
                    // Bypass userspace processing for incoming traffic
                    // We pass through the function but with the flag set to incoming
                    ssize_t processed_len = process_packet_userspace(buffer, n, "link->sender", outgoing);
                    
                    // Forward if not dropped
                    if (processed_len > 0) {
                        ssize_t bytes_written = write(sender_fd, buffer, processed_len);
                        if (bytes_written < 0) {
                            perror("write to sender");
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
            fprintf(stderr, "===== TRAFFIC STATISTICS =====\n");
            fprintf(stderr, "Link     | In (pkts/bytes) | Out (pkts/bytes)\n");
            fprintf(stderr, "---------+-----------------+------------------\n");
            
            for (uint8_t i = 0; i < num_links; i++) {
                fprintf(stderr, "Link%-4d | %10u/%10lu | %10u/%10lu\n", 
                        i+1, links[i].in_packets, links[i].in_bytes,
                        links[i].out_packets, links[i].out_bytes);
            }
            
            fprintf(stderr, "===============================\n");
            last_stats_time = now;
        }
    }
    
    // Clean shutdown
    fprintf(stderr, "[*] Shutting down multi-link forwarder...\n");
    close(sender_fd);
    for (uint8_t i = 0; i < num_links; i++) {
        close(links[i].fd);
    }
    close(host_fd);
    
    return 0;
}

int main(int argc, char *argv[]) {
    // Set up signal handlers for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Inside main() function, modify this block:
    if (argc >= 3 && strcmp(argv[1], "--multi-link") == 0) {
        uint8_t num_links = atoi(argv[2]);
        
        if (num_links < 1 || num_links > MAX_LINKS) {
            fprintf(stderr, "Error: Number of links must be between 1 and %d\n", MAX_LINKS);
            return 1;
        }
        
        // This is the problematic line - it's checking for exactly 4 + num_links * 2 arguments
        // But your command should have 5 + num_links * 2 arguments including the program name
        if (argc != 5 + num_links * 2) {  // Changed from 4 to 5
            fprintf(stderr, "Usage for multi-link mode:\n");
            fprintf(stderr, "  %s --multi-link <num_links> <sender_ns> <sender_tun> <link1_ns> <link1_tun> ... \n", argv[0]);
            return 1;
        }
        
        const char *sender_ns = argv[3];
        const char *sender_tun = argv[4];
        
        const char *link_ns[MAX_LINKS];
        const char *link_tun[MAX_LINKS];
        
        for (uint8_t i = 0; i < num_links; i++) {
            link_ns[i] = argv[5 + i*2];
            link_tun[i] = argv[5 + i*2 + 1];
        }
        
        return run_multi_link_forwarder(sender_ns, sender_tun, num_links, link_ns, link_tun);
    }

    // Simplified argument parsing for host to namespace mode
    if (argc == 4 && strcmp(argv[1], "--host-mode") == 0) {
        // Special debug mode for host-namespace
        fprintf(stderr, "[*] Running in special host-namespace mode\n");
        
        const char *tun_host = argv[2];
        const char *ns_name = argv[3];  // Changed from 'namespace' to 'ns_name'
        
        // Save host namespace
        int host_fd = open("/proc/self/ns/net", O_RDONLY);
        if (host_fd < 0) {
            perror("open /proc/self/ns/net");
            return 1;
        }
        
        // Open host TUN
        int fd1 = open_tun_device(tun_host);
        if (fd1 < 0) {
            fprintf(stderr, "Failed to open TUN '%s' in host namespace\n", tun_host);
            close(host_fd);
            return 1;
        }
        fprintf(stderr, "[*] Opened host TUN: %s (fd=%d)\n", tun_host, fd1);
        
        // Find the tun device name on receiver side
        char tun_ns[64] = "tun_recv_host";  // Default name
        
        // Enter receiver namespace
        fprintf(stderr, "[*] Entering namespace: %s\n", ns_name);
        if (enter_namespace(ns_name) < 0) {
            close(fd1);
            close(host_fd);
            return 1;
        }
        
        // Open the TUN in the namespace
        int fd2 = open_tun_device(tun_ns);
        if (fd2 < 0) {
            fprintf(stderr, "Failed to open TUN '%s' in namespace '%s'\n", tun_ns, ns_name);
            close(fd1);
            close(host_fd);
            return 1;
        }
        fprintf(stderr, "[*] Opened TUN in %s: %s (fd=%d)\n", ns_name, tun_ns, fd2);
        
        // Return to host namespace
        if (setns(host_fd, CLONE_NEWNET) < 0) {
            perror("setns(host_fd)");
            close(fd1);
            close(fd2);
            close(host_fd);
            return 1;
        }
        close(host_fd);
        
        // Start forwarding
        fprintf(stderr, "[*] Starting packet forwarding\n");
        fprintf(stderr, "[*] Forwarding %s <-> %s:%s\n", tun_host, ns_name, tun_ns);
        fprintf(stderr, "[*] Press Ctrl+C to stop\n");
        
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
            
            // Handle packets from host to namespace
            if (FD_ISSET(fd1, &readfds)) {
                ssize_t n = read(fd1, buffer, sizeof(buffer));
                if (n > 0) {
                    ssize_t processed_len = process_packet_userspace(buffer, n, "host->ns", true);
                    
                    // Forward if not dropped
                    if (processed_len > 0) {
                        ssize_t bytes_written = write(fd2, buffer, processed_len);
                        if (bytes_written < 0) {
                            perror("write to namespace");
                        }
                    }
                }
            }
            
            // Handle packets from namespace to host
            if (FD_ISSET(fd2, &readfds)) {
                ssize_t n = read(fd2, buffer, sizeof(buffer));
                if (n > 0) {
                    ssize_t processed_len = process_packet_userspace(buffer, n, "ns->host", false);
                    
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
        fprintf(stderr, "[*] Shutting down...\n");
        close(fd1);
        close(fd2);
        
        return 0;
    }

    // Regular argument checking
    if (argc != 5 && argc != 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Between namespaces: %s <namespace1> <tun1> <namespace2> <tun2>\n", argv[0]);
        fprintf(stderr, "  Host to namespace:  %s <tun_host> <namespace> <tun_ns>\n", argv[0]);
        fprintf(stderr, "  Special host mode:  %s --host-mode <tun_host> <namespace>\n", argv[0]);
        fprintf(stderr, "  Multi-link mode:    %s --multi-link <num_links> <sender_ns> <sender_tun> <link1_ns> <link1_tun> ...\n", argv[0]);
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  Host to receiver:   %s tun_host_recv node_receiver tun_recv_host\n", argv[0]);
        fprintf(stderr, "  Host mode:          %s --host-mode tun_host_recv node_receiver\n", argv[0]);
        fprintf(stderr, "  Receiver to link:   %s node_receiver tun_recv_l1 node_link1 tun_l1_r\n", argv[0]);
        fprintf(stderr, "  Link to sender:     %s node_link1 tun_l1_s node_sender tun_s_l1\n", argv[0]);
        fprintf(stderr, "  Multi-link:         %s --multi-link 2 node_sender tun_s_multi node_link1 tun_l1_s node_link2 tun_l2_s\n", argv[0]);
        return 1;
    }

    // Parse arguments
    const char *ns1_name = NULL;
    const char *tun1_name = NULL;
    const char *ns2_name = NULL;
    const char *tun2_name = NULL;
    int host_mode = 0;

    if (argc == 5) {
        // Mode 1: Between two namespaces
        ns1_name = argv[1];
        tun1_name = argv[2];
        ns2_name = argv[3];
        tun2_name = argv[4];
    } else {
        // Mode 2: Host to namespace
        host_mode = 1;
        tun1_name = argv[1]; // Host TUN
        ns2_name = argv[2];  // Namespace
        tun2_name = argv[3]; // Namespace TUN
    }

    // Save host network namespace FD for later restoration
    int host_fd = open("/proc/self/ns/net", O_RDONLY);
    if (host_fd < 0) {
        perror("open /proc/self/ns/net");
        return 1;
    }

    // Open first TUN interface
    int fd1;
    if (host_mode) {
        // First TUN is in host namespace (already there)
        fd1 = open_tun_device(tun1_name);
        if (fd1 < 0) {
            fprintf(stderr, "Failed to open TUN '%s' in host namespace\n", tun1_name);
            close(host_fd);
            return 1;
        }
        fprintf(stderr, "[*] Opened host TUN: %s (fd=%d)\n", tun1_name, fd1);
    } else {
        // First TUN is in a namespace
        fprintf(stderr, "[*] Entering namespace: %s\n", ns1_name);
        if (enter_namespace(ns1_name) < 0) {
            close(host_fd);
            return 1;
        }
        
        fd1 = open_tun_device(tun1_name);
        if (fd1 < 0) {
            fprintf(stderr, "Failed to open TUN '%s' in namespace '%s'\n", tun1_name, ns1_name);
            close(host_fd);
            return 1;
        }
        fprintf(stderr, "[*] Opened TUN in %s: %s (fd=%d)\n", ns1_name, tun1_name, fd1);
        
        // Return to host namespace
        if (setns(host_fd, CLONE_NEWNET) < 0) {
            perror("setns(host_fd)");
            close(fd1);
            close(host_fd);
            return 1;
        }
    }

    // Open second TUN interface in the second namespace
    fprintf(stderr, "[*] Entering namespace: %s\n", ns2_name);
    if (enter_namespace(ns2_name) < 0) {
        close(fd1);
        close(host_fd);
        return 1;
    }
    
    int fd2 = open_tun_device(tun2_name);
    if (fd2 < 0) {
        fprintf(stderr, "Failed to open TUN '%s' in namespace '%s'\n", tun2_name, ns2_name);
        close(fd1);
        close(host_fd);
        return 1;
    }
    fprintf(stderr, "[*] Opened TUN in %s: %s (fd=%d)\n", ns2_name, tun2_name, fd2);
    
    // Return to host namespace (needed for proper operation)
    if (setns(host_fd, CLONE_NEWNET) < 0) {
        perror("setns(host_fd)");
        close(fd1);
        close(fd2);
        close(host_fd);
        return 1;
    }
    close(host_fd);

    // Packet forwarding loop with select()
    fprintf(stderr, "[*] Starting packet forwarding\n");
    if (host_mode) {
        fprintf(stderr, "[*] Forwarding %s <-> %s:%s\n", tun1_name, ns2_name, tun2_name);
    } else {
        fprintf(stderr, "[*] Forwarding %s:%s <-> %s:%s\n", ns1_name, tun1_name, ns2_name, tun2_name);
    }
    fprintf(stderr, "[*] Press Ctrl+C to stop\n");

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

        // Handle packets from first interface to second
        if (FD_ISSET(fd1, &readfds)) {
            ssize_t n = read(fd1, buffer, sizeof(buffer));
            if (n > 0) {
                const char *direction = host_mode ? "host->ns" : "ns1->ns2";
                bool is_outgoing = ns1_name != NULL && strcmp(ns1_name, "node_sender") == 0;
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd2, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to second interface");
                    }
                }
            }
        }

        // Handle packets from second interface to first
        if (FD_ISSET(fd2, &readfds)) {
            ssize_t n = read(fd2, buffer, sizeof(buffer));
            if (n > 0) {
                const char *direction = host_mode ? "ns->host" : "ns2->ns1";
                // If this is return traffic to sender, we don't process it in userspace
                bool is_outgoing = ns2_name != NULL && strcmp(ns2_name, "node_sender") == 0;
                ssize_t processed_len = process_packet_userspace(buffer, n, direction, is_outgoing);
                
                // Forward if not dropped
                if (processed_len > 0) {
                    ssize_t bytes_written = write(fd1, buffer, processed_len);
                    if (bytes_written < 0) {
                        perror("write to first interface");
                    }
                }
            }
        }
    }

    // Clean shutdown
    fprintf(stderr, "[*] Shutting down...\n");
    close(fd1);
    close(fd2);
    
    return 0;
}