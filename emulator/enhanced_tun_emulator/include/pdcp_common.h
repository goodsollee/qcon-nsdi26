#ifndef PDCP_COMMON_H
#define PDCP_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Maximum packet size
#define PDCP_MAX_PACKET_SIZE 4096

// PDCP header structure (to be inserted before IP header)
struct pdcp_hdr {
    uint32_t sequence_number;  // 32-bit sequence number
    uint8_t  flags;            // Flags for future extensions
} __attribute__((packed));

// PDCP roles
typedef enum {
    PDCP_ROLE_NONE = 0,
    PDCP_ROLE_SENDER,
    PDCP_ROLE_LINK,
    PDCP_ROLE_RECEIVER
} pdcp_role_t;

// Common functions

// Check if this is an IPv4 packet that should be processed by PDCP
bool pdcp_should_process_packet(const unsigned char *packet, size_t len);

// Calculate IPv4 header checksum
uint16_t pdcp_ip_checksum(void *vdata, size_t length);

// Helper for debugging to dump IP packet information
void pdcp_dump_ip_packet(const unsigned char *buf, size_t len, const char *direction);

// Calculate time difference in milliseconds
long pdcp_timespec_diff_ms(struct timespec *start, struct timespec *end);

#endif /* PDCP_COMMON_H */