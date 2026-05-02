#ifndef PDCP_SENDER_H
#define PDCP_SENDER_H

#include "pdcp_common.h"

// PDCP sender context
typedef struct {
    uint32_t next_sequence;        // Next sequence number to assign
    pthread_mutex_t sequence_mutex; // Mutex for sequence number
} pdcp_sender_ctx_t;

// Initialize sender context
void pdcp_sender_init(pdcp_sender_ctx_t *ctx);

// Clean up sender context
void pdcp_sender_cleanup(pdcp_sender_ctx_t *ctx);

// Process a packet at the sender (add sequence number)
// Returns the new packet length
size_t pdcp_sender_process(pdcp_sender_ctx_t *ctx, unsigned char *packet, size_t len);

// Get current sequence number (for statistics)
uint32_t pdcp_sender_get_sequence(pdcp_sender_ctx_t *ctx);

#endif /* PDCP_SENDER_H */