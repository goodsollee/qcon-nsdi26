#include "nr_pdcp_dc_split.h"
#include <sys/time.h>  // Required for gettimeofday()
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define LOG_BASE_DIR "../../../qcon/config.csv"

// Global variables for storing configuration settings
char mp_technique[50] = "Commercial5G";  // Default technique
double buffer_threshold = 50000;  // Default buffer threshold (in B)
double dc_disable_buffer_thres = 1000;
double alpha = 0.2;  // EWMA smoothing factor

// Function to load the configuration from config.csv
void load_config() {
    FILE *file = fopen(LOG_BASE_DIR, "r");
    if (!file) {
        //printf("Error: Could not open %s\n", LOG_BASE_DIR);
        return;
    }

    //printf("Load configuration for dual-connectivity splitting!\n");

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *field = strtok(line, ",");
        if (field) {
            if (strcmp(field, "MP-TECHNIQUE") == 0) {
                char *value = strtok(NULL, "\n");
                if (value) {
                    strncpy(mp_technique, value, sizeof(mp_technique) - 1);
                    printf("MP-TECHNIQUE: %s\n", mp_technique);
                }
            } else if (strcmp(field, "BUFFER-THRESHOLD") == 0) {
                char *value = strtok(NULL, "\n");
                if (value) {
                    buffer_threshold = atof(value);
                    printf("BUFFER-THRESHOLD: %.2f KB\n", buffer_threshold);
                }
            } else if (strcmp(field, "ALPHA") == 0) {
                char *value = strtok(NULL, "\n");
                if (value) {
                    alpha = atof(value);
                    printf("ALPHA: %.2f\n", alpha);
                }
            }
        }
    }

    fclose(file);
}

// Function to update queue sizes and throughput info for each user
void update_nr_info(DC_user *user, int nr_acked, int nr_queue_size) {
    // Update the queue sizes
    pthread_mutex_lock(&user->mutex);
    user->nr_queue_size = (double)nr_queue_size;
    user->nr_acked_bytes += (double)nr_acked;

    // Print the updated values
    //printf("NR Info Updated for User %d:\n", user->user_id);
    //printf("  NR Acuumulated Acked Bytes: %.2f bytes, Current Acked Bytes: %d\n", user->nr_acked_bytes, nr_acked);
    //printf("  NR Queue Size: %.2f bytes\n", user->nr_queue_size);

    pthread_mutex_unlock(&user->mutex);
}

// Function to update queue sizes and throughput info for each user
void update_lte_info(DC_user *user, int lte_acked, int lte_queue_size) {
    // Update the queue sizes
    pthread_mutex_lock(&user->mutex);
    user->lte_queue_size = (double)lte_queue_size;
    user->lte_acked_bytes += (double)lte_acked;

    //printf("LTE Info Updated for User %d:\n", user->user_id);
    //printf("  LTE Acuumulated Acked Bytes: %.2f bytes, Current Acked Bytes: %d\n", user->lte_acked_bytes, lte_acked);
    //printf("  LTE Queue Size: %.2f bytes\n", user->lte_queue_size);
    pthread_mutex_unlock(&user->mutex);
}


// Function to update EWMA (Exponentially Weighted Moving Average)
double update_ewma(double current_throughput, double previous_ewma, double alpha) {
    if (previous_ewma == 0) {
        // If this is the first time receiving a throughput value, return current throughput directly
        return current_throughput;
    }
    // Otherwise, compute EWMA with the given alpha
    return alpha * current_throughput + (1.0 - alpha) * previous_ewma;
}

