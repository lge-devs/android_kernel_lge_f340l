/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/diagchar.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/spinlock.h>
#include <asm/current.h>
#ifdef CONFIG_DIAG_OVER_USB
#include <mach/usbdiag.h>
#endif
#include "diagchar_hdlc.h"
#include "diagmem.h"
#include "diagchar.h"
#include "diagfwd.h"
#include "diagfwd_cntl.h"
#include "diag_dci.h"

unsigned int dci_max_reg = 100;
unsigned int dci_max_clients = 10;
unsigned char dci_cumulative_log_mask[DCI_LOG_MASK_SIZE];
unsigned char dci_cumulative_event_mask[DCI_EVENT_MASK_SIZE];
struct mutex dci_log_mask_mutex;
struct mutex dci_event_mask_mutex;
struct mutex dci_health_mutex;

spinlock_t ws_lock;
unsigned long ws_lock_flags;

/* Number of milliseconds anticipated to process the DCI data */
#define DCI_WAKEUP_TIMEOUT 1

#define DCI_CHK_CAPACITY(entry, new_data_len)				\
((entry->data_len + new_data_len > entry->total_capacity) ? 1 : 0)	\

#ifdef CONFIG_DEBUG_FS
struct diag_dci_data_info *dci_data_smd;
struct mutex dci_stat_mutex;

void diag_dci_smd_record_info(int read_bytes, uint8_t ch_type)
{
	static int curr_dci_data_smd;
	static unsigned long iteration;
	struct diag_dci_data_info *temp_data = dci_data_smd;
	if (!temp_data)
		return;
	mutex_lock(&dci_stat_mutex);
	if (curr_dci_data_smd == DIAG_DCI_DEBUG_CNT)
		curr_dci_data_smd = 0;
	temp_data += curr_dci_data_smd;
	temp_data->iteration = iteration + 1;
	temp_data->data_size = read_bytes;
	temp_data->ch_type = ch_type;
	diag_get_timestamp(temp_data->time_stamp);
	curr_dci_data_smd++;
	iteration++;
	mutex_unlock(&dci_stat_mutex);
}
#else
void diag_dci_smd_record_info(int read_bytes, uint8_t ch_type) { }
#endif

/* Process the data read from the smd dci channel */
int diag_process_smd_dci_read_data(struct diag_smd_info *smd_info, void *buf,
								int recd_bytes)
{
	int read_bytes, dci_pkt_len, i;
	uint8_t recv_pkt_cmd_code;

	diag_dci_smd_record_info(recd_bytes, (uint8_t)smd_info->type);
	/* Each SMD read can have multiple DCI packets */
	read_bytes = 0;
	while (read_bytes < recd_bytes) {
		/* read actual length of dci pkt */
		dci_pkt_len = *(uint16_t *)(buf+2);
		/* process one dci packet */
		pr_debug("diag: bytes read = %d, single dci pkt len = %d\n",
			read_bytes, dci_pkt_len);
		/* print_hex_dump(KERN_DEBUG, "Single DCI packet :",
		 DUMP_PREFIX_ADDRESS, 16, 1, buf, 5 + dci_pkt_len, 1); */
		recv_pkt_cmd_code = *(uint8_t *)(buf+4);
		if (recv_pkt_cmd_code == LOG_CMD_CODE)
			extract_dci_log(buf+4);
		else if (recv_pkt_cmd_code == EVENT_CMD_CODE)
			extract_dci_events(buf+4);
		else
			extract_dci_pkt_rsp(smd_info, buf); /* pkt response */
		read_bytes += 5 + dci_pkt_len;
		buf += 5 + dci_pkt_len; /* advance to next DCI pkt */
	}
	/* Release wakeup source when there are no more clients to
	   process DCI data */
	if (driver->num_dci_client == 0)
		diag_dci_try_deactivate_wakeup_source(smd_info->ch);

	/* wake up all sleeping DCI clients which have some data */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client &&
			driver->dci_client_tbl[i].data_len) {
			smd_info->in_busy_1 = 1;
			diag_update_sleeping_process(
				driver->dci_client_tbl[i].client->tgid,
					 DCI_DATA_TYPE);
		}
	}

	return 0;
}

static struct dci_pkt_req_entry_t *diag_register_dci_transaction(int uid)
{
	struct dci_pkt_req_entry_t *entry = NULL;
	entry = kzalloc(sizeof(struct dci_pkt_req_entry_t), GFP_KERNEL);
	if (!entry)
		return NULL;

	mutex_lock(&driver->dci_mutex);
	driver->dci_tag++;
	entry->pid = current->tgid;
	entry->uid = uid;
	entry->tag = driver->dci_tag;
	list_add_tail(&entry->track, &driver->dci_req_list);
	mutex_unlock(&driver->dci_mutex);

	return entry;
}

static struct dci_pkt_req_entry_t *diag_dci_get_request_entry(int tag)
{
	struct list_head *start, *temp;
	struct dci_pkt_req_entry_t *entry = NULL;
	list_for_each_safe(start, temp, &driver->dci_req_list) {
		entry = list_entry(start, struct dci_pkt_req_entry_t, track);
		if (entry->tag == tag)
			return entry;
	}
	return NULL;
}

static int diag_dci_remove_req_entry(unsigned char *buf, int len,
				     struct dci_pkt_req_entry_t *entry)
{
	uint16_t rsp_count = 0, delayed_rsp_id = 0;
	if (!buf || len <= 0 || !entry) {
		pr_err("diag: In %s, invalid input buf: %p, len: %d, entry: %p\n",
			__func__, buf, len, entry);
		return -EIO;
	}

	/* It is an immediate response, delete it from the table */
	if (*buf != 0x80) {
		list_del(&entry->track);
		kfree(entry);
		return 1;
	}

	/* It is a delayed response. Check if the length is valid */
	if (len < MIN_DELAYED_RSP_LEN) {
		pr_err("diag: Invalid delayed rsp packet length %d\n", len);
		return -EINVAL;
	}

	/*
	 * If the delayed response id field (uint16_t at byte 8) is 0 then
	 * there is only one response and we can remove the request entry.
	 */
	delayed_rsp_id = *(uint16_t *)(buf + 8);
	if (delayed_rsp_id == 0) {
		list_del(&entry->track);
		kfree(entry);
		return 1;
	}

	/*
	 * Check the response count field (uint16 at byte 10). The request
	 * entry can be deleted it it is the last response in the sequence.
	 * It is the last response in the sequence if the response count
	 * is 1 or if the signed bit gets dropped.
	 */
	rsp_count = *(uint16_t *)(buf + 10);
	if (rsp_count > 0 && rsp_count < 0x1000) {
		list_del(&entry->track);
		kfree(entry);
		return 1;
	}

	return 0;
}

void extract_dci_pkt_rsp(unsigned char *buf, int len, int data_source,
			 struct diag_smd_info *smd_info)
{
	int tag, curr_client_pid = 0;
	struct diag_dci_client_tbl *entry = NULL;
	void *temp_buf = NULL;
	uint8_t dci_cmd_code, cmd_code_len, delete_flag = 0;
	uint32_t rsp_len = 0;
	struct diag_dci_buffer_t *rsp_buf = NULL;
	struct dci_pkt_req_entry_t *req_entry = NULL;
	unsigned char *temp = buf;
	int save_req_uid = 0;
	struct diag_dci_pkt_rsp_header_t pkt_rsp_header;

