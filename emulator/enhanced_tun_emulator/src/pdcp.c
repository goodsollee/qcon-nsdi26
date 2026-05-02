#include "pdcp.h"
#include <arpa/inet.h>
#include <sys/time.h>

// IPv4 header structure (simplified for reference)
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

// Calculate IPv4 header checksum
static uint16_t ip_checksum(void *vdata, size_t length) {
    // Cast the data to uint16_t pointer
    uint16_t *data = (uint16_t*)vdata;
    
    // Initialize sum to zero
    uint32_t sum = 0;
    
    // Main loop to sum up 16-bit words
    while (length > 1) {
        sum += *data++;
        length -= 2;
    }
    
    // Add left-over byte, if any
    if (length > 0) {
        sum += *(uint8_t*)data;
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    
    // Take one's complement
    return ~sum;
}

// Calculate time difference in milliseconds
static long timespec_diff_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000 + 
           (end->tv_nsec - start->tv_nsec) / 1000000;
}

// Initialize PDCP context
void pdcp_init(pdcp_context_t *ctx, pdcp_role_t role) {
    ctx->role = role;
    ctx->next_sequence = 0;
    
    if (role == PDCP_ROLE_RECEIVER) {
        // Initialize reordering buffer
        memset(ctx->reorder_buffer.packet_present, 0, 
               sizeof(ctx->reorder_buffer.packet_present));
        ctx->reorder_buffer.next_expected = 0;
        ctx->reorder_buffer.highest_received = 0;
        pthread_mutex_init(&ctx->reorder_buffer.buffer_mutex, NULL);
        ctx->reorder_buffer.reorder_timeout_ms = PDCP_DEFAULT_REORDER_TIMEOUT_MS;
    }
    
    pthread_mutex_init(&ctx->sequence_mutex, NULL);
}

// Clean up PDCP context
void pdcp_cleanup(pdcp_context_t *ctx) {
    if (ctx->role == PDCP_ROLE_RECEIVER) {
        pthread_mutex_destroy(&ctx->reorder_buffer.buffer_mutex);
    }
    pthread_mutex_destroy(&ctx->sequence_mutex);
}

// Check if packet should be processed by PDCP
bool pdcp_should_process(const unsigned char *packet, size_t len) {
    // Minimum IP header is 20 bytes
    if (len < 20) {
        return false;
    }
    
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *) packet;
    
    // Only process IPv4 packets
    if (iph->version != 4) {
        return false;
    }
    
    // For simplicity, we'll process all IPv4 packets
    // You might want to filter by source/destination IPs or protocols
    return true;
}

// Process a packet according to PDCP role
size_t pdcp_process_packet(pdcp_context_t *ctx, unsigned char *packet, size_t len) {
    if (!pdcp_should_process(packet, len)) {
        return len; // Pass through unchanged
    }
    
    switch (ctx->role) {
        case PDCP_ROLE_SENDER: {
            // Make room for PDCP header
            memmove(packet + sizeof(struct pdcp_hdr), packet, len);
            
            // Get next sequence number
            pthread_mutex_lock(&ctx->sequence_mutex);
            uint32_t seq = ctx->next_sequence++;
            pthread_mutex_unlock(&ctx->sequence_mutex);
            
            // Add PDCP header
            struct pdcp_hdr *hdr = (struct pdcp_hdr *)packet;
            hdr->sequence_number = htonl(seq);
            hdr->flags = 0;
            
            // Return new length
            return len + sizeof(struct pdcp_hdr);
        }
        
        case PDCP_ROLE_LINK: {
            // Just pass through, no modification needed
            return len;
        }
        
        case PDCP_ROLE_RECEIVER: {
            // Check if this is a PDCP packet (must be at least header size)
            if (len < sizeof(struct pdcp_hdr)) {
                return len; // Not a PDCP packet, pass through
            }
            
            // Extract PDCP header
            struct pdcp_hdr *hdr = (struct pdcp_hdr *)packet;
            uint32_t seq = ntohl(hdr->sequence_number);
            
            // Make a copy of the packet without PDCP header
            unsigned char temp[PDCP_MAX_PACKET_SIZE];
            size_t ip_len = len - sizeof(struct pdcp_hdr);
            memcpy(temp, packet + sizeof(struct pdcp_hdr), ip_len);
            
            // Add to reordering buffer
            pthread_mutex_lock(&ctx->reorder_buffer.buffer_mutex);
            
            uint32_t index = seq % PDCP_MAX_REORDER_WINDOW;
            
            if (ctx->reorder_buffer.packet_present[index]) {
                // Duplicate packet - drop it
                pthread_mutex_unlock(&ctx->reorder_buffer.buffer_mutex);
                return 0;
            }
            
            // Store packet in reordering buffer
            pdcp_packet_t *pkt = &ctx->reorder_buffer.packets[index];
            memcpy(pkt->data, temp, ip_len);
            pkt->len = ip_len;
            pkt->sequence_number = seq;
            clock_gettime(CLOCK_MONOTONIC, &pkt->timestamp);
            pkt->in_use = true;
            
            ctx->reorder_buffer.packet_present[index] = true;
            
            if (seq > ctx->reorder_buffer.highest_received) {
                ctx->reorder_buffer.highest_received = seq;
            }
            
            // Immediately check if this is the next expected packet
            if (seq == ctx->reorder_buffer.next_expected) {
                // Can deliver this packet immediately
                memcpy(packet, pkt->data, pkt->len);
                ctx->reorder_buffer.packet_present[index] = false;
                ctx->reorder_buffer.next_expected++;
                
                // Return new length
                size_t new_len = pkt->len;
                pthread_mutex_unlock(&ctx->reorder_buffer.buffer_mutex);
                return new_len;
            }
            
            pthread_mutex_unlock(&ctx->reorder_buffer.buffer_mutex);
            
            // Packet added to buffer but not delivered yet
            return 0; // Drop the original packet, will be delivered later
        }
        
        default:
            return len; // Unknown role, pass through
    }
}

