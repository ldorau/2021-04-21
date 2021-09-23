// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020-2021, Intel Corporation */
/* Copyright 2021, Fujitsu */

/*
 * client.c -- a client of the flush-to-persistent example
 *
 * Please see README.md for a detailed description of this example.
 */

#include <librpma.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "connection.h"

#define USAGE_STR "usage: %s <server_address> <port>\n"

#define HELLO_STR_SIZE 1024

enum lang_t {en, es};

struct hello_t {
	enum lang_t lang;
	char str[HELLO_STR_SIZE];
};

static const char *hello_str[] = {
	[en] = "Hello world!",
	[es] = "¡Hola Mundo!"
};

#define HELLO_STR_OFFSET	offsetof(struct hello_t, str)
#define HELLO_T_SIZE		(sizeof(struct hello_t))
#define LANG_NUM		(sizeof(hello_str) / sizeof(hello_str[0]))

#define FLUSH_ID		(void *)0xF01D /* a random identifier */

static inline void
write_hello_str(struct hello_t *hello, enum lang_t lang)
{
	hello->lang = lang;
	strncpy(hello->str, hello_str[hello->lang], KILOBYTE - 1);
	hello->str[KILOBYTE - 1] = '\0';
}

static void
translate(struct hello_t *hello)
{
	printf("translating...\n");
	enum lang_t lang = (enum lang_t)((hello->lang + 1) % LANG_NUM);
	write_hello_str(hello, lang);
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
	void *mr_ptr;
	size_t mr_size = 0;
	size_t data_offset = 0;
	struct rpma_mr_remote *dst_mr = NULL;
	size_t dst_size = 0;
	size_t dst_offset = 0;
	struct rpma_mr_local *src_mr = NULL;
	struct rpma_completion cmpl;

	struct hello_t *hello = NULL;

	mr_ptr = malloc_aligned(sizeof(struct hello_t));
	if (mr_ptr == NULL)
		return -1;

	mr_size = sizeof(struct hello_t);
	hello = mr_ptr;

	/* write an initial value */
	write_hello_str(hello, en);

	(void) printf("Next value: %s\n", hello->str);

	/* RPMA resources */
	struct rpma_peer_cfg *pcfg = NULL;
	struct rpma_peer *peer = NULL;
	struct rpma_conn *conn = NULL;
	bool direct_write_to_pmem = false;
	enum rpma_flush_type flush_type;

	/*
	 * lookup an ibv_context via the address and create a new peer using it
	 */
	ret = client_peer_via_address(addr, &peer);
	if (ret)
		goto err_free;

	/* establish a new connection to a server listening at addr:port */
	ret = client_connect(peer, addr, port, NULL, NULL, &conn);
	if (ret)
		goto err_peer_delete;

	/* register the memory RDMA write */
	ret = rpma_mr_reg(peer, mr_ptr, mr_size, RPMA_MR_USAGE_WRITE_SRC,
				&src_mr);
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
	struct common_data *dst_data = pdata.ptr;
	ret = rpma_peer_cfg_from_descriptor(
			&dst_data->descriptors[dst_data->mr_desc_size],
			dst_data->pcfg_desc_size, &pcfg);
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
	ret = rpma_mr_remote_from_descriptor(&dst_data->descriptors[0],
			dst_data->mr_desc_size, &dst_mr);
	if (ret)
		goto err_mr_dereg;

	/* get the remote memory region size */
	ret = rpma_mr_remote_get_size(dst_mr, &dst_size);
	if (ret) {
		goto err_mr_remote_delete;
	} else if (dst_size < KILOBYTE) {
		fprintf(stderr,
				"Remote memory region size too small for writing the data of the assumed size (%zu < %d)\n",
				dst_size, KILOBYTE);
		goto err_mr_remote_delete;
	}

	dst_offset = dst_data->data_offset;
	ret = rpma_write(conn, dst_mr, dst_offset, src_mr,
			(data_offset + offsetof(struct hello_t, str)), KILOBYTE,
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

	ret = rpma_flush(conn, dst_mr, dst_offset, KILOBYTE, flush_type,
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

	/*
	 * Translate the message so the next time the greeting will be
	 * surprising.
	 */
	translate(hello);

	(void) printf("Translation: %s\n", hello->str);

err_mr_remote_delete:
	/* delete the remote memory region's structure */
	(void) rpma_mr_remote_delete(&dst_mr);

err_mr_dereg:
	/* deregister the memory region */
	(void) rpma_mr_dereg(&src_mr);

err_conn_disconnect:
	(void) common_disconnect_and_wait_for_conn_close(&conn);

err_peer_delete:
	/* delete the peer */
	(void) rpma_peer_delete(&peer);

err_free:
	free(mr_ptr);

	return ret;
}