	if (!buf) {
		pr_err("diag: Invalid pointer in %s\n", __func__);
		return;
	}
	dci_cmd_code = *(uint8_t *)(temp);
	if (dci_cmd_code == DCI_PKT_RSP_CODE) {
		cmd_code_len = sizeof(uint8_t);
	} else if (dci_cmd_code == DCI_DELAYED_RSP_CODE) {
		cmd_code_len = sizeof(uint32_t);
	} else {
		pr_err("diag: In %s, invalid command code %d\n", __func__,
								dci_cmd_code);
		return;
	}
	temp += cmd_code_len;
	tag = *(int *)temp;
	temp += sizeof(int);

	/*
	 * The size of the response is (total length) - (length of the command
	 * code, the tag (int)
	 */
	rsp_len = len - (cmd_code_len + sizeof(int));
	/*
	 * Check if the length embedded in the packet is correct.
	 * Include the start (1), version (1), length (2) and the end
	 * (1) bytes while checking. Total = 5 bytes
	 */
	if ((rsp_len == 0) || (rsp_len > (len - 5))) {
		pr_err("diag: Invalid length in %s, len: %d, rsp_len: %d",
						__func__, len, rsp_len);
		return;
	}

	req_entry = diag_dci_get_request_entry(tag);
	if (!req_entry) {
		pr_err("diag: No matching PID for DCI data\n");
		return;
	}
	curr_client_pid = req_entry->pid;
	save_req_uid = req_entry->uid;

	/* Remove the headers and send only the response to this function */
	mutex_lock(&driver->dci_mutex);
	delete_flag = diag_dci_remove_req_entry(temp, rsp_len, req_entry);
	if (delete_flag < 0) {
		mutex_unlock(&driver->dci_mutex);
		return;
	}
	mutex_unlock(&driver->dci_mutex);

	entry = __diag_dci_get_client_entry(curr_client_pid);
	if (!entry) {
		pr_err("diag: In %s, couldn't find entry\n", __func__);
		return;
	}

	rsp_buf = entry->buffers[data_source].buf_cmd;

	mutex_lock(&rsp_buf->data_mutex);
	/*
	 * Check if we can fit the data in the rsp buffer. The total length of
	 * the rsp is the rsp length (write_len) + DCI_PKT_RSP_TYPE header (int)
	 * + field for length (int) + delete_flag (uint8_t)
	 */
	if ((rsp_buf->data_len + 9 + rsp_len) > rsp_buf->capacity) {
		pr_alert("diag: create capacity for pkt rsp\n");
		rsp_buf->capacity += 9 + rsp_len;
		temp_buf = krealloc(rsp_buf->data, rsp_buf->capacity,
				    GFP_KERNEL);
		if (!temp_buf) {
			pr_err("diag: DCI realloc failed\n");
			mutex_unlock(&rsp_buf->data_mutex);
			return;
		} else {
			rsp_buf->data = temp_buf;
		}
	}

	/* Fill in packet response header information */
	pkt_rsp_header.type = DCI_PKT_RSP_TYPE;
	/* Packet Length = Response Length + Length of uid field (int) */
	pkt_rsp_header.length = rsp_len + sizeof(int);
	pkt_rsp_header.delete_flag = delete_flag;
	pkt_rsp_header.uid = save_req_uid;
	memcpy(rsp_buf->data, &pkt_rsp_header,
		sizeof(struct diag_dci_pkt_rsp_header_t));
	rsp_buf->data_len += sizeof(struct diag_dci_pkt_rsp_header_t);
	memcpy(rsp_buf->data + rsp_buf->data_len, temp, rsp_len);
	rsp_buf->data_len += rsp_len;
	rsp_buf->data_source = data_source;
	if (smd_info)
		smd_info->in_busy_1 = 1;
	mutex_unlock(&rsp_buf->data_mutex);


	/*
	 * Add directly to the list for writing responses to the
	 * userspace as these shouldn't be buffered and shouldn't wait
	 * for log and event buffers to be full
	 */
	dci_add_buffer_to_list(entry, rsp_buf);
}

static void copy_dci_event(unsigned char *buf, int len,
			   struct diag_dci_client_tbl *client, int data_source)
{
	struct diag_dci_buffer_t *data_buffer = NULL;
	struct diag_dci_buf_peripheral_t *proc_buf = NULL;
	int err = 0, total_len = 0;

	if (!buf || !client) {
		pr_err("diag: Invalid pointers in %s", __func__);
		return;
	}

	total_len = sizeof(int) + len;

	proc_buf = &client->buffers[data_source];
	mutex_lock(&proc_buf->buf_mutex);
	mutex_lock(&proc_buf->health_mutex);
	err = diag_dci_get_buffer(client, data_source, total_len);
	if (err) {
		if (err == -ENOMEM)
			proc_buf->health.dropped_events++;
		else
			pr_err("diag: In %s, invalid packet\n", __func__);
		mutex_unlock(&proc_buf->health_mutex);
		mutex_unlock(&proc_buf->buf_mutex);
		return;
	}

	data_buffer = proc_buf->buf_curr;

	proc_buf->health.received_events++;
	mutex_unlock(&proc_buf->health_mutex);
	mutex_unlock(&proc_buf->buf_mutex);

	mutex_lock(&data_buffer->data_mutex);
	*(int *)(data_buffer->data + data_buffer->data_len) = DCI_EVENT_TYPE;
	data_buffer->data_len += sizeof(int);
	memcpy(data_buffer->data + data_buffer->data_len, buf, len);
	data_buffer->data_len += len;
	data_buffer->data_source = data_source;
	mutex_unlock(&data_buffer->data_mutex);

}

