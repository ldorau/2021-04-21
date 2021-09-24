/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2020-2021, Intel Corporation */

/*
 * connection.h -- connection functions declarations
 */

#ifndef EXAMPLES_COMMON
#define EXAMPLES_COMMON

#include <string.h>
#include <librpma.h>

#define KILOBYTE 1024

/* signature marking the persistent contents as valid */
#define SIGNATURE_STR "RPMA_EXAMPLE_SIG"
#define SIGNATURE_LEN (strlen(SIGNATURE_STR) + 1)

#define NO_PMEM_MSG "No <pmem-path> provided. Using DRAM instead.\n"

/*
 * Limited by the maximum length of the private data
 * for rdma_connect() in case of RDMA_PS_TCP (56 bytes).
 */
#define DESCRIPTORS_MAX_SIZE 24

struct common_data {
	uint16_t data_offset;	/* user data offset */
	uint8_t mr_desc_size;	/* size of mr_desc in descriptors[] */
	uint8_t pcfg_desc_size;	/* size of pcfg_desc in descriptors[] */
	/* buffer containing mr_desc and pcfg_desc */
	char descriptors[DESCRIPTORS_MAX_SIZE];
};

void *malloc_aligned(size_t size);

int common_peer_via_address(const char *addr,
		enum rpma_util_ibv_context_type type,
		struct rpma_peer **peer_ptr);

#define client_peer_via_address(addr, peer_ptr) \
		common_peer_via_address(addr, RPMA_UTIL_IBV_CONTEXT_REMOTE, \
				peer_ptr)

#define server_peer_via_address(addr, peer_ptr) \
		common_peer_via_address(addr, RPMA_UTIL_IBV_CONTEXT_LOCAL, \
				peer_ptr)

int client_connect(struct rpma_peer *peer, const char *addr, const char *port,
		struct rpma_conn_cfg *cfg, struct rpma_conn_private_data *pdata,
		struct rpma_conn **conn_ptr);

int server_accept_connection(struct rpma_ep *ep, struct rpma_conn_cfg *cfg,
		struct rpma_conn_private_data *pdata,
		struct rpma_conn **conn_ptr);

int common_wait_for_conn_close_and_disconnect(struct rpma_conn **conn_ptr);
int common_disconnect_and_wait_for_conn_close(struct rpma_conn **conn_ptr);

#endif /* EXAMPLES_COMMON */
