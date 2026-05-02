#ifndef PDCP_RECEIVER_H
#define PDCP_RECEIVER_H

#include "pdcp_common.h"

// Maximum reordering window size
#define PDCP_MAX_REORDER_WINDOW 1024
// Default reordering timeout in milliseconds
#define PDCP_DEFAULT_REORDER_TIMEOUT_MS 300

// Structure to hold a packet in the reordering buffer
typedef struct {
    unsigned char data[PDCP_MAX_PACKET_SIZE];
    size_t len;
    uint32_t sequence_number;
    struct timespec timestamp;
    bool in_use;
} pdcp_packet_t;

// PDCP receiver context
typedef struct {
    pdcp_packet_t packets[PDCP_MAX_REORDER_WINDOW];
    bool packet_present[PDCP_MAX_REORDER_WINDOW];
    uint32_t next_expected;        // Next expected sequence number
    uint32_t highest_received;     // Highest received sequence number
    pthread_mutex_t buffer_mutex;
    int reorder_timeout_ms;        // Timeout in milliseconds
    uint32_t delivered_count;      // Statistics
    uint32_t dropped_count;        // Statistics
} pdcp_receiver_ctx_t;

// Initialize receiver context
void pdcp_receiver_init(pdcp_receiver_ctx_t *ctx, int timeout_ms);

// Clean up receiver context
void pdcp_receiver_cleanup(pdcp_receiver_ctx_t *ctx);

// Process a packet at the receiver (extract sequence number and reorder)
// Returns:
// - > 0: length of the processed packet ready for delivery
// - 0: packet was buffered or dropped, nothing to deliver now
size_t pdcp_receiver_process(pdcp_receiver_ctx_t *ctx, unsigned char *packet, size_t len);

// Process reordering buffer to deliver any ready packets
// Returns:
// - > 0: number of packets delivered (copied to packet_out)
// - 0: no packets were ready to deliver
int pdcp_receiver_deliver(pdcp_receiver_ctx_t *ctx, unsigned char *packet_out, size_t *len_out);

// Get receiver statistics for monitoring
void pdcp_receiver_get_stats(pdcp_receiver_ctx_t *ctx, uint32_t *next_expected,
                            uint32_t *highest_received, uint32_t *delivered, uint32_t *dropped);

#endif /* PDCP_RECEIVER_H */