void extract_dci_events(unsigned char *buf)
{
	uint16_t event_id, event_id_packet, length, temp_len;
	uint8_t payload_len, payload_len_field;
	uint8_t timestamp[8], timestamp_len;
	unsigned char event_data[MAX_EVENT_SIZE];
	unsigned int total_event_len;
	struct list_head *start, *temp;
	struct diag_dci_client_tbl *entry = NULL;

	if (!buf) {
		pr_err("diag: In %s buffer is NULL\n", __func__);
		return;
	}
	/*
	 * 1 byte for event code and 2 bytes for the length field.
	 */
	if (len < 3) {
		pr_err("diag: In %s invalid len: %d\n", __func__, len);
		return;
	}
	length = *(uint16_t *)(buf + 1); /* total length of event series */
	if ((length == 0) || (len != (length + 3))) {
		pr_err("diag: Incoming dci event length: %d is invalid\n",
			length);
		return;
	}
	/*
	 * Move directly to the start of the event series.
	 * The event parsing should happen from start of event
	 * series till the end.
	 */
	temp_len = 3;
	while (temp_len < (length - 1)) {
		event_id_packet = *(uint16_t *)(buf + temp_len);
		event_id = event_id_packet & 0x0FFF; /* extract 12 bits */
		if (event_id_packet & 0x8000) {
			/* The packet has the two smallest byte of the
			 * timestamp
			 */
			timestamp_len = 2;
		} else {
			/* The packet has the full timestamp. The first event
			 * will always have full timestamp. Save it in the
			 * timestamp buffer and use it for subsequent events if
			 * necessary.
			 */
			timestamp_len = 8;
			if ((temp_len + timestamp_len + 2) <= len)
				memcpy(timestamp, buf + temp_len + 2,
					timestamp_len);
			else {
				pr_err("diag: Invalid length in %s, len: %d, temp_len: %d",
						__func__, len, temp_len);
				return;
			}
		}
		/* 13th and 14th bit represent the payload length */
		if (((event_id_packet & 0x6000) >> 13) == 3) {
			payload_len_field = 1;
			if ((temp_len + timestamp_len + 3) <= len) {
				payload_len = *(uint8_t *)
					(buf + temp_len + 2 + timestamp_len);
			} else {
				pr_err("diag: Invalid length in %s, len: %d, temp_len: %d",
						__func__, len, temp_len);
				return;
			}
			if ((payload_len < (MAX_EVENT_SIZE - 13)) &&
			((temp_len + timestamp_len + payload_len + 3) <= len)) {
				/*
				 * Copy the payload length and the payload
				 * after skipping temp_len bytes for already
				 * parsed packet, timestamp_len for timestamp
				 * buffer, 2 bytes for event_id_packet.
				 */
				memcpy(event_data + 12, buf + temp_len + 2 +
							timestamp_len, 1);
				memcpy(event_data + 13, buf + temp_len + 2 +
					timestamp_len + 1, payload_len);
			} else {
				pr_err("diag: event > %d, payload_len = %d, temp_len = %d\n",
				(MAX_EVENT_SIZE - 13), payload_len, temp_len);
				return;
			}
		} else {
			payload_len_field = 0;
			payload_len = (event_id_packet & 0x6000) >> 13;
			/*
			 * Copy the payload after skipping temp_len bytes
			 * for already parsed packet, timestamp_len for
			 * timestamp buffer, 2 bytes for event_id_packet.
			 */
			if ((payload_len < (MAX_EVENT_SIZE - 12)) &&
			((temp_len + timestamp_len + payload_len + 2) <= len))
				memcpy(event_data + 12, buf + temp_len + 2 +
						timestamp_len, payload_len);
			else {
				pr_err("diag: event > %d, payload_len = %d, temp_len = %d\n",
				(MAX_EVENT_SIZE - 12), payload_len, temp_len);
				return;
			}
		}

		/* Before copying the data to userspace, check if we are still
		 * within the buffer limit. This is an error case, don't count
		 * it towards the health statistics.
		 *
		 * Here, the offset of 2 bytes(uint16_t) is for the
		 * event_id_packet length
		 */
		temp_len += sizeof(uint16_t) + timestamp_len +
						payload_len_field + payload_len;
		if (temp_len > len) {
			pr_err("diag: Invalid length in %s, len: %d, read: %d",
						__func__, len, temp_len);
			return;
		}

		/* 2 bytes for the event id & timestamp len is hard coded to 8,
		   as individual events have full timestamp */
		*(uint16_t *)(event_data) = 10 +
					payload_len_field + payload_len;
		*(uint16_t *)(event_data + 2) = event_id_packet & 0x7FFF;
		memcpy(event_data + 4, timestamp, 8);
		/* 2 bytes for the event length field which is added to
		   the event data */
		total_event_len = 2 + 10 + payload_len_field + payload_len;
		byte_index = event_id / 8;
		bit_index = event_id % 8;
		byte_mask = 0x1 << bit_index;
		/* parse through event mask tbl of each client and check mask */
		for (i = 0; i < MAX_DCI_CLIENTS; i++) {
			if (driver->dci_client_tbl[i].client) {
				entry = &(driver->dci_client_tbl[i]);
				event_mask_ptr = entry->dci_event_mask +
								 byte_index;
				mutex_lock(&dci_health_mutex);
				mutex_lock(&entry->data_mutex);
				if (*event_mask_ptr & byte_mask) {
					/* copy to client buffer */
					if (DCI_CHK_CAPACITY(entry,
							 4 + total_event_len)) {
						pr_err("diag: DCI event drop\n");
						driver->dci_client_tbl[i].
							dropped_events++;
						mutex_unlock(
							&entry->data_mutex);
						mutex_unlock(
							&dci_health_mutex);
						break;
					}
					driver->dci_client_tbl[i].
							received_events++;
					*(int *)(entry->dci_data+
					entry->data_len) = DCI_EVENT_TYPE;
					/* 4 bytes for DCI_EVENT_TYPE */
					memcpy(entry->dci_data +
						entry->data_len + 4, event_data
						, total_event_len);
					entry->data_len += 4 + total_event_len;
				}
				mutex_unlock(&entry->data_mutex);
				mutex_unlock(&dci_health_mutex);
			}
		}
		temp_len += 2 + timestamp_len + payload_len_field + payload_len;
	}
}

void extract_dci_log(unsigned char *buf, int len, int data_source)
{
	uint16_t log_code, read_bytes = 0;
	struct list_head *start, *temp;
	struct diag_dci_client_tbl *entry = NULL;

	if (!buf) {
		pr_err("diag: In %s buffer is NULL\n", __func__);
		return;
	}
	/*
	 * The first eight bytes for the incoming log packet contains
	 * Command code (2), the length of the packet (2), the length
	 * of the log (2) and log code (2)
	 */
	if (len < 8) {
		pr_err("diag: In %s invalid len: %d\n", __func__, len);
		return;
	}

	log_code = *(uint16_t *)(buf + 6);
	read_bytes += sizeof(uint16_t) + 6;

	/* parse through log mask table of each client and check mask */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client) {
			entry = &(driver->dci_client_tbl[i]);
			log_mask_ptr = entry->dci_log_mask;
			if (!log_mask_ptr)
				return;
			log_mask_ptr = log_mask_ptr + byte_offset;
			mutex_lock(&dci_health_mutex);
			mutex_lock(&entry->data_mutex);
			if (*log_mask_ptr & byte_mask) {
				pr_debug("\t log code %x needed by client %d",
					 log_code, entry->client->tgid);
				/* copy to client buffer */
				if (DCI_CHK_CAPACITY(entry,
						 4 + *(uint16_t *)(buf + 2))) {
						pr_err("diag: DCI log drop\n");
						driver->dci_client_tbl[i].
								dropped_logs++;
						mutex_unlock(
							&entry->data_mutex);
						mutex_unlock(
							&dci_health_mutex);
						return;
				}
				driver->dci_client_tbl[i].received_logs++;
				*(int *)(entry->dci_data+entry->data_len) =
								DCI_LOG_TYPE;
				memcpy(entry->dci_data + entry->data_len + 4,
					    buf + 4, *(uint16_t *)(buf + 2));
				entry->data_len += 4 + *(uint16_t *)(buf + 2);
			}
			mutex_unlock(&entry->data_mutex);
			mutex_unlock(&dci_health_mutex);
		}
	}
}

void diag_update_smd_dci_work_fn(struct work_struct *work)
{
	struct diag_smd_info *smd_info = container_of(work,
						struct diag_smd_info,
						diag_notify_update_smd_work);
	int i, j;
	char dirty_bits[16];
	uint8_t *client_log_mask_ptr;
	uint8_t *log_mask_ptr;
	int ret;
	int index = smd_info->peripheral;

	/* Update the peripheral(s) with the dci log and event masks */

	/* If the cntl channel is not up, we can't update logs and events */
	if (!driver->smd_cntl[index].ch)
		return;

	memset(dirty_bits, 0, 16 * sizeof(uint8_t));

	/*
	 * From each log entry used by each client, determine
	 * which log entries in the cumulative logs that need
	 * to be updated on the peripheral.
	 */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client) {
			client_log_mask_ptr =
				driver->dci_client_tbl[i].dci_log_mask;
			for (j = 0; j < 16; j++) {
				if (*(client_log_mask_ptr+1))
					dirty_bits[j] = 1;
				client_log_mask_ptr += 514;
			}
		}
	}

	mutex_lock(&dci_log_mask_mutex);
	/* Update the appropriate dirty bits in the cumulative mask */
	log_mask_ptr = dci_cumulative_log_mask;
	for (i = 0; i < 16; i++) {
		if (dirty_bits[i])
			*(log_mask_ptr+1) = dirty_bits[i];

		log_mask_ptr += 514;
	}
	mutex_unlock(&dci_log_mask_mutex);

	ret = diag_send_dci_log_mask(driver->smd_cntl[index].ch);

	ret = diag_send_dci_event_mask(driver->smd_cntl[index].ch);

	smd_info->notify_context = 0;
}