// Process packets in receiver's reordering buffer that are ready to be delivered
int pdcp_process_reorder_buffer(pdcp_context_t *ctx, unsigned char *out_buffer, 
                               size_t *out_len, size_t max_len) {
    if (ctx->role != PDCP_ROLE_RECEIVER) {
        return 0; // Not a receiver, nothing to do
    }
    
    pthread_mutex_lock(&ctx->reorder_buffer.buffer_mutex);
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    uint32_t start_seq = ctx->reorder_buffer.next_expected;
    uint32_t max_seq = ctx->reorder_buffer.highest_received;
    int packets_delivered = 0;
    
    // First, try to deliver packets in sequence
    while (ctx->reorder_buffer.next_expected <= max_seq) {
        uint32_t index = ctx->reorder_buffer.next_expected % PDCP_MAX_REORDER_WINDOW;
        
        if (!ctx->reorder_buffer.packet_present[index]) {
            // Gap detected
            break;
        }
        
        pdcp_packet_t *pkt = &ctx->reorder_buffer.packets[index];
        
        // Copy packet to output buffer if space allows
        if (pkt->len <= max_len) {
            memcpy(out_buffer, pkt->data, pkt->len);
            *out_len = pkt->len;
            
            // Mark as delivered
            ctx->reorder_buffer.packet_present[index] = false;
            ctx->reorder_buffer.next_expected++;
            packets_delivered++;
            
            // Only deliver one packet per call
            pthread_mutex_unlock(&ctx->reorder_buffer.buffer_mutex);
            return packets_delivered;
        } else {
            // Output buffer too small
            break;
        }
    }
    
    // If we couldn't deliver in sequence, check for timed out packets
    if (packets_delivered == 0) {
        for (uint32_t seq = start_seq; seq <= max_seq; seq++) {
            uint32_t index = seq % PDCP_MAX_REORDER_WINDOW;
            
            if (!ctx->reorder_buffer.packet_present[index]) {
                continue;
            }
            
            pdcp_packet_t *pkt = &ctx->reorder_buffer.packets[index];
            long elapsed_ms = timespec_diff_ms(&pkt->timestamp, &now);
            
            if (elapsed_ms > ctx->reorder_buffer.reorder_timeout_ms) {
                // Packet timed out, deliver it out of order if space allows
                if (pkt->len <= max_len) {
                    memcpy(out_buffer, pkt->data, pkt->len);
                    *out_len = pkt->len;
                    
                    // Mark as delivered
                    ctx->reorder_buffer.packet_present[index] = false;
                    
                    // Update next_expected if this was the next one
                    if (seq == ctx->reorder_buffer.next_expected) {
                        ctx->reorder_buffer.next_expected++;
                    }
                    
                    packets_delivered = 1;
                    break;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&ctx->reorder_buffer.buffer_mutex);
    return packets_delivered;
}