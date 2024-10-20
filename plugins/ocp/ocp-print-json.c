// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/types.h"
#include "common.h"
#include "nvme-print.h"
#include "ocp-print.h"
#include "ocp-hardware-component-log.h"
#include "ocp-fw-activation-history.h"
#include "ocp-smart-extended-log.h"
#include "ocp-telemetry-decode.h"
#include "ocp-nvme.h"

static void print_hwcomp_desc_json(struct hwcomp_desc_entry *e, struct json_object *r)
{
	obj_add_str(r, "Description", hwcomp_id_to_string(le32_to_cpu(e->desc->id)));
	obj_add_nprix64(r, "Date/Lot Size", e->date_lot_size);
	obj_add_nprix64(r, "Additional Information Size", e->add_info_size);
	obj_add_uint_0nx(r, "Identifier", le32_to_cpu(e->desc->id), 8);
	obj_add_0nprix64(r, "Manufacture", le64_to_cpu(e->desc->mfg), 16);
	obj_add_0nprix64(r, "Revision", le64_to_cpu(e->desc->rev), 16);
	obj_add_0nprix64(r, "Manufacture Code", le64_to_cpu(e->desc->mfg_code), 16);
	obj_add_byte_array(r, "Date/Lot Code", e->date_lot_code, e->date_lot_size);
	obj_add_byte_array(r, "Additional Information", e->add_info, e->add_info_size);
}

static void print_hwcomp_desc_list_json(struct json_object *r, struct hwcomp_desc_entry *e,
					bool list, int num)
{
	_cleanup_free_ char *k = NULL;

	if (asprintf(&k, "Component %d", num) < 0)
		return;

	if (list) {
		obj_add_str(r, k, hwcomp_id_to_string(le32_to_cpu(e->desc->id)));
		return;
	}

	print_hwcomp_desc_json(e, obj_create_array_obj(r, k));
}

static void print_hwcomp_descs_json(struct hwcomp_desc *desc, long double log_size, __u32 id,
				    bool list, struct json_object *r)
{
	size_t date_lot_code_offset = sizeof(struct hwcomp_desc);
	struct hwcomp_desc_entry e = { desc };
	int num = 1;

	while (log_size > 0) {
		e.date_lot_size = le64_to_cpu(e.desc->date_lot_size) * sizeof(__le32);
		e.date_lot_code = e.date_lot_size ?
		    (__u8 *)e.desc + date_lot_code_offset : NULL;
		e.add_info_size = le64_to_cpu(e.desc->add_info_size) * sizeof(__le32);
		e.add_info = e.add_info_size ? e.date_lot_code ?
		    e.date_lot_code + e.date_lot_size :
		    (__u8 *)e.desc + date_lot_code_offset : NULL;
		if (!id || id == le32_to_cpu(e.desc->id))
			print_hwcomp_desc_list_json(r, &e, list, num++);
		e.desc_size = date_lot_code_offset + e.date_lot_size + e.add_info_size;
		e.desc = (struct hwcomp_desc *)((__u8 *)e.desc + e.desc_size);
		log_size -= e.desc_size;
	}
}

static void json_hwcomp_log(struct hwcomp_log *log, __u32 id, bool list)
{
	struct json_object *r = json_create_object();

	long double log_size = uint128_t_to_double(le128_to_cpu(log->size)) * sizeof(__le32);

	obj_add_uint_02x(r, "Log Identifier", LID_HWCOMP);
	obj_add_uint_0x(r, "Log Page Version", le16_to_cpu(log->ver));
	obj_add_byte_array(r, "Reserved2", log->rsvd2, ARRAY_SIZE(log->rsvd2));
	obj_add_byte_array(r, "Log page GUID", log->guid, ARRAY_SIZE(log->guid));
	obj_add_nprix64(r, "Hardware Component Log Size", (unsigned long long)log_size);
	obj_add_byte_array(r, "Reserved48", log->rsvd48, ARRAY_SIZE(log->rsvd48));
	print_hwcomp_descs_json(log->desc, log_size, id, list,
				obj_create_array_obj(r, "Component Descriptions"));

	json_print(r);
}

