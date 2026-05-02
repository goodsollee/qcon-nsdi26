#include "nr_pdcp_ran_func.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <jsoncpp/json/json.h>

extern "C" {
#include "common/platform_types.h"
#include "common/platform_constants.h"
#include "openair2/LAYER2/RLC/rlc.h"
#include "openair2/SDAP/nr_sdap/nr_sdap.h"
}

/* QCON Stage 8: minimal base64 decoder for the reinject payload.
 * We control both ends (mock_ric writes payload via b64encode), so we
 * keep this small and tolerant — strict char tables, no padding fixup. */
static int qcon_b64_decode(const std::string &in, std::vector<uint8_t> &out) {
    static const char *ALPHA =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int8_t T[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        for (int i = 0; i < 64; i++)  T[(uint8_t)ALPHA[i]] = (int8_t)i;
        inited = true;
    }
    out.clear();
    int v = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t d = T[(uint8_t)c];
        if (d < 0) continue;
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((v >> bits) & 0xff));
        }
    }
    return (int)out.size();
}

// Forward declarations
static void* recv_msg(void *arg);
static void* kpm_publisher_thread(void *arg);

// Global RAN function instance
static RAN_Function ran_func_instance;
static pthread_t kpm_thread_handle;
static int kpm_period_ms = 100;

// Initialize RAN function
bool init_ran_function() {
    // Initialize ZMQ context and socket
    ran_func_instance.zmq_context = zmq_ctx_new();
    ran_func_instance.zmq_socket = zmq_socket(ran_func_instance.zmq_context, ZMQ_DEALER);
    
    /* Block in zmq_recv at most 20ms so the recv thread releases the
     * socket_mutex periodically — avoids starving sends and avoids the
     * tight ZMQ_DONTWAIT polling loop that conflicted with OAI's RT
     * scheduler (manifested as __pthread_tpp_change_priority assert). */
    int rcv_to_ms = 20;
    zmq_setsockopt(ran_func_instance.zmq_socket, ZMQ_RCVTIMEO, &rcv_to_ms, sizeof(rcv_to_ms));

    if (zmq_connect(ran_func_instance.zmq_socket, ZMQ_ENDPOINT) != 0) {
        printf("Failed to connect to RIC at %s\n", ZMQ_ENDPOINT);
        return false;
    }

    // Initialize other members
    ran_func_instance.is_running = true;
    pthread_mutex_init(&ran_func_instance.mutex, NULL);
    pthread_mutex_init(&ran_func_instance.socket_mutex, NULL);
    ran_func_instance.users = NULL;
    ran_func_instance.num_users = 0;

    // Start recv msg thread
    if (pthread_create(&ran_func_instance.recv_thread, NULL, recv_msg, NULL) != 0) {
        printf("Failed to create receive thread\n");
        pthread_mutex_destroy(&ran_func_instance.mutex);
        zmq_close(ran_func_instance.zmq_socket);
        zmq_ctx_destroy(ran_func_instance.zmq_context);
        return false;
    }

    printf("RAN function initialized and connected to RIC at %s\n", ZMQ_ENDPOINT);

    /* QCON: emit a ready heartbeat so ROUTER side learns our identity and
     * our pipe is provably alive even before MAC starts publishing MCS/RB. */
    {
      Json::Value p;
      p["build"] = "qcon-bringup";
      p["pid"]   = (int)getpid();
      Json::FastWriter w;
      std::string s = w.write(p);
      deliver_info_to_ric("ready", 0, 0, (char*)s.c_str());
    }

    /* QCON Stage 6a: spawn KPM publisher thread (per-UE EWMA throughput).
     * The thread iterates ran_func_instance.users every kpm_period_ms and
     * sends one "kpm" message per registered DC_user. */
    if (pthread_create(&kpm_thread_handle, NULL, kpm_publisher_thread, NULL) != 0) {
      printf("Failed to create KPM publisher thread\n");
      // non-fatal; keep ran_func up
    }

    return true;
}