void diag_dci_notify_client(int peripheral_mask, int data)
{
	int i, stat;
	struct siginfo info;
	memset(&info, 0, sizeof(struct siginfo));
	info.si_code = SI_QUEUE;
	info.si_int = (peripheral_mask | data);

	/* Notify the DCI process that the peripheral DCI Channel is up */
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (!driver->dci_client_tbl[i].client)
			continue;
		if (driver->dci_client_tbl[i].list & peripheral_mask) {
			info.si_signo = driver->dci_client_tbl[i].signal_type;
			stat = send_sig_info(
				driver->dci_client_tbl[i].signal_type,
				&info, driver->dci_client_tbl[i].client);
			if (stat)
				pr_err("diag: Err sending dci signal to client, signal data: 0x%x, stat: %d\n",
				info.si_int, stat);
		}
	} /* end of loop for all DCI clients */
}

static int diag_send_dci_pkt(struct diag_master_table entry, unsigned char *buf,
					 int len, int tag)
{
	int i, status = 0;
	unsigned int read_len = 0;

	/* The first 4 bytes is the uid tag and the next four bytes is
	   the minmum packet length of a request packet */
	if (len < DCI_PKT_REQ_MIN_LEN) {
		pr_err("diag: dci: Invalid pkt len %d in %s\n", len, __func__);
		return -EIO;
	}
	if (len > APPS_BUF_SIZE - 10) {
		pr_err("diag: dci: Invalid payload length in %s\n", __func__);
		return -EIO;
	}
	/* remove UID from user space pkt before sending to peripheral*/
	buf = buf + sizeof(int);
	read_len += sizeof(int);
	len = len - sizeof(int);
	mutex_lock(&driver->dci_mutex);
	/* prepare DCI packet */
	driver->apps_dci_buf[0] = CONTROL_CHAR; /* start */
	driver->apps_dci_buf[1] = 1; /* version */
	*(uint16_t *)(driver->apps_dci_buf + 2) = len + 4 + 1; /* length */
	driver->apps_dci_buf[4] = DCI_PKT_RSP_CODE;
	*(int *)(driver->apps_dci_buf + 5) = tag;
	for (i = 0; i < len; i++)
		driver->apps_dci_buf[i+9] = *(buf+i);
	read_len += len;
	driver->apps_dci_buf[9+len] = CONTROL_CHAR; /* end */
	if ((read_len + 9) >= USER_SPACE_DATA) {
		pr_err("diag: dci: Invalid length while forming dci pkt in %s",
								__func__);
		mutex_unlock(&driver->dci_mutex);
		return -EIO;
	}

	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++) {
		struct diag_smd_info *smd_info = driver->separate_cmdrsp[i] ?
					&driver->smd_dci_cmd[i] :
					&driver->smd_dci[i];
		if (entry.client_id == smd_info->peripheral) {
			if (smd_info->ch) {
				smd_write(smd_info->ch,
					driver->apps_dci_buf, len + 10);
				status = DIAG_DCI_NO_ERROR;
			}
			break;
		}
	}

	if (status != DIAG_DCI_NO_ERROR) {
		pr_alert("diag: check DCI channel\n");
		status = DIAG_DCI_SEND_DATA_FAIL;
	}
	mutex_unlock(&driver->dci_mutex);
	return status;
}

int diag_process_dci_transaction(unsigned char *buf, int len)
{
	unsigned char *temp = buf;
	uint16_t subsys_cmd_code, log_code, item_num;
	int subsys_id, cmd_code, ret = -1, found = 0;
	struct diag_master_table entry;
	int count, set_mask, num_codes, bit_index, event_id, offset = 0, i;
	unsigned int byte_index, read_len = 0;
	uint8_t equip_id, *log_mask_ptr, *head_log_mask_ptr, byte_mask;
	uint8_t *event_mask_ptr;
	struct dci_pkt_req_entry_t *req_entry = NULL;

	if (!driver->smd_dci[MODEM_DATA].ch) {
		pr_err("diag: DCI smd channel for peripheral %d not valid for dci updates\n",
			driver->smd_dci[MODEM_DATA].peripheral);
		return DIAG_DCI_SEND_DATA_FAIL;
	}

	/*
	 * Previous packet is yet to be consumed by the client. Wait
	 * till the buffer is free.
	 */
	while (retry_count < max_retries) {
		retry_count++;
		if (driver->in_busy_dcipktdata)
			usleep_range(10000, 10100);
		else
			break;
	}
	/* The buffer is still busy */
	if (driver->in_busy_dcipktdata) {
		pr_err("diag: In %s, apps dci buffer is still busy. Dropping packet\n",
								__func__);
		return -EAGAIN;
	}

	/* Register this new DCI packet */
	req_entry = diag_register_dci_transaction(req_uid);
	if (!req_entry) {
		pr_alert("diag: registering new DCI transaction failed\n");
		return DIAG_DCI_NO_REG;
	}

	/* Check if it is a dedicated Apps command */
	ret = diag_dci_process_apps_pkt(header, req_buf, req_entry->tag);
	if (ret == DIAG_DCI_NO_ERROR || ret < 0)
		return ret;

	/* Check the registration table for command entries */
	for (i = 0; i < diag_max_reg && !found; i++) {
		entry = driver->table[i];
		if (entry.process_id == NO_PROCESS)
			continue;
		if (entry.cmd_code == header->cmd_code &&
			    entry.subsys_id == header->subsys_id &&
			    entry.cmd_code_lo <= header->subsys_cmd_code &&
			    entry.cmd_code_hi >= header->subsys_cmd_code) {
			ret = diag_send_dci_pkt(entry, buf, len,
						req_entry->tag);
			found = 1;
		} else if (entry.cmd_code == 255 && header->cmd_code == 75) {
			if (entry.subsys_id == header->subsys_id &&
			    entry.cmd_code_lo <= header->subsys_cmd_code &&
			    entry.cmd_code_hi >= header->subsys_cmd_code) {
				ret = diag_send_dci_pkt(entry, buf, len,
							req_entry->tag);
				found = 1;
			}
		} else if (entry.cmd_code == 255 && entry.subsys_id == 255) {
			if (entry.cmd_code_lo <= header->cmd_code &&
			    entry.cmd_code_hi >= header->cmd_code) {
				/*
				 * If its a Mode reset command, make sure it is
				 * registered on the Apps Processor
				 */

/* [VZW][OBDM] Temporary fix "mode change cmd(0x29) issue." */
//#ifdef CONFIG_MACH_MSM8974_G3_VZW
#if 1
#define MODE_RESET 2

				if (entry.cmd_code_lo == MODE_CMD &&
					entry.cmd_code_hi == MODE_CMD){
					if (header->subsys_id == MODE_RESET){
						if (entry.client_id != APPS_DATA)
							continue;
					}
					else {
						if (entry.client_id != MODEM_DATA)
							continue;
					}
				}
#else
				/* QCT Original  */
				if (entry.cmd_code_lo == MODE_CMD &&
				    entry.cmd_code_hi == MODE_CMD &&
					header->subsys_id == RESET_ID) {
					if (entry.client_id != APPS_DATA)
						continue;
				}
#endif  /* End VZW Feature  */

					ret = diag_send_dci_pkt(entry, buf, len,
								req_entry->tag);
					found = 1;
			}
		}
	}

	return ret;
}