static void json_fw_activation_history(const struct fw_activation_history *fw_history)
{
	struct json_object *root = json_create_object();

	json_object_add_value_uint(root, "log identifier", fw_history->log_id);
	json_object_add_value_uint(root, "valid entries", le32_to_cpu(fw_history->valid_entries));

	struct json_object *entries = json_create_array();

	for (int index = 0; index < fw_history->valid_entries; index++) {
		const struct fw_activation_history_entry *entry = &fw_history->entries[index];
		struct json_object *entry_obj = json_create_object();

		json_object_add_value_uint(entry_obj, "version number", entry->ver_num);
		json_object_add_value_uint(entry_obj, "entry length", entry->entry_length);
		json_object_add_value_uint(entry_obj, "activation count",
					   le16_to_cpu(entry->activation_count));
		json_object_add_value_uint64(entry_obj, "timestamp",
				(0x0000FFFFFFFFFFFF & le64_to_cpu(entry->timestamp)));
		json_object_add_value_uint(entry_obj, "power cycle count",
					   le64_to_cpu(entry->power_cycle_count));

		struct json_object *fw = json_object_new_string_len(entry->previous_fw,
								    sizeof(entry->previous_fw));

		json_object_add_value_object(entry_obj, "previous firmware", fw);

		fw = json_object_new_string_len(entry->new_fw, sizeof(entry->new_fw));

		json_object_add_value_object(entry_obj, "new firmware", fw);
		json_object_add_value_uint(entry_obj, "slot number", entry->slot_number);
		json_object_add_value_uint(entry_obj, "commit action type", entry->commit_action);
		json_object_add_value_uint(entry_obj, "result", le16_to_cpu(entry->result));

		json_array_add_value_object(entries, entry_obj);
	}

	json_object_add_value_array(root, "entries", entries);

	json_object_add_value_uint(root, "log page version",
				   le16_to_cpu(fw_history->log_page_version));

	char guid[2 * sizeof(fw_history->log_page_guid) + 3] = { 0 };

	sprintf(guid, "0x%"PRIx64"%"PRIx64"",
		le64_to_cpu(fw_history->log_page_guid[1]),
		le64_to_cpu(fw_history->log_page_guid[0]));
	json_object_add_value_string(root, "log page guid", guid);

	json_print_object(root, NULL);
	json_free_object(root);

	printf("\n");
}