// Periodic KPM publisher: per-UE LTE/NR throughput + queue + ratio.
// Stage 6a — uses DC_user's existing EWMA throughput maintained by the
// dc_split EWMA thread. Stage 6b will add MCS+RB-derived BW estimate.
static void* kpm_publisher_thread(void *arg) {
  (void)arg;
  while (ran_func_instance.is_running) {
    pthread_mutex_lock(&ran_func_instance.mutex);
    int n = ran_func_instance.num_users;
    for (int i = 0; i < n; i++) {
      DC_user *u = ran_func_instance.users[i];
      if (!u || !u->is_active) continue;
      pthread_mutex_lock(&u->mutex);
      Json::Value p;
      p["user_id"]                 = u->user_id;
      p["ewma_lte_mbps"]           = u->ewma_lte_throughput * 8.0 / 1e6;
      p["ewma_nr_mbps"]            = u->ewma_nr_throughput  * 8.0 / 1e6;
      p["lte_queue_bytes"]         = u->lte_queue_size;
      p["nr_queue_bytes"]          = u->nr_queue_size;
      p["target_split_ratio"]      = u->target_split_ratio;
      /* Stage 8 microbench observables — read counters under same lock
       * as updater (pdcp_multi_path_routing). jsoncpp UInt64 supported
       * since 1.7; cast keeps it explicit. */
      p["lte_pkts"]                = (Json::UInt64)u->lte_packets;
      p["nr_pkts"]                 = (Json::UInt64)u->nr_packets;
      pthread_mutex_unlock(&u->mutex);
      Json::FastWriter w;
      std::string s = w.write(p);
      deliver_info_to_ric("kpm", u->user_id, 0, (char*)s.c_str());
    }
    pthread_mutex_unlock(&ran_func_instance.mutex);
    /* Stage 6b: also publish gNB MAC capacity (MCS+RB-derived) per-UE. */
    qcon_emit_mac_kpm();
    usleep(kpm_period_ms * 1000);
  }
  return NULL;
}

// Function to create JSON message for user metrics
static char* create_metrics_json(DC_user *user) {
    Json::Value metrics;
    Json::FastWriter writer;
    
    metrics["user_id"] = user->user_id;
    metrics["lte_throughput"] = user->ewma_lte_throughput;
    metrics["nr_throughput"] = user->ewma_nr_throughput;
    metrics["lte_queue_size"] = user->lte_queue_size;
    metrics["nr_queue_size"] = user->nr_queue_size;
    metrics["current_split_ratio"] = user->target_split_ratio;

    std::string json_str = writer.write(metrics);
    return strdup(json_str.c_str());
}