// Function to calculate the split ratio based on LTE and NR throughput and queue sizes
void calculate_split_ratio(DC_user *user) {
    pthread_mutex_lock(&user->mutex);
    // Calculate queueing delay for both LTE and NR
    double lte_delay = (user->lte_queue_size / (user->ewma_lte_throughput + 1e-3)); 
    double nr_delay = (user->nr_queue_size / (user->ewma_nr_throughput + 1e-3));     

    // Apply multi-path technique based on configuration
    if (strcmp(mp_technique, "OnlyLTE") == 0) {
        // Route all packets to LTE
        user->target_split_ratio = 1.0;  // 100% to LTE
        //printf("DC_user %d - OnlyLTE selected. All packets routed to LTE.\n", user->user_id);
    } 
    else if (strcmp(mp_technique, "OnlyNR") == 0) {
        // Route all packets to NR
        user->target_split_ratio = 0.0;  // 100% to NR
        //printf("DC_user %d - OnlyNR selected. All packets routed to NR.\n", user->user_id);
    } 
    else if (strcmp(mp_technique, "minRTT") == 0) { // minRTT
        // Route based on buffer threshold
        if (user->nr_queue_size > buffer_threshold || user->use_dc) {
            if (user->nr_queue_size < dc_disable_buffer_thres) {
                user->use_dc = false;
            } else {
                user->use_dc = true;
            }
            if (lte_delay < nr_delay) {
                // Route all packets to LTE if RTT is lower
                user->target_split_ratio = 1;  // 100% LTE
                //printf("DC_user %d - Min-RTT: Routing all packets to LTE. RTT_LTE: %.2f, RTT_NR: %.2f\n", user->user_id, lte_delay, nr_delay);
            } else {
                // Route all packets to NR if RTT is lower
                user->target_split_ratio = 0.0;  // 100% NR
                //printf("DC_user %d - Min-RTT: Routing all packets to NR. RTT_LTE: %.2f, RTT_NR: %.2f\n", user->user_id, lte_delay, nr_delay);
            }
        } else {
            // If LTE queue is within threshold, route all packets to NR
            user->target_split_ratio = 0.0;  // Route all to NR
            //printf("DC_user %d - Commercial5G: LTE Queue <= %.2f KB. Routing all packets to LTE.\n", user->user_id, buffer_threshold);
        }
    } 
    else if (strcmp(mp_technique, "Throughput") == 0) { // throughput-based [MobiCom'19, Musher]
        // Advanced multi-path balancing logic with buffer threshold and EWMA throughput ratio
        if (user->nr_queue_size > buffer_threshold || user->use_dc) {
            if (user->nr_queue_size < dc_disable_buffer_thres) {
                user->use_dc = false;
            } else {
                user->use_dc = true;
            }
            // If LTE queue size exceeds the buffer threshold, offload traffic to NR
            if (user->ewma_lte_throughput < 1e-3) {
                // NR throughput is zero, but offload some traffic to NR due to LTE buffer overflow
                user->target_split_ratio = 0.1;  // 10% to LTE, 90% to NR
                //printf("DC_user %d - LTE queue exceeds buffer threshold (%.2f KB), offloading 20%% to NR.\n", user->user_id, buffer_threshold);
            } else {
                // Normal throughput-based splitting when NR throughput is non-zero
                double total_ewma_throughput = user->ewma_lte_throughput + user->ewma_nr_throughput;
                user->target_split_ratio = user->ewma_lte_throughput / total_ewma_throughput;
                //printf("DC_user %d - LTE queue exceeds threshold, splitting based on EWMA throughput ratio (LTE: %.2f, NR: %.2f).\n", user->user_id, user->ewma_lte_throughput, user->ewma_nr_throughput);
            }
        } else {
            user->target_split_ratio = 0.0;
            printf("DC_user %d - NR queue does not exceeds threshold, use only NR link.\n", user->user_id);
            if (user->nr_queue_size < dc_disable_buffer_thres) {
                user->use_dc = false;
            }
        }
    }
    else if (strcmp(mp_technique, "PAVE") == 0) { 
        // TODO (in RIC)
        user->target_split_ratio = 0.0;
    }

    // Convert delay to milliseconds
    double lte_delay_ms = lte_delay * 1000;
    double nr_delay_ms = nr_delay * 1000;

    // Convert throughput to Mbps (bytes to bits * 8, then divide by 1,000,000 to get Mbps)
    double lte_throughput_mbps = (user->ewma_lte_throughput * 8) / 1e6;
    double nr_throughput_mbps = (user->ewma_nr_throughput * 8) / 1e6;

    // Print the technique, target split ratio, delay, throughput, and queue size information
    
    /*
    printf("UE: %d, Technique: %s, Target split ratio: %.2f, Is active: %s\n", user->user_id, mp_technique, user->target_split_ratio, user->is_active ? "true" : "false");
    printf("LTE delay (ms): %.3f, NR delay (ms): %.3f\n", lte_delay_ms, nr_delay_ms);
    printf("LTE throughput (Mbps): %.6f, NR throughput (Mbps): %.6f\n", lte_throughput_mbps, nr_throughput_mbps);
    printf("LTE queue size: %.2f B, NR queue size: %.2f B\n", user->lte_queue_size, user->nr_queue_size);
    */
    
    pthread_mutex_unlock(&user->mutex);
}