static void json_smart_extended_log(void *data)
{
	struct json_object *root;
	struct json_object *pmuw;
	struct json_object *pmur;
	uint16_t smart_log_ver = 0;
	__u8 *log_data = data;
	char guid[40];

	root = json_create_object();
	pmuw = json_create_object();
	pmur = json_create_object();

	json_object_add_value_uint64(pmuw, "hi",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_PMUW + 8] & 0xFFFFFFFFFFFFFFFF));
	json_object_add_value_uint64(pmuw, "lo",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_PMUW] & 0xFFFFFFFFFFFFFFFF));
	json_object_add_value_object(root, "Physical media units written", pmuw);
	json_object_add_value_uint64(pmur, "hi",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_PMUR + 8] & 0xFFFFFFFFFFFFFFFF));
	json_object_add_value_uint64(pmur, "lo",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_PMUR] & 0xFFFFFFFFFFFFFFFF));
	json_object_add_value_object(root, "Physical media units read", pmur);
	json_object_add_value_uint64(root, "Bad user nand blocks - Raw",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_BUNBR] & 0x0000FFFFFFFFFFFF));
	json_object_add_value_uint(root, "Bad user nand blocks - Normalized",
		(uint16_t)le16_to_cpu(*(uint16_t *)&log_data[SCAO_BUNBN]));
	json_object_add_value_uint64(root, "Bad system nand blocks - Raw",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_BSNBR] & 0x0000FFFFFFFFFFFF));
	json_object_add_value_uint(root, "Bad system nand blocks - Normalized",
		(uint16_t)le16_to_cpu(*(uint16_t *)&log_data[SCAO_BSNBN]));
	json_object_add_value_uint64(root, "XOR recovery count",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_XRC]));
	json_object_add_value_uint64(root, "Uncorrectable read error count",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_UREC]));
	json_object_add_value_uint64(root, "Soft ecc error count",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_SEEC]));
	json_object_add_value_uint(root, "End to end detected errors",
		(uint32_t)le32_to_cpu(*(uint32_t *)&log_data[SCAO_EEDC]));
	json_object_add_value_uint(root, "End to end corrected errors",
		(uint32_t)le32_to_cpu(*(uint32_t *)&log_data[SCAO_EECE]));
	json_object_add_value_uint(root, "System data percent used",
		(__u8)log_data[SCAO_SDPU]);
	json_object_add_value_uint64(root, "Refresh counts",
		(uint64_t)(le64_to_cpu(*(uint64_t *)&log_data[SCAO_RFSC]) & 0x00FFFFFFFFFFFFFF));
	json_object_add_value_uint(root, "Max User data erase counts",
		(uint32_t)le32_to_cpu(*(uint32_t *)&log_data[SCAO_MXUDEC]));
	json_object_add_value_uint(root, "Min User data erase counts",
		(uint32_t)le32_to_cpu(*(uint32_t *)&log_data[SCAO_MNUDEC]));
	json_object_add_value_uint(root, "Number of Thermal throttling events",
		(__u8)log_data[SCAO_NTTE]);
	json_object_add_value_uint(root, "Current throttling status",
		(__u8)log_data[SCAO_CTS]);
	json_object_add_value_uint64(root, "PCIe correctable error count",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_PCEC]));
	json_object_add_value_uint(root, "Incomplete shutdowns",
		(uint32_t)le32_to_cpu(*(uint32_t *)&log_data[SCAO_ICS]));
	json_object_add_value_uint(root, "Percent free blocks",
		(__u8)log_data[SCAO_PFB]);
	json_object_add_value_uint(root, "Capacitor health",
		(uint16_t)le16_to_cpu(*(uint16_t *)&log_data[SCAO_CPH]));
	json_object_add_value_uint64(root, "Unaligned I/O",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_UIO]));
	json_object_add_value_uint64(root, "Security Version Number",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_SVN]));
	json_object_add_value_uint64(root, "NUSE - Namespace utilization",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_NUSE]));
	json_object_add_value_uint128(root, "PLP start count",
		le128_to_cpu(&log_data[SCAO_PSC]));
	json_object_add_value_uint128(root, "Endurance estimate",
		le128_to_cpu(&log_data[SCAO_EEST]));
	smart_log_ver = (uint16_t)le16_to_cpu(*(uint16_t *)&log_data[SCAO_LPV]);

	json_object_add_value_uint(root, "Log page version", smart_log_ver);

	memset((void *)guid, 0, 40);
	sprintf((char *)guid, "0x%"PRIx64"%"PRIx64"",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_LPG + 8]),
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data[SCAO_LPG]));
	json_object_add_value_string(root, "Log page GUID", guid);

	switch (smart_log_ver) {
	case 0 ... 1:
		break;
	default:
	case 4:
		json_object_add_value_uint(root, "NVMe Command Set Errata Version",
					   (__u8)log_data[SCAO_NCSEV]);
		json_object_add_value_uint(root, "Lowest Permitted Firmware Revision",
					   le64_to_cpu(*(uint64_t *)&log_data[SCAO_PSCC]));
		fallthrough;
	case 2 ... 3:
		json_object_add_value_uint(root, "Errata Version Field",
					   (__u8)log_data[SCAO_EVF]);
		json_object_add_value_uint(root, "Point Version Field",
					   le16_to_cpu(*(uint16_t *)&log_data[SCAO_PVF]));
		json_object_add_value_uint(root, "Minor Version Field",
					   le16_to_cpu(*(uint16_t *)&log_data[SCAO_MIVF]));
		json_object_add_value_uint(root, "Major Version Field",
					   (__u8)log_data[SCAO_MAVF]);
		json_object_add_value_uint(root, "NVMe Base Errata Version",
					   (__u8)log_data[SCAO_NBEV]);
		json_object_add_value_uint(root, "PCIe Link Retraining Count",
					   le64_to_cpu(*(uint64_t *)&log_data[SCAO_PLRC]));
		json_object_add_value_uint(root, "Power State Change Count",
					   le64_to_cpu(*(uint64_t *)&log_data[SCAO_PSCC]));
	}
	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static void json_telemetry_log(struct ocp_telemetry_parse_options *options)
{
	print_ocp_telemetry_json(options);
}