int diag_process_dci_transaction(unsigned char *buf, int len)
{
	unsigned char *temp = buf;
	uint16_t log_code, item_num;
	int ret = -1, found = 0;
	int count, set_mask, num_codes, bit_index, event_id, offset = 0;
	unsigned int byte_index, read_len = 0;
	uint8_t equip_id, *log_mask_ptr, *head_log_mask_ptr, byte_mask;
	uint8_t *event_mask_ptr;
	struct diag_dci_client_tbl *dci_entry = NULL;

	if (!temp) {
		pr_err("diag: Invalid buffer in %s\n", __func__);
		return -ENOMEM;
	}

	/* This is Pkt request/response transaction */
	if (*(int *)temp > 0) {
		return diag_process_dci_pkt_rsp(buf, len);
	} else if (*(int *)temp == DCI_LOG_TYPE) {
		/* Minimum length of a log mask config is 12 + 2 bytes for
		   atleast one log code to be set or reset */
		if (len < DCI_LOG_CON_MIN_LEN || len > USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length in %s\n", __func__);
			return -EIO;
		}
		/* find client id and table */
		i = diag_dci_find_client_index(current->tgid);
		if (i == DCI_CLIENT_INDEX_INVALID) {
			pr_err("diag: dci client not registered/found\n");
			return ret;
		}
		/* Extract each log code and put in client table */
		temp += sizeof(int);
		read_len += sizeof(int);
		set_mask = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);
		num_codes = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);

		if (num_codes == 0 || (num_codes >= (USER_SPACE_DATA - 8)/2)) {
			pr_err("diag: dci: Invalid number of log codes %d\n",
								num_codes);
			return -EIO;
		}

		head_log_mask_ptr = driver->dci_client_tbl[i].dci_log_mask;
		if (!head_log_mask_ptr) {
			pr_err("diag: dci: Invalid Log mask pointer in %s\n",
								__func__);
			return -ENOMEM;
		}
		pr_debug("diag: head of dci log mask %p\n", head_log_mask_ptr);
		count = 0; /* iterator for extracting log codes */
		while (count < num_codes) {
			if (read_len >= USER_SPACE_DATA) {
				pr_err("diag: dci: Invalid length for log type in %s",
								__func__);
				return -EIO;
			}
			log_code = *(uint16_t *)temp;
			equip_id = LOG_GET_EQUIP_ID(log_code);
			item_num = LOG_GET_ITEM_NUM(log_code);
			byte_index = item_num/8 + 2;
			if (byte_index >= (DCI_MAX_ITEMS_PER_LOG_CODE+2)) {
				pr_err("diag: dci: Log type, invalid byte index\n");
				return ret;
			}
			byte_mask = 0x01 << (item_num % 8);
			/*
			 * Parse through log mask table and find
			 * relevant range
			 */
			log_mask_ptr = head_log_mask_ptr;
			found = 0;
			offset = 0;
			while (log_mask_ptr && (offset < DCI_LOG_MASK_SIZE)) {
				if (*log_mask_ptr == equip_id) {
					found = 1;
					pr_debug("diag: find equip id = %x at %p\n",
						 equip_id, log_mask_ptr);
					break;
				} else {
					pr_debug("diag: did not find equip id = %x at %p\n",
						 equip_id, log_mask_ptr);
					log_mask_ptr += 514;
					offset += 514;
				}
			}
			if (!found) {
				pr_err("diag: dci equip id not found\n");
				return ret;
			}
			*(log_mask_ptr+1) = 1; /* set the dirty byte */
			log_mask_ptr = log_mask_ptr + byte_index;
			if (set_mask)
				*log_mask_ptr |= byte_mask;
			else
				*log_mask_ptr &= ~byte_mask;
			/* add to cumulative mask */
			update_dci_cumulative_log_mask(
				offset, byte_index,
				byte_mask);
			temp += 2;
			read_len += 2;
			count++;
			ret = DIAG_DCI_NO_ERROR;
		}
		/* send updated mask to peripherals */
		ret = diag_send_dci_log_mask(driver->smd_cntl[MODEM_DATA].ch);
	} else if (*(int *)temp == DCI_EVENT_TYPE) {
		/* Minimum length of a event mask config is 12 + 4 bytes for
		  atleast one event id to be set or reset. */
		if (len < DCI_EVENT_CON_MIN_LEN || len > USER_SPACE_DATA) {
			pr_err("diag: dci: Invalid length in %s\n", __func__);
			return -EIO;
		}
		/* find client id and table */
		i = diag_dci_find_client_index(current->tgid);
		if (i == DCI_CLIENT_INDEX_INVALID) {
			pr_err("diag: dci client not registered/found\n");
			return ret;
		}
		/* Extract each log code and put in client table */
		temp += sizeof(int);
		read_len += sizeof(int);
		set_mask = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);
		num_codes = *(int *)temp;
		temp += sizeof(int);
		read_len += sizeof(int);

		/* Check for positive number of event ids. Also, the number of
		   event ids should fit in the buffer along with set_mask and
		   num_codes which are 4 bytes each */
		if (num_codes == 0 || (num_codes >= (USER_SPACE_DATA - 8)/2)) {
			pr_err("diag: dci: Invalid number of event ids %d\n",
								num_codes);
			return -EIO;
		}

		event_mask_ptr = driver->dci_client_tbl[i].dci_event_mask;
		if (!event_mask_ptr) {
			pr_err("diag: dci: Invalid event mask pointer in %s\n",
								__func__);
			return -ENOMEM;
		}
		pr_debug("diag: head of dci event mask %p\n", event_mask_ptr);
		count = 0; /* iterator for extracting log codes */
		while (count < num_codes) {
			if (read_len >= USER_SPACE_DATA) {
				pr_err("diag: dci: Invalid length for event type in %s",
								__func__);
				return -EIO;
			}
			event_id = *(int *)temp;
			byte_index = event_id/8;
			if (byte_index >= DCI_EVENT_MASK_SIZE) {
				pr_err("diag: dci: Event type, invalid byte index\n");
				return ret;
			}
			bit_index = event_id % 8;
			byte_mask = 0x1 << bit_index;
			/*
			 * Parse through event mask table and set
			 * relevant byte & bit combination
			 */
			if (set_mask)
				*(event_mask_ptr + byte_index) |= byte_mask;
			else
				*(event_mask_ptr + byte_index) &= ~byte_mask;
			/* add to cumulative mask */
			update_dci_cumulative_event_mask(byte_index, byte_mask);
			temp += sizeof(int);
			read_len += sizeof(int);
			count++;
			ret = DIAG_DCI_NO_ERROR;
		}
		/* send updated mask to peripherals */
		ret = diag_send_dci_event_mask(driver->smd_cntl[MODEM_DATA].ch);
	} else {
		pr_alert("diag: Incorrect DCI transaction\n");
	}
	return ret;
}

int diag_dci_find_client_index(int client_id)
{
	int i, ret = DCI_CLIENT_INDEX_INVALID;

	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		if (driver->dci_client_tbl[i].client != NULL) {
			if (driver->dci_client_tbl[i].client->tgid ==
					client_id) {
				ret = i;
				break;
			}
		}
	}
	return ret;
}

void update_dci_cumulative_event_mask(int offset, uint8_t byte_mask)
{
	int i;
	uint8_t *event_mask_ptr;
	uint8_t *update_ptr = dci_cumulative_event_mask;
	bool is_set = false;

	mutex_lock(&dci_event_mask_mutex);
	update_ptr += offset;
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		event_mask_ptr =
			driver->dci_client_tbl[i].dci_event_mask;
		event_mask_ptr += offset;
		if ((*event_mask_ptr & byte_mask) == byte_mask) {
			is_set = true;
			/* break even if one client has the event mask set */
			break;
		}
	}
	if (is_set == false)
		*update_ptr &= ~byte_mask;
	else
		*update_ptr |= byte_mask;
	mutex_unlock(&dci_event_mask_mutex);
}