// Function to count how many packets were routed to LTE in the user's recent history
int count_lte_routing(DC_user *user) {
    int count = 0;
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (user->routing_history[i]) {
            count++;
        }
    }
    return count;
}

// Function to perform packet routing based on the user's split ratio
bool pdcp_multi_path_routing(DC_user *user) {
    // Calculate the current split ratio from history
    // Recalculate split ratio
    //calculate_split_ratio(user);

    pthread_mutex_lock(&user->mutex);
    int lte_count = count_lte_routing(user);
    double current_split_ratio = (double)lte_count / HISTORY_SIZE;

    // Adjust routing based on target split ratio
    if (current_split_ratio <= user->target_split_ratio -1e-3) {
        // Route to LTE if we haven't met the target split ratio
        user->routing_history[user->routing_index] = true;
        user->routing_index = (user->routing_index + 1) % HISTORY_SIZE;
        user->lte_packets++;
        pthread_mutex_unlock(&user->mutex);
        return true;  // Route to LTE
    } else {
        // Otherwise, route to NR
        user->routing_history[user->routing_index] = false;
        user->routing_index = (user->routing_index + 1) % HISTORY_SIZE;
        user->nr_packets++;
        pthread_mutex_unlock(&user->mutex);
        return false;  // Route to NR
    }
}

void *update_ewma_thread(void *arg) {
    DC_user *user = (DC_user *)arg;
    struct timeval last_time, current_time;  // Correct declaration of timeval
    struct timespec sleep_time;
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 100 * 1000000;  // 100ms

    // Get current time
    gettimeofday(&last_time, NULL);

    while (1) {
        nanosleep(&sleep_time, NULL);

        // Get the current time
        gettimeofday(&current_time, NULL);

        // Calculate elapsed time in seconds (including fractions)
        double elapsed_time = (current_time.tv_sec - last_time.tv_sec) + 
                              (current_time.tv_usec - last_time.tv_usec) / 1e6;

        // If no time has elapsed (very unlikely), use a small value to avoid division by zero
        if (elapsed_time == 0) {
            elapsed_time = 0.1;  // Fallback to 100ms
        }

        pthread_mutex_lock(&user->mutex);

        // Check if the thread should exit
        if (!user->is_active) {
            pthread_mutex_unlock(&user->mutex);
            //printf("Update thread ends\n");
            break;
        }

        // Calculate current throughput (bytes per second)
        double current_lte_throughput = user->lte_acked_bytes / elapsed_time;
        double current_nr_throughput = user->nr_acked_bytes / elapsed_time;

        //printf("NR TP (Mbps): %.2f, %.2f, %.3f\n", current_nr_throughput * 8 / 1e6, user->nr_acked_bytes, elapsed_time);

        // Update EWMA
        user->ewma_lte_throughput = update_ewma(current_lte_throughput, user->ewma_lte_throughput, alpha);
        user->ewma_nr_throughput = update_ewma(current_nr_throughput, user->ewma_nr_throughput, alpha);

        // Reset acked bytes
        user->lte_acked_bytes = 0;
        user->nr_acked_bytes = 0;

        pthread_mutex_unlock(&user->mutex);

        // Update last_time for the next iteration
        last_time = current_time;

        // Perform additional processing (e.g., calculate split ratio)
        calculate_split_ratio(user);
    }

    return NULL;
}

void start_ewma_update_thread(DC_user *user) {
    int result = pthread_create(&user->thread_id, NULL, update_ewma_thread, (void *)user);  // Store thread_id in DC_user

    if (result != 0) {
        //printf("Failed to create throughput update thread for user %d\n", user->user_id);
    } else {
        //printf("Started throughput update thread for user %d\n", user->user_id);
    }
}

// Function to stop the thread and remove the DC_user
void remove_dc_user(DC_user *user) {
    // Signal the thread to stop by setting the is_active flag to false
    pthread_mutex_lock(&user->mutex);
    user->is_active = false;
    pthread_mutex_unlock(&user->mutex);
    // Wait for the thread to exit
    pthread_join(user->thread_id, NULL);

    // Clean up resources (e.g., destroy mutex)
    pthread_mutex_destroy(&user->mutex);

    // Free the memory associated with the user (if dynamically allocated)
    free(user);
}