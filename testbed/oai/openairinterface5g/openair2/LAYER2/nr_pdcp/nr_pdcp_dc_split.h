#ifndef NR_PDCP_DC_SPLIT_H
#define NR_PDCP_DC_SPLIT_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#define HISTORY_SIZE 50  // Track the last 50 packets per user


// Structure to hold per-user state
typedef struct {
    int user_id;
    double ewma_lte_throughput;
    double ewma_nr_throughput;
    double lte_queue_size;
    double nr_queue_size;
    double lte_acked_bytes;
    double nr_acked_bytes;
    double target_split_ratio;  // Target split ratio for this user
    bool routing_history[HISTORY_SIZE];  // User-specific routing history (true for LTE, false for NR)
    int routing_index;  // Current position in the routing history buffer
    bool use_dc;
    pthread_mutex_t mutex;
    bool is_active;  // Flag to signal thread to exit
    pthread_t thread_id;  // Thread identifier for the EWMA update thread

    /* QCON Stage 8 microbench counters — incremented inside
     * pdcp_multi_path_routing() while user->mutex is held; publisher
     * reads them under the same lock. */
    uint64_t lte_packets;
    uint64_t nr_packets;
} DC_user;

// Load the configuration from config.csv
void load_config();

// Function to update queue sizes and throughput info for each user
void update_nr_info(DC_user *user, int nr_acked, int nr_queue_size);
void update_lte_info(DC_user *user, int lte_acked, int lte_queue_size);

// Update EWMA (Exponentially Weighted Moving Average) for throughput
double update_ewma(double current_throughput, double previous_ewma, double alpha);

// Calculate split ratio based on recent throughput and queue size
void calculate_split_ratio(DC_user *user);

// Perform packet routing based on the calculated split ratio
bool pdcp_multi_path_routing(DC_user *user);

// Count how many packets were routed to LTE in the user's recent history
int count_lte_packets(DC_user *user);

// Thread function declaration
void *update_ewma_thread(void *arg);

// Function to start the EWMA update thread for a user
void start_ewma_update_thread(DC_user *user);

// Function to stop the thread and remove the DC_user
void remove_dc_user(DC_user *user);

#endif