void clear_client_dci_cumulative_event_mask(int client_index)
{
	int i, j;
	uint8_t *update_ptr = dci_cumulative_event_mask;
	uint8_t *event_mask_ptr, *client_event_mask_ptr, byte_mask = 0;
	bool is_set = false;

	event_mask_ptr =
		(driver->dci_client_tbl[client_index].dci_event_mask);

	mutex_lock(&dci_event_mask_mutex);
	for (i = 0; i < DCI_EVENT_MASK_SIZE; i++) {
		is_set = false;
		/* Already cleared event masks need not to be considered */
		if (*event_mask_ptr != 0) {
			byte_mask = *event_mask_ptr;
		} else {
			update_ptr++;
			event_mask_ptr++;
			continue;
		}
		for (j = 0; j < MAX_DCI_CLIENTS; j++) {
			/* continue searching for valid client */
			if (driver->dci_client_tbl[j].client == NULL ||
				client_index == j)
				continue;
			client_event_mask_ptr =
				(driver->dci_client_tbl[j].dci_event_mask);
			client_event_mask_ptr += i;
			if (*client_event_mask_ptr & byte_mask) {
				/*
				* Break if another client has same
				* event mask set
				*/
				if ((*client_event_mask_ptr &
					byte_mask) == byte_mask) {
					is_set = true;
					break;
				} else {
					byte_mask =
					(~(*client_event_mask_ptr) &
					byte_mask);
					is_set = false;
				}
			}
		}
		/*
		* Clear only if this client has event mask set else
		* don't update cumulative event mask ptr
		*/
		if (is_set == false)
			*update_ptr &= ~byte_mask;

		update_ptr++;
		event_mask_ptr++;
	}
	event_mask_ptr =
		(driver->dci_client_tbl[client_index].dci_event_mask);
	memset(event_mask_ptr, 0, DCI_EVENT_MASK_SIZE);
	mutex_unlock(&dci_event_mask_mutex);
}


int diag_send_dci_event_mask(smd_channel_t *ch)
{
	void *buf = driver->buf_event_mask_update;
	int header_size = sizeof(struct diag_ctrl_event_mask);
	int wr_size = -ENOMEM, retry_count = 0, timer;
	int ret = DIAG_DCI_NO_ERROR, i;

	mutex_lock(&driver->diag_cntl_mutex);
	/* send event mask update */
	driver->event_mask->cmd_type = DIAG_CTRL_MSG_EVENT_MASK;
	driver->event_mask->data_len = 7 + DCI_EVENT_MASK_SIZE;
	driver->event_mask->stream_id = DCI_MASK_STREAM;
	driver->event_mask->status = 3; /* status for valid mask */
	driver->event_mask->event_config = 0; /* event config */
	driver->event_mask->event_mask_size = DCI_EVENT_MASK_SIZE;
	for (i = 0; i < DCI_EVENT_MASK_SIZE; i++) {
		if (dci_cumulative_event_mask[i] != 0) {
			driver->event_mask->event_config = 1;
			break;
		}
	}
	memcpy(buf, driver->event_mask, header_size);
	memcpy(buf+header_size, dci_cumulative_event_mask, DCI_EVENT_MASK_SIZE);
	if (ch) {
		while (retry_count < 3) {
			wr_size = smd_write(ch, buf,
					 header_size + DCI_EVENT_MASK_SIZE);
			if (wr_size == -ENOMEM) {
				retry_count++;
				for (timer = 0; timer < 5; timer++)
					udelay(2000);
			} else {
				break;
			}
		}
		if (wr_size != header_size + DCI_EVENT_MASK_SIZE) {
			pr_err("diag: error writing dci event mask %d, tried %d\n",
				 wr_size, header_size + DCI_EVENT_MASK_SIZE);
			ret = DIAG_DCI_SEND_DATA_FAIL;
		}
	} else {
		pr_err("diag: ch not valid for dci event mask update\n");
		ret = DIAG_DCI_SEND_DATA_FAIL;
	}
	mutex_unlock(&driver->diag_cntl_mutex);

	return ret;
}

void update_dci_cumulative_log_mask(int offset, unsigned int byte_index,
						uint8_t byte_mask)
{
	int i;
	uint8_t *update_ptr = dci_cumulative_log_mask;
	uint8_t *log_mask_ptr;
	bool is_set = false;

	mutex_lock(&dci_log_mask_mutex);
	*update_ptr = 0;
	/* set the equipment IDs */
	for (i = 0; i < 16; i++)
		*(update_ptr + (i*514)) = i;

	update_ptr += offset;
	/* update the dirty bit */
	*(update_ptr+1) = 1;
	update_ptr = update_ptr + byte_index;
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		log_mask_ptr =
			(driver->dci_client_tbl[i].dci_log_mask);
		log_mask_ptr = log_mask_ptr + offset + byte_index;
		if ((*log_mask_ptr & byte_mask) == byte_mask) {
			is_set = true;
			/* break even if one client has the log mask set */
			break;
		}
	}

	if (is_set == false)
		*update_ptr &= ~byte_mask;
	else
		*update_ptr |= byte_mask;
	mutex_unlock(&dci_log_mask_mutex);
}

void clear_client_dci_cumulative_log_mask(int client_index)
{
	int i, j, k;
	uint8_t *update_ptr = dci_cumulative_log_mask;
	uint8_t *log_mask_ptr, *client_log_mask_ptr, byte_mask = 0;
	bool is_set = false;

	log_mask_ptr = driver->dci_client_tbl[client_index].dci_log_mask;

	mutex_lock(&dci_log_mask_mutex);
	*update_ptr = 0;
	/* set the equipment IDs */
	for (i = 0; i < 16; i++)
		*(update_ptr + (i*514)) = i;

	/* update cumulative log mask ptr*/
	update_ptr += 2;
	log_mask_ptr += 2;
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 512; j++) {
			is_set = false;
			/*
			* Already cleared log masks need
			* not to be considered
			*/
			if (*log_mask_ptr != 0) {
				byte_mask = *log_mask_ptr;
			} else {
				update_ptr++;
				log_mask_ptr++;
				continue;
			}
			for (k = 0; k < MAX_DCI_CLIENTS; k++) {
				/* continue searching for valid client */
				if (driver->dci_client_tbl[k].client == NULL ||
					client_index == k)
					continue;
				client_log_mask_ptr =
				 (driver->dci_client_tbl[k].dci_log_mask);
				client_log_mask_ptr += (i*514) + 2 + j;
				if (*client_log_mask_ptr & byte_mask) {
					/*
					* Break if another client has same
					* log mask set
					*/
					if ((*client_log_mask_ptr &
						byte_mask) == byte_mask) {
						is_set = true;
						break;
					} else {
						byte_mask =
						 (~(*client_log_mask_ptr) &
						 byte_mask);
						is_set = false;
					}
				}
			}
			/*
			* Clear only if this client has log mask set else
			* don't update cumulative log mask ptr
			*/
			if (is_set == false) {
				/*
				* Update the dirty bit for the equipment
				* whose mask is changing
				*/
				dci_cumulative_log_mask[1+(i*514)] = 1;
				*update_ptr &= ~byte_mask;
			}

			update_ptr++;
			log_mask_ptr++;
		}
		update_ptr += 2;
		log_mask_ptr += 2;
	}
	log_mask_ptr = driver->dci_client_tbl[client_index].dci_log_mask;
	memset(log_mask_ptr, 0, DCI_LOG_MASK_SIZE);
	mutex_unlock(&dci_log_mask_mutex);
}

