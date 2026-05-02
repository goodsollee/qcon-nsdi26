/* QCON Stage 6b LTE-side companion: publish per-UE LTE MCS+RB to mock_ric.
 *
 * Mirrors nr_pdcp_ran_func.cpp's deliver_mcs_rb_to_ric() but emits leg=0
 * (LTE) instead of leg=1 (NR). Runs inside lte-softmodem so it has direct
 * access to RC.mac[]->UE_info / eNB_UE_stats[][].
 *
 * Architecture: separate ZMQ DEALER from this binary to mock_ric:7878.
 * mock_ric (ROUTER) accepts both gNB and eNB peers concurrently and
 * stamps each peer in its peers[] map. Inbound commands (split_ratio,
 * etc.) are gNB-targeted, so we only PUBLISH from this side and ignore
 * any FIFO broadcasts that hit our DEALER.
 */
#include <string>
#include <jsoncpp/json/json.h>
#include <zmq.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

extern "C" {
#include "common/utils/LOG/log.h"
#include "common/ran_context.h"
#include "openair2/LAYER2/MAC/mac.h"
#include "openair2/LAYER2/MAC/mac_extern.h"
#include "openair2/ENB_APP/enb_config.h"
}

#define LTE_RIC_ENDPOINT "tcp://127.0.0.1:7878"

static void *g_ctx = nullptr;
static void *g_sock = nullptr;
static pthread_mutex_t g_sock_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_pub_thread;
static volatile int g_running = 0;
static int g_period_ms = 100;

static void send_json(const char *header, int ue_id, const std::string &payload) {
    Json::Value msg;
    msg["header"] = header;
    msg["payload"] = payload;
    msg["ue_id"]   = ue_id;
    msg["sn"]      = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* Json::Int64 wraps as a plain integer — RIC parses fine */
    msg["timestamp"] = (Json::Int64)((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);

    Json::FastWriter w;
    std::string s = w.write(msg);
    pthread_mutex_lock(&g_sock_mu);
    zmq_send(g_sock, s.c_str(), s.length(), 0);
    pthread_mutex_unlock(&g_sock_mu);
}

extern "C" void lte_emit_mcs_rb_kpm(void) {
    if (RC.mac == NULL || RC.mac[0] == NULL) return;
    eNB_MAC_INST *mac = RC.mac[0];
    UE_info_t *UE_info = &mac->UE_info;

    for (int UE_id = 0; UE_id < MAX_MOBILES_PER_ENB; UE_id++) {
        if (!UE_info->active[UE_id]) continue;
        eNB_UE_STATS *stats = &UE_info->eNB_UE_stats[0][UE_id];   /* CC_id=0 */
        if (stats->crnti == 0) continue;

        int rnti = (int)stats->crnti;
        int mcs  = stats->dlsch_mcs2 ? stats->dlsch_mcs2 : stats->dlsch_mcs1;
        int rbs  = (int)stats->rbs_used;

        int total = 0, retx = 0;
        for (int i = 0; i < 8; i++) total += stats->dlsch_rounds[i];
        for (int i = 1; i < 8; i++) retx  += stats->dlsch_rounds[i];
        int bler_pct = (total > 0) ? (retx * 100 / total) : 0;

        /* LTE FDD subframes are 1ms — 1000 slots/sec. SISO single layer. */
        int slots_per_sec = 1000;
        int n_layers = 1;

        Json::Value p;
        p["leg"]            = 0;             /* 0 = LTE */
        p["mcs"]            = mcs;
        p["rbs"]            = rbs;
        p["layers"]         = n_layers;
        p["bler_pct"]       = bler_pct;
        p["bw_est_mbps"]    = 0.0;           /* receiver derives from MCS+RB tables */
        p["slots_per_sec"]  = slots_per_sec;

        Json::FastWriter w;
        std::string ps = w.write(p);
        send_json("mcs_rb", rnti, ps);
    }
}

static void* publisher_loop(void *arg) {
    (void)arg;
    while (g_running) {
        lte_emit_mcs_rb_kpm();
        usleep(g_period_ms * 1000);
    }
    return nullptr;
}

extern "C" void lte_init_ran_function(void) {
    if (g_running) return;     /* idempotent */

    g_ctx  = zmq_ctx_new();
    g_sock = zmq_socket(g_ctx, ZMQ_DEALER);
    int linger = 0;
    zmq_setsockopt(g_sock, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(g_sock, LTE_RIC_ENDPOINT) != 0) {
        printf("[QCON-LTE-RIC] zmq_connect failed for %s\n", LTE_RIC_ENDPOINT);
        return;
    }
    printf("[QCON-LTE-RIC] connected to RIC at %s\n", LTE_RIC_ENDPOINT);

    /* Send a ready heartbeat so mock_ric registers the eNB peer. */
    Json::Value p;
    p["build"] = "qcon-bringup";
    p["node"]  = "lte-eNB";
    p["pid"]   = (int)getpid();
    Json::FastWriter w;
    std::string ps = w.write(p);
    send_json("ready", 0, ps);

    g_running = 1;
    if (pthread_create(&g_pub_thread, nullptr, publisher_loop, nullptr) != 0) {
        printf("[QCON-LTE-RIC] failed to spawn publisher thread\n");
        g_running = 0;
        return;
    }
    printf("[QCON-LTE-RIC] publisher thread up (period=%dms)\n", g_period_ms);
}