static void json_c3_log(struct nvme_dev *dev, struct ssd_latency_monitor_log *log_data)
{
	struct json_object *root;
	char ts_buf[128];
	char buf[128];
	int i, j;
	char *operation[3] = {"Trim", "Write", "Read"};

	root = json_create_object();

	json_object_add_value_uint(root, "Feature Status",
		log_data->feature_status);
	json_object_add_value_uint(root, "Active Bucket Timer",
		C3_ACTIVE_BUCKET_TIMER_INCREMENT *
		le16_to_cpu(log_data->active_bucket_timer));
	json_object_add_value_uint(root, "Active Bucket Timer Threshold",
		C3_ACTIVE_BUCKET_TIMER_INCREMENT *
		le16_to_cpu(log_data->active_bucket_timer_threshold));
	json_object_add_value_uint(root, "Active Threshold A",
		C3_ACTIVE_THRESHOLD_INCREMENT *
		le16_to_cpu(log_data->active_threshold_a + 1));
	json_object_add_value_uint(root, "Active Threshold B",
		C3_ACTIVE_THRESHOLD_INCREMENT *
		le16_to_cpu(log_data->active_threshold_b + 1));
	json_object_add_value_uint(root, "Active Threshold C",
		C3_ACTIVE_THRESHOLD_INCREMENT *
		le16_to_cpu(log_data->active_threshold_c + 1));
	json_object_add_value_uint(root, "Active Threshold D",
		C3_ACTIVE_THRESHOLD_INCREMENT *
		le16_to_cpu(log_data->active_threshold_d + 1));
	json_object_add_value_uint(root, "Active Latency Configuration",
		le16_to_cpu(log_data->active_latency_config));
	json_object_add_value_uint(root, "Active Latency Minimum Window",
		C3_MINIMUM_WINDOW_INCREMENT *
		le16_to_cpu(log_data->active_latency_min_window));

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Active Bucket Counter: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			json_object_add_value_uint(bucket, operation[j],
				le32_to_cpu(log_data->active_bucket_counter[i][j+1]));
		}
		json_object_add_value_object(root, buf, bucket);
	}

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Active Latency Time Stamp: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			if (le64_to_cpu(log_data->active_latency_timestamp[3-i][j]) == -1) {
				json_object_add_value_string(bucket, operation[j], "NA");
			} else {
				convert_ts(le64_to_cpu(log_data->active_latency_timestamp[3-i][j]),
					   ts_buf);
				json_object_add_value_string(bucket, operation[j], ts_buf);
			}
		}
		json_object_add_value_object(root, buf, bucket);
	}

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Active Measured Latency: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			json_object_add_value_uint(bucket, operation[j],
				le16_to_cpu(log_data->active_measured_latency[3-i][j]));
		}
		json_object_add_value_object(root, buf, bucket);
	}

	json_object_add_value_uint(root, "Active Latency Stamp Units",
		le16_to_cpu(log_data->active_latency_stamp_units));

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Static Bucket Counter: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			json_object_add_value_uint(bucket, operation[j],
				le32_to_cpu(log_data->static_bucket_counter[i][j+1]));
		}
		json_object_add_value_object(root, buf, bucket);
	}

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Static Latency Time Stamp: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			if (le64_to_cpu(log_data->static_latency_timestamp[3-i][j]) == -1) {
				json_object_add_value_string(bucket, operation[j], "NA");
			} else {
				convert_ts(le64_to_cpu(log_data->static_latency_timestamp[3-i][j]),
					   ts_buf);
				json_object_add_value_string(bucket, operation[j], ts_buf);
			}
		}
		json_object_add_value_object(root, buf, bucket);
	}

	for (i = 0; i < C3_BUCKET_NUM; i++) {
		struct json_object *bucket;

		bucket = json_create_object();
		sprintf(buf, "Static Measured Latency: Bucket %d", i);
		for (j = 2; j >= 0; j--) {
			json_object_add_value_uint(bucket, operation[j],
				le16_to_cpu(log_data->static_measured_latency[3-i][j]));
		}
		json_object_add_value_object(root, buf, bucket);
	}

	json_object_add_value_uint(root, "Static Latency Stamp Units",
		le16_to_cpu(log_data->static_latency_stamp_units));
	json_object_add_value_uint(root, "Debug Log Trigger Enable",
		le16_to_cpu(log_data->debug_log_trigger_enable));
	json_object_add_value_uint(root, "Debug Log Measured Latency",
		le16_to_cpu(log_data->debug_log_measured_latency));
	if (le64_to_cpu(log_data->debug_log_latency_stamp) == -1) {
		json_object_add_value_string(root, "Debug Log Latency Time Stamp", "NA");
	} else {
		convert_ts(le64_to_cpu(log_data->debug_log_latency_stamp), ts_buf);
		json_object_add_value_string(root, "Debug Log Latency Time Stamp", ts_buf);
	}
	json_object_add_value_uint(root, "Debug Log Pointer",
		le16_to_cpu(log_data->debug_log_ptr));
	json_object_add_value_uint(root, "Debug Counter Trigger Source",
		le16_to_cpu(log_data->debug_log_counter_trigger));
	json_object_add_value_uint(root, "Debug Log Stamp Units",
		le16_to_cpu(log_data->debug_log_stamp_units));
	json_object_add_value_uint(root, "Log Page Version",
		le16_to_cpu(log_data->log_page_version));

	char guid[(C3_GUID_LENGTH * 2) + 1];
	char *ptr = &guid[0];

	for (i = C3_GUID_LENGTH - 1; i >= 0; i--)
		ptr += sprintf(ptr, "%02X", log_data->log_page_guid[i]);

	json_object_add_value_string(root, "Log Page GUID", guid);

	json_print_object(root, NULL);
	printf("\n");

	json_free_object(root);
}

