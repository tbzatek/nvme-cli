// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nbft.c
 *
 * Copyright (c) 2021-2022, Dell Inc. or its subsidiaries.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <errno.h>
#include <stdio.h>
#include <fnmatch.h>
#include <uuid/uuid.h>

#include "nvme.h"
#include "libnvme.h"
#include "nvme-print.h"

#define NBFT_SYSFS_PATH		"/sys/firmware/acpi/tables"
#define NBFT_SYSFS_FILENAME	"NBFT*"
#define PATH_NVMF_CONFIG	"/etc/nvme/config.json"

static const char *nvmf_config_file	= "Use specified JSON configuration file or 'none' to disable";
static bool dump_config;
static const char dash[100] = {[0 ... 99] = '-'};

#define PCI_SEGMENT(sbdf) ((sbdf & 0xffff0000) >> 16)
#define PCI_BUS(sbdf) ((sbdf & 0x0000ff00) >> 8)
#define PCI_DEV(sbdf) ((sbdf & 0x000000f8) >> 3)
#define PCI_FUNC(sbdf) ((sbdf & 0x00000007) >> 0)

static const char *pci_sbdf_to_string(__u16 pci_sbdf)
{
	static char pcidev[13];

	snprintf(pcidev, sizeof(pcidev), "%x:%x:%x.%x",
		 PCI_SEGMENT(pci_sbdf),
		 PCI_BUS(pci_sbdf),
		 PCI_DEV(pci_sbdf),
		 PCI_FUNC(pci_sbdf));
	return pcidev;
}

static char *mac_addr_to_string(unsigned char *mac_addr)
{
	static char mac_string[18];

	snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac_addr[0],
		 mac_addr[1],
		 mac_addr[2],
		 mac_addr[3],
		 mac_addr[4],
		 mac_addr[5]);
	return mac_string;
}

static void print_connect_msg(nvme_ctrl_t c)
{
	printf("device: %s\n", nvme_ctrl_get_name(c));
}

