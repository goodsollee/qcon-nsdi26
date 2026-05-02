/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file x2ap_eNB.h
 * \brief x2ap tasks for eNB
 * \author Konstantinos Alexandris <Konstantinos.Alexandris@eurecom.fr>, Cedric Roux <Cedric.Roux@eurecom.fr>, Navid Nikaein <Navid.Nikaein@eurecom.fr>
 * \date 2018
 * \version 1.0
 */

#include <stdio.h>
#include <stdint.h>

/** @defgroup _x2ap_impl_ X2AP Layer Reference Implementation
 * @ingroup _ref_implementation_
 * @{
 */

#ifndef X2AP_H_
#define X2AP_H_

#define X2AP_SCTP_PPID   (27)    ///< X2AP SCTP Payload Protocol Identifier (PPID)
#include "x2ap_eNB_defs.h"
//#include "../COMMON/rrc_messages_def.h"

// PAVE
#include <zmq.h>
#include <string.h>


int x2ap_eNB_init_sctp (x2ap_eNB_instance_t *instance_p,
                        net_ip_address_t    *local_ip_addr,
                        uint32_t enb_port_for_X2C);

void *x2ap_task(void *arg);

int is_x2ap_enabled(void);
void x2ap_trigger(void);

// Declare global ZeroMQ context and socket
extern void *x2ap_zmq_send_socket;
extern void *x2ap_zmq_recv_socket;
extern void *x2ap_zmq_context;

// Initialize ZeroMQ (call this once, e.g., during initialization)
void x2ap_zmq_init(const char* send_ip_addr, const char* recv_ip_addr);

// Function to send message using the persistent ZeroMQ socket
void x2ap_zmq_send(const char *data);
void x2ap_zmq_recv(char *buffer, size_t buffer_size);

#endif /* X2AP_H_ */

/**
 * @}
 */
