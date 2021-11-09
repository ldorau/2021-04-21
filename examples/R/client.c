// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020-2021, Intel Corporation */
/* Copyright 2021, Fujitsu */

/*
 * client.c -- a client of the flush-to-persistent example
 */

#include <inttypes.h>
#include <stdlib.h>
#include <librpma.h>
#include <unistd.h>
#include "common.h"

#define USAGE_STR	"usage: %s <server_address> <port>\n"
#define FLUSH_ID	(void *)0xF01D /* a random identifier */

static const char *hello_str = "Hello world!";

/*
 * malloc_aligned -- allocate an aligned chunk of memory
 */
void *
malloc_aligned(size_t size)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize < 0) {
		perror("sysconf");
		return NULL;
	}

	/* allocate a page size aligned local memory pool */
	void *mem;
	int ret = posix_memalign(&mem, (size_t)pagesize, size);
	if (ret) {
		(void) fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
		return NULL;
	}

	/* zero the allocated memory */
	memset(mem, 0, size);

	return mem;
}

int
main(int argc, char *argv[])
{
	/* validate parameters */
	if (argc < 3) {
		fprintf(stderr, USAGE_STR, argv[0]);
		exit(-1);
	}

	/* configure logging thresholds to see more details */
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD, RPMA_LOG_LEVEL_INFO);
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD_AUX, RPMA_LOG_LEVEL_INFO);

	/* read common parameters */
	char *addr = argv[1];
	char *port = argv[2];
	int ret;

	/* resources - memory region */
	void *local_mr_ptr;
	size_t local_mr_size;
	size_t local_offset = 0;
	struct rpma_mr_remote *remote_mr = NULL;
	size_t remote_size = 0;
	size_t remote_offset = 0;
	struct rpma_mr_local *local_mr = NULL;
	struct rpma_completion cmpl;

	/* RPMA resources */
	struct rpma_peer_cfg *pcfg = NULL;
	struct rpma_peer *peer = NULL;
	struct rpma_conn *conn = NULL;
	bool direct_write_to_pmem = false;
	enum rpma_flush_type flush_type;

	local_mr_size = KILOBYTE;
	local_mr_ptr = malloc_aligned(local_mr_size);
	if (local_mr_ptr == NULL)
		return -1;

	/*
	 * lookup an ibv_context via the address and create a new peer using it
	 */
	ret = common_client_peer_via_address(addr, &peer);
	if (ret)
		goto err_free;

	/* establish a new connection to a server listening at addr:port */
	ret = common_client_connect(peer, addr, port, NULL, NULL, &conn);
	if (ret)
		goto err_peer_delete;

	/* register the memory for RDMA read and write operations */
	ret = rpma_mr_reg(peer, local_mr_ptr, local_mr_size,
			(RPMA_MR_USAGE_READ_DST | RPMA_MR_USAGE_WRITE_SRC),
			&local_mr);
	if (ret)
		goto err_conn_disconnect;

	/* obtain the remote side resources description */
	struct rpma_conn_private_data pdata;
	ret = rpma_conn_get_private_data(conn, &pdata);
	if (ret != 0 || pdata.len < sizeof(struct common_data))
		goto err_mr_dereg;

	/*
	 * Create a remote peer configuration structure from the received
	 * descriptor and apply it to the current connection.
	 */
	struct common_data *remote_data = pdata.ptr;
	ret = rpma_peer_cfg_from_descriptor(
			&remote_data->descriptors[remote_data->mr_desc_size],
			remote_data->pcfg_desc_size, &pcfg);
	if (ret)
		goto err_mr_dereg;
	ret = rpma_peer_cfg_get_direct_write_to_pmem(pcfg,
			&direct_write_to_pmem);
	ret |= rpma_conn_apply_remote_peer_cfg(conn, pcfg);
	(void) rpma_peer_cfg_delete(&pcfg);
	/* either get or apply failed */
	if (ret)
		goto err_mr_dereg;

	/*
	 * Create a remote memory registration structure from the received
	 * descriptor.
	 */
	ret = rpma_mr_remote_from_descriptor(&remote_data->descriptors[0],
			remote_data->mr_desc_size, &remote_mr);
	if (ret)
		goto err_mr_dereg;

	/* get the remote memory region size */
	ret = rpma_mr_remote_get_size(remote_mr, &remote_size);
	if (ret) {
		goto err_mr_remote_delete;
	} else if (remote_size < KILOBYTE) {
		fprintf(stderr,
			"Remote memory region size too small for writing the data of the assumed size (%zu < %d)\n",
			remote_size, KILOBYTE);
		goto err_mr_remote_delete;
	}

	/* read the initial value */
	size_t len = (local_mr_size < remote_size) ? local_mr_size : remote_size;
	ret = rpma_read(conn, local_mr, local_offset,
			remote_mr, remote_offset, len,
			RPMA_F_COMPLETION_ALWAYS, NULL);
	if (ret)
		goto err_mr_remote_delete;

	/* wait for the completion to be ready */
	ret = rpma_conn_completion_wait(conn);
	if (ret)
		goto err_mr_remote_delete;

	/* wait for a completion of the RDMA read */
	ret = rpma_conn_completion_get(conn, &cmpl);
	if (ret)
		goto err_mr_remote_delete;

	if (cmpl.op_status != IBV_WC_SUCCESS) {
		ret = -1;
		(void) fprintf(stderr, "rpma_read() failed: %s\n",
				ibv_wc_status_str(cmpl.op_status));
		goto err_mr_remote_delete;
	}

	if (cmpl.op != RPMA_OP_READ) {
		ret = -1;
		(void) fprintf(stderr, "unexpected cmpl.op value (%d != %d)\n",
				cmpl.op, RPMA_OP_READ);
		goto err_mr_remote_delete;
	}

	(void) fprintf(stdout, "The initial content of the server memory (just read): %s\n",
			(char *)local_mr_ptr + local_offset);

	/* write the next value */
	strncpy(local_mr_ptr, hello_str, KILOBYTE);
	(void) printf("Writing the message: %s\n", (char *)local_mr_ptr);

	ret = rpma_write(conn, remote_mr, remote_offset,
			local_mr, local_offset, KILOBYTE,
			RPMA_F_COMPLETION_ON_ERROR, NULL);
	if (ret)
		goto err_mr_remote_delete;

	/* determine the flush type */
	if (direct_write_to_pmem) {
		printf("RPMA_FLUSH_TYPE_PERSISTENT is supported\n");
		flush_type = RPMA_FLUSH_TYPE_PERSISTENT;
	} else {
		printf(
			"RPMA_FLUSH_TYPE_PERSISTENT is NOT supported, RPMA_FLUSH_TYPE_VISIBILITY is used instead\n");
		flush_type = RPMA_FLUSH_TYPE_VISIBILITY;
	}

	ret = rpma_flush(conn, remote_mr, remote_offset, KILOBYTE, flush_type,
			RPMA_F_COMPLETION_ALWAYS, FLUSH_ID);
	if (ret)
		goto err_mr_remote_delete;

	/* wait for the completion to be ready */
	ret = rpma_conn_completion_wait(conn);
	if (ret)
		goto err_mr_remote_delete;

	ret = rpma_conn_completion_get(conn, &cmpl);
	if (ret)
		goto err_mr_remote_delete;

	if (cmpl.op_context != FLUSH_ID) {
		ret = -1;
		(void) fprintf(stderr,
				"unexpected cmpl.op_context value "
				"(0x%" PRIXPTR " != 0x%" PRIXPTR ")\n",
				(uintptr_t)cmpl.op_context,
				(uintptr_t)FLUSH_ID);
		goto err_mr_remote_delete;
	}
	if (cmpl.op_status != IBV_WC_SUCCESS) {
		ret = -1;
		(void) fprintf(stderr, "rpma_flush() failed: %s\n",
				ibv_wc_status_str(cmpl.op_status));
		goto err_mr_remote_delete;
	}

err_mr_remote_delete:
	/* delete the remote memory region's structure */
	(void) rpma_mr_remote_delete(&remote_mr);

err_mr_dereg:
	/* deregister the memory region */
	(void) rpma_mr_dereg(&local_mr);

err_conn_disconnect:
	/*
	 * Disconnect, wait for RPMA_CONN_CLOSED
	 * and delete the connection structure.
	 */
	(void) common_disconnect_and_wait_for_conn_close(&conn);

err_peer_delete:
	/* delete the peer */
	(void) rpma_peer_delete(&peer);

err_free:
	free(local_mr_ptr);

	return ret;
}