int diag_send_dci_log_mask(smd_channel_t *ch)
{
	void *buf = driver->buf_log_mask_update;
	int header_size = sizeof(struct diag_ctrl_log_mask);
	uint8_t *log_mask_ptr = dci_cumulative_log_mask;
	int i, wr_size = -ENOMEM, retry_count = 0, timer;
	int ret = DIAG_DCI_NO_ERROR;

	if (!ch) {
		pr_err("diag: ch not valid for dci log mask update\n");
		return DIAG_DCI_SEND_DATA_FAIL;
	}

	mutex_lock(&driver->diag_cntl_mutex);
	for (i = 0; i < 16; i++) {
		retry_count = 0;
		driver->log_mask->cmd_type = DIAG_CTRL_MSG_LOG_MASK;
		driver->log_mask->num_items = 512;
		driver->log_mask->data_len  = 11 + 512;
		driver->log_mask->stream_id = DCI_MASK_STREAM;
		driver->log_mask->status = 3; /* status for valid mask */
		driver->log_mask->equip_id = *log_mask_ptr;
		driver->log_mask->log_mask_size = 512;
		memcpy(buf, driver->log_mask, header_size);
		memcpy(buf+header_size, log_mask_ptr+2, 512);
		/* if dirty byte is set and channel is valid */
		if (ch && *(log_mask_ptr+1)) {
			while (retry_count < 3) {
				wr_size = smd_write(ch, buf, header_size + 512);
				if (wr_size == -ENOMEM) {
					retry_count++;
					for (timer = 0; timer < 5; timer++)
						udelay(2000);
				} else
					break;
			}
			if (wr_size != header_size + 512) {
				pr_err("diag: dci log mask update failed %d, tried %d for equip_id %d\n",
					wr_size, header_size + 512,
					driver->log_mask->equip_id);
				ret = DIAG_DCI_SEND_DATA_FAIL;

			} else {
				*(log_mask_ptr+1) = 0; /* clear dirty byte */
				pr_debug("diag: updated dci log equip ID %d\n",
						 *log_mask_ptr);
			}
		}
		log_mask_ptr += 514;
	}
	mutex_unlock(&driver->diag_cntl_mutex);

	return ret;
}

void create_dci_log_mask_tbl(unsigned char *tbl_buf)
{
	uint8_t i; int count = 0;

	/* create hard coded table for log mask with 16 categories */
	for (i = 0; i < 16; i++) {
		*(uint8_t *)tbl_buf = i;
		pr_debug("diag: put value %x at %p\n", i, tbl_buf);
		memset(tbl_buf+1, 0, 513); /* set dirty bit as 0 */
		tbl_buf += 514;
		count += 514;
	}
}

void create_dci_event_mask_tbl(unsigned char *tbl_buf)
{
	memset(tbl_buf, 0, 512);
}

static int diag_dci_probe(struct platform_device *pdev)
{
	int err = 0;
	int index;

	if (pdev->id == SMD_APPS_MODEM) {
		index = MODEM_DATA;
		err = smd_open("DIAG_2",
			&driver->smd_dci[index].ch,
			&driver->smd_dci[index],
			diag_smd_notify);
		driver->smd_dci[index].ch_save =
			driver->smd_dci[index].ch;
		driver->dci_device = &pdev->dev;
		driver->dci_device->power.wakeup = wakeup_source_register
							("DIAG_DCI_WS");
		if (err)
			pr_err("diag: In %s, cannot open DCI port, Id = %d, err: %d\n",
				__func__, pdev->id, err);
	}

	return err;
}

static int diag_dci_cmd_probe(struct platform_device *pdev)
{
	int err = 0;
	int index;

	if (pdev->id == SMD_APPS_MODEM) {
		index = MODEM_DATA;
		err = smd_named_open_on_edge("DIAG_2_CMD",
			pdev->id,
			&driver->smd_dci_cmd[index].ch,
			&driver->smd_dci_cmd[index],
			diag_smd_notify);
		driver->smd_dci_cmd[index].ch_save =
			driver->smd_dci_cmd[index].ch;
		driver->dci_cmd_device = &pdev->dev;
		driver->dci_cmd_device->power.wakeup = wakeup_source_register
							("DIAG_DCI_CMD_WS");
		if (err)
			pr_err("diag: In %s, cannot open DCI port, Id = %d, err: %d\n",
				__func__, pdev->id, err);
	}

	return err;
}

static int diag_dci_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: suspending...\n");
	return 0;
}

static int diag_dci_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: resuming...\n");
	return 0;
}

static const struct dev_pm_ops diag_dci_dev_pm_ops = {
	.runtime_suspend = diag_dci_runtime_suspend,
	.runtime_resume = diag_dci_runtime_resume,
};

struct platform_driver msm_diag_dci_driver = {
	.probe = diag_dci_probe,
	.driver = {
		.name = "DIAG_2",
		.owner = THIS_MODULE,
		.pm   = &diag_dci_dev_pm_ops,
	},
};

struct platform_driver msm_diag_dci_cmd_driver = {
	.probe = diag_dci_cmd_probe,
	.driver = {
		.name = "DIAG_2_CMD",
		.owner = THIS_MODULE,
		.pm   = &diag_dci_dev_pm_ops,
	},
};

int diag_dci_init(void)
{
	int success = 0;
	int i;

	driver->dci_tag = 0;
	driver->dci_client_id = 0;
	driver->num_dci_client = 0;
	driver->dci_device = NULL;
	driver->dci_cmd_device = NULL;
	mutex_init(&driver->dci_mutex);
	mutex_init(&dci_log_mask_mutex);
	mutex_init(&dci_event_mask_mutex);
	mutex_init(&dci_health_mutex);
	spin_lock_init(&ws_lock);

	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++) {
		success = diag_smd_constructor(&driver->smd_dci[i], i,
							SMD_DCI_TYPE);
		if (!success)
			goto err;
	}

	if (driver->supports_separate_cmdrsp) {
		for (i = 0; i < NUM_SMD_DCI_CMD_CHANNELS; i++) {
			success = diag_smd_constructor(&driver->smd_dci_cmd[i],
							i, SMD_DCI_CMD_TYPE);
			if (!success)
				goto err;
		}
	}
	if (driver->apps_dci_buf == NULL) {
		driver->apps_dci_buf = kzalloc(APPS_BUF_SIZE, GFP_KERNEL);
		if (driver->apps_dci_buf == NULL)
			goto err;
	}
	if (driver->dci_client_tbl == NULL) {
		driver->dci_client_tbl = kzalloc(MAX_DCI_CLIENTS *
			sizeof(struct diag_dci_client_tbl), GFP_KERNEL);
		if (driver->dci_client_tbl == NULL)
			goto err;
	}
	driver->diag_dci_wq = create_singlethread_workqueue("diag_dci_wq");
	INIT_LIST_HEAD(&driver->dci_req_list);
	success = platform_driver_register(&msm_diag_dci_driver);
	if (success) {
		pr_err("diag: Could not register DCI driver\n");
		goto err;
	}
	if (driver->supports_separate_cmdrsp) {
		success = platform_driver_register(&msm_diag_dci_cmd_driver);
		if (success) {
			pr_err("diag: Could not register DCI cmd driver\n");
			goto err;
		}
	}
	return DIAG_DCI_NO_ERROR;