// Thread function to receive messages from RIC
static void* recv_msg(void *arg) {
    char buffer[MAX_MSG_SIZE];
    Json::Reader reader;
    Json::Value msg;

    while (ran_func_instance.is_running) {
        /* RCVTIMEO=20ms is set on the socket. zmq_recv blocks at most
         * that long, then returns EAGAIN. We hold socket_mutex for the
         * call so it doesn't race with send_msg's zmq_send. */
        pthread_mutex_lock(&ran_func_instance.socket_mutex);
        int bytes = zmq_recv(ran_func_instance.zmq_socket, buffer, MAX_MSG_SIZE - 1, 0);
        pthread_mutex_unlock(&ran_func_instance.socket_mutex);
        if (bytes < 0) continue;   /* EAGAIN -> retry */
        if (bytes > 0) {
            buffer[bytes] = '\0';
            printf("[QCON-RIC] recv %dB: %.200s\n", bytes, buffer);

            if (reader.parse(buffer, buffer + bytes, msg)) {
                if (msg.isMember("header") && msg.isMember("payload")) {
                    std::string header = msg["header"].asString();
                    
                    // Handle different message types based on header
                    if (header == "split_ratio") {
                        // Handle split ratio update
                        Json::Value payload = msg["payload"];
                        if (payload.isMember("user_id") && payload.isMember("ratio")) {
                            int user_id = payload["user_id"].asInt();
                            double new_ratio = payload["ratio"].asDouble();
                            
                            pthread_mutex_lock(&ran_func_instance.mutex);
                            for (int i = 0; i < ran_func_instance.num_users; i++) {
                                if (ran_func_instance.users[i]->user_id == user_id) {
                                    update_split_ratio(ran_func_instance.users[i], new_ratio);
                                    break;
                                }
                            }
                            pthread_mutex_unlock(&ran_func_instance.mutex);
                        }
                    }
                    else if (header == "reinject") {
                        /* Inject a synthetic IP packet into the gNB DL pipeline so
                         * it reaches the UE on rmnet1, exactly as if it had come
                         * from the TUN read thread (sdap_data_req at oai_api.c:452).
                         * Payload schema:
                         *   { "user_id": <int>, "rb_id": <int=1>, "packet_data": <b64> }
                         * If user_id is omitted, inject for the first registered DC_user. */
                        Json::Value payload = msg["payload"];
                        if (!payload.isMember("packet_data")) {
                            printf("[QCON-RIC] reinject: missing packet_data\n");
                        } else {
                            std::vector<uint8_t> bytes;
                            qcon_b64_decode(payload["packet_data"].asString(), bytes);
                            int rb_id = payload.get("rb_id", 1).asInt();
                            int target_uid = payload.get("user_id", 0).asInt();

                            ue_id_t UEid = 0;
                            pthread_mutex_lock(&ran_func_instance.mutex);
                            for (int i = 0; i < ran_func_instance.num_users; i++) {
                                DC_user *u = ran_func_instance.users[i];
                                if (!u) continue;
                                if (target_uid == 0 || u->user_id == target_uid) {
                                    UEid = u->user_id;
                                    break;
                                }
                            }
                            pthread_mutex_unlock(&ran_func_instance.mutex);

                            if (UEid == 0 || bytes.empty()) {
                                printf("[QCON-RIC] reinject DROP user_id=%d size=%zu (no UE / empty)\n",
                                       target_uid, bytes.size());
                            } else {
                                protocol_ctxt_t ctxt = {};
                                ctxt.module_id = 0;
                                ctxt.enb_flag  = 1;
                                ctxt.instance  = 0;
                                ctxt.frame     = 0;
                                ctxt.subframe  = 0;
                                ctxt.eNB_index = 0;
                                ctxt.brOption  = 0;
                                ctxt.rntiMaybeUEid = UEid;
                                uint8_t qfi = 7;
                                bool rqi    = 0;
                                int pdusession_id = 10;
                                bool ok = sdap_data_req(&ctxt, UEid, SRB_FLAG_NO,
                                                        rb_id, RLC_MUI_UNDEFINED, RLC_SDU_CONFIRM_NO,
                                                        (sdu_size_t)bytes.size(), bytes.data(),
                                                        PDCP_TRANSMISSION_MODE_DATA, NULL, NULL,
                                                        qfi, rqi, pdusession_id);
                                printf("[QCON-RIC] reinject UE=%lu rb=%d size=%zu ok=%d\n",
                                       (unsigned long)UEid, rb_id, bytes.size(), ok);
                            }
                        }
                    }
                    /* drx_config intentionally unsupported: DRX is statically
                     * disabled at the LTE eNB via drx_Config_present="prRelease"
                     * in enb.nsa.band7.25prb.usrpb200.conf. Runtime DRX changes
                     * require RRC Reconfiguration, not a PDCP-level handler. */
                    // Can add more header types as needed
                }
            }
        }
    }
    return NULL;
}

// Structure for thread argument
struct msg_info {
    char* header;
    char* buf;
    int ue_id;
    int sn;
    uint64_t timestamp;
};

// Thread function that just sends the message
static void* deliver_thread_func(void* arg) {
    struct msg_info* msg = (struct msg_info*)arg;
    send_msg(msg->header, msg->buf, msg->ue_id, msg->sn, msg->timestamp);
    free(msg);
    return NULL;
}

// Function called by other modules
void deliver_info_to_ric (const char* header, int ue_id, int sn, char* buf) {
    struct msg_info* msg = (struct msg_info*)malloc(sizeof(struct msg_info));
    msg->header = strdup(header);
    msg->buf = strdup(buf);
    msg->ue_id = ue_id;
    msg->sn = sn;  

    // Get current timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp = ts.tv_sec * 1000000000LL + ts.tv_nsec;  // nanoseconds

    msg->timestamp = timestamp;

    pthread_t thread;
    pthread_create(&thread, NULL, deliver_thread_func, msg);
    pthread_detach(thread);
}

void send_msg (const char* header, char* buf, int ue_id, int sn, uint64_t timestamp) {

    Json::Value msg;
    Json::FastWriter writer;
    
    msg["header"] = header;
    msg["payload"] = buf;
    msg["ue_id"] = ue_id;
    msg["sn"] = sn;
    msg["timestamp"] = timestamp;
    
    std::string msg_str = writer.write(msg);
    /* QCON: ZMQ sockets are NOT thread-safe; serialize sends. The earlier
     * "Assertion failed: check () (src/msg.cpp:387)" crash came from KPM
     * publisher and recv_msg both touching the same socket. */
    pthread_mutex_lock(&ran_func_instance.socket_mutex);
    zmq_send(ran_func_instance.zmq_socket, msg_str.c_str(), msg_str.length(), 0);
    pthread_mutex_unlock(&ran_func_instance.socket_mutex);

    free(buf);
}