static void json_c5_log(struct nvme_dev *dev, struct unsupported_requirement_log *log_data)
{
	int j;
	struct json_object *root;
	char unsup_req_list_str[40];
	char guid_buf[C5_GUID_LENGTH];
	char *guid = guid_buf;

	root = json_create_object();

	json_object_add_value_int(root, "Number Unsupported Req IDs",
				  le16_to_cpu(log_data->unsupported_count));

	memset((void *)unsup_req_list_str, 0, 40);
	for (j = 0; j < le16_to_cpu(log_data->unsupported_count); j++) {
		sprintf((char *)unsup_req_list_str, "Unsupported Requirement List %d", j);
		json_object_add_value_string(root, unsup_req_list_str,
					     (char *)log_data->unsupported_req_list[j]);
	}

	json_object_add_value_int(root, "Log Page Version",
				  le16_to_cpu(log_data->log_page_version));

	memset((void *)guid, 0, C5_GUID_LENGTH);
	for (j = C5_GUID_LENGTH - 1; j >= 0; j--)
		guid += sprintf(guid, "%02x", log_data->log_page_guid[j]);
	json_object_add_value_string(root, "Log page GUID", guid_buf);

	json_print_object(root, NULL);
	printf("\n");

	json_free_object(root);
}

static void json_c1_log(struct ocp_error_recovery_log_page *log_data)
{
	struct json_object *root;

	root = json_create_object();
	char guid[64];

	json_object_add_value_int(root, "Panic Reset Wait Time",
				  le16_to_cpu(log_data->panic_reset_wait_time));
	json_object_add_value_int(root, "Panic Reset Action", log_data->panic_reset_action);
	json_object_add_value_int(root, "Device Recovery Action 1",
				  log_data->device_recover_action_1);
	json_object_add_value_int(root, "Panic ID", le32_to_cpu(log_data->panic_id));
	json_object_add_value_int(root, "Device Capabilities",
				  le32_to_cpu(log_data->device_capabilities));
	json_object_add_value_int(root, "Vendor Specific Recovery Opcode",
				  log_data->vendor_specific_recovery_opcode);
	json_object_add_value_int(root, "Vendor Specific Command CDW12",
				  le32_to_cpu(log_data->vendor_specific_command_cdw12));
	json_object_add_value_int(root, "Vendor Specific Command CDW13",
				  le32_to_cpu(log_data->vendor_specific_command_cdw13));
	json_object_add_value_int(root, "Vendor Specific Command Timeout",
				  log_data->vendor_specific_command_timeout);
	json_object_add_value_int(root, "Device Recovery Action 2",
				  log_data->device_recover_action_2);
	json_object_add_value_int(root, "Device Recovery Action 2 Timeout",
				  log_data->device_recover_action_2_timeout);
	json_object_add_value_int(root, "Log Page Version",
				  le16_to_cpu(log_data->log_page_version));

	memset((void *)guid, 0, 64);
	sprintf((char *)guid, "0x%"PRIx64"%"PRIx64"",
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data->log_page_guid[8]),
		(uint64_t)le64_to_cpu(*(uint64_t *)&log_data->log_page_guid[0]));
	json_object_add_value_string(root, "Log page GUID", guid);

	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static struct ocp_print_ops json_print_ops = {
	.hwcomp_log = json_hwcomp_log,
	.fw_act_history = json_fw_activation_history,
	.smart_extended_log = json_smart_extended_log,
	.telemetry_log = json_telemetry_log,
	.c3_log = json_c3_log,
	.c5_log = json_c5_log,
	.c1_log = json_c1_log,
};

struct ocp_print_ops *ocp_get_json_print_ops(nvme_print_flags_t flags)
{
	json_print_ops.flags = flags;
	return &json_print_ops;
}