err:
	pr_err("diag: Could not initialize diag DCI buffers");
	kfree(driver->dci_client_tbl);
	kfree(driver->apps_dci_buf);
	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++)
		diag_smd_destructor(&driver->smd_dci[i]);

	if (driver->supports_separate_cmdrsp)
		for (i = 0; i < NUM_SMD_DCI_CMD_CHANNELS; i++)
			diag_smd_destructor(&driver->smd_dci_cmd[i]);

	if (driver->diag_dci_wq)
		destroy_workqueue(driver->diag_dci_wq);
	mutex_destroy(&driver->dci_mutex);
	mutex_destroy(&dci_log_mask_mutex);
	mutex_destroy(&dci_event_mask_mutex);
	mutex_destroy(&dci_health_mutex);
	return DIAG_DCI_NO_REG;
}

void diag_dci_exit(void)
{
	int i;

	for (i = 0; i < NUM_SMD_DCI_CHANNELS; i++)
		diag_smd_destructor(&driver->smd_dci[i]);

	platform_driver_unregister(&msm_diag_dci_driver);

	if (driver->dci_client_tbl) {
		for (i = 0; i < MAX_DCI_CLIENTS; i++) {
			kfree(driver->dci_client_tbl[i].dci_data);
			mutex_destroy(&driver->dci_client_tbl[i].data_mutex);
		}
	}

	if (driver->supports_separate_cmdrsp) {
		for (i = 0; i < NUM_SMD_DCI_CMD_CHANNELS; i++)
			diag_smd_destructor(&driver->smd_dci_cmd[i]);

		platform_driver_unregister(&msm_diag_dci_cmd_driver);
	}

	kfree(driver->dci_client_tbl);
	kfree(driver->apps_dci_buf);
	mutex_destroy(&driver->dci_mutex);
	mutex_destroy(&dci_log_mask_mutex);
	mutex_destroy(&dci_event_mask_mutex);
	mutex_destroy(&dci_health_mutex);
	destroy_workqueue(driver->diag_dci_wq);
}

int diag_dci_clear_log_mask()
{
	int i, j, k, err = DIAG_DCI_NO_ERROR;
	uint8_t *log_mask_ptr, *update_ptr;

	i = diag_dci_find_client_index(current->tgid);
	if (i == DCI_CLIENT_INDEX_INVALID)
		return DIAG_DCI_TABLE_ERR;

	mutex_lock(&dci_log_mask_mutex);
	create_dci_log_mask_tbl(
			driver->dci_client_tbl[i].dci_log_mask);
	memset(dci_cumulative_log_mask,
				0x0, DCI_LOG_MASK_SIZE);
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		update_ptr = dci_cumulative_log_mask;
		if (driver->dci_client_tbl[i].client) {
			log_mask_ptr =
				driver->dci_client_tbl[i].dci_log_mask;
			for (j = 0; j < 16; j++) {
				*update_ptr = j;
				*(update_ptr + 1) = 1;
				update_ptr += 2;
				log_mask_ptr += 2;
				for (k = 0; k < 513; k++) {
					*update_ptr |= *log_mask_ptr;
					update_ptr++;
					log_mask_ptr++;
				}
			}
		}
	}
	mutex_unlock(&dci_log_mask_mutex);
	err = diag_send_dci_log_mask(driver->smd_cntl[MODEM_DATA].ch);
	return err;
}

int diag_dci_clear_event_mask()
{
	int i, j, err = DIAG_DCI_NO_ERROR;
	uint8_t *event_mask_ptr, *update_ptr;

	i = diag_dci_find_client_index(current->tgid);
	if (i == DCI_CLIENT_INDEX_INVALID)
		return DIAG_DCI_TABLE_ERR;

	mutex_lock(&dci_event_mask_mutex);
	memset(driver->dci_client_tbl[i].dci_event_mask,
			0x0, DCI_EVENT_MASK_SIZE);
	memset(dci_cumulative_event_mask,
			0x0, DCI_EVENT_MASK_SIZE);
	update_ptr = dci_cumulative_event_mask;
	for (i = 0; i < MAX_DCI_CLIENTS; i++) {
		event_mask_ptr =
			driver->dci_client_tbl[i].dci_event_mask;
		for (j = 0; j < DCI_EVENT_MASK_SIZE; j++)
			*(update_ptr + j) |= *(event_mask_ptr + j);
	}
	mutex_unlock(&dci_event_mask_mutex);
	err = diag_send_dci_event_mask(driver->smd_cntl[MODEM_DATA].ch);
	return err;
}

int diag_dci_query_log_mask(uint16_t log_code)
{
	uint16_t item_num;
	uint8_t equip_id, *log_mask_ptr, byte_mask;
	int i, byte_index, offset;

	equip_id = LOG_GET_EQUIP_ID(log_code);
	item_num = LOG_GET_ITEM_NUM(log_code);
	byte_index = item_num/8 + 2;
	byte_mask = 0x01 << (item_num % 8);
	offset = equip_id * 514;

	i = diag_dci_find_client_index(current->tgid);
	if (i != DCI_CLIENT_INDEX_INVALID) {
		log_mask_ptr = driver->dci_client_tbl[i].dci_log_mask;
		log_mask_ptr = log_mask_ptr + offset + byte_index;
		return ((*log_mask_ptr & byte_mask) == byte_mask) ?
								1 : 0;
	}
	return 0;
}


int diag_dci_query_event_mask(uint16_t event_id)
{
	uint8_t *event_mask_ptr, byte_mask;
	int i, byte_index, bit_index;
	byte_index = event_id/8;
	bit_index = event_id % 8;
	byte_mask = 0x1 << bit_index;

	i = diag_dci_find_client_index(current->tgid);
	if (i != DCI_CLIENT_INDEX_INVALID) {
		event_mask_ptr =
		driver->dci_client_tbl[i].dci_event_mask;
		event_mask_ptr = event_mask_ptr + byte_index;
		if ((*event_mask_ptr & byte_mask) == byte_mask)
			return 1;
		else
			return 0;
	}
	return 0;
}

uint8_t diag_dci_get_cumulative_real_time()
{
	uint8_t real_time = MODE_NONREALTIME, i;
	for (i = 0; i < MAX_DCI_CLIENTS; i++)
		if (driver->dci_client_tbl[i].client &&
				driver->dci_client_tbl[i].real_time ==
				MODE_REALTIME) {
			real_time = 1;
			break;
		}
	return real_time;
}

int diag_dci_set_real_time(int client_id, uint8_t real_time)
{
	int i = DCI_CLIENT_INDEX_INVALID;
	i = diag_dci_find_client_index(client_id);

	if (i != DCI_CLIENT_INDEX_INVALID)
		driver->dci_client_tbl[i].real_time = real_time;
	return i;
}

void diag_dci_try_activate_wakeup_source(smd_channel_t *channel)
{
	spin_lock_irqsave(&ws_lock, ws_lock_flags);
	if (channel == driver->smd_dci[MODEM_DATA].ch) {
		pm_wakeup_event(driver->dci_device, DCI_WAKEUP_TIMEOUT);
		pm_stay_awake(driver->dci_device);
	} else if (channel == driver->smd_dci_cmd[MODEM_DATA].ch) {
		pm_wakeup_event(driver->dci_cmd_device, DCI_WAKEUP_TIMEOUT);
		pm_stay_awake(driver->dci_cmd_device);
	}
	spin_unlock_irqrestore(&ws_lock, ws_lock_flags);
}

void diag_dci_try_deactivate_wakeup_source(smd_channel_t *channel)
{
	spin_lock_irqsave(&ws_lock, ws_lock_flags);
	if (channel == driver->smd_dci[MODEM_DATA].ch)
		pm_relax(driver->dci_device);
	else if (channel == driver->smd_dci_cmd[MODEM_DATA].ch)
		pm_relax(driver->dci_cmd_device);
	spin_unlock_irqrestore(&ws_lock, ws_lock_flags);
}