static void json_connect_msg(nvme_ctrl_t c)
{
	struct json_object *root;

	root = json_create_object();
	json_object_add_value_string(root, "device", nvme_ctrl_get_name(c));

	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

int nbft_filter(const struct dirent *dent)
{
	return !fnmatch(NBFT_SYSFS_FILENAME, dent->d_name, FNM_PATHNAME);
}

int read_sysfs_nbft_files(struct list_head *nbft_list, char *path)
{
	struct dirent **dent;
	char filename[PATH_MAX];
	int i, count, ret;
	struct nbft_info *nbft;

	count = scandir(path, &dent, nbft_filter, NULL);
	if (count < 0) {
		fprintf(stderr, "Failed to open %s.\n", path);
		return -1;
	}

	for (i = 0; i < count; i++) {
		snprintf(filename, sizeof(filename), "%s/%s", path, dent[i]->d_name);
		ret = nbft_read(&nbft, filename);
		if (!ret)
			list_add_tail(nbft_list, &nbft->node);
		free(dent[i]);
	}
	free(dent);
	return 0;
}

static void free_nbfts(struct list_head *nbft_list)
{
	struct nbft_info *nbft;

	while ((nbft = list_pop(nbft_list, struct nbft_info, node)))
		nbft_free(nbft);
}

#define check_fail(x)		\
	if (x)			\
		goto fail;

static json_object *hfi_to_json(struct nbft_hfi *hfi)
{
	struct json_object *hfi_json;

	hfi_json = json_create_object();
	if (!hfi_json)
		return NULL;

	check_fail(json_object_add_value_int(hfi_json, "index", hfi->index));
	check_fail(json_object_add_value_string(hfi_json, "transport", hfi->transport));

	if (strcmp(hfi->transport, "tcp") == 0) {
		check_fail(json_object_add_value_string(hfi_json, "pcidev", pci_sbdf_to_string(hfi->tcp_info.pci_sbdf)));
		if (hfi->tcp_info.mac_addr)
			check_fail(json_object_add_value_string(hfi_json, "mac_addr", mac_addr_to_string(hfi->tcp_info.mac_addr)));
		check_fail(json_object_add_value_int(hfi_json, "vlan", hfi->tcp_info.vlan));
		check_fail(json_object_add_value_int(hfi_json, "ip_origin", hfi->tcp_info.ip_origin));
		check_fail(json_object_add_value_string(hfi_json, "ipaddr", hfi->tcp_info.ipaddr));
		check_fail(json_object_add_value_int(hfi_json, "subnet_mask_prefix", hfi->tcp_info.subnet_mask_prefix));
		check_fail(json_object_add_value_string(hfi_json, "gateway_ipaddr", hfi->tcp_info.gateway_ipaddr));
		check_fail(json_object_add_value_int(hfi_json, "route_metric", hfi->tcp_info.route_metric));
		check_fail(json_object_add_value_string(hfi_json, "primary_dns_ipaddr", hfi->tcp_info.primary_dns_ipaddr));
		check_fail(json_object_add_value_string(hfi_json, "secondary_dns_ipaddr", hfi->tcp_info.secondary_dns_ipaddr));
		check_fail(json_object_add_value_string(hfi_json, "dhcp_server_ipaddr", hfi->tcp_info.dhcp_server_ipaddr));
		if (hfi->tcp_info.host_name)
			check_fail(json_object_add_value_string(hfi_json, "host_name", hfi->tcp_info.host_name));
		check_fail(json_object_add_value_int(hfi_json, "this_hfi_is_default_route", hfi->tcp_info.this_hfi_is_default_route));
		check_fail(json_object_add_value_int(hfi_json, "dhcp_override", hfi->tcp_info.dhcp_override));
	}

	return hfi_json;
fail:
	json_free_object(hfi_json);
	return NULL;
}

static json_object *ssns_to_json(struct nbft_subsystem_ns *ss)
{
	struct json_object *ss_json;
	struct json_object *hfi_array_json;
	int i;

	ss_json = json_create_object();
	if (!ss_json)
		return NULL;

	hfi_array_json = json_create_array();
	if (!hfi_array_json)
		goto fail;

	for (i = 0; i < ss->num_hfis; i++)
		check_fail(json_array_add_value_object(hfi_array_json, json_object_new_int(ss->hfis[i]->index)));

	check_fail(json_object_add_value_int(ss_json, "index", ss->index));
	check_fail(json_object_add_value_int(ss_json, "num_hfis", ss->num_hfis));
	check_fail(json_object_object_add(ss_json, "hfis", hfi_array_json));

	check_fail(json_object_add_value_string(ss_json, "transport", ss->transport));
	check_fail(json_object_add_value_string(ss_json, "transport_address", ss->transport_address));
	check_fail(json_object_add_value_string(ss_json, "transport_svcid", ss->transport_svcid));
	check_fail(json_object_add_value_int(ss_json, "subsys_port_id", ss->subsys_port_id));
	check_fail(json_object_add_value_int(ss_json, "nsid", ss->nsid));
	{
		char json_str[40];
		char *json_str_p;

		memset(json_str, 0, sizeof(json_str));
		json_str_p = json_str;

		switch (ss->nid_type) {
		case ieee_eui_64:
			check_fail(json_object_add_value_string(ss_json, "nid_type", "eui64"));
			for (i = 0; i < 8; i++)
				json_str_p += sprintf(json_str_p, "%02x", ss->nid[i]);
			break;

		case nguid:
			check_fail(json_object_add_value_string(ss_json, "nid_type", "nguid"));
			for (i = 0; i < 16; i++)
				json_str_p += sprintf(json_str_p, "%02x", ss->nid[i]);
			break;

#ifdef CONFIG_LIBUUID
		case ns_uuid:
			check_fail(json_object_add_value_string(ss_json, "nid_type", "uuid"));
			uuid_unparse_lower(ss->nid, json_str);
			break;
#endif
		default:
			break;
		}
		check_fail(json_object_add_value_string(ss_json, "nid", json_str));
	}
	if (ss->subsys_nqn)
		check_fail(json_object_add_value_string(ss_json, "subsys_nqn", ss->subsys_nqn));
	check_fail(json_object_add_value_int(ss_json, "controller_id", ss->controller_id));
	check_fail(json_object_add_value_int(ss_json, "asqsz", ss->asqsz));
	if (ss->dhcp_root_path_string)
		check_fail(json_object_add_value_string(ss_json, "dhcp_root_path_string", ss->dhcp_root_path_string));
	check_fail(json_object_add_value_int(ss_json, "pdu_header_digest_required", ss->pdu_header_digest_required));
	check_fail(json_object_add_value_int(ss_json, "data_digest_required", ss->data_digest_required));

	return ss_json;
fail:
	json_free_object(ss_json);
	return NULL;
}

static json_object *discovery_to_json(struct nbft_discovery *disc)
{
	struct json_object *disc_json;

	disc_json = json_create_object();
	if (disc_json) {
		check_fail(json_object_add_value_int(disc_json, "index", disc->index));
		if (disc->security)
			check_fail(json_object_add_value_int(disc_json, "security", disc->security->index));
		if (disc->hfi)
			check_fail(json_object_add_value_int(disc_json, "hfi", disc->hfi->index));
		if (disc->uri)
			check_fail(json_object_add_value_string(disc_json, "uri", disc->uri));
		if (disc->nqn)
			check_fail(json_object_add_value_string(disc_json, "nqn", disc->nqn));
	}
	return disc_json;
fail:
	json_free_object(disc_json);
	return NULL;
}

static struct json_object *nbft_to_json(struct nbft_info *nbft, bool show_subsys, bool show_hfi, bool show_discovery)
{
	struct json_object *nbft_json;

	nbft_json = json_create_object();
	if (!nbft_json)
		return NULL;

	check_fail(json_object_add_value_string(nbft_json, "filename", nbft->filename));
	{
		struct json_object *host_json;

		host_json = json_create_object();
		if (!host_json)
			goto fail;
		if (nbft->host.nqn)
			check_fail(json_object_add_value_string(host_json, "nqn", nbft->host.nqn));
		if (nbft->host.id)
			check_fail(json_object_add_value_string(host_json, "id", util_uuid_to_string(*nbft->host.id)));
		json_object_add_value_int(host_json, "host_id_configured", nbft->host.host_id_configured);
		json_object_add_value_int(host_json, "host_nqn_configured", nbft->host.host_nqn_configured);
		json_object_add_value_string(host_json, "primary_admin_host_flag",
					     nbft->host.primary == not_indicated ? "not indicated" :
					     nbft->host.primary == unselected ? "unselected" :
					     nbft->host.primary == selected ? "selected" : "reserved");
		if (json_object_object_add(nbft_json, "host", host_json)) {
			json_free_object(host_json);
			goto fail;
		}
	}
	if (show_subsys) {
		struct json_object *subsys_array_json, *subsys_json;
		struct nbft_subsystem_ns *ss;

		subsys_array_json = json_create_array();
		if (!subsys_array_json)
			goto fail;
		list_for_each(&nbft->subsystem_ns_list, ss, node) {
			subsys_json = ssns_to_json(ss);
			if (!subsys_json)
				goto fail;
			if (json_object_array_add(subsys_array_json, subsys_json)) {
				json_free_object(subsys_json);
				goto fail;
			}
		}
		if (json_object_object_add(nbft_json, "subsystem", subsys_array_json)) {
			json_free_object(subsys_array_json);
			goto fail;
		}
	}
	if (show_hfi) {
		struct json_object *hfi_array_json, *hfi_json;
		struct nbft_hfi *hfi;

		hfi_array_json = json_create_array();
		if (!hfi_array_json)
			goto fail;
		list_for_each(&nbft->hfi_list, hfi, node) {
			hfi_json = hfi_to_json(hfi);
			if (!hfi_json)
				goto fail;
			if (json_object_array_add(hfi_array_json, hfi_json)) {
				json_free_object(hfi_json);
				goto fail;
			}
		}
		if (json_object_object_add(nbft_json, "hfi", hfi_array_json)) {
			json_free_object(hfi_array_json);
			goto fail;
		}
	}
	if (show_discovery) {
		struct json_object *discovery_array_json, *discovery_json;
		struct nbft_discovery *disc;

		discovery_array_json = json_create_array();
		if (!discovery_array_json)
			goto fail;
		list_for_each(&nbft->discovery_list, disc, node) {
			discovery_json = discovery_to_json(disc);
			if (!discovery_json)
				goto fail;
			if (json_object_array_add(discovery_array_json, discovery_json)) {
				json_free_object(discovery_json);
				goto fail;
			}
		}
		if (json_object_object_add(nbft_json, "discovery", discovery_array_json)) {
			json_free_object(discovery_array_json);
			goto fail;
		}
	}
	return nbft_json;
fail:
	json_free_object(nbft_json);
	return NULL;
}

static int json_show_nbfts(struct list_head *nbft_list, bool show_subsys, bool show_hfi, bool show_discovery)
{
	struct json_object *nbft_json_array, *nbft_json;
	struct nbft_info *nbft;

	nbft_json_array = json_create_array();
	if (!nbft_json_array)
		return ENOMEM;

	list_for_each(nbft_list, nbft, node) {
		nbft_json = nbft_to_json(nbft, show_subsys, show_hfi, show_discovery);
		if (!nbft_json)
			goto fail;
		if (json_object_array_add(nbft_json_array, nbft_json)) {
			json_free_object(nbft_json);
			goto fail;
		}
	}

	json_print_object(nbft_json_array, NULL);
	printf("\n");
	json_free_object(nbft_json_array);
	return 0;
fail:
	json_free_object(nbft_json_array);
	return ENOMEM;
}

static void print_nbft_hfi_info(struct nbft_info *nbft)
{
	struct nbft_hfi *hfi;

	if (list_empty(&nbft->hfi_list))
		return;

	printf("\nNBFT HFIs:\n\n");
	printf("%-5s %-9s %-12s %-17s %-4s %-39s %-16s %-39s %-39s\n", "Index", "Transport", "PCI Address", "MAC Address", "DHCP", "IP Address", "Subnet Mask Bits", "Gateway", "DNS");
	printf("%-.5s %-.9s %-.12s %-.17s %-.4s %-.39s %-.16s %-.39s %-.39s\n", dash, dash, dash, dash, dash, dash, dash, dash, dash);
	list_for_each(&nbft->hfi_list, hfi, node)
		printf("%-5d %-9s %-12s %-17s %-4s %-39s %-16d %-39s %-39s\n",
		       hfi->index,
		       hfi->transport,
		       pci_sbdf_to_string(hfi->tcp_info.pci_sbdf),
		       mac_addr_to_string(hfi->tcp_info.mac_addr),
		       hfi->tcp_info.dhcp_override ? "yes" : "no",
		       hfi->tcp_info.ipaddr,
		       hfi->tcp_info.subnet_mask_prefix,
		       hfi->tcp_info.gateway_ipaddr,
		       hfi->tcp_info.primary_dns_ipaddr);
}

static void print_nbft_discovery_info(struct nbft_info *nbft)
{
	struct nbft_discovery *disc;

	if (list_empty(&nbft->discovery_list))
		return;

	printf("\nNBFT Discovery Controllers:\n\n");
	printf("%-5s %-96s %-96s\n", "Index", "Discovery-URI", "Discovery-NQN");
	printf("%-.5s %-.96s %-.96s\n", dash, dash, dash);
	list_for_each(&nbft->discovery_list, disc, node)
		printf("%-5d %-96s %-96s\n", disc->index, disc->uri, disc->nqn);
}

static void print_nbft_subsys_info(struct nbft_info *nbft)
{
	struct nbft_subsystem_ns *ss;
	int i;

	if (list_empty(&nbft->subsystem_ns_list))
		return;

	printf("\nNBFT Subsystems:\n\n");
	printf("%-5s %-96s %-9s %-39s %-5s %-20s\n", "Index", "Host-NQN", "Transport", "Address", "SvcId", "HFIs");
	printf("%-.5s %-.96s %-.9s %-.39s %-.5s %-.20s\n", dash, dash, dash, dash, dash, dash);
	list_for_each(&nbft->subsystem_ns_list, ss, node) {
		printf("%-5d %-96s %-9s %-39s %-5s", ss->index, ss->subsys_nqn, ss->transport, ss->transport_address, ss->transport_svcid);
		for (i = 0; i < ss->num_hfis; i++)
			printf(" %d", ss->hfis[i]->index);
		printf("\n");
	}
}

static void normal_show_nbft(struct nbft_info *nbft, bool show_subsys, bool show_hfi, bool show_discovery)
{
	printf("%s:\n", nbft->filename);
	if (list_empty(&nbft->hfi_list) &&
	    list_empty(&nbft->security_list) &&
	    list_empty(&nbft->discovery_list) &&
	    list_empty(&nbft->subsystem_ns_list))
		printf("(empty)\n");
	else {
		if (show_subsys)
			print_nbft_subsys_info(nbft);
		if (show_hfi)
			print_nbft_hfi_info(nbft);
		if (show_discovery)
			print_nbft_discovery_info(nbft);
	}
}

static void normal_show_nbfts(struct list_head *nbft_list, bool show_subsys, bool show_hfi, bool show_discovery)
{
	bool not_first = false;
	struct nbft_info *nbft;

	list_for_each(nbft_list, nbft, node) {
		if (not_first)
			printf("\n");
		normal_show_nbft(nbft, show_subsys, show_hfi, show_discovery);
		not_first = true;
	}
}

int show_nbft(const char *desc, int argc, char **argv)
{
	struct list_head nbft_list;
	char *format = "normal";
	char *nbft_path = NBFT_SYSFS_PATH;
	enum nvme_print_flags flags = -1;
	int ret;
	bool show_subsys = false, show_hfi = false, show_discovery = false;

	OPT_ARGS(opts) = {
		OPT_FMT("output-format", 'o', &format, "Output format: normal|json"),
		OPT_FLAG("subsystem", 's', &show_subsys, "show NBFT subsystems"),
		OPT_FLAG("hfi", 'H', &show_hfi, "show NBFT HFIs"),
		OPT_FLAG("discovery", 'd', &show_discovery, "show NBFT discovery controllers"),
		OPT_STRING("nbft-path", 'P', "STR", &nbft_path, "user-defined path for NBFT tables"),
		OPT_END()
	};

	ret = argconfig_parse(argc, argv, desc, opts);
	if (ret)
		return ret;

	if (!strcmp(format, ""))
		flags = -1;
	else if (!strcmp(format, "normal"))
		flags = NORMAL;
	else if (!strcmp(format, "json"))
		flags = JSON;
	else
		return EINVAL;

	list_head_init(&nbft_list);
	ret = read_sysfs_nbft_files(&nbft_list, nbft_path);
	if (!ret) {
		if (flags == NORMAL)
			normal_show_nbfts(&nbft_list, show_subsys, show_hfi, show_discovery);
		else if (flags == JSON)
			ret = json_show_nbfts(&nbft_list, show_subsys, show_hfi, show_discovery);
		free_nbfts(&nbft_list);
	}
	return ret;
}

int connect_nbft(const char *desc, int argc, char **argv)
{
	char *hnqn = NULL, *hid = NULL;
	char *hostnqn = NULL, *hostid = NULL;
	char *host_traddr = NULL;
	char *nbft_path = NBFT_SYSFS_PATH;
	bool user_hostnqn = false, user_hostid = false, user_host_traddr = false;
	int free_hnqn, free_hid;
	char *config_file = PATH_NVMF_CONFIG;
	//char *hostkey = NULL, *ctrlkey = NULL;
	unsigned int verbose = 0;
	nvme_root_t r;
	nvme_host_t h;
	nvme_ctrl_t c;
	int ret, i;
	struct nvme_fabrics_config cfg;
	enum nvme_print_flags flags = -1;
	char *format = "normal";
	struct list_head nbft_list;
	struct nbft_info *nbft;
	struct nbft_subsystem_ns *ss;
	struct nbft_hfi *hfi;

	OPT_ARGS(opts) = {
		OPT_STRING("config", 'J', "FILE", &config_file, nvmf_config_file),
		OPT_INCR("verbose", 'v', &verbose, "Increase logging verbosity"),
		OPT_FLAG("dump-config", 'O', &dump_config, "Dump JSON configuration to stdout"),
		OPT_FMT("output-format", 'o', &format, "Output format: normal|json"),
		OPT_STRING("hostnqn", 'q', "STR", &hostnqn, "user-defined hostnqn"),
		OPT_STRING("hostid", 'I', "STR", &hostid, "user-defined hostid"),
		OPT_STRING("host-traddr", 'w', "STR", &host_traddr, "user-defined host traddr"),
		OPT_STRING("nbft-path", 'P', "STR", &nbft_path, "user-defined path for NBFT tables"),
		OPT_END()
	};

	nvmf_default_config(&cfg);
	ret = argconfig_parse(argc, argv, desc, opts);
	if (ret)
		return ret;

	if (!strcmp(format, ""))
		flags = -1;
	else if (!strcmp(format, "normal"))
		flags = NORMAL;
	else if (!strcmp(format, "json"))
		flags = JSON;
	else
		return EINVAL;

	if (!strcmp(config_file, "none"))
		config_file = NULL;

	r = nvme_create_root(stderr, map_log_level(verbose, false));
	if (!r) {
		fprintf(stderr, "Failed to create topology root: %s\n",
			nvme_strerror(errno));
		return -errno;
	}

	ret = nvme_scan_topology(r, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "Failed to scan topology: %s\n",
			nvme_strerror(errno));
		nvme_free_tree(r);
	}
	nvme_read_config(r, config_file);

	if (host_traddr)
		user_host_traddr = true;
	if (hostnqn)
		user_hostnqn = true;
	if (hostid)
		user_hostid = true;

	list_head_init(&nbft_list);
	ret = read_sysfs_nbft_files(&nbft_list, nbft_path);
	if (ret)
		goto out_free_2;

	list_for_each(&nbft_list, nbft, node)
		list_for_each(&nbft->subsystem_ns_list, ss, node)
			for (i = 0; i < ss->num_hfis; i++) {
				hfi = ss->hfis[i];
				free_hnqn = false;
				if (!user_hostnqn) {
					hostnqn = hnqn = nbft->host.nqn;
					if (!hostnqn) {
						hostnqn = hnqn = nvmf_hostnqn_from_file();
						free_hnqn = true;
					}
				}

				free_hid = false;
				if (!user_hostid) {
					if (*nbft->host.id) {
						hostid = hid = (char *)util_uuid_to_string(*nbft->host.id);
						if (!hostid) {
							hostid = hid = nvmf_hostid_from_file();
							free_hid = true;
						}
					}
				}

				h = nvme_lookup_host(r, hostnqn, hostid);
				if (!h) {
					errno = ENOMEM;
					goto out_free;
				}

				if (!user_host_traddr) {
					host_traddr = NULL;
					if (!strncmp(ss->transport, "tcp", 3))
						host_traddr = hfi->tcp_info.ipaddr;
				}

				//if (hostkey)
				//	nvme_host_set_dhchap_key(h, hostkey);
				c = nvme_create_ctrl(r, ss->subsys_nqn, ss->transport, ss->transport_address,
						     host_traddr, NULL, ss->transport_svcid);
				if (!c) {
					errno = ENOMEM;
					goto out_free;
				}
				//if (ctrlkey)
				//	nvme_ctrl_set_dhchap_key(c, ctrlkey);

				errno = 0;
				ret = nvmf_add_ctrl(h, c, &cfg);

				/*
				 * With TCP/DHCP, it can happen that the OS
				 * obtains a different local IP address than the
				 * firwmare had. Retry without host_traddr.
				*/
				if (ret == -1 && errno == ENVME_CONNECT_WRITE &&
				    host_traddr && !user_host_traddr &&
				    !strcmp(ss->transport, "tcp") &&
				    strlen(hfi->tcp_info.dhcp_server_ipaddr) > 0) {
					nvme_free_ctrl(c);
					c = nvme_create_ctrl(r, ss->subsys_nqn, ss->transport,
							     ss->transport_address,
							     NULL, NULL, ss->transport_svcid);
					if (!c) {
						errno = ENOMEM;
						goto out_free;
					}
					errno = 0;
					ret = nvmf_add_ctrl(h, c, &cfg);
					if (ret == 0 && verbose >= 1)
						fprintf(stderr,
							"connect with host_traddr=\"%s\" failed, success after omitting host_traddr\n",
							host_traddr);
				}

				if (ret)
					fprintf(stderr, "no controller found\n");
				else {
					if (flags == NORMAL)
						print_connect_msg(c);
					else if (flags == JSON)
						json_connect_msg(c);
				}
out_free:
				if (free_hnqn)
					free(hnqn);
				if (free_hid)
					free(hid);
				if (errno == ENOMEM)
					goto out_free_2;
			}
out_free_2:
	if (dump_config)
		nvme_dump_config(r);
	nvme_free_tree(r);
	free_nbfts(&nbft_list);
	return errno;
}