// TODO: Verification
void update_split_ratio(DC_user *user, double new_ratio) {
    pthread_mutex_lock(&user->mutex);
    user->target_split_ratio = new_ratio;
    pthread_mutex_unlock(&user->mutex);
    printf("Updated split ratio for user %d to %.2f\n", user->user_id, new_ratio);
}

void register_dc_user_with_ric(DC_user *user) {
    if (!user) return;
    pthread_mutex_lock(&ran_func_instance.mutex);
    // simple linear: ensure not already in list
    for (int i = 0; i < ran_func_instance.num_users; i++) {
        if (ran_func_instance.users[i] == user) {
            pthread_mutex_unlock(&ran_func_instance.mutex);
            return;
        }
    }
    DC_user **bigger = (DC_user**)realloc(ran_func_instance.users,
                                           sizeof(DC_user*) * (ran_func_instance.num_users + 1));
    if (bigger) {
        ran_func_instance.users = bigger;
        ran_func_instance.users[ran_func_instance.num_users++] = user;
        printf("[QCON] Registered DC_user %d with RIC publisher (total=%d)\n",
               user->user_id, ran_func_instance.num_users);
    }
    pthread_mutex_unlock(&ran_func_instance.mutex);
}

void unregister_dc_user_from_ric(DC_user *user) {
    if (!user) return;
    pthread_mutex_lock(&ran_func_instance.mutex);
    for (int i = 0; i < ran_func_instance.num_users; i++) {
        if (ran_func_instance.users[i] == user) {
            for (int j = i; j < ran_func_instance.num_users - 1; j++)
                ran_func_instance.users[j] = ran_func_instance.users[j+1];
            ran_func_instance.num_users--;
            break;
        }
    }
    pthread_mutex_unlock(&ran_func_instance.mutex);
}

// QCON Stage 6 — MCS+RB based bandwidth estimation publisher.
// Computes bw_est_mbps from MCS table + RB count and ships to RIC.
// Called by gNB MAC scheduler each KPM tick (~100ms).
void deliver_mcs_rb_to_ric(int ue_id, int leg,
                           int mcs_index, int allocated_rbs,
                           int n_layers, double bits_per_re,
                           int slots_per_sec, int bler_pct) {
  if (!ran_func_instance.is_running) return;

  // 12 REs per RB per symbol, 14 symbols per slot, minus DMRS overhead (~20%)
  static const double DMRS_OH = 0.80;
  double bw_est_mbps = bits_per_re * 12.0 * 14.0 * DMRS_OH
                       * (double)allocated_rbs * (double)n_layers
                       * (double)slots_per_sec / 1.0e6;
  // BLER correction
  bw_est_mbps *= (1.0 - (double)bler_pct / 100.0);

  Json::Value p;
  p["leg"]            = leg;             // 0=LTE 1=NR
  p["mcs"]            = mcs_index;
  p["rbs"]            = allocated_rbs;
  p["layers"]         = n_layers;
  p["bler_pct"]       = bler_pct;
  p["bw_est_mbps"]    = bw_est_mbps;
  p["slots_per_sec"]  = slots_per_sec;

  Json::FastWriter writer;
  std::string s = writer.write(p);
  deliver_info_to_ric("mcs_rb", ue_id, 0, (char*)s.c_str());
}

void cleanup_ran_function() {
    // Stop receive thread
    ran_func_instance.is_running = false;
    pthread_join(ran_func_instance.recv_thread, NULL);
    pthread_join(ran_func_instance.send_thread, NULL);
    
    // Cleanup ZMQ
    zmq_close(ran_func_instance.zmq_socket);
    zmq_ctx_destroy(ran_func_instance.zmq_context);
    
    // Cleanup users array
    free(ran_func_instance.users);

    // Destroy mutex
    pthread_mutex_destroy(&ran_func_instance.mutex);
    pthread_mutex_destroy(&ran_func_instance.socket_mutex);

    printf("RAN function cleaned up\n");
}
