#ifndef NR_PDCP_RAN_FUNC_H_
#define NR_PDCP_RAN_FUNC_H_

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <zmq.h>

#include "nr_pdcp_dc_split.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZMQ_ENDPOINT "tcp://127.0.0.1:7878"
#define MAX_MSG_SIZE  4096

typedef struct {
  void *zmq_context;
  void *zmq_socket;
  pthread_t recv_thread;
  pthread_t send_thread;
  pthread_mutex_t mutex;          /* protects users[] and zmq_socket struct */
  pthread_mutex_t socket_mutex;   /* protects zmq_send / zmq_recv on the socket */
  bool is_running;
  DC_user **users;
  int num_users;
} RAN_Function;

// Lifecycle
bool init_ran_function(void);
void cleanup_ran_function(void);

// Outbound — call from PDCP/MAC hot paths to publish KPI
//   header: e.g. "pdcp", "mcs_rb", "frame"
//   buf: JSON-encoded payload (will be free'd by send_msg)
void deliver_info_to_ric(const char* header, int ue_id, int sn, char* buf);

// Internal: actually emit one ZMQ message (consumes buf)
void send_msg(const char* header, char* buf, int ue_id, int sn, uint64_t timestamp);

// Inbound — applied when RIC sends a split_ratio update
void update_split_ratio(DC_user *user, double new_ratio);

// Stage 6: register a DC_user so the KPM publisher includes it.
//   Call once after DC_user is fully initialized (use_dc=false initially is OK).
void register_dc_user_with_ric(DC_user *user);
void unregister_dc_user_from_ric(DC_user *user);

// Stage 6b: MAC-side KPM emitter (defined in nr_pdcp_oai_api.c).
//   Iterates RC.nrmac[0]->UE_info and ships per-UE deliver_mcs_rb_to_ric.
//   Safe to call from any thread; takes mac->sched_lock internally.
void qcon_emit_mac_kpm(void);

// QCON Stage 6 — bandwidth estimation from MAC scheduler MCS+RB
//   Called from gNB MAC scheduler with the per-slot allocation snapshot.
//   bits_per_re = TBS-derived bits per RE for the chosen MCS index.
//   slots_per_sec = 14 sym * 1000 ms * (1<<scs_idx) / 14 ≈ slots/sec for the BWP.
void deliver_mcs_rb_to_ric(int ue_id, int leg /* 0=LTE 1=NR */,
                            int mcs_index, int allocated_rbs,
                            int n_layers, double bits_per_re,
                            int slots_per_sec, int bler_pct);

#ifdef __cplusplus
}
#endif

#endif /* NR_PDCP_RAN_FUNC_H_ */
