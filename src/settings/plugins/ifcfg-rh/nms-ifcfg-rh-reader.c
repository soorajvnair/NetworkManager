/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2008 - 2015 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-ifcfg-rh-reader.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nm-connection.h"
#include "nm-dbus-interface.h"
#include "nm-setting-connection.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-vlan.h"
#include "nm-setting-ip6-config.h"
#include "nm-setting-wired.h"
#include "nm-setting-wireless.h"
#include "nm-setting-8021x.h"
#include "nm-setting-bond.h"
#include "nm-setting-team.h"
#include "nm-setting-team-port.h"
#include "nm-setting-bridge.h"
#include "nm-setting-bridge-port.h"
#include "nm-setting-dcb.h"
#include "nm-setting-proxy.h"
#include "nm-setting-generic.h"
#include "nm-core-internal.h"
#include "nm-utils.h"

#include "platform/nm-platform.h"
#include "NetworkManagerUtils.h"

#include "nms-ifcfg-rh-common.h"
#include "nms-ifcfg-rh-utils.h"
#include "shvar.h"

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_SETTINGS
#define _NMLOG_PREFIX_NAME "ifcfg-rh"
#define _NMLOG(level, ...) \
    G_STMT_START { \
        nm_log ((level), (_NMLOG_DOMAIN), \
                "%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__), \
                _NMLOG_PREFIX_NAME": " \
                _NM_UTILS_MACRO_REST(__VA_ARGS__)); \
    } G_STMT_END

#define PARSE_WARNING(...) _LOGW ("%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__), "    " _NM_UTILS_MACRO_REST(__VA_ARGS__))

/*****************************************************************************/

static gboolean
get_uint (const char *str, guint32 *value)
{
	gint64 tmp;

	tmp = _nm_utils_ascii_str_to_int64 (str, 0, 0, G_MAXUINT32, -1);
	if (tmp == -1)
		return FALSE;
	*value = tmp;
	return TRUE;
}

static char *
make_connection_name (shvarFile *ifcfg,
                      const char *ifcfg_name,
                      const char *suggested,
                      const char *prefix)
{
	char *full_name = NULL, *name;

	/* If the ifcfg file already has a NAME, always use that */
	name = svGetValueString (ifcfg, "NAME");
	if (name && strlen (name))
		return name;

	/* Otherwise construct a new NAME */
	g_free (name);
	if (!prefix)
		prefix = _("System");

	/* For cosmetic reasons, if the suggested name is the same as
	 * the ifcfg files name, don't use it.  Mainly for wifi so that
	 * the SSID is shown in the connection ID instead of just "wlan0".
	 */
	if (suggested && strcmp (ifcfg_name, suggested))
		full_name = g_strdup_printf ("%s %s (%s)", prefix, suggested, ifcfg_name);
	else
		full_name = g_strdup_printf ("%s %s", prefix, ifcfg_name);

	return full_name;
}

static NMSetting *
make_connection_setting (const char *file,
                         shvarFile *ifcfg,
                         const char *type,
                         const char *suggested,
                         const char *prefix)
{
	NMSettingConnection *s_con;
	NMSettingConnectionLldp lldp;
	const char *ifcfg_name = NULL;
	char *new_id, *uuid = NULL, *zone = NULL, *value;
	gs_free char *stable_id = NULL;

	ifcfg_name = utils_get_ifcfg_name (file, TRUE);
	if (!ifcfg_name)
		return NULL;

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());

	new_id = make_connection_name (ifcfg, ifcfg_name, suggested, prefix);
	g_object_set (s_con, NM_SETTING_CONNECTION_ID, new_id, NULL);
	g_free (new_id);

	/* Try for a UUID key before falling back to hashing the file name */
	uuid = svGetValueString (ifcfg, "UUID");
	if (!uuid || !strlen (uuid)) {
		g_free (uuid);
		uuid = nm_utils_uuid_generate_from_string (svFileGetName (ifcfg), -1, NM_UTILS_UUID_TYPE_LEGACY, NULL);
	}

	stable_id = svGetValueString (ifcfg, "STABLE_ID");

	g_object_set (s_con,
	              NM_SETTING_CONNECTION_TYPE, type,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_STABLE_ID, stable_id,
	              NULL);
	g_free (uuid);

	value = svGetValueString (ifcfg, "DEVICE");
	if (value) {
		GError *error = NULL;

		if (nm_utils_is_valid_iface_name (value, &error)) {
			g_object_set (s_con,
			              NM_SETTING_CONNECTION_INTERFACE_NAME, value,
			              NULL);
		} else {
			PARSE_WARNING ("invalid DEVICE name '%s': %s", value, error->message);
			g_error_free (error);
		}
		g_free (value);
	}

	value = svGetValueString (ifcfg, "LLDP");
	if (!g_strcmp0 (value, "rx"))
		lldp = NM_SETTING_CONNECTION_LLDP_ENABLE_RX;
	else
		lldp = svParseBoolean (value, NM_SETTING_CONNECTION_LLDP_DEFAULT);
	g_free (value);

	/* Missing ONBOOT is treated as "ONBOOT=true" by the old network service */
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_AUTOCONNECT,
	              svGetValueBoolean (ifcfg, "ONBOOT", TRUE),
	              NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY,
	              (gint) svGetValueInt64 (ifcfg, "AUTOCONNECT_PRIORITY", 10,
	                                      NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY_MIN,
	                                      NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY_MAX,
	                                      NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY_DEFAULT),
	              NM_SETTING_CONNECTION_AUTOCONNECT_RETRIES,
	              (gint) svGetValueInt64 (ifcfg, "AUTOCONNECT_RETRIES", 10,
	                                      -1, G_MAXINT32, -1),
	              NM_SETTING_CONNECTION_AUTOCONNECT_SLAVES,
	              svGetValueBoolean (ifcfg, "AUTOCONNECT_SLAVES", NM_SETTING_CONNECTION_AUTOCONNECT_SLAVES_DEFAULT),
	              NM_SETTING_CONNECTION_LLDP, lldp,
	              NULL);

	value = svGetValueString (ifcfg, "USERS");
	if (value) {
		char **items, **iter;

		items = g_strsplit_set (value, " ", -1);
		for (iter = items; iter && *iter; iter++) {
			if (strlen (*iter)) {
				if (!nm_setting_connection_add_permission (s_con, "user", *iter, NULL))
					PARSE_WARNING ("invalid USERS item '%s'", *iter);
			}
		}
		g_free (value);
		g_strfreev (items);
	}


	zone = svGetValueString (ifcfg, "ZONE");
	if (!zone || !strlen (zone)) {
		g_free (zone);
		zone = NULL;
	}
	g_object_set (s_con, NM_SETTING_CONNECTION_ZONE, zone, NULL);
	g_free (zone);

	value = svGetValueString (ifcfg, "SECONDARY_UUIDS");
	if (value) {
		char **items, **iter;

		items = g_strsplit_set (value, " \t", -1);
		for (iter = items; iter && *iter; iter++) {
			if (strlen (*iter)) {
				if (!nm_setting_connection_add_secondary (s_con, *iter))
					PARSE_WARNING ("secondary connection UUID '%s' already added", *iter);
			}
		}
		g_free (value);
		g_strfreev (items);
	}

	value = svGetValueString (ifcfg, "BRIDGE_UUID");
	if (!value)
		value = svGetValueString (ifcfg, "BRIDGE");
	if (value) {
		const char *old_value;

		if ((old_value = nm_setting_connection_get_master (s_con))) {
			PARSE_WARNING ("Already configured as slave of %s. Ignoring BRIDGE=\"%s\"",
			               old_value, value);
		} else {
			g_object_set (s_con, NM_SETTING_CONNECTION_MASTER, value, NULL);
			g_object_set (s_con, NM_SETTING_CONNECTION_SLAVE_TYPE,
			              NM_SETTING_BRIDGE_SETTING_NAME, NULL);
		}
		g_free (value);
	}

	value = svGetValueString (ifcfg, "GATEWAY_PING_TIMEOUT");
	if (value) {
		gint64 tmp;

		tmp = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXINT32 - 1, -1);
		if (tmp >= 0)
			g_object_set (s_con, NM_SETTING_CONNECTION_GATEWAY_PING_TIMEOUT, (guint) tmp, NULL);
		else
			PARSE_WARNING ("invalid GATEWAY_PING_TIMEOUT time");
		g_free (value);
	}

	switch (svGetValueBoolean (ifcfg, "CONNECTION_METERED", -1)) {
	case TRUE:
		g_object_set (s_con, NM_SETTING_CONNECTION_METERED, NM_METERED_YES, NULL);
		break;
	case FALSE:
		g_object_set (s_con, NM_SETTING_CONNECTION_METERED, NM_METERED_NO, NULL);
		break;
	}

	return NM_SETTING (s_con);
}

/* Returns TRUE on missing address or valid address */
static gboolean
read_ip4_address (shvarFile *ifcfg,
                  const char *tag,
                  char **out_addr,
                  GError **error)
{
	char *value = NULL;

	g_return_val_if_fail (ifcfg != NULL, FALSE);
	g_return_val_if_fail (tag != NULL, FALSE);
	g_return_val_if_fail (out_addr != NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	*out_addr = NULL;

	value = svGetValueString (ifcfg, tag);
	if (!value)
		return TRUE;

	if (nm_utils_ipaddr_valid (AF_INET, value)) {
		*out_addr = value;
		return TRUE;
	} else {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Invalid %s IP4 address '%s'", tag, value);
		g_free (value);
		return FALSE;
	}
}

static char *
get_numbered_tag (char *tag_name, int which)
{
	if (which == -1)
		return g_strdup (tag_name);
	return g_strdup_printf ("%s%u", tag_name, which);
}

static gboolean
is_any_ip4_address_defined (shvarFile *ifcfg, int *idx)
{
	int i, ignore, *ret_idx;;

	ret_idx = idx ? idx : &ignore;

	for (i = -1; i <= 2; i++) {
		char *tag;
		char *value;

		tag = get_numbered_tag ("IPADDR", i);
		value = svGetValueString (ifcfg, tag);
		g_free (tag);
		if (value) {
			g_free (value);
			*ret_idx = i;
			return TRUE;
		}

		tag = get_numbered_tag ("PREFIX", i);
		value = svGetValueString (ifcfg, tag);
		g_free(tag);
		if (value) {
			g_free (value);
			*ret_idx = i;
			return TRUE;
		}

		tag = get_numbered_tag ("NETMASK", i);
		value = svGetValueString (ifcfg, tag);
		g_free(tag);
		if (value) {
			g_free (value);
			*ret_idx = i;
			return TRUE;
		}
	}
	return FALSE;
}

/* Returns TRUE on missing address or valid address */
static gboolean
read_full_ip4_address (shvarFile *ifcfg,
                       gint32 which,
                       NMIPAddress *base_addr,
                       NMIPAddress **out_address,
                       char **out_gateway,
                       GError **error)
{
	char *ip_tag, *prefix_tag, *netmask_tag, *gw_tag;
	char *ip = NULL;
	int prefix = 0;
	gboolean success = FALSE;
	char *value;
	guint32 tmp;

	g_return_val_if_fail (which >= -1, FALSE);
	g_return_val_if_fail (ifcfg != NULL, FALSE);
	g_return_val_if_fail (out_address != NULL, FALSE);
	g_return_val_if_fail (*out_address == NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	ip_tag = get_numbered_tag ("IPADDR", which);
	prefix_tag = get_numbered_tag ("PREFIX", which);
	netmask_tag = get_numbered_tag ("NETMASK", which);
	gw_tag = get_numbered_tag ("GATEWAY", which);

	/* IP address */
	if (!read_ip4_address (ifcfg, ip_tag, &ip, error))
		goto done;
	if (!ip) {
		if (base_addr)
			ip = g_strdup (nm_ip_address_get_address (base_addr));
		else {
			success = TRUE;
			goto done;
		}
	}

	/* Gateway */
	if (out_gateway && !*out_gateway) {
		if (!read_ip4_address (ifcfg, gw_tag, out_gateway, error))
			goto done;
	}

	/* Prefix */
	value = svGetValueString (ifcfg, prefix_tag);
	if (value) {
		prefix = _nm_utils_ascii_str_to_int64 (value, 10, 0, 32, -1);
		if (prefix < 0) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid IP4 prefix '%s'", value);
			g_free (value);
			goto done;
		}
		g_free (value);
	} else {
		/* Fall back to NETMASK if no PREFIX was specified */
		if (!read_ip4_address (ifcfg, netmask_tag, &value, error))
			goto done;
		if (value) {
			inet_pton (AF_INET, value, &tmp);
			prefix = nm_utils_ip4_netmask_to_prefix (tmp);
			g_free (value);
		} else {
			if (base_addr)
				prefix = nm_ip_address_get_prefix (base_addr);
			else {
				/* Try to autodetermine the prefix for the address' class */
				if (inet_pton (AF_INET, ip, &tmp) == 1) {
					prefix = nm_utils_ip4_get_default_prefix (tmp);

					PARSE_WARNING ("missing %s, assuming %s/%d", prefix_tag, ip, prefix);
				} else {
					g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
					             "Missing IP4 prefix");
					goto done;
				}
			}
		}
	}

	*out_address = nm_ip_address_new (AF_INET, ip, prefix, error);
	if (*out_address)
		success = TRUE;

done:
	g_free (ip);
	g_free (ip_tag);
	g_free (prefix_tag);
	g_free (netmask_tag);
	g_free (gw_tag);

	return success;
}

/* Returns TRUE on missing route or valid route */
static gboolean
read_one_ip4_route (shvarFile *ifcfg,
                    guint32 which,
                    NMIPRoute **out_route,
                    GError **error)
{
	char *ip_tag, *netmask_tag, *gw_tag, *metric_tag, *value;
	char *dest = NULL, *next_hop = NULL;
	gint64 prefix, metric;
	gboolean success = FALSE;

	g_return_val_if_fail (ifcfg != NULL, FALSE);
	g_return_val_if_fail (out_route != NULL, FALSE);
	g_return_val_if_fail (*out_route == NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	ip_tag = g_strdup_printf ("ADDRESS%u", which);
	netmask_tag = g_strdup_printf ("NETMASK%u", which);
	gw_tag = g_strdup_printf ("GATEWAY%u", which);
	metric_tag = g_strdup_printf ("METRIC%u", which);

	/* Destination */
	if (!read_ip4_address (ifcfg, ip_tag, &dest, error))
		goto out;
	if (!dest) {
		/* Check whether IP is missing or 0.0.0.0 */
		char *val;
		val = svGetValueString (ifcfg, ip_tag);
		if (!val) {
			*out_route = NULL;
			success = TRUE;  /* missing route = success */
			goto out;
		}
		g_free (val);
	}

	/* Next hop */
	if (!read_ip4_address (ifcfg, gw_tag, &next_hop, error))
		goto out;
	/* We don't make distinction between missing GATEWAY IP and 0.0.0.0 */

	/* Prefix */
	if (!read_ip4_address (ifcfg, netmask_tag, &value, error))
		goto out;
	if (value) {
		guint32 netmask;

		inet_pton (AF_INET, value, &netmask);
		prefix = nm_utils_ip4_netmask_to_prefix (netmask);
		g_free (value);
		if (prefix == 0 || netmask != nm_utils_ip4_prefix_to_netmask (prefix)) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid IP4 netmask '%s' \"%s\"", netmask_tag, nm_utils_inet4_ntop (netmask, NULL));
			goto out;
		}
	} else {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IP4 route element '%s'", netmask_tag);
		goto out;
	}

	/* Metric */
	value = svGetValueString (ifcfg, metric_tag);
	if (value) {
		metric = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXUINT32, -1);
		if (metric < 0) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid IP4 route metric '%s'", value);
			g_free (value);
			goto out;
		}
		g_free (value);
	} else
		metric = -1;

	*out_route = nm_ip_route_new (AF_INET, dest, prefix, next_hop, metric, error);
	if (*out_route)
		success = TRUE;

out:
	g_free (dest);
	g_free (next_hop);
	g_free (ip_tag);
	g_free (netmask_tag);
	g_free (gw_tag);
	g_free (metric_tag);
	return success;
}

static gboolean
read_route_file_legacy (const char *filename, NMSettingIPConfig *s_ip4, GError **error)
{
	char *contents = NULL;
	gsize len = 0;
	char **lines = NULL, **iter;
	GRegex *regex_to1, *regex_to2, *regex_via, *regex_metric;
	GMatchInfo *match_info;
	int prefix_int;
	gint64 metric_int;
	gboolean success = FALSE;

	const char *pattern_empty = "^\\s*(\\#.*)?$";
	const char *pattern_to1 = "^\\s*(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}|default)"  /* IP or 'default' keyword */
	                          "(?:/(\\d{1,2}))?";                                         /* optional prefix */
	const char *pattern_to2 = "to\\s+(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}|default)" /* IP or 'default' keyword */
	                          "(?:/(\\d{1,2}))?";                                         /* optional prefix */
	const char *pattern_via = "via\\s+(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})";       /* IP of gateway */
	const char *pattern_metric = "metric\\s+(\\d+)";                                      /* metric */

	g_return_val_if_fail (filename != NULL, FALSE);
	g_return_val_if_fail (s_ip4 != NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* Read the route file */
	if (!g_file_get_contents (filename, &contents, &len, NULL) || !len) {
		g_free (contents);
		return TRUE;  /* missing/empty = success */
	}

	/* Create regexes for pieces to be matched */
	regex_to1 = g_regex_new (pattern_to1, 0, 0, NULL);
	regex_to2 = g_regex_new (pattern_to2, 0, 0, NULL);
	regex_via = g_regex_new (pattern_via, 0, 0, NULL);
	regex_metric = g_regex_new (pattern_metric, 0, 0, NULL);

	/* Iterate through file lines */
	lines = g_strsplit_set (contents, "\n\r", -1);
	for (iter = lines; iter && *iter; iter++) {
		gs_free char *next_hop = NULL, *dest = NULL;
		char *prefix, *metric;
		NMIPRoute *route;

		/* Skip empty lines */
		if (g_regex_match_simple (pattern_empty, *iter, 0, 0))
			continue;

		/* Destination */
		g_regex_match (regex_to1, *iter, 0, &match_info);
		if (!g_match_info_matches (match_info)) {
			g_match_info_free (match_info);
			g_regex_match (regex_to2, *iter, 0, &match_info);
			if (!g_match_info_matches (match_info)) {
				g_match_info_free (match_info);
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Missing IP4 route destination address in record: '%s'", *iter);
				goto error;
			}
		}
		dest = g_match_info_fetch (match_info, 1);
		if (!strcmp (dest, "default")) {
			g_match_info_free (match_info);
			PARSE_WARNING ("ignoring manual default route: '%s' (%s)", *iter, filename);
			continue;
		}
		if (!nm_utils_ipaddr_valid (AF_INET, dest)) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid IP4 route destination address '%s'", dest);
			g_match_info_free (match_info);
			goto error;
		}

		/* Prefix - is optional; 32 if missing */
		prefix = g_match_info_fetch (match_info, 2);
		g_match_info_free (match_info);
		prefix_int = 32;
		if (prefix) {
			prefix_int = _nm_utils_ascii_str_to_int64 (prefix, 10, 1, 32, -1);
			if (prefix_int == -1) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP4 route destination prefix '%s'", prefix);
				g_free (prefix);
				goto error;
			}
		}
		g_free (prefix);

		/* Next hop */
		g_regex_match (regex_via, *iter, 0, &match_info);
		if (g_match_info_matches (match_info)) {
			next_hop = g_match_info_fetch (match_info, 1);
			if (!nm_utils_ipaddr_valid (AF_INET, next_hop)) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP4 route gateway address '%s'",
				             next_hop);
				g_match_info_free (match_info);
				goto error;
			}
		} else {
			/* we don't make distinction between missing GATEWAY IP and 0.0.0.0 */
		}
		g_match_info_free (match_info);

		/* Metric */
		g_regex_match (regex_metric, *iter, 0, &match_info);
		metric_int = -1;
		if (g_match_info_matches (match_info)) {
			metric = g_match_info_fetch (match_info, 1);
			metric_int = _nm_utils_ascii_str_to_int64 (metric, 10, 0, G_MAXUINT32, -1);
			if (metric_int == -1) {
				g_match_info_free (match_info);
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP4 route metric '%s'", metric);
				g_free (metric);
				goto error;
			}
			g_free (metric);
		}
		g_match_info_free (match_info);

		route = nm_ip_route_new (AF_INET, dest, prefix_int, next_hop, metric_int, error);
		if (!route)
			goto error;
		if (!nm_setting_ip_config_add_route (s_ip4, route))
			PARSE_WARNING ("duplicate IP4 route");
		nm_ip_route_unref (route);
	}

	success = TRUE;

error:
	g_free (contents);
	g_strfreev (lines);
	g_regex_unref (regex_to1);
	g_regex_unref (regex_to2);
	g_regex_unref (regex_via);
	g_regex_unref (regex_metric);

	return success;
}

static void
parse_dns_options (NMSettingIPConfig *ip_config, const char *value)
{
	char **options = NULL;

	g_return_if_fail (ip_config);

	if (!value)
		return;

	if (!nm_setting_ip_config_has_dns_options (ip_config))
		nm_setting_ip_config_clear_dns_options (ip_config, TRUE);

	options = g_strsplit (value, " ", 0);
	if (options) {
		char **item;
		for (item = options; *item; item++) {
			if (strlen (*item)) {
				if (!nm_setting_ip_config_add_dns_option (ip_config, *item))
					PARSE_WARNING ("can't add DNS option '%s'", *item);
			}
		}
		g_strfreev (options);
	}
}

static gboolean
parse_full_ip6_address (shvarFile *ifcfg,
                        const char *addr_str,
                        int i,
                        NMIPAddress **out_address,
                        GError **error)
{
	char **list;
	char *ip_val, *prefix_val;
	int prefix;
	gboolean success = FALSE;

	g_return_val_if_fail (addr_str != NULL, FALSE);
	g_return_val_if_fail (out_address != NULL, FALSE);
	g_return_val_if_fail (*out_address == NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* Split the address and prefix */
	list = g_strsplit_set (addr_str, "/", 2);
	if (g_strv_length (list) < 1) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Invalid IP6 address '%s'", addr_str);
		goto error;
	}

	ip_val = list[0];

	prefix_val = list[1];
	if (prefix_val) {
		prefix = _nm_utils_ascii_str_to_int64 (prefix_val, 10, 0, 128, -1);
		if (prefix < 0) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid IP6 prefix '%s'", prefix_val);
			goto error;
		}
	} else {
		/* Missing prefix is treated as prefix of 64 */
		prefix = 64;
	}

	*out_address = nm_ip_address_new (AF_INET6, ip_val, prefix, error);
	if (*out_address)
		success = TRUE;

error:
	g_strfreev (list);
	return success;
}

/* IPv6 address is very complex to describe completely by a regular expression,
 * so don't try to, rather use looser syntax to comprise all possibilities
 * NOTE: The regexes below don't describe all variants allowed by 'ip route add',
 * namely destination IP without 'to' keyword is recognized just at line start.
 */
#define IPV6_ADDR_REGEX "[0-9A-Fa-f:.]+"

static gboolean
read_route6_file (const char *filename, NMSettingIPConfig *s_ip6, GError **error)
{
	char *contents = NULL;
	gsize len = 0;
	char **lines = NULL, **iter;
	GRegex *regex_to1, *regex_to2, *regex_via, *regex_metric;
	GMatchInfo *match_info;
	char *dest = NULL, *prefix = NULL, *next_hop = NULL, *metric = NULL;
	int prefix_int;
	gint64 metric_int;
	gboolean success = FALSE;

	const char *pattern_empty = "^\\s*(\\#.*)?$";
	const char *pattern_to1 = "^\\s*(default|" IPV6_ADDR_REGEX ")"  /* IPv6 or 'default' keyword */
	                          "(?:/(\\d{1,3}))?";                   /* optional prefix */
	const char *pattern_to2 = "to\\s+(default|" IPV6_ADDR_REGEX ")" /* IPv6 or 'default' keyword */
	                          "(?:/(\\d{1,3}))?";                   /* optional prefix */
	const char *pattern_via = "via\\s+(" IPV6_ADDR_REGEX ")";       /* IPv6 of gateway */
	const char *pattern_metric = "metric\\s+(\\d+)";                /* metric */

	g_return_val_if_fail (filename != NULL, FALSE);
	g_return_val_if_fail (s_ip6 != NULL, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* Read the route file */
	if (!g_file_get_contents (filename, &contents, &len, NULL) || !len) {
		g_free (contents);
		return TRUE;  /* missing/empty = success */
	}

	/* Create regexes for pieces to be matched */
	regex_to1 = g_regex_new (pattern_to1, 0, 0, NULL);
	regex_to2 = g_regex_new (pattern_to2, 0, 0, NULL);
	regex_via = g_regex_new (pattern_via, 0, 0, NULL);
	regex_metric = g_regex_new (pattern_metric, 0, 0, NULL);

	/* Iterate through file lines */
	lines = g_strsplit_set (contents, "\n\r", -1);
	for (iter = lines; iter && *iter; iter++) {
		NMIPRoute *route;

		/* Skip empty lines */
		if (g_regex_match_simple (pattern_empty, *iter, 0, 0))
			continue;

		/* Destination */
		g_regex_match (regex_to1, *iter, 0, &match_info);
		if (!g_match_info_matches (match_info)) {
			g_match_info_free (match_info);
			g_regex_match (regex_to2, *iter, 0, &match_info);
			if (!g_match_info_matches (match_info)) {
				g_match_info_free (match_info);
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Missing IP6 route destination address in record: '%s'", *iter);
				goto error;
			}
		}
		dest = g_match_info_fetch (match_info, 1);
		if (!g_strcmp0 (dest, "default")) {
			/* Ignore default route - NM handles it internally */
			g_clear_pointer (&dest, g_free);
			g_match_info_free (match_info);
			PARSE_WARNING ("ignoring manual default route: '%s' (%s)", *iter, filename);
			continue;
		}

		/* Prefix - is optional; 128 if missing */
		prefix = g_match_info_fetch (match_info, 2);
		g_match_info_free (match_info);
		prefix_int = 128;
		if (prefix) {
			prefix_int = _nm_utils_ascii_str_to_int64 (prefix, 10, 1, 128, -1);
			if (prefix_int == -1) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP6 route destination prefix '%s'", prefix);
				g_free (dest);
				g_free (prefix);
				goto error;
			}
		}
		g_free (prefix);

		/* Next hop */
		g_regex_match (regex_via, *iter, 0, &match_info);
		if (g_match_info_matches (match_info)) {
			next_hop = g_match_info_fetch (match_info, 1);
			if (!nm_utils_ipaddr_valid (AF_INET6, next_hop)) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IPv6 route nexthop address '%s'",
				             next_hop);
				g_match_info_free (match_info);
				g_free (dest);
				g_free (next_hop);
				goto error;
			}
		} else {
			/* Missing "via" is taken as :: */
			next_hop = NULL;
		}
		g_match_info_free (match_info);

		/* Metric */
		g_regex_match (regex_metric, *iter, 0, &match_info);
		metric_int = -1;
		if (g_match_info_matches (match_info)) {
			metric = g_match_info_fetch (match_info, 1);
			metric_int = _nm_utils_ascii_str_to_int64 (metric, 10, 0, G_MAXUINT32, -1);
			if (metric_int == -1) {
				g_match_info_free (match_info);
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP6 route metric '%s'", metric);
				g_free (dest);
				g_free (next_hop);
				g_free (metric);
				goto error;
			}
			g_free (metric);
		}
		g_match_info_free (match_info);

		route = nm_ip_route_new (AF_INET6, dest, prefix_int, next_hop, metric_int, error);
		g_free (dest);
		g_free (next_hop);
		if (!route)
			goto error;
		if (!nm_setting_ip_config_add_route (s_ip6, route))
			PARSE_WARNING ("duplicate IP6 route");
		nm_ip_route_unref (route);
	}

	success = TRUE;

error:
	g_free (contents);
	g_strfreev (lines);
	g_regex_unref (regex_to1);
	g_regex_unref (regex_to2);
	g_regex_unref (regex_via);
	g_regex_unref (regex_metric);

	return success;
}

static NMSetting *
make_proxy_setting (shvarFile *ifcfg, GError **error)
{
	NMSettingProxy *s_proxy = NULL;
	char *value = NULL;
	NMSettingProxyMethod method;

	value = svGetValueString (ifcfg, "PROXY_METHOD");
	if (!value)
		return NULL;

	if (!g_ascii_strcasecmp (value, "auto"))
		method = NM_SETTING_PROXY_METHOD_AUTO;
	else
		method = NM_SETTING_PROXY_METHOD_NONE;
	g_free (value);

	s_proxy = (NMSettingProxy *) nm_setting_proxy_new ();

	switch (method) {
	case NM_SETTING_PROXY_METHOD_AUTO:
		g_object_set (s_proxy,
		              NM_SETTING_PROXY_METHOD, (int) NM_SETTING_PROXY_METHOD_AUTO,
		              NULL);

		value = svGetValueString (ifcfg, "PAC_URL");
		if (value) {
			value = g_strstrip (value);
			g_object_set (s_proxy, NM_SETTING_PROXY_PAC_URL, value, NULL);
			g_free (value);
		}

		value = svGetValueString (ifcfg, "PAC_SCRIPT");
		if (value) {
			value = g_strstrip (value);
			g_object_set (s_proxy, NM_SETTING_PROXY_PAC_SCRIPT, value, NULL);
			g_free (value);
		}

		break;
	case NM_SETTING_PROXY_METHOD_NONE:
		g_object_set (s_proxy,
		              NM_SETTING_PROXY_METHOD, (int) NM_SETTING_PROXY_METHOD_NONE,
		              NULL);
		break;
	}

	value = svGetValueString (ifcfg, "BROWSER_ONLY");
	if (value) {
		if (!g_ascii_strcasecmp (value, "yes"))
			g_object_set (s_proxy, NM_SETTING_PROXY_BROWSER_ONLY, TRUE, NULL);
		g_free (value);
	}

	return NM_SETTING (s_proxy);
}

static NMSetting *
make_ip4_setting (shvarFile *ifcfg,
                  const char *network_file,
                  GError **error)
{
	NMSettingIPConfig *s_ip4 = NULL;
	char *value = NULL;
	char *route_path = NULL;
	char *method;
	gs_free char *dns_options_free = NULL;
	const char *dns_options = NULL;
	gs_free char *gateway = NULL;
	gint32 i;
	shvarFile *network_ifcfg;
	shvarFile *route_ifcfg;
	gboolean never_default = FALSE;
	gint64 timeout;
	gint priority;

	s_ip4 = (NMSettingIPConfig *) nm_setting_ip4_config_new ();

	/* First check if DEFROUTE is set for this device; DEFROUTE has the
	 * opposite meaning from never-default. The default if DEFROUTE is not
	 * specified is DEFROUTE=yes which means that this connection can be used
	 * as a default route
	 */
	never_default = !svGetValueBoolean (ifcfg, "DEFROUTE", TRUE);

	/* Then check if GATEWAYDEV; it's global and overrides DEFROUTE */
	network_ifcfg = svOpenFile (network_file, NULL);
	if (network_ifcfg) {
		char *gatewaydev;

		/* Get the connection ifcfg device name and the global gateway device */
		value = svGetValueString (ifcfg, "DEVICE");
		gatewaydev = svGetValueString (network_ifcfg, "GATEWAYDEV");
		dns_options = svGetValue (network_ifcfg, "RES_OPTIONS", &dns_options_free);

		/* If there was a global gateway device specified, then only connections
		 * for that device can be the default connection.
		 */
		if (gatewaydev && value)
			never_default = !!strcmp (value, gatewaydev);

		g_free (gatewaydev);
		g_free (value);
		svCloseFile (network_ifcfg);
	}

	value = svGetValueString (ifcfg, "BOOTPROTO");

	if (!value || !*value || !g_ascii_strcasecmp (value, "none")) {
		if (is_any_ip4_address_defined (ifcfg, NULL))
			method = NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
		else
			method = NM_SETTING_IP4_CONFIG_METHOD_DISABLED;
	} else if (!g_ascii_strcasecmp (value, "bootp") || !g_ascii_strcasecmp (value, "dhcp")) {
		method = NM_SETTING_IP4_CONFIG_METHOD_AUTO;
	} else if (!g_ascii_strcasecmp (value, "static")) {
		if (is_any_ip4_address_defined (ifcfg, NULL))
			method = NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
		else
			method = NM_SETTING_IP4_CONFIG_METHOD_DISABLED;
	} else if (!g_ascii_strcasecmp (value, "autoip")) {
		method = NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL;
	} else if (!g_ascii_strcasecmp (value, "shared")) {
		int idx;

		g_free (value);
		g_object_set (s_ip4,
		              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_SHARED,
		              NM_SETTING_IP_CONFIG_NEVER_DEFAULT, never_default,
		              NULL);
		/* 1 IP address is allowed for shared connections. Read it. */
		if (is_any_ip4_address_defined (ifcfg, &idx)) {
			NMIPAddress *addr = NULL;

			if (!read_full_ip4_address (ifcfg, idx, NULL, &addr, NULL, error))
				goto done;
			if (!read_ip4_address (ifcfg, "GATEWAY", &gateway, error))
				goto done;
			(void) nm_setting_ip_config_add_address (s_ip4, addr);
			nm_ip_address_unref (addr);
			if (never_default)
				PARSE_WARNING ("GATEWAY will be ignored when DEFROUTE is disabled");
			g_object_set (s_ip4, NM_SETTING_IP_CONFIG_GATEWAY, gateway, NULL);
		}
		return NM_SETTING (s_ip4);
	} else {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Unknown BOOTPROTO '%s'", value);
		g_free (value);
		goto done;
	}
	g_free (value);

	g_object_set (s_ip4,
	              NM_SETTING_IP_CONFIG_METHOD, method,
	              NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS, !svGetValueBoolean (ifcfg, "PEERDNS", TRUE),
	              NM_SETTING_IP_CONFIG_IGNORE_AUTO_ROUTES, !svGetValueBoolean (ifcfg, "PEERROUTES", TRUE),
	              NM_SETTING_IP_CONFIG_NEVER_DEFAULT, never_default,
	              NM_SETTING_IP_CONFIG_MAY_FAIL, !svGetValueBoolean (ifcfg, "IPV4_FAILURE_FATAL", FALSE),
	              NM_SETTING_IP_CONFIG_ROUTE_METRIC, svGetValueInt64 (ifcfg, "IPV4_ROUTE_METRIC", 10,
	                                                                  -1, G_MAXUINT32, -1),
	              NULL);

	if (strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_DISABLED) == 0)
		return NM_SETTING (s_ip4);

	/* Handle DHCP settings */
	value = svGetValueString (ifcfg, "DHCP_HOSTNAME");
	if (value && *value)
		g_object_set (s_ip4, NM_SETTING_IP_CONFIG_DHCP_HOSTNAME, value, NULL);
	g_free (value);

	value = svGetValueString (ifcfg, "DHCP_FQDN");
	if (value && *value) {
		g_object_set (s_ip4,
			      NM_SETTING_IP_CONFIG_DHCP_HOSTNAME, NULL,
			      NM_SETTING_IP4_CONFIG_DHCP_FQDN, value,
			      NULL);
	}
	g_free (value);

	g_object_set (s_ip4,
		      NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME, svGetValueBoolean (ifcfg, "DHCP_SEND_HOSTNAME", TRUE),
		      NM_SETTING_IP_CONFIG_DHCP_TIMEOUT, svGetValueInt64 (ifcfg, "IPV4_DHCP_TIMEOUT", 10, 0, G_MAXINT32, 0),
		      NULL);

	value = svGetValueString (ifcfg, "DHCP_CLIENT_ID");
	if (value && strlen (value))
		g_object_set (s_ip4, NM_SETTING_IP4_CONFIG_DHCP_CLIENT_ID, value, NULL);
	g_free (value);

	/* Read static IP addresses.
	 * Read them even for AUTO method - in this case the addresses are
	 * added to the automatic ones. Note that this is not currently supported by
	 * the legacy 'network' service (ifup-eth).
	 */
	for (i = -1; i < 256; i++) {
		NMIPAddress *addr = NULL;

		/* gateway will only be set if still unset. Hence, we don't leak gateway
		 * here by calling read_full_ip4_address() repeatedly */
		if (!read_full_ip4_address (ifcfg, i, NULL, &addr, &gateway, error))
			goto done;

		if (!addr) {
			/* The first mandatory variable is 2-indexed (IPADDR2)
			 * Variables IPADDR, IPADDR0 and IPADDR1 are optional */
			if (i > 1)
				break;
			continue;
		}

		if (!nm_setting_ip_config_add_address (s_ip4, addr))
			PARSE_WARNING ("duplicate IP4 address");
		nm_ip_address_unref (addr);
	}

	/* Gateway */
	if (!gateway) {
		network_ifcfg = svOpenFile (network_file, NULL);
		if (network_ifcfg) {
			gboolean read_success;

			read_success = read_ip4_address (network_ifcfg, "GATEWAY", &gateway, error);
			svCloseFile (network_ifcfg);
			if (!read_success)
				goto done;

			if (gateway && nm_setting_ip_config_get_num_addresses (s_ip4) == 0) {
				gs_free char *f = g_path_get_basename (svFileGetName (ifcfg));
				PARSE_WARNING ("ignoring GATEWAY (/etc/sysconfig/network) for %s "
				               "because the connection has no static addresses", f);
				g_clear_pointer (&gateway, g_free);
			}
		}
	}
	g_object_set (s_ip4, NM_SETTING_IP_CONFIG_GATEWAY, gateway, NULL);

	if (gateway && never_default)
		PARSE_WARNING ("GATEWAY will be ignored when DEFROUTE is disabled");

	/* DNS servers
	 * Pick up just IPv4 addresses (IPv6 addresses are taken by make_ip6_setting())
	 */
	for (i = 1; i <= 10; i++) {
		char *tag;

		tag = g_strdup_printf ("DNS%u", i);
		value = svGetValueString (ifcfg, tag);
		if (value) {
			if (nm_utils_ipaddr_valid (AF_INET, value)) {
				if (!nm_setting_ip_config_add_dns (s_ip4, value))
					PARSE_WARNING ("duplicate DNS server %s", tag);
			} else if (nm_utils_ipaddr_valid (AF_INET6, value)) {
				/* Ignore IPv6 addresses */
			} else {
				PARSE_WARNING ("invalid DNS server address %s", value);
				g_free (tag);
				g_free (value);
				goto done;
			}

			g_free (value);
		}

		g_free (tag);
	}

	/* DNS searches */
	value = svGetValueString (ifcfg, "DOMAIN");
	if (value) {
		char **searches = NULL;

		searches = g_strsplit (value, " ", 0);
		if (searches) {
			char **item;
			for (item = searches; *item; item++) {
				if (strlen (*item)) {
					if (!nm_setting_ip_config_add_dns_search (s_ip4, *item))
						PARSE_WARNING ("duplicate DNS domain '%s'", *item);
				}
			}
			g_strfreev (searches);
		}
		g_free (value);
	}

	/* DNS options */
	parse_dns_options (s_ip4, svGetValue (ifcfg, "RES_OPTIONS", &value));
	parse_dns_options (s_ip4, dns_options);
	g_free (value);

	/* DNS priority */
	priority = svGetValueInt64 (ifcfg, "IPV4_DNS_PRIORITY", 10, G_MININT32, G_MAXINT32, 0);
	g_object_set (s_ip4,
	              NM_SETTING_IP_CONFIG_DNS_PRIORITY,
	              priority,
	              NULL);

	/* Static routes  - route-<name> file */
	route_path = utils_get_route_path (svFileGetName (ifcfg));

	if (utils_has_complex_routes (route_path)) {
		PARSE_WARNING ("'rule-' or 'rule6-' file is present; you will need to use a dispatcher script to apply these routes");
	} else if (utils_has_route_file_new_syntax (route_path)) {
		/* Parse route file in new syntax */
		route_ifcfg = utils_get_route_ifcfg (svFileGetName (ifcfg), FALSE);
		if (route_ifcfg) {
			for (i = 0; i < 256; i++) {
				NMIPRoute *route = NULL;

				if (!read_one_ip4_route (route_ifcfg, i, &route, error)) {
					svCloseFile (route_ifcfg);
					goto done;
				}

				if (!route)
					break;

				if (!nm_setting_ip_config_add_route (s_ip4, route))
					PARSE_WARNING ("duplicate IP4 route");
				nm_ip_route_unref (route);
			}
			svCloseFile (route_ifcfg);
		}
	} else {
		if (!read_route_file_legacy (route_path, s_ip4, error))
			goto done;
	}
	g_free (route_path);

	/* Legacy value NM used for a while but is incorrect (rh #459370) */
	if (!nm_setting_ip_config_get_num_dns_searches (s_ip4)) {
		value = svGetValueString (ifcfg, "SEARCH");
		if (value) {
			char **searches = NULL;

			searches = g_strsplit (value, " ", 0);
			if (searches) {
				char **item;
				for (item = searches; *item; item++) {
					if (strlen (*item)) {
						if (!nm_setting_ip_config_add_dns_search (s_ip4, *item))
							PARSE_WARNING ("duplicate DNS search '%s'", *item);
					}
				}
				g_strfreev (searches);
			}
			g_free (value);
		}
	}

	timeout = svGetValueInt64 (ifcfg, "ARPING_WAIT", 10, -1,
	                           NM_SETTING_IP_CONFIG_DAD_TIMEOUT_MAX / 1000, -1);
	g_object_set (s_ip4, NM_SETTING_IP_CONFIG_DAD_TIMEOUT,
	              (gint) (timeout <= 0 ? timeout : timeout * 1000), NULL);

	return NM_SETTING (s_ip4);

done:
	g_free (route_path);
	g_object_unref (s_ip4);
	return NULL;
}

static void
read_aliases (NMSettingIPConfig *s_ip4, const char *filename)
{
	GDir *dir;
	char *dirname, *base;
	shvarFile *parsed;
	NMIPAddress *base_addr = NULL;
	GError *err = NULL;

	g_return_if_fail (s_ip4 != NULL);
	g_return_if_fail (filename != NULL);

	if (nm_setting_ip_config_get_num_addresses (s_ip4) > 0)
		base_addr = nm_setting_ip_config_get_address (s_ip4, 0);

	dirname = g_path_get_dirname (filename);
	g_return_if_fail (dirname != NULL);
	base = g_path_get_basename (filename);
	g_return_if_fail (base != NULL);

	dir = g_dir_open (dirname, 0, &err);
	if (dir) {
		const char *item;
		NMIPAddress *addr;
		gboolean ok;

		while ((item = g_dir_read_name (dir))) {
			char *full_path, *device;
			const char *p;

			if (!utils_is_ifcfg_alias_file (item, base))
				continue;

			full_path = g_build_filename (dirname, item, NULL);

			p = strchr (item, ':');
			g_assert (p != NULL); /* we know this is true from utils_is_ifcfg_alias_file() */
			for (p++; *p; p++) {
				if (!g_ascii_isalnum (*p) && *p != '_')
					break;
			}
			if (*p) {
				PARSE_WARNING ("ignoring alias file '%s' with invalid name", full_path);
				g_free (full_path);
				continue;
			}

			parsed = svOpenFile (full_path, &err);
			if (!parsed) {
				PARSE_WARNING ("couldn't parse alias file '%s': %s", full_path, err->message);
				g_free (full_path);
				g_clear_error (&err);
				continue;
			}

			device = svGetValueString (parsed, "DEVICE");
			if (!device) {
				PARSE_WARNING ("alias file '%s' has no DEVICE", full_path);
				svCloseFile (parsed);
				g_free (full_path);
				continue;
			}
			/* We know that item starts with IFCFG_TAG from utils_is_ifcfg_alias_file() */
			if (strcmp (device, item + strlen (IFCFG_TAG)) != 0) {
				PARSE_WARNING ("alias file '%s' has invalid DEVICE (%s) for filename",
				               full_path, device);
				g_free (device);
				svCloseFile (parsed);
				g_free (full_path);
				continue;
			}

			addr = NULL;
			ok = read_full_ip4_address (parsed, -1, base_addr, &addr, NULL, &err);
			svCloseFile (parsed);
			if (ok) {
				nm_ip_address_set_attribute (addr, "label", g_variant_new_string (device));
				if (!nm_setting_ip_config_add_address (s_ip4, addr))
					PARSE_WARNING ("duplicate IP4 address in alias file %s", item);
			} else {
				PARSE_WARNING ("error reading IP4 address from alias file '%s': %s",
				               full_path, err ? err->message : "no address");
				g_clear_error (&err);
			}
			nm_ip_address_unref (addr);

			g_free (device);
			g_free (full_path);
		}

		g_dir_close (dir);
	} else {
		PARSE_WARNING ("can not read directory '%s': %s", dirname, err->message);
		g_error_free (err);
	}

	g_free (base);
	g_free (dirname);
}

static NMSetting *
make_ip6_setting (shvarFile *ifcfg,
                  const char *network_file,
                  GError **error)
{
	NMSettingIPConfig *s_ip6 = NULL;
	char *value = NULL;
	char *str_value;
	char *route6_path = NULL;
	gs_free char *dns_options_free = NULL;
	const char *dns_options = NULL;
	gboolean ipv6init, ipv6forwarding, dhcp6 = FALSE;
	char *method = NM_SETTING_IP6_CONFIG_METHOD_MANUAL;
	char *ipv6addr, *ipv6addr_secondaries;
	char **list = NULL, **iter;
	guint32 i;
	gint priority;
	shvarFile *network_ifcfg;
	gboolean never_default = FALSE;
	gboolean ip6_privacy = FALSE, ip6_privacy_prefer_public_ip;
	NMSettingIP6ConfigPrivacy ip6_privacy_val;
	NMSettingIP6ConfigAddrGenMode addr_gen_mode;

	s_ip6 = (NMSettingIPConfig *) nm_setting_ip6_config_new ();

	/* First check if IPV6_DEFROUTE is set for this device; IPV6_DEFROUTE has the
	 * opposite meaning from never-default. The default if IPV6_DEFROUTE is not
	 * specified is IPV6_DEFROUTE=yes which means that this connection can be used
	 * as a default route
	 */
	never_default = !svGetValueBoolean (ifcfg, "IPV6_DEFROUTE", TRUE);

	/* Then check if IPV6_DEFAULTGW or IPV6_DEFAULTDEV is specified;
	 * they are global and override IPV6_DEFROUTE
	 * When both are set, the device specified in IPV6_DEFAULTGW takes preference.
	 */
	network_ifcfg = svOpenFile (network_file, NULL);
	if (network_ifcfg) {
		char *ipv6_defaultgw, *ipv6_defaultdev;
		char *default_dev = NULL;

		/* Get the connection ifcfg device name and the global default route device */
		value = svGetValueString (ifcfg, "DEVICE");
		ipv6_defaultgw = svGetValueString (network_ifcfg, "IPV6_DEFAULTGW");
		ipv6_defaultdev = svGetValueString (network_ifcfg, "IPV6_DEFAULTDEV");
		dns_options = svGetValue (network_ifcfg, "RES_OPTIONS", &dns_options_free);

		if (ipv6_defaultgw) {
			default_dev = strchr (ipv6_defaultgw, '%');
			if (default_dev)
				default_dev++;
		}
		if (!default_dev)
			default_dev = ipv6_defaultdev;

		/* If there was a global default route device specified, then only connections
		 * for that device can be the default connection.
		 */
		if (default_dev && value)
			never_default = !!strcmp (value, default_dev);

		g_free (ipv6_defaultgw);
		g_free (ipv6_defaultdev);
		g_free (value);
		svCloseFile (network_ifcfg);
	}

	/* Find out method property */
	/* Is IPV6 enabled? Set method to "ignored", when not enabled */
	str_value = svGetValueString (ifcfg, "IPV6INIT");
	ipv6init = svGetValueBoolean (ifcfg, "IPV6INIT", FALSE);
	if (!str_value) {
		network_ifcfg = svOpenFile (network_file, NULL);
		if (network_ifcfg) {
			ipv6init = svGetValueBoolean (network_ifcfg, "IPV6INIT", FALSE);
			svCloseFile (network_ifcfg);
		}
	}
	g_free (str_value);

	if (!ipv6init)
		method = NM_SETTING_IP6_CONFIG_METHOD_IGNORE;  /* IPv6 is disabled */
	else {
		ipv6forwarding = svGetValueBoolean (ifcfg, "IPV6FORWARDING", FALSE);
		str_value = svGetValueString (ifcfg, "IPV6_AUTOCONF");
		dhcp6 = svGetValueBoolean (ifcfg, "DHCPV6C", FALSE);

		if (!g_strcmp0 (str_value, "shared"))
			method = NM_SETTING_IP6_CONFIG_METHOD_SHARED;
		else if (svParseBoolean (str_value, !ipv6forwarding))
			method = NM_SETTING_IP6_CONFIG_METHOD_AUTO;
		else if (dhcp6)
			method = NM_SETTING_IP6_CONFIG_METHOD_DHCP;
		else {
			/* IPV6_AUTOCONF=no and no IPv6 address -> method 'link-local' */
			g_free (str_value);
			str_value = svGetValueString (ifcfg, "IPV6ADDR");
			if (!str_value)
				str_value = svGetValueString (ifcfg, "IPV6ADDR_SECONDARIES");

			if (!str_value)
				method = NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL;
		}
		g_free (str_value);
	}
	/* TODO - handle other methods */

	/* Read IPv6 Privacy Extensions configuration */
	str_value = svGetValueString (ifcfg, "IPV6_PRIVACY");
	if (str_value) {
		ip6_privacy = svParseBoolean (str_value, FALSE);
		if (!ip6_privacy)
			ip6_privacy = (g_strcmp0 (str_value, "rfc4941") == 0) ||
			              (g_strcmp0 (str_value, "rfc3041") == 0);
	}
	ip6_privacy_prefer_public_ip = svGetValueBoolean (ifcfg, "IPV6_PRIVACY_PREFER_PUBLIC_IP", FALSE);
	ip6_privacy_val = str_value ?
	                      (ip6_privacy ?
	                          (ip6_privacy_prefer_public_ip ? NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_PUBLIC_ADDR : NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_TEMP_ADDR) :
	                          NM_SETTING_IP6_CONFIG_PRIVACY_DISABLED) :
	                      NM_SETTING_IP6_CONFIG_PRIVACY_UNKNOWN;
	g_free (str_value);

	g_object_set (s_ip6,
	              NM_SETTING_IP_CONFIG_METHOD, method,
	              NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS, !svGetValueBoolean (ifcfg, "IPV6_PEERDNS", TRUE),
	              NM_SETTING_IP_CONFIG_IGNORE_AUTO_ROUTES, !svGetValueBoolean (ifcfg, "IPV6_PEERROUTES", TRUE),
	              NM_SETTING_IP_CONFIG_NEVER_DEFAULT, never_default,
	              NM_SETTING_IP_CONFIG_MAY_FAIL, !svGetValueBoolean (ifcfg, "IPV6_FAILURE_FATAL", FALSE),
	              NM_SETTING_IP_CONFIG_ROUTE_METRIC, svGetValueInt64 (ifcfg, "IPV6_ROUTE_METRIC", 10,
	                                                                  -1, G_MAXUINT32, -1),
	              NM_SETTING_IP6_CONFIG_IP6_PRIVACY, ip6_privacy_val,
	              NULL);

	/* Don't bother to read IP, DNS and routes when IPv6 is disabled */
	if (strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_IGNORE) == 0)
		return NM_SETTING (s_ip6);

	value = svGetValueString (ifcfg, "DHCPV6_HOSTNAME");
	/* Use DHCP_HOSTNAME as fallback if it is in FQDN format and ipv6.method is
	 * auto or dhcp: this is required to support old ifcfg files
	 */
	if (!value && (   !strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_AUTO)
		       || !strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_DHCP))) {
		value = svGetValueString (ifcfg, "DHCP_HOSTNAME");
		if (value && !strchr (value, '.'))
			g_clear_pointer (&value, g_free);
	}
	if (value)
		g_object_set (s_ip6, NM_SETTING_IP_CONFIG_DHCP_HOSTNAME, value, NULL);
	g_free (value);

	g_object_set (s_ip6, NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME,
		      svGetValueBoolean (ifcfg, "DHCPV6_SEND_HOSTNAME", TRUE), NULL);

	/* Read static IP addresses.
	 * Read them even for AUTO and DHCP methods - in this case the addresses are
	 * added to the automatic ones. Note that this is not currently supported by
	 * the legacy 'network' service (ifup-eth).
	 */
	ipv6addr = svGetValueString (ifcfg, "IPV6ADDR");
	ipv6addr_secondaries = svGetValueString (ifcfg, "IPV6ADDR_SECONDARIES");

	value = g_strjoin (ipv6addr && ipv6addr_secondaries ? " " : NULL,
	                   ipv6addr ? ipv6addr : "",
	                   ipv6addr_secondaries ? ipv6addr_secondaries : "",
	                   NULL);
	g_free (ipv6addr);
	g_free (ipv6addr_secondaries);

	list = g_strsplit_set (value, " ", 0);
	g_free (value);
	for (iter = list, i = 0; iter && *iter; iter++, i++) {
		NMIPAddress *addr = NULL;

		if (!parse_full_ip6_address (ifcfg, *iter, i, &addr, error)) {
			g_strfreev (list);
			goto error;
		}

		if (!nm_setting_ip_config_add_address (s_ip6, addr))
			PARSE_WARNING ("duplicate IP6 address");
		nm_ip_address_unref (addr);
	}
	g_strfreev (list);

	/* Gateway */
	if (nm_setting_ip_config_get_num_addresses (s_ip6)) {
		value = svGetValueString (ifcfg, "IPV6_DEFAULTGW");
		if (!value) {
			/* If no gateway in the ifcfg, try global /etc/sysconfig/network instead */
			network_ifcfg = svOpenFile (network_file, NULL);
			if (network_ifcfg) {
				value = svGetValueString (network_ifcfg, "IPV6_DEFAULTGW");
				svCloseFile (network_ifcfg);
			}
		}
		if (value) {
			char *ptr;
			if ((ptr = strchr (value, '%')) != NULL)
				*ptr = '\0';  /* remove %interface prefix if present */
			if (!nm_utils_ipaddr_valid (AF_INET6, value)) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Invalid IP6 address '%s'", value);
				g_free (value);
				goto error;
			}

			g_object_set (s_ip6, NM_SETTING_IP_CONFIG_GATEWAY, value, NULL);
			g_free (value);
		}
	}

	/* IPv6 addressing mode configuration */
	str_value = svGetValueString (ifcfg, "IPV6_ADDR_GEN_MODE");
	if (str_value) {
		if (nm_utils_enum_from_str (nm_setting_ip6_config_addr_gen_mode_get_type (), str_value,
		                            (int *) &addr_gen_mode, NULL))
			g_object_set (s_ip6, NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE, addr_gen_mode, NULL);
		else
			PARSE_WARNING ("Invalid IPV6_ADDR_GEN_MODE");
		g_free (str_value);
	} else {
		g_object_set (s_ip6,
		              NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE,
		              NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64,
		              NULL);
	}

	/* IPv6 tokenized interface identifier */
	str_value = svGetValueString (ifcfg, "IPV6_TOKEN");
	if (str_value) {
		g_object_set (s_ip6, NM_SETTING_IP6_CONFIG_TOKEN, str_value, NULL);
		g_free (str_value);
	}

	/* DNS servers
	 * Pick up just IPv6 addresses (IPv4 addresses are taken by make_ip4_setting())
	 */
	for (i = 1; i <= 10; i++) {
		char *tag;

		tag = g_strdup_printf ("DNS%u", i);
		value = svGetValueString (ifcfg, tag);
		if (!value) {
			g_free (tag);
			break; /* all done */
		}

		if (nm_utils_ipaddr_valid (AF_INET6, value)) {
			if (!nm_setting_ip_config_add_dns (s_ip6, value))
				PARSE_WARNING ("duplicate DNS server %s", tag);
		} else if (nm_utils_ipaddr_valid (AF_INET, value)) {
			/* Ignore IPv4 addresses */
		} else {
			PARSE_WARNING ("invalid DNS server address %s", value);
			g_free (tag);
			g_free (value);
			goto error;
		}

		g_free (tag);
		g_free (value);
	}

	/* DNS searches ('DOMAIN' key) are read by make_ip4_setting() and included in NMSettingIPConfig */

	if (!utils_has_complex_routes (svFileGetName (ifcfg))) {
		/* Read static routes from route6-<interface> file */
		route6_path = utils_get_route6_path (svFileGetName (ifcfg));
		if (!read_route6_file (route6_path, s_ip6, error))
			goto error;

		g_free (route6_path);
	}

	/* DNS options */
	parse_dns_options (s_ip6, svGetValue (ifcfg, "RES_OPTIONS", &value));
	parse_dns_options (s_ip6, dns_options);
	g_free (value);

	/* DNS priority */
	priority = svGetValueInt64 (ifcfg, "IPV6_DNS_PRIORITY", 10, G_MININT32, G_MAXINT32, 0);
	g_object_set (s_ip6,
	              NM_SETTING_IP_CONFIG_DNS_PRIORITY,
	              priority,
	              NULL);

	return NM_SETTING (s_ip6);

error:
	g_free (route6_path);
	g_object_unref (s_ip6);
	return NULL;
}

static void
check_if_bond_slave (shvarFile *ifcfg,
                     NMSettingConnection *s_con)
{
	char *value;

	value = svGetValueString (ifcfg, "MASTER_UUID");
	if (!value)
		value = svGetValueString (ifcfg, "MASTER");

	if (value) {
		g_object_set (s_con, NM_SETTING_CONNECTION_MASTER, value, NULL);
		g_object_set (s_con,
		              NM_SETTING_CONNECTION_SLAVE_TYPE, NM_SETTING_BOND_SETTING_NAME,
		              NULL);
		g_free (value);
	}

	/* We should be checking for SLAVE=yes as well, but NM used to not set that,
	 * so for backward-compatibility, we don't check.
	 */
}

static gboolean
check_if_team_slave (shvarFile *ifcfg,
                     NMSettingConnection *s_con)
{
	gs_free char *value = NULL;

	value = svGetValueString (ifcfg, "TEAM_MASTER_UUID");
	if (!value)
		value = svGetValueString (ifcfg, "TEAM_MASTER");
	if (!value)
		return FALSE;

	g_object_set (s_con, NM_SETTING_CONNECTION_MASTER, value, NULL);
	g_object_set (s_con, NM_SETTING_CONNECTION_SLAVE_TYPE, NM_SETTING_TEAM_SETTING_NAME, NULL);
	return TRUE;
}

static void
check_if_slave (shvarFile *ifcfg,
                NMSettingConnection *s_con)
{
	g_return_if_fail (NM_IS_SETTING_CONNECTION (s_con));

	if (check_if_team_slave (ifcfg, s_con))
		return;
	check_if_bond_slave (ifcfg, s_con);
}

typedef struct {
	const char *enable_key;
	const char *advertise_key;
	const char *willing_key;
	const char *flags_prop;
} DcbFlagsProperty;

enum {
	DCB_APP_FCOE_FLAGS = 0,
	DCB_APP_ISCSI_FLAGS = 1,
	DCB_APP_FIP_FLAGS = 2,
	DCB_PFC_FLAGS = 3,
	DCB_PG_FLAGS = 4,
};

static DcbFlagsProperty dcb_flags_props[] = {
	{ KEY_DCB_APP_FCOE_ENABLE,  KEY_DCB_APP_FCOE_ADVERTISE,  KEY_DCB_APP_FCOE_WILLING,  NM_SETTING_DCB_APP_FCOE_FLAGS },
	{ KEY_DCB_APP_ISCSI_ENABLE, KEY_DCB_APP_ISCSI_ADVERTISE, KEY_DCB_APP_ISCSI_WILLING, NM_SETTING_DCB_APP_ISCSI_FLAGS },
	{ KEY_DCB_APP_FIP_ENABLE,   KEY_DCB_APP_FIP_ADVERTISE,   KEY_DCB_APP_FIP_WILLING,   NM_SETTING_DCB_APP_FIP_FLAGS },
	{ KEY_DCB_PFC_ENABLE,       KEY_DCB_PFC_ADVERTISE,       KEY_DCB_PFC_WILLING,       NM_SETTING_DCB_PRIORITY_FLOW_CONTROL_FLAGS },
	{ KEY_DCB_PG_ENABLE,        KEY_DCB_PG_ADVERTISE,        KEY_DCB_PG_WILLING,        NM_SETTING_DCB_PRIORITY_GROUP_FLAGS },
	{ NULL },
};

static NMSettingDcbFlags
read_dcb_flags (shvarFile *ifcfg, DcbFlagsProperty *property)
{
	NMSettingDcbFlags flags = NM_SETTING_DCB_FLAG_NONE;

	if (svGetValueBoolean (ifcfg, property->enable_key, FALSE))
		flags |= NM_SETTING_DCB_FLAG_ENABLE;
	if (svGetValueBoolean (ifcfg, property->advertise_key, FALSE))
		flags |= NM_SETTING_DCB_FLAG_ADVERTISE;
	if (svGetValueBoolean (ifcfg, property->willing_key, FALSE))
		flags |= NM_SETTING_DCB_FLAG_WILLING;

	return flags;
}

static gboolean
read_dcb_app (shvarFile *ifcfg,
              NMSettingDcb *s_dcb,
              const char *app,
              DcbFlagsProperty *flags_prop,
              const char *priority_prop,
              GError **error)
{
	NMSettingDcbFlags flags = NM_SETTING_DCB_FLAG_NONE;
	char *tmp, *val;
	gboolean success = TRUE;
	int priority = -1;

	flags = read_dcb_flags (ifcfg, flags_prop);

	/* Priority */
	tmp = g_strdup_printf ("DCB_APP_%s_PRIORITY", app);
	val = svGetValueString (ifcfg, tmp);
	if (val) {
		priority = _nm_utils_ascii_str_to_int64 (val, 0, 0, 7, -1);
		if (priority < 0) {
			success = FALSE;
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid %s value '%s' (expected 0 - 7)",
			             tmp, val);
		}
		g_free (val);

		if (!(flags & NM_SETTING_DCB_FLAG_ENABLE))
			PARSE_WARNING ("ignoring DCB %s priority; app not enabled", app);
	}
	g_free (tmp);

	if (success) {
		g_object_set (G_OBJECT (s_dcb),
		              flags_prop->flags_prop, flags,
		              priority_prop, (guint) priority,
		              NULL);
	}

	return success;
}

typedef void (*DcbSetBoolFunc) (NMSettingDcb *, guint, gboolean);

static gboolean
read_dcb_bool_array (shvarFile *ifcfg,
                     NMSettingDcb *s_dcb,
                     NMSettingDcbFlags flags,
                     const char *prop,
                     const char *desc,
                     DcbSetBoolFunc set_func,
                     GError **error)
{
	char *val;
	gboolean success = FALSE;
	guint i;

	val = svGetValueString (ifcfg, prop);
	if (!val)
		return TRUE;

	if (!(flags & NM_SETTING_DCB_FLAG_ENABLE)) {
		PARSE_WARNING ("ignoring %s; %s is not enabled", prop, desc);
		success = TRUE;
		goto out;
	}

	val = g_strstrip (val);
	if (strlen (val) != 8) {
		PARSE_WARNING ("%s value '%s' must be 8 characters long", prop, val);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "boolean array must be 8 characters");
		goto out;
	}

	/* All characters must be either 0 or 1 */
	for (i = 0; i < 8; i++) {
		if (val[i] != '0' && val[i] != '1') {
			PARSE_WARNING ("invalid %s value '%s': not all 0s and 1s", prop, val);
			g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			                     "invalid boolean digit");
			goto out;
		}
		set_func (s_dcb, i, (val[i] == '1'));
	}
	success = TRUE;

out:
	g_free (val);
	return success;
}

typedef void (*DcbSetUintFunc) (NMSettingDcb *, guint, guint);

static gboolean
read_dcb_uint_array (shvarFile *ifcfg,
                     NMSettingDcb *s_dcb,
                     NMSettingDcbFlags flags,
                     const char *prop,
                     const char *desc,
                     gboolean f_allowed,
                     DcbSetUintFunc set_func,
                     GError **error)
{
	char *val;
	gboolean success = FALSE;
	guint i;

	val = svGetValueString (ifcfg, prop);
	if (!val)
		return TRUE;

	if (!(flags & NM_SETTING_DCB_FLAG_ENABLE)) {
		PARSE_WARNING ("ignoring %s; %s is not enabled", prop, desc);
		success = TRUE;
		goto out;
	}

	val = g_strstrip (val);
	if (strlen (val) != 8) {
		PARSE_WARNING ("%s value '%s' must be 8 characters long", prop, val);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "uint array must be 8 characters");
		goto out;
	}

	/* All characters must be either 0 - 7 or (optionally) f */
	for (i = 0; i < 8; i++) {
		if (val[i] >= '0' && val[i] <= '7')
			set_func (s_dcb, i, val[i] - '0');
		else if (f_allowed && (val[i] == 'f' || val[i] == 'F'))
			set_func (s_dcb, i, 15);
		else {
			PARSE_WARNING ("invalid %s value '%s': not 0 - 7%s",
			               prop, val, f_allowed ? " or 'f'" : "");
			g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			                     "invalid uint digit");
			goto out;
		}
	}
	success = TRUE;

out:
	g_free (val);
	return success;
}

static gboolean
read_dcb_percent_array (shvarFile *ifcfg,
                        NMSettingDcb *s_dcb,
                        NMSettingDcbFlags flags,
                        const char *prop,
                        const char *desc,
                        gboolean sum_pct,
                        DcbSetUintFunc set_func,
                        GError **error)
{
	char *val;
	gboolean success = FALSE;
	char **split = NULL, **iter;
	guint i, sum = 0;

	val = svGetValueString (ifcfg, prop);
	if (!val)
		return TRUE;

	if (!(flags & NM_SETTING_DCB_FLAG_ENABLE)) {
		PARSE_WARNING ("ignoring %s; %s is not enabled", prop, desc);
		success = TRUE;
		goto out;
	}

	val = g_strstrip (val);
	split = g_strsplit_set (val, ",", 0);
	if (!split || (g_strv_length (split) != 8)) {
		PARSE_WARNING ("invalid %s percentage list value '%s'", prop, val);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "percent array must be 8 elements");
		goto out;
	}

	for (iter = split, i = 0; iter && *iter; iter++, i++) {
		int tmp;

		tmp = _nm_utils_ascii_str_to_int64 (*iter, 0, 0, 100, -1);
		if (tmp < 0) {
			PARSE_WARNING ("invalid %s percentage value '%s'", prop, *iter);
			g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			                     "invalid percent element");
			goto out;
		}
		set_func (s_dcb, i, (guint) tmp);
		sum += (guint) tmp;
	}

	if (sum_pct && (sum != 100)) {
		PARSE_WARNING ("%s percentages do not equal 100%%", prop);
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "invalid percentage sum");
		goto out;
	}

	success = TRUE;

out:
	if (split)
		g_strfreev (split);
	g_free (val);
	return success;
}

static gboolean
make_dcb_setting (shvarFile *ifcfg,
                  const char *network_file,
                  NMSetting **out_setting,
                  GError **error)
{
	NMSettingDcb *s_dcb = NULL;
	gboolean dcb_on;
	NMSettingDcbFlags flags = NM_SETTING_DCB_FLAG_NONE;
	char *val;

	g_return_val_if_fail (out_setting != NULL, FALSE);

	dcb_on = !!svGetValueBoolean (ifcfg, "DCB", FALSE);
	if (!dcb_on)
		return TRUE;

	s_dcb = (NMSettingDcb *) nm_setting_dcb_new ();
	g_assert (s_dcb);

	/* FCOE */
	if (!read_dcb_app (ifcfg, s_dcb, "FCOE",
	                   &dcb_flags_props[DCB_APP_FCOE_FLAGS],
	                   NM_SETTING_DCB_APP_FCOE_PRIORITY,
	                   error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}
	if (nm_setting_dcb_get_app_fcoe_flags (s_dcb) & NM_SETTING_DCB_FLAG_ENABLE) {
		val = svGetValueString (ifcfg, KEY_DCB_APP_FCOE_MODE);
		if (val) {
			if (strcmp (val, NM_SETTING_DCB_FCOE_MODE_FABRIC) == 0 ||
			    strcmp (val, NM_SETTING_DCB_FCOE_MODE_VN2VN) == 0)
				g_object_set (G_OBJECT (s_dcb), NM_SETTING_DCB_APP_FCOE_MODE, val, NULL);
			else {
				PARSE_WARNING ("invalid FCoE mode '%s'", val);
				g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				                     "invalid FCoE mode");
				g_free (val);
				g_object_unref (s_dcb);
				return FALSE;
			}
			g_free (val);
		}
	}

	/* iSCSI */
	if (!read_dcb_app (ifcfg, s_dcb, "ISCSI",
	                   &dcb_flags_props[DCB_APP_ISCSI_FLAGS],
	                   NM_SETTING_DCB_APP_ISCSI_PRIORITY,
	                   error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* FIP */
	if (!read_dcb_app (ifcfg, s_dcb, "FIP",
	                   &dcb_flags_props[DCB_APP_FIP_FLAGS],
	                   NM_SETTING_DCB_APP_FIP_PRIORITY,
	                   error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* Priority Flow Control */
	flags = read_dcb_flags (ifcfg, &dcb_flags_props[DCB_PFC_FLAGS]);
	g_object_set (G_OBJECT (s_dcb), NM_SETTING_DCB_PRIORITY_FLOW_CONTROL_FLAGS, flags, NULL);

	if (!read_dcb_bool_array (ifcfg,
	                          s_dcb,
	                          flags,
	                          KEY_DCB_PFC_UP,
	                          "PFC",
	                          nm_setting_dcb_set_priority_flow_control,
	                          error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* Priority Groups */
	flags = read_dcb_flags (ifcfg, &dcb_flags_props[DCB_PG_FLAGS]);
	g_object_set (G_OBJECT (s_dcb), NM_SETTING_DCB_PRIORITY_GROUP_FLAGS, flags, NULL);

	if (!read_dcb_uint_array (ifcfg,
	                          s_dcb,
	                          flags,
	                          KEY_DCB_PG_ID,
	                          "PGID",
	                          TRUE,
	                          nm_setting_dcb_set_priority_group_id,
	                          error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* Group bandwidth */
	if (!read_dcb_percent_array (ifcfg,
	                             s_dcb,
	                             flags,
	                             KEY_DCB_PG_PCT,
	                             "PGPCT",
	                             TRUE,
	                             nm_setting_dcb_set_priority_group_bandwidth,
	                             error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* Priority bandwidth */
	if (!read_dcb_percent_array (ifcfg,
	                             s_dcb,
	                             flags,
	                             KEY_DCB_PG_UPPCT,
	                             "UPPCT",
	                             FALSE,
	                             nm_setting_dcb_set_priority_bandwidth,
	                             error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	/* Strict Bandwidth */
	if (!read_dcb_bool_array (ifcfg,
	                          s_dcb,
	                          flags,
	                          KEY_DCB_PG_STRICT,
	                          "STRICT",
	                          nm_setting_dcb_set_priority_strict_bandwidth,
	                          error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	if (!read_dcb_uint_array (ifcfg,
	                          s_dcb,
	                          flags,
	                          KEY_DCB_PG_UP2TC,
	                          "UP2TC",
	                          FALSE,
	                          nm_setting_dcb_set_priority_traffic_class,
	                          error)) {
		g_object_unref (s_dcb);
		return FALSE;
	}

	*out_setting = NM_SETTING (s_dcb);
	return TRUE;
}

static gboolean
add_one_wep_key (shvarFile *ifcfg,
                 const char *shvar_key,
                 guint8 key_idx,
                 gboolean passphrase,
                 NMSettingWirelessSecurity *s_wsec,
                 GError **error)
{
	char *key = NULL;
	char *value = NULL;
	gboolean success = FALSE;

	g_return_val_if_fail (ifcfg != NULL, FALSE);
	g_return_val_if_fail (shvar_key != NULL, FALSE);
	g_return_val_if_fail (key_idx <= 3, FALSE);
	g_return_val_if_fail (s_wsec != NULL, FALSE);

	value = svGetValueString (ifcfg, shvar_key);
	if (!value || !strlen (value)) {
		g_free (value);
		return TRUE;
	}

	/* Validate keys */
	if (passphrase) {
		if (strlen (value) && strlen (value) < 64) {
			key = g_strdup (value);
			g_object_set (G_OBJECT (s_wsec),
			              NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
			              NM_WEP_KEY_TYPE_PASSPHRASE,
			              NULL);
		}
	} else {
		if (strlen (value) == 10 || strlen (value) == 26) {
			/* Hexadecimal WEP key */
			char *p = value;

			while (*p) {
				if (!g_ascii_isxdigit (*p)) {
					g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
					             "Invalid hexadecimal WEP key.");
					goto out;
				}
				p++;
			}
			key = g_strdup (value);
		} else if (   !strncmp (value, "s:", 2)
		           && (strlen (value) == 7 || strlen (value) == 15)) {
			/* ASCII key */
			char *p = value + 2;

			while (*p) {
				if (!g_ascii_isprint ((int) (*p))) {
					g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
					             "Invalid ASCII WEP key.");
					goto out;
				}
				p++;
			}

			/* Remove 's:' prefix.
			 * Don't convert to hex string. wpa_supplicant takes 'wep_key0' option over D-Bus as byte array
			 * and converts it to hex string itself. Even though we convert hex string keys into a bin string
			 * before passing to wpa_supplicant, this prevents two unnecessary conversions. And mainly,
			 * ASCII WEP key doesn't change to HEX WEP key in UI, which could confuse users.
			 */
			key = g_strdup (value + 2);
		}
	}

	if (key) {
		nm_setting_wireless_security_set_wep_key (s_wsec, key_idx, key);
		g_free (key);
		success = TRUE;
	} else
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Invalid WEP key length.");

out:
	g_free (value);
	return success;
}

static gboolean
read_wep_keys (shvarFile *ifcfg,
               guint8 def_idx,
               NMSettingWirelessSecurity *s_wsec,
               GError **error)
{
	/* Try hex/ascii keys first */
	if (!add_one_wep_key (ifcfg, "KEY1", 0, FALSE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY2", 1, FALSE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY3", 2, FALSE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY4", 3, FALSE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY", def_idx, FALSE, s_wsec, error))
		return FALSE;

	/* And then passphrases */
	if (!add_one_wep_key (ifcfg, "KEY_PASSPHRASE1", 0, TRUE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY_PASSPHRASE2", 1, TRUE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY_PASSPHRASE3", 2, TRUE, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ifcfg, "KEY_PASSPHRASE4", 3, TRUE, s_wsec, error))
		return FALSE;

	return TRUE;
}

static NMSettingSecretFlags
read_secret_flags (shvarFile *ifcfg, const char *flags_key)
{
	NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;
	char *val;

	g_return_val_if_fail (flags_key != NULL, NM_SETTING_SECRET_FLAG_NONE);
	g_return_val_if_fail (flags_key[0] != '\0', NM_SETTING_SECRET_FLAG_NONE);
	g_return_val_if_fail (g_str_has_suffix (flags_key, "_FLAGS"), NM_SETTING_SECRET_FLAG_NONE);

	val = svGetValueString (ifcfg, flags_key);
	if (val) {
		if (strstr (val, SECRET_FLAG_AGENT))
			flags |= NM_SETTING_SECRET_FLAG_AGENT_OWNED;
		if (strstr (val, SECRET_FLAG_NOT_SAVED))
			flags |= NM_SETTING_SECRET_FLAG_NOT_SAVED;
		if (strstr (val, SECRET_FLAG_NOT_REQUIRED))
			flags |= NM_SETTING_SECRET_FLAG_NOT_REQUIRED;

		g_free (val);
	}
	return flags;
}

static NMSetting *
make_wep_setting (shvarFile *ifcfg,
                  const char *file,
                  GError **error)
{
	NMSettingWirelessSecurity *s_wsec;
	char *value;
	shvarFile *keys_ifcfg = NULL;
	int default_key_idx = 0;
	gboolean has_default_key = FALSE;
	NMSettingSecretFlags key_flags;

	s_wsec = NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());
	g_object_set (s_wsec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "none", NULL);

	value = svGetValueString (ifcfg, "DEFAULTKEY");
	if (value) {
		default_key_idx = _nm_utils_ascii_str_to_int64 (value, 0, 1, 4, 0);
		if (default_key_idx == 0) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid default WEP key '%s'", value);
			g_free (value);
			goto error;
		}
		has_default_key = TRUE;
		default_key_idx--;  /* convert to [0...3] */
		g_object_set (s_wsec, NM_SETTING_WIRELESS_SECURITY_WEP_TX_KEYIDX, (guint) default_key_idx, NULL);
		g_free (value);
	}

	/* Read WEP key flags */
	key_flags = read_secret_flags (ifcfg, "WEP_KEY_FLAGS");
	g_object_set (s_wsec, NM_SETTING_WIRELESS_SECURITY_WEP_KEY_FLAGS, key_flags, NULL);

	/* Read keys in the ifcfg file if they are system-owned */
	if (key_flags == NM_SETTING_SECRET_FLAG_NONE) {
		if (!read_wep_keys (ifcfg, default_key_idx, s_wsec, error))
			goto error;

		/* Try to get keys from the "shadow" key file */
		keys_ifcfg = utils_get_keys_ifcfg (file, FALSE);
		if (keys_ifcfg) {
			if (!read_wep_keys (keys_ifcfg, default_key_idx, s_wsec, error)) {
				svCloseFile (keys_ifcfg);
				goto error;
			}
			svCloseFile (keys_ifcfg);
			g_assert (error == NULL || *error == NULL);
		}
	}

	value = svGetValueString (ifcfg, "SECURITYMODE");
	if (value) {
		char *lcase;

		lcase = g_ascii_strdown (value, -1);
		g_free (value);

		if (!strcmp (lcase, "open")) {
			g_object_set (s_wsec, NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "open", NULL);
		} else if (!strcmp (lcase, "restricted")) {
			g_object_set (s_wsec, NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "shared", NULL);
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid WEP authentication algorithm '%s'",
			             lcase);
			g_free (lcase);
			goto error;
		}
		g_free (lcase);
	}

	/* If no WEP keys were given, and the keys are not agent-owned, and no
	 * default WEP key index was given, then the connection is unencrypted.
	 */
	if (   !nm_setting_wireless_security_get_wep_key (s_wsec, 0)
	    && !nm_setting_wireless_security_get_wep_key (s_wsec, 1)
	    && !nm_setting_wireless_security_get_wep_key (s_wsec, 2)
	    && !nm_setting_wireless_security_get_wep_key (s_wsec, 3)
	    && (has_default_key == FALSE)
	    && (key_flags == NM_SETTING_SECRET_FLAG_NONE)) {
		const char *auth_alg;

		auth_alg = nm_setting_wireless_security_get_auth_alg (s_wsec);
		if (auth_alg && !strcmp (auth_alg, "shared")) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "WEP Shared Key authentication is invalid for "
			             "unencrypted connections.");
			goto error;
		}

		/* Unencrypted */
		g_object_unref (s_wsec);
		s_wsec = NULL;
	}

	return (NMSetting *) s_wsec;

error:
	if (s_wsec)
		g_object_unref (s_wsec);
	return NULL;
}

static gboolean
fill_wpa_ciphers (shvarFile *ifcfg,
                  NMSettingWirelessSecurity *wsec,
                  gboolean group,
                  gboolean adhoc)
{
	char *value = NULL, *p;
	char **list = NULL, **iter;
	int i = 0;

	p = value = svGetValueString (ifcfg, group ? "CIPHER_GROUP" : "CIPHER_PAIRWISE");
	if (!value)
		return TRUE;

	list = g_strsplit_set (p, " ", 0);
	for (iter = list; iter && *iter; iter++, i++) {
		/* Ad-Hoc configurations cannot have pairwise ciphers, and can only
		 * have one group cipher.  Ignore any additional group ciphers and
		 * any pairwise ciphers specified.
		 */
		if (adhoc) {
			if (group && (i > 0)) {
				PARSE_WARNING ("ignoring group cipher '%s' (only one group cipher allowed "
				               "in Ad-Hoc mode)", *iter);
				continue;
			} else if (!group) {
				PARSE_WARNING ("ignoring pairwise cipher '%s' (pairwise not used "
				               "in Ad-Hoc mode)", *iter);
				continue;
			}
		}

		if (!strcmp (*iter, "CCMP")) {
			if (group)
				nm_setting_wireless_security_add_group (wsec, "ccmp");
			else
				nm_setting_wireless_security_add_pairwise (wsec, "ccmp");
		} else if (!strcmp (*iter, "TKIP")) {
			if (group)
				nm_setting_wireless_security_add_group (wsec, "tkip");
			else
				nm_setting_wireless_security_add_pairwise (wsec, "tkip");
		} else if (group && !strcmp (*iter, "WEP104"))
			nm_setting_wireless_security_add_group (wsec, "wep104");
		else if (group && !strcmp (*iter, "WEP40"))
			nm_setting_wireless_security_add_group (wsec, "wep40");
		else {
			PARSE_WARNING ("ignoring invalid %s cipher '%s'",
			               group ? "CIPHER_GROUP" : "CIPHER_PAIRWISE",
			               *iter);
		}
	}

	if (list)
		g_strfreev (list);
	g_free (value);
	return TRUE;
}

#define WPA_PMK_LEN 32

static char *
parse_wpa_psk (shvarFile *ifcfg,
               const char *file,
               GBytes *ssid,
               GError **error)
{
	shvarFile *keys_ifcfg;
	gs_free char *psk = NULL;
	size_t plen;

	/* Passphrase must be between 10 and 66 characters in length because WPA
	 * hex keys are exactly 64 characters (no quoting), and WPA passphrases
	 * are between 8 and 63 characters (inclusive), plus optional quoting if
	 * the passphrase contains spaces.
	 */

	/* Try to get keys from the "shadow" key file */
	keys_ifcfg = utils_get_keys_ifcfg (file, FALSE);
	if (keys_ifcfg) {
		psk = svGetValueString (keys_ifcfg, "WPA_PSK");
		svCloseFile (keys_ifcfg);
	}

	/* Fall back to the original ifcfg */
	if (!psk)
		psk = svGetValueString (ifcfg, "WPA_PSK");

	if (!psk)
		return NULL;

	plen = strlen (psk);

	if (plen == 64) {
		/* Verify the hex PSK; 64 digits */
		if (!NM_STRCHAR_ALL (psk, ch, g_ascii_isxdigit (ch))) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid WPA_PSK (contains non-hexadecimal characters)");
			return NULL;
		}
	} else {
		if (plen < 8 || plen > 63) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid WPA_PSK (passphrases must be between "
			             "8 and 63 characters long (inclusive))");
			return NULL;
		}
	}

	return g_steal_pointer (&psk);
}

static gboolean
eap_simple_reader (const char *eap_method,
                   shvarFile *ifcfg,
                   shvarFile *keys,
                   NMSetting8021x *s_8021x,
                   gboolean phase2,
                   GError **error)
{
	NMSettingSecretFlags flags;
	char *value;

	value = svGetValueString (ifcfg, "IEEE_8021X_IDENTITY");
	if (!value) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IEEE_8021X_IDENTITY for EAP method '%s'.",
		             eap_method);
		return FALSE;
	}
	g_object_set (s_8021x, NM_SETTING_802_1X_IDENTITY, value, NULL);
	g_free (value);

	flags = read_secret_flags (ifcfg, "IEEE_8021X_PASSWORD_FLAGS");
	g_object_set (s_8021x, NM_SETTING_802_1X_PASSWORD_FLAGS, flags, NULL);

	/* Only read the password if it's system-owned */
	if (flags == NM_SETTING_SECRET_FLAG_NONE) {
		value = svGetValueString (ifcfg, "IEEE_8021X_PASSWORD");
		if (!value && keys) {
			/* Try the lookaside keys file */
			value = svGetValueString (keys, "IEEE_8021X_PASSWORD");
		}

		if (!value) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Missing IEEE_8021X_PASSWORD for EAP method '%s'.",
			             eap_method);
			return FALSE;
		}

		g_object_set (s_8021x, NM_SETTING_802_1X_PASSWORD, value, NULL);
		g_free (value);
	}

	return TRUE;
}

static char *
get_full_file_path (const char *ifcfg_path, const char *file_path)
{
	const char *base = file_path;
	char *p, *ret, *dirname;

	g_return_val_if_fail (ifcfg_path != NULL, NULL);
	g_return_val_if_fail (file_path != NULL, NULL);

	if (file_path[0] == '/')
		return g_strdup (file_path);

	p = strrchr (file_path, '/');
	if (p)
		base = p + 1;

	dirname = g_path_get_dirname (ifcfg_path);
	ret = g_build_path ("/", dirname, base, NULL);
	g_free (dirname);
	return ret;
}

static gboolean
eap_tls_reader (const char *eap_method,
                shvarFile *ifcfg,
                shvarFile *keys,
                NMSetting8021x *s_8021x,
                gboolean phase2,
                GError **error)
{
	char *value;
	char *ca_cert = NULL;
	char *real_path = NULL;
	char *client_cert = NULL;
	char *privkey = NULL;
	char *privkey_password = NULL;
	gboolean success = FALSE;
	NMSetting8021xCKFormat privkey_format = NM_SETTING_802_1X_CK_FORMAT_UNKNOWN;
	const char *ca_cert_key = phase2 ? "IEEE_8021X_INNER_CA_CERT" : "IEEE_8021X_CA_CERT";
	const char *pk_pw_key = phase2 ? "IEEE_8021X_INNER_PRIVATE_KEY_PASSWORD": "IEEE_8021X_PRIVATE_KEY_PASSWORD";
	const char *pk_key = phase2 ? "IEEE_8021X_INNER_PRIVATE_KEY" : "IEEE_8021X_PRIVATE_KEY";
	const char *cli_cert_key = phase2 ? "IEEE_8021X_INNER_CLIENT_CERT" : "IEEE_8021X_CLIENT_CERT";
	const char *pk_pw_flags_key = phase2 ? "IEEE_8021X_INNER_PRIVATE_KEY_PASSWORD_FLAGS": "IEEE_8021X_PRIVATE_KEY_PASSWORD_FLAGS";
	const char *pk_pw_flags_prop = phase2 ? NM_SETTING_802_1X_PHASE2_PRIVATE_KEY_PASSWORD_FLAGS : NM_SETTING_802_1X_PRIVATE_KEY_PASSWORD_FLAGS;
	NMSettingSecretFlags flags;

	value = svGetValueString (ifcfg, "IEEE_8021X_IDENTITY");
	if (value) {
		g_object_set (s_8021x, NM_SETTING_802_1X_IDENTITY, value, NULL);
		g_free (value);
	}

	ca_cert = svGetValueString (ifcfg, ca_cert_key);
	if (ca_cert) {
		real_path = get_full_file_path (svFileGetName (ifcfg), ca_cert);
		if (phase2) {
			if (!nm_setting_802_1x_set_phase2_ca_cert (s_8021x,
			                                           real_path,
			                                           NM_SETTING_802_1X_CK_SCHEME_PATH,
			                                           NULL,
			                                           error))
				goto done;
		} else {
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
			                                    real_path,
			                                    NM_SETTING_802_1X_CK_SCHEME_PATH,
			                                    NULL,
			                                    error))
				goto done;
		}
		g_free (real_path);
		real_path = NULL;
	} else {
		PARSE_WARNING ("missing %s for EAP method '%s'; this is insecure!",
		               ca_cert_key, eap_method);
	}

	/* Read and set private key password flags */
	flags = read_secret_flags (ifcfg, pk_pw_flags_key);
	g_object_set (s_8021x, pk_pw_flags_prop, flags, NULL);

	/* Read the private key password if it's system-owned */
	if (flags == NM_SETTING_SECRET_FLAG_NONE) {
		/* Private key password */
		privkey_password = svGetValueString (ifcfg, pk_pw_key);
		if (!privkey_password && keys) {
			/* Try the lookaside keys file */
			privkey_password = svGetValueString (keys, pk_pw_key);
		}

		if (!privkey_password) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Missing %s for EAP method '%s'.",
			             pk_pw_key,
			             eap_method);
			goto done;
		}
	}

	/* The private key itself */
	privkey = svGetValueString (ifcfg, pk_key);
	if (!privkey) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing %s for EAP method '%s'.",
		             pk_key,
		             eap_method);
		goto done;
	}

	real_path = get_full_file_path (svFileGetName (ifcfg), privkey);
	if (phase2) {
		if (!nm_setting_802_1x_set_phase2_private_key (s_8021x,
		                                               real_path,
		                                               privkey_password,
		                                               NM_SETTING_802_1X_CK_SCHEME_PATH,
		                                               &privkey_format,
		                                               error))
			goto done;
	} else {
		if (!nm_setting_802_1x_set_private_key (s_8021x,
		                                        real_path,
		                                        privkey_password,
		                                        NM_SETTING_802_1X_CK_SCHEME_PATH,
		                                        &privkey_format,
		                                        error))
			goto done;
	}
	g_free (real_path);
	real_path = NULL;

	/* Only set the client certificate if the private key is not PKCS#12 format,
	 * as NM (due to supplicant restrictions) requires.  If the key was PKCS#12,
	 * then nm_setting_802_1x_set_private_key() already set the client certificate
	 * to the same value as the private key.
	 */
	if (   privkey_format == NM_SETTING_802_1X_CK_FORMAT_RAW_KEY
	    || privkey_format == NM_SETTING_802_1X_CK_FORMAT_X509) {
		client_cert = svGetValueString (ifcfg, cli_cert_key);
		if (!client_cert) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Missing %s for EAP method '%s'.",
			             cli_cert_key,
			             eap_method);
			goto done;
		}

		real_path = get_full_file_path (svFileGetName (ifcfg), client_cert);
		if (phase2) {
			if (!nm_setting_802_1x_set_phase2_client_cert (s_8021x,
			                                               real_path,
			                                               NM_SETTING_802_1X_CK_SCHEME_PATH,
			                                               NULL,
			                                               error))
				goto done;
		} else {
			if (!nm_setting_802_1x_set_client_cert (s_8021x,
			                                        real_path,
			                                        NM_SETTING_802_1X_CK_SCHEME_PATH,
			                                        NULL,
			                                        error))
				goto done;
		}
		g_free (real_path);
		real_path = NULL;
	}

	success = TRUE;

done:
	g_free (real_path);
	g_free (ca_cert);
	g_free (client_cert);
	g_free (privkey);
	g_free (privkey_password);
	return success;
}

static gboolean
eap_peap_reader (const char *eap_method,
                 shvarFile *ifcfg,
                 shvarFile *keys,
                 NMSetting8021x *s_8021x,
                 gboolean phase2,
                 GError **error)
{
	char *anon_ident = NULL;
	char *ca_cert = NULL;
	char *real_cert_path = NULL;
	char *inner_auth = NULL;
	char *peapver = NULL;
	char *lower;
	char **list = NULL, **iter;
	gboolean success = FALSE;

	ca_cert = svGetValueString (ifcfg, "IEEE_8021X_CA_CERT");
	if (ca_cert) {
		real_cert_path = get_full_file_path (svFileGetName (ifcfg), ca_cert);
		if (!nm_setting_802_1x_set_ca_cert (s_8021x,
		                                    real_cert_path,
		                                    NM_SETTING_802_1X_CK_SCHEME_PATH,
		                                    NULL,
		                                    error))
			goto done;
	} else {
		PARSE_WARNING ("missing IEEE_8021X_CA_CERT for EAP method '%s'; this is insecure!",
		               eap_method);
	}

	peapver = svGetValueString (ifcfg, "IEEE_8021X_PEAP_VERSION");
	if (peapver) {
		if (!strcmp (peapver, "0"))
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPVER, "0", NULL);
		else if (!strcmp (peapver, "1"))
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPVER, "1", NULL);
		else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Unknown IEEE_8021X_PEAP_VERSION value '%s'",
			             peapver);
			goto done;
		}
	}

	if (svGetValueBoolean (ifcfg, "IEEE_8021X_PEAP_FORCE_NEW_LABEL", FALSE))
		g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPLABEL, "1", NULL);

	anon_ident = svGetValueString (ifcfg, "IEEE_8021X_ANON_IDENTITY");
	if (anon_ident && strlen (anon_ident))
		g_object_set (s_8021x, NM_SETTING_802_1X_ANONYMOUS_IDENTITY, anon_ident, NULL);

	inner_auth = svGetValueString (ifcfg, "IEEE_8021X_INNER_AUTH_METHODS");
	if (!inner_auth) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IEEE_8021X_INNER_AUTH_METHODS.");
		goto done;
	}

	/* Handle options for the inner auth method */
	list = g_strsplit (inner_auth, " ", 0);
	for (iter = list; iter && *iter; iter++) {
		if (!strlen (*iter))
			continue;

		if (   !strcmp (*iter, "MSCHAPV2")
		    || !strcmp (*iter, "MD5")
		    || !strcmp (*iter, "GTC")) {
			if (!eap_simple_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
		} else if (!strcmp (*iter, "TLS")) {
			if (!eap_tls_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Unknown IEEE_8021X_INNER_AUTH_METHOD '%s'.",
			             *iter);
			goto done;
		}

		lower = g_ascii_strdown (*iter, -1);
		g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTH, lower, NULL);
		g_free (lower);
		break;
	}

	if (!nm_setting_802_1x_get_phase2_auth (s_8021x)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "No valid IEEE_8021X_INNER_AUTH_METHODS found.");
		goto done;
	}

	success = TRUE;

done:
	if (list)
		g_strfreev (list);
	g_free (inner_auth);
	g_free (peapver);
	g_free (real_cert_path);
	g_free (ca_cert);
	g_free (anon_ident);
	return success;
}

static gboolean
eap_ttls_reader (const char *eap_method,
                 shvarFile *ifcfg,
                 shvarFile *keys,
                 NMSetting8021x *s_8021x,
                 gboolean phase2,
                 GError **error)
{
	gboolean success = FALSE;
	char *anon_ident = NULL;
	char *ca_cert = NULL;
	char *real_cert_path = NULL;
	char *inner_auth = NULL;
	char *tmp;
	char **list = NULL, **iter;

	ca_cert = svGetValueString (ifcfg, "IEEE_8021X_CA_CERT");
	if (ca_cert) {
		real_cert_path = get_full_file_path (svFileGetName (ifcfg), ca_cert);
		if (!nm_setting_802_1x_set_ca_cert (s_8021x,
		                                    real_cert_path,
		                                    NM_SETTING_802_1X_CK_SCHEME_PATH,
		                                    NULL,
		                                    error))
			goto done;
	} else {
		PARSE_WARNING ("missing IEEE_8021X_CA_CERT for EAP method '%s'; this is insecure!",
		               eap_method);
	}

	anon_ident = svGetValueString (ifcfg, "IEEE_8021X_ANON_IDENTITY");
	if (anon_ident && strlen (anon_ident))
		g_object_set (s_8021x, NM_SETTING_802_1X_ANONYMOUS_IDENTITY, anon_ident, NULL);

	tmp = svGetValueString (ifcfg, "IEEE_8021X_INNER_AUTH_METHODS");
	if (!tmp) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IEEE_8021X_INNER_AUTH_METHODS.");
		goto done;
	}

	inner_auth = g_ascii_strdown (tmp, -1);
	g_free (tmp);

	/* Handle options for the inner auth method */
	list = g_strsplit (inner_auth, " ", 0);
	for (iter = list; iter && *iter; iter++) {
		if (!strlen (*iter))
			continue;

		if (   !strcmp (*iter, "mschapv2")
		    || !strcmp (*iter, "mschap")
		    || !strcmp (*iter, "pap")
		    || !strcmp (*iter, "chap")) {
			if (!eap_simple_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTH, *iter, NULL);
		} else if (!strcmp (*iter, "eap-tls")) {
			if (!eap_tls_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTHEAP, "tls", NULL);
		} else if (   !strcmp (*iter, "eap-mschapv2")
		           || !strcmp (*iter, "eap-md5")
		           || !strcmp (*iter, "eap-gtc")) {
			if (!eap_simple_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTHEAP, (*iter + NM_STRLEN ("eap-")), NULL);
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Unknown IEEE_8021X_INNER_AUTH_METHOD '%s'.",
			             *iter);
			goto done;
		}
		break;
	}

	success = TRUE;

done:
	if (list)
		g_strfreev (list);
	g_free (inner_auth);
	g_free (real_cert_path);
	g_free (ca_cert);
	g_free (anon_ident);
	return success;
}

static gboolean
eap_fast_reader (const char *eap_method,
                 shvarFile *ifcfg,
                 shvarFile *keys,
                 NMSetting8021x *s_8021x,
                 gboolean phase2,
                 GError **error)
{
	char *anon_ident = NULL;
	char *pac_file = NULL;
	char *real_pac_path = NULL;
	char *inner_auth = NULL;
	char *fast_provisioning = NULL;
	char *lower;
	char **list = NULL, **iter;
	const char* pac_prov_str;
	gboolean allow_unauth = FALSE, allow_auth = FALSE;
	gboolean success = FALSE;

	pac_file = svGetValueString (ifcfg, "IEEE_8021X_PAC_FILE");
	if (pac_file) {
		real_pac_path = get_full_file_path (svFileGetName (ifcfg), pac_file);
		g_object_set (s_8021x, NM_SETTING_802_1X_PAC_FILE, real_pac_path, NULL);
	}

	fast_provisioning = svGetValueString (ifcfg, "IEEE_8021X_FAST_PROVISIONING");
	if (fast_provisioning) {
		list = g_strsplit_set (fast_provisioning, " \t", 0);
		for (iter = list; iter && *iter; iter++) {
			if (**iter == '\0')
				continue;
			if (strcmp (*iter, "allow-unauth") == 0)
				allow_unauth = TRUE;
			else if (strcmp (*iter, "allow-auth") == 0)
				allow_auth = TRUE;
			else {
				PARSE_WARNING ("invalid IEEE_8021X_FAST_PROVISIONING '%s' "
				               "(space-separated list of these values [allow-auth, allow-unauth] expected)",
				               *iter);
			}
		}
		g_strfreev (list);
		list = NULL;
	}
	pac_prov_str = allow_unauth ? (allow_auth ? "3" : "1") : (allow_auth ? "2" : "0");
	g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_FAST_PROVISIONING, pac_prov_str, NULL);

	if (!pac_file && !(allow_unauth || allow_auth)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "IEEE_8021X_PAC_FILE not provided and EAP-FAST automatic PAC provisioning disabled.");
		goto done;
	}

	anon_ident = svGetValueString (ifcfg, "IEEE_8021X_ANON_IDENTITY");
	if (anon_ident && strlen (anon_ident))
		g_object_set (s_8021x, NM_SETTING_802_1X_ANONYMOUS_IDENTITY, anon_ident, NULL);

	inner_auth = svGetValueString (ifcfg, "IEEE_8021X_INNER_AUTH_METHODS");
	if (!inner_auth) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IEEE_8021X_INNER_AUTH_METHODS.");
		goto done;
	}

	/* Handle options for the inner auth method */
	list = g_strsplit (inner_auth, " ", 0);
	for (iter = list; iter && *iter; iter++) {
		if (!strlen (*iter))
			continue;

		if (   !strcmp (*iter, "MSCHAPV2")
		    || !strcmp (*iter, "GTC")) {
			if (!eap_simple_reader (*iter, ifcfg, keys, s_8021x, TRUE, error))
				goto done;
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Unknown IEEE_8021X_INNER_AUTH_METHOD '%s'.",
			             *iter);
			goto done;
		}

		lower = g_ascii_strdown (*iter, -1);
		g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTH, lower, NULL);
		g_free (lower);
		break;
	}

	if (!nm_setting_802_1x_get_phase2_auth (s_8021x)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "No valid IEEE_8021X_INNER_AUTH_METHODS found.");
		goto done;
	}

	success = TRUE;

done:
	g_strfreev (list);
	g_free (inner_auth);
	g_free (fast_provisioning);
	g_free (real_pac_path);
	g_free (pac_file);
	g_free (anon_ident);
	return success;
}

typedef struct {
	const char *method;
	gboolean (*reader)(const char *eap_method,
	                   shvarFile *ifcfg,
	                   shvarFile *keys,
	                   NMSetting8021x *s_8021x,
	                   gboolean phase2,
	                   GError **error);
	gboolean wifi_phase2_only;
} EAPReader;

static EAPReader eap_readers[] = {
	{ "md5", eap_simple_reader, TRUE },
	{ "pap", eap_simple_reader, TRUE },
	{ "chap", eap_simple_reader, TRUE },
	{ "mschap", eap_simple_reader, TRUE },
	{ "mschapv2", eap_simple_reader, TRUE },
	{ "leap", eap_simple_reader, FALSE },
	{ "pwd", eap_simple_reader, FALSE },
	{ "tls", eap_tls_reader, FALSE },
	{ "peap", eap_peap_reader, FALSE },
	{ "ttls", eap_ttls_reader, FALSE },
	{ "fast", eap_fast_reader, FALSE },
	{ NULL, NULL }
};

static void
read_8021x_list_value (shvarFile *ifcfg,
                       const char *ifcfg_var_name,
                       NMSetting8021x *setting,
                       const char *prop_name)
{
	char *value;
	char **strv;

	g_return_if_fail (ifcfg != NULL);
	g_return_if_fail (ifcfg_var_name != NULL);
	g_return_if_fail (prop_name != NULL);

	value = svGetValueString (ifcfg, ifcfg_var_name);
	if (!value)
		return;

	strv = g_strsplit_set (value, " \t", 0);
	if (strv && strv[0])
		g_object_set (setting, prop_name, strv, NULL);
	g_strfreev (strv);
	g_free (value);
}

static NMSetting8021x *
fill_8021x (shvarFile *ifcfg,
            const char *file,
            const char *key_mgmt,
            gboolean wifi,
            GError **error)
{
	NMSetting8021x *s_8021x;
	shvarFile *keys = NULL;
	char *value;
	char **list = NULL, **iter;

	value = svGetValueString (ifcfg, "IEEE_8021X_EAP_METHODS");
	if (!value) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing IEEE_8021X_EAP_METHODS for key management '%s'",
		             key_mgmt);
		return NULL;
	}

	list = g_strsplit (value, " ", 0);
	g_free (value);

	s_8021x = (NMSetting8021x *) nm_setting_802_1x_new ();

	/* Read in the lookaside keys file, if present */
	keys = utils_get_keys_ifcfg (file, FALSE);

	/* Validate and handle each EAP method */
	for (iter = list; iter && *iter; iter++) {
		EAPReader *eap = &eap_readers[0];
		gboolean found = FALSE;
		char *lower = NULL;

		lower = g_ascii_strdown (*iter, -1);
		while (eap->method) {
			if (strcmp (eap->method, lower))
				goto next;

			/* Some EAP methods don't provide keying material, thus they
			 * cannot be used with WiFi unless they are an inner method
			 * used with TTLS or PEAP or whatever.
			 */
			if (wifi && eap->wifi_phase2_only) {
				PARSE_WARNING ("ignored invalid IEEE_8021X_EAP_METHOD '%s'; not allowed for wifi.",
				               lower);
				goto next;
			}

			/* Parse EAP method specific options */
			if (!(*eap->reader)(lower, ifcfg, keys, s_8021x, FALSE, error)) {
				g_free (lower);
				goto error;
			}
			nm_setting_802_1x_add_eap_method (s_8021x, lower);
			found = TRUE;
			break;

		next:
			eap++;
		}

		if (!found)
			PARSE_WARNING ("ignored unknown IEEE_8021X_EAP_METHOD '%s'.", lower);
		g_free (lower);
	}

	if (nm_setting_802_1x_get_num_eap_methods (s_8021x) == 0) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "No valid EAP methods found in IEEE_8021X_EAP_METHODS.");
		goto error;
	}

	value = svGetValueString (ifcfg, "IEEE_8021X_SUBJECT_MATCH");
	g_object_set (s_8021x, NM_SETTING_802_1X_SUBJECT_MATCH, value, NULL);
	g_free (value);

	value = svGetValueString (ifcfg, "IEEE_8021X_PHASE2_SUBJECT_MATCH");
	g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_SUBJECT_MATCH, value, NULL);
	g_free (value);

	read_8021x_list_value (ifcfg, "IEEE_8021X_ALTSUBJECT_MATCHES",
	                       s_8021x, NM_SETTING_802_1X_ALTSUBJECT_MATCHES);
	read_8021x_list_value (ifcfg, "IEEE_8021X_PHASE2_ALTSUBJECT_MATCHES",
	                       s_8021x, NM_SETTING_802_1X_PHASE2_ALTSUBJECT_MATCHES);

	value = svGetValueString (ifcfg, "IEEE_8021X_DOMAIN_SUFFIX_MATCH");
	g_object_set (s_8021x, NM_SETTING_802_1X_DOMAIN_SUFFIX_MATCH, value, NULL);
	g_free (value);
	value = svGetValueString (ifcfg, "IEEE_8021X_PHASE2_DOMAIN_SUFFIX_MATCH");
	g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_DOMAIN_SUFFIX_MATCH, value, NULL);
	g_free (value);

	if (list)
		g_strfreev (list);
	if (keys)
		svCloseFile (keys);
	return s_8021x;

error:
	if (list)
		g_strfreev (list);
	if (keys)
		svCloseFile (keys);
	g_object_unref (s_8021x);
	return NULL;
}

static NMSetting *
make_wpa_setting (shvarFile *ifcfg,
                  const char *file,
                  GBytes *ssid,
                  gboolean adhoc,
                  NMSetting8021x **s_8021x,
                  GError **error)
{
	NMSettingWirelessSecurity *wsec;
	char *value, *psk, *lower;
	gboolean wpa_psk = FALSE, wpa_eap = FALSE, ieee8021x = FALSE;

	wsec = NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());

	value = svGetValueString (ifcfg, "KEY_MGMT");
	wpa_psk = !g_strcmp0 (value, "WPA-PSK");
	wpa_eap = !g_strcmp0 (value, "WPA-EAP");
	ieee8021x = !g_strcmp0 (value, "IEEE8021X");
	if (!wpa_psk && !wpa_eap && !ieee8021x)
		goto error; /* Not WPA or Dynamic WEP */

	/* Pairwise and Group ciphers (only relevant for WPA/RSN) */
	if (wpa_psk || wpa_eap) {
		fill_wpa_ciphers (ifcfg, wsec, FALSE, adhoc);
		fill_wpa_ciphers (ifcfg, wsec, TRUE, adhoc);
	}

	/* WPA and/or RSN */
	if (adhoc) {
		/* Ad-Hoc mode only supports WPA proto for now */
		nm_setting_wireless_security_add_proto (wsec, "wpa");
	} else {
		char *allow_wpa, *allow_rsn;

		allow_wpa = svGetValueString (ifcfg, "WPA_ALLOW_WPA");
		allow_rsn = svGetValueString (ifcfg, "WPA_ALLOW_WPA2");

		if (allow_wpa && svGetValueBoolean (ifcfg, "WPA_ALLOW_WPA", TRUE))
			nm_setting_wireless_security_add_proto (wsec, "wpa");
		if (allow_rsn && svGetValueBoolean (ifcfg, "WPA_ALLOW_WPA2", TRUE))
			nm_setting_wireless_security_add_proto (wsec, "rsn");

		g_free (allow_wpa);
		g_free (allow_rsn);
	}

	if (wpa_psk) {
		NMSettingSecretFlags psk_flags;

		psk_flags = read_secret_flags (ifcfg, "WPA_PSK_FLAGS");
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS, psk_flags, NULL);

		/* Read PSK if it's system-owned */
		if (psk_flags == NM_SETTING_SECRET_FLAG_NONE) {
			psk = parse_wpa_psk (ifcfg, file, ssid, error);
			if (psk) {
				g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_PSK, psk, NULL);
				g_free (psk);
			} else if (error)
				goto error;
		}

		if (adhoc)
			g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-none", NULL);
		else
			g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk", NULL);
	} else if (wpa_eap || ieee8021x) {
		/* Adhoc mode is mutually exclusive with any 802.1x-based authentication */
		if (adhoc) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Ad-Hoc mode cannot be used with KEY_MGMT type '%s'", value);
			goto error;
		}

		*s_8021x = fill_8021x (ifcfg, file, value, TRUE, error);
		if (!*s_8021x)
			goto error;

		lower = g_ascii_strdown (value, -1);
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, lower, NULL);
		g_free (lower);
	} else {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Unknown wireless KEY_MGMT type '%s'", value);
		goto error;
	}

	g_free (value);

	value = svGetValueString (ifcfg, "SECURITYMODE");
	if (NM_IN_STRSET (value, NULL, "open"))
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, value, NULL);

	g_free (value);
	return (NMSetting *) wsec;

error:
	g_free (value);
	if (wsec)
		g_object_unref (wsec);
	return NULL;
}

static NMSetting *
make_leap_setting (shvarFile *ifcfg,
                   const char *file,
                   GError **error)
{
	NMSettingWirelessSecurity *wsec;
	shvarFile *keys_ifcfg;
	char *value;
	NMSettingSecretFlags flags;

	wsec = NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());

	value = svGetValueString (ifcfg, "KEY_MGMT");
	if (!value || strcmp (value, "IEEE8021X"))
		goto error; /* Not LEAP */

	g_free (value);
	value = svGetValueString (ifcfg, "SECURITYMODE");
	if (!value || strcasecmp (value, "leap"))
		goto error; /* Not LEAP */

	g_free (value);

	flags = read_secret_flags (ifcfg, "IEEE_8021X_PASSWORD_FLAGS");
	g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_LEAP_PASSWORD_FLAGS, flags, NULL);

	/* Read LEAP password if it's system-owned */
	if (flags == NM_SETTING_SECRET_FLAG_NONE) {
		value = svGetValueString (ifcfg, "IEEE_8021X_PASSWORD");
		if (!value) {
			/* Try to get keys from the "shadow" key file */
			keys_ifcfg = utils_get_keys_ifcfg (file, FALSE);
			if (keys_ifcfg) {
				value = svGetValueString (keys_ifcfg, "IEEE_8021X_PASSWORD");
				svCloseFile (keys_ifcfg);
			}
		}
		if (value && strlen (value))
			g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_LEAP_PASSWORD, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "IEEE_8021X_IDENTITY");
	if (!value || !strlen (value)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Missing LEAP identity");
		goto error;
	}
	g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_LEAP_USERNAME, value, NULL);
	g_free (value);

	g_object_set (wsec,
	              NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "ieee8021x",
	              NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "leap",
	              NULL);

	return (NMSetting *) wsec;

error:
	g_free (value);
	if (wsec)
		g_object_unref (wsec);
	return NULL;
}

static NMSetting *
make_wireless_security_setting (shvarFile *ifcfg,
                                const char *file,
                                GBytes *ssid,
                                gboolean adhoc,
                                NMSetting8021x **s_8021x,
                                GError **error)
{
	NMSetting *wsec;

	g_return_val_if_fail (error && !*error, NULL);

	if (!adhoc) {
		wsec = make_leap_setting (ifcfg, file, error);
		if (wsec)
			return wsec;
		else if (*error)
			return NULL;
	}

	wsec = make_wpa_setting (ifcfg, file, ssid, adhoc, s_8021x, error);
	if (wsec)
		return wsec;
	else if (*error)
		return NULL;

	wsec = make_wep_setting (ifcfg, file, error);
	if (wsec)
		return wsec;
	else if (*error)
		return NULL;

	return NULL; /* unencrypted */
}

static char **
transform_hwaddr_blacklist (const char *blacklist)
{
	char **strv, **iter;
	int shift = 0;

	strv = _nm_utils_strsplit_set (blacklist, " \t", 0);
	for (iter = strv; iter && *iter; iter++) {
		if (shift) {
			*(iter - shift) = *iter;
			*iter = NULL;
		}
		if (!nm_utils_hwaddr_valid (*(iter - shift), ETH_ALEN)) {
			PARSE_WARNING ("invalid MAC in HWADDR_BLACKLIST '%s'", *(iter - shift));
			g_free (*(iter - shift));
			*(iter - shift) = NULL;
			shift++;
		}
	}
	return strv;
}

static NMSetting *
make_wireless_setting (shvarFile *ifcfg,
                       GError **error)
{
	NMSettingWireless *s_wireless;
	const char *cvalue;
	char *value = NULL;
	gint64 chan = 0;
	NMSettingMacRandomization mac_randomization = NM_SETTING_MAC_RANDOMIZATION_NEVER;
	NMSettingWirelessPowersave powersave = NM_SETTING_WIRELESS_POWERSAVE_DEFAULT;

	s_wireless = NM_SETTING_WIRELESS (nm_setting_wireless_new ());

	value = svGetValueString (ifcfg, "HWADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_wireless, NM_SETTING_WIRELESS_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "MACADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_wireless, NM_SETTING_WIRELESS_CLONED_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "GENERATE_MAC_ADDRESS_MASK");
	g_object_set (s_wireless, NM_SETTING_WIRELESS_GENERATE_MAC_ADDRESS_MASK, value, NULL);
	g_free (value);

	value = svGetValueString (ifcfg, "HWADDR_BLACKLIST");
	if (value) {
		char **strv;

		strv = transform_hwaddr_blacklist (value);
		g_object_set (s_wireless, NM_SETTING_WIRELESS_MAC_ADDRESS_BLACKLIST, strv, NULL);
		g_strfreev (strv);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "ESSID");
	if (value) {
		gs_unref_bytes GBytes *bytes = NULL;
		gsize ssid_len = 0;
		gsize value_len = strlen (value);

		if (   value_len > 2
		    && (value_len % 2) == 0
		    && g_str_has_prefix (value, "0x")
		    && NM_STRCHAR_ALL (&value[2], ch, g_ascii_isxdigit (ch))) {
			/* interpret the value as hex-digits iff value starts
			 * with "0x" followed by pairs of hex digits */
			bytes = nm_utils_hexstr2bin (&value[2]);
		} else
			bytes = g_bytes_new (value, value_len);

		ssid_len = g_bytes_get_size (bytes);
		if (ssid_len > 32 || ssid_len == 0) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid SSID '%s' (size %zu not between 1 and 32 inclusive)",
			             value, ssid_len);
			g_free (value);
			goto error;
		}

		g_object_set (s_wireless, NM_SETTING_WIRELESS_SSID, bytes, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "MODE");
	if (value) {
		char *lcase;
		const char *mode = NULL;

		lcase = g_ascii_strdown (value, -1);
		g_free (value);

		if (!strcmp (lcase, "ad-hoc")) {
			mode = "adhoc";
		} else if (!strcmp (lcase, "ap")) {
			mode = "ap";
		} else if (!strcmp (lcase, "managed") || !strcmp (lcase, "auto")) {
			mode = "infrastructure";
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid mode '%s' (not 'Ad-Hoc', 'Ap', 'Managed', or 'Auto')",
			             lcase);
			g_free (lcase);
			goto error;
		}
		g_free (lcase);

		g_object_set (s_wireless, NM_SETTING_WIRELESS_MODE, mode, NULL);
	}

	value = svGetValueString (ifcfg, "BSSID");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_wireless, NM_SETTING_WIRELESS_BSSID, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "CHANNEL");
	if (value) {
		errno = 0;
		chan = _nm_utils_ascii_str_to_int64 (value, 10, 1, 196, 0);
		if (errno || (chan == 0)) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid wireless channel '%s'", value);
			g_free (value);
			goto error;
		}
		g_object_set (s_wireless, NM_SETTING_WIRELESS_CHANNEL, (guint32) chan, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "BAND");
	if (value) {
		if (!strcmp (value, "a")) {
			if (chan && chan <= 14) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Band '%s' invalid for channel %u", value, (guint32) chan);
				g_free (value);
				goto error;
			}
		} else if (!strcmp (value, "bg")) {
			if (chan && chan > 14) {
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
				             "Band '%s' invalid for channel %u", value, (guint32) chan);
				g_free (value);
				goto error;
			}
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid wireless band '%s'", value);
			g_free (value);
			goto error;
		}
		g_object_set (s_wireless, NM_SETTING_WIRELESS_BAND, value, NULL);
		g_free (value);
	} else if (chan > 0) {
		if (chan > 14)
			g_object_set (s_wireless, NM_SETTING_WIRELESS_BAND, "a", NULL);
		else
			g_object_set (s_wireless, NM_SETTING_WIRELESS_BAND, "bg", NULL);
	}

	value = svGetValueString (ifcfg, "MTU");
	if (value) {
		int mtu;

		mtu = _nm_utils_ascii_str_to_int64 (value, 10, 0, 50000, -1);
		if (mtu == -1) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid wireless MTU '%s'", value);
			g_free (value);
			goto error;
		}
		g_object_set (s_wireless, NM_SETTING_WIRELESS_MTU, (guint) mtu, NULL);
		g_free (value);
	}

	g_object_set (s_wireless,
	              NM_SETTING_WIRELESS_HIDDEN,
	              svGetValueBoolean (ifcfg, "SSID_HIDDEN", FALSE),
	              NULL);

	cvalue = svGetValue (ifcfg, "POWERSAVE", &value);
	if (cvalue) {
		if (!strcmp (cvalue, "default"))
			powersave = NM_SETTING_WIRELESS_POWERSAVE_DEFAULT;
		else if (!strcmp (cvalue, "ignore"))
			powersave = NM_SETTING_WIRELESS_POWERSAVE_IGNORE;
		else if (!strcmp (cvalue, "disable") || !strcmp (cvalue, "no"))
			powersave = NM_SETTING_WIRELESS_POWERSAVE_DISABLE;
		else if (!strcmp (cvalue, "enable") || !strcmp (cvalue, "yes"))
			powersave = NM_SETTING_WIRELESS_POWERSAVE_ENABLE;
		else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid POWERSAVE value '%s'", cvalue);
			g_free (value);
			goto error;
		}
		g_free (value);
	}

	g_object_set (s_wireless,
	              NM_SETTING_WIRELESS_POWERSAVE,
	              powersave,
	              NULL);

	cvalue = svGetValue (ifcfg, "MAC_ADDRESS_RANDOMIZATION", &value);
	if (cvalue) {
		if (strcmp (cvalue, "default") == 0)
			mac_randomization = NM_SETTING_MAC_RANDOMIZATION_DEFAULT;
		else if (strcmp (cvalue, "never") == 0)
			mac_randomization = NM_SETTING_MAC_RANDOMIZATION_NEVER;
		else if (strcmp (cvalue, "always") == 0)
			mac_randomization = NM_SETTING_MAC_RANDOMIZATION_ALWAYS;
		else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid MAC_ADDRESS_RANDOMIZATION value '%s'", cvalue);
			g_free (value);
			goto error;
		}
		g_free (value);
	} else
		mac_randomization = NM_SETTING_MAC_RANDOMIZATION_NEVER;

	g_object_set (s_wireless,
	              NM_SETTING_WIRELESS_MAC_ADDRESS_RANDOMIZATION,
	              mac_randomization,
	              NULL);

	return NM_SETTING (s_wireless);

error:
	if (s_wireless)
		g_object_unref (s_wireless);
	return NULL;
}

static NMConnection *
wireless_connection_from_ifcfg (const char *file,
                                shvarFile *ifcfg,
                                GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *wireless_setting = NULL;
	NMSetting8021x *s_8021x = NULL;
	GBytes *ssid;
	NMSetting *security_setting = NULL;
	char *printable_ssid = NULL;
	const char *mode;
	gboolean adhoc = FALSE;
	GError *local = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	connection = nm_simple_connection_new ();

	/* Wireless */
	wireless_setting = make_wireless_setting (ifcfg, error);
	if (!wireless_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, wireless_setting);

	ssid = nm_setting_wireless_get_ssid (NM_SETTING_WIRELESS (wireless_setting));
	if (ssid) {
		printable_ssid = nm_utils_ssid_to_utf8 (g_bytes_get_data (ssid, NULL),
		                                        g_bytes_get_size (ssid));
	} else
		printable_ssid = g_strdup_printf ("unmanaged");

	mode = nm_setting_wireless_get_mode (NM_SETTING_WIRELESS (wireless_setting));
	if (mode && !strcmp (mode, "adhoc"))
		adhoc = TRUE;

	/* Wireless security */
	security_setting = make_wireless_security_setting (ifcfg, file, ssid, adhoc, &s_8021x, &local);
	if (local) {
		g_free (printable_ssid);
		g_object_unref (connection);
		g_propagate_error (error, local);
		return NULL;
	}
	if (security_setting) {
		nm_connection_add_setting (connection, security_setting);
		if (s_8021x)
			nm_connection_add_setting (connection, NM_SETTING (s_8021x));
	}

	/* Connection */
	con_setting = make_connection_setting (file, ifcfg,
	                                       NM_SETTING_WIRELESS_SETTING_NAME,
	                                       printable_ssid, NULL);
	g_free (printable_ssid);
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, con_setting);

	return connection;
}

static void
parse_ethtool_option_autoneg (const char *value, gboolean *out_autoneg)
{
	if (!value) {
		PARSE_WARNING ("Auto-negotiation option missing");
		return;
	}

	if (g_str_equal (value, "off"))
		*out_autoneg = FALSE;
	else if (g_str_equal (value, "on"))
		*out_autoneg = TRUE;
	else
		PARSE_WARNING ("Auto-negotiation unknown value: %s", value);
}

static void
parse_ethtool_option_speed (const char *value, guint32 *out_speed)
{
	if (!value) {
		PARSE_WARNING ("Speed option missing");
		return;
	}

	*out_speed =  _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXUINT32, 0);
	if (errno)
		PARSE_WARNING ("Speed value '%s' is invalid", value);
}

static void
parse_ethtool_option_duplex (const char *value, const char **out_duplex)
{
	if (!value) {
		PARSE_WARNING ("Duplex option missing");
		return;
	}

	if (g_str_equal (value, "half"))
		*out_duplex = "half";
	else if (g_str_equal (value, "full"))
		*out_duplex = "full";
	else
		PARSE_WARNING ("Duplex unknown value: %s", value);

}

static void
parse_ethtool_option_wol (const char *value, NMSettingWiredWakeOnLan *out_flags)
{
	NMSettingWiredWakeOnLan wol_flags = NM_SETTING_WIRED_WAKE_ON_LAN_NONE;

	if (!value) {
		PARSE_WARNING ("Wake-on-LAN options missing");
		return;
	}

	for (; *value; value++) {
		switch (*value) {
		case 'p':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_PHY;
			break;
		case 'u':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_UNICAST;
			break;
		case 'm':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_MULTICAST;
			break;
		case 'b':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_BROADCAST;
			break;
		case 'a':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_ARP;
			break;
		case 'g':
			wol_flags |= NM_SETTING_WIRED_WAKE_ON_LAN_MAGIC;
			break;
		case 's':
			break;
		case 'd':
			wol_flags = NM_SETTING_WIRED_WAKE_ON_LAN_NONE;
			break;
		default:
			PARSE_WARNING ("unrecognized Wake-on-LAN option '%c'", *value);
		}
	}

	*out_flags = wol_flags;
}

static void parse_ethtool_option_sopass (const char *value, char **out_password)
{
	if (!value) {
		PARSE_WARNING ("Wake-on-LAN password missing");
		return;
	}

	g_clear_pointer (out_password, g_free);
	if (!nm_utils_hwaddr_valid (value, ETH_ALEN)) {
		PARSE_WARNING ("Wake-on-LAN password '%s' is invalid", value);
		return;
	}

	*out_password = g_strdup (value);
}

static void
parse_ethtool_option (const char *value,
                      NMSettingWiredWakeOnLan *out_flags,
                      char **out_password,
                      gboolean *out_autoneg,
                      guint32 *out_speed,
                      const char **out_duplex)
{
	gs_strfreev char **words = NULL;
	const char **iter = NULL, *opt_val, *opt;

	if (!value || !value[0])
		return;

	words = g_strsplit_set (value, "\t ", 0);
	iter = (const char **) words;

	while (iter[0]) {
		/* g_strsplit_set() returns empty tokens when extra spaces are found: skip them */
		if (!*iter[0]) {
			iter++;
			continue;
		}

		opt = iter++[0];

		/* skip over repeated space characters like to parse "wol     d". */
		while (iter[0] && !*iter[0])
			iter++;

		opt_val = iter[0];

		if (g_str_equal (opt, "autoneg"))
			parse_ethtool_option_autoneg (opt_val, out_autoneg);
		else if (g_str_equal (opt, "speed"))
			parse_ethtool_option_speed (opt_val, out_speed);
		else if (g_str_equal (opt, "duplex"))
			parse_ethtool_option_duplex (opt_val, out_duplex);
		else if (g_str_equal (opt, "wol"))
			parse_ethtool_option_wol (opt_val, out_flags);
		else if (g_str_equal (opt, "sopass"))
			parse_ethtool_option_sopass (opt_val, out_password);
		else {
			/* Silently skip unknown options */
			continue;
		}

		if (iter[0])
			iter++;
	}
}

static void
parse_ethtool_options (shvarFile *ifcfg, NMSettingWired *s_wired, const char *value)
{
	NMSettingWiredWakeOnLan wol_flags = NM_SETTING_WIRED_WAKE_ON_LAN_DEFAULT;
	gs_free char *wol_password = NULL, *wol_value = NULL;
	gboolean ignore_wol_password = FALSE, autoneg = FALSE;
	guint32 speed = 0;
	const char *duplex = NULL;

	if (value) {
		gs_strfreev char **opts = NULL;
		const char **iter;

		/* WAKE_ON_LAN_IGNORE is inferred from a specified but empty ETHTOOL_OPTS */
		if (!value[0])
			wol_flags = NM_SETTING_WIRED_WAKE_ON_LAN_IGNORE;

		opts = g_strsplit_set (value, ";", 0);
		for (iter = (const char **) opts; iter[0]; iter++) {
			/* in case of repeated wol_passwords, parse_ethtool_option()
			 * will do the right thing and clear wol_password before resetting. */
			parse_ethtool_option (iter[0], &wol_flags, &wol_password, &autoneg, &speed, &duplex);
		}
	}

	/* ETHTOOL_WAKE_ON_LAN = ignore overrides WoL settings in ETHTOOL_OPTS */
	wol_value = svGetValueString (ifcfg, "ETHTOOL_WAKE_ON_LAN");
	if (wol_value) {
		if (strcmp (wol_value, "ignore") == 0)
			wol_flags = NM_SETTING_WIRED_WAKE_ON_LAN_IGNORE;
		else
			PARSE_WARNING ("invalid ETHTOOL_WAKE_ON_LAN value '%s'", wol_value);
	}

	if (   wol_password
	    && !NM_FLAGS_HAS (wol_flags, NM_SETTING_WIRED_WAKE_ON_LAN_MAGIC)) {
		PARSE_WARNING ("Wake-on-LAN password not expected");
		ignore_wol_password = TRUE;
	}

	g_object_set (s_wired,
	              NM_SETTING_WIRED_WAKE_ON_LAN, wol_flags,
	              NM_SETTING_WIRED_WAKE_ON_LAN_PASSWORD, ignore_wol_password ? NULL : wol_password,
	              NM_SETTING_WIRED_AUTO_NEGOTIATE, autoneg,
	              NM_SETTING_WIRED_SPEED, autoneg ? 0 : speed,
	              NM_SETTING_WIRED_DUPLEX, autoneg ? NULL : duplex,
	              NULL);
}

static NMSetting *
make_wired_setting (shvarFile *ifcfg,
                    const char *file,
                    NMSetting8021x **s_8021x,
                    GError **error)
{
	NMSettingWired *s_wired;
	char *value = NULL;
	char *nettype;

	s_wired = NM_SETTING_WIRED (nm_setting_wired_new ());

	value = svGetValueString (ifcfg, "MTU");
	if (value) {
		int mtu;

		mtu = _nm_utils_ascii_str_to_int64 (value, 0, 0, 65535, -1);
		if (mtu >= 0)
			g_object_set (s_wired, NM_SETTING_WIRED_MTU, (guint) mtu, NULL);
		else
			PARSE_WARNING ("invalid MTU '%s'", value);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "HWADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_wired, NM_SETTING_WIRED_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "SUBCHANNELS");
	if (value) {
		const char *p = value;
		gboolean success = TRUE;
		char **chans = NULL;

		/* basic sanity checks */
		while (*p) {
			if (!g_ascii_isxdigit (*p) && (*p != ',') && (*p != '.')) {
				PARSE_WARNING ("invalid SUBCHANNELS '%s'", value);
				success = FALSE;
				break;
			}
			p++;
		}

		if (success) {
			guint32 num_chans;

			chans = g_strsplit_set (value, ",", 0);
			num_chans = g_strv_length (chans);
			if (num_chans < 2 || num_chans > 3) {
				PARSE_WARNING ("invalid SUBCHANNELS '%s' (%d channels, 2 or 3 expected)",
				               value, g_strv_length (chans));
			} else
				g_object_set (s_wired, NM_SETTING_WIRED_S390_SUBCHANNELS, chans, NULL);
			g_strfreev (chans);
		}
		g_free (value);
	}

	value = svGetValueString (ifcfg, "PORTNAME");
	if (value && strlen (value)) {
		nm_setting_wired_add_s390_option (s_wired, "portname", value);
	}
	g_free (value);

	value = svGetValueString (ifcfg, "CTCPROT");
	if (value && strlen (value))
		nm_setting_wired_add_s390_option (s_wired, "ctcprot", value);
	g_free (value);

	nettype = svGetValueString (ifcfg, "NETTYPE");
	if (nettype && strlen (nettype)) {
		if (!strcmp (nettype, "qeth") || !strcmp (nettype, "lcs") || !strcmp (nettype, "ctc"))
			g_object_set (s_wired, NM_SETTING_WIRED_S390_NETTYPE, nettype, NULL);
		else
			PARSE_WARNING ("unknown s390 NETTYPE '%s'", nettype);
	}
	g_free (nettype);

	value = svGetValueString (ifcfg, "OPTIONS");
	if (value && strlen (value)) {
		char **options, **iter;

		iter = options = g_strsplit_set (value, " ", 0);
		while (iter && *iter) {
			char *equals = strchr (*iter, '=');
			gboolean valid = FALSE;

			if (equals) {
				*equals = '\0';
				valid = nm_setting_wired_add_s390_option (s_wired, *iter, equals + 1);
			}
			if (!valid)
				PARSE_WARNING ("invalid s390 OPTION '%s'", *iter);
			iter++;
		}
		g_strfreev (options);
	}
	g_free (value);

	value = svGetValueString (ifcfg, "MACADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_wired, NM_SETTING_WIRED_CLONED_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "GENERATE_MAC_ADDRESS_MASK");
	g_object_set (s_wired, NM_SETTING_WIRED_GENERATE_MAC_ADDRESS_MASK, value, NULL);
	g_free (value);

	value = svGetValueString (ifcfg, "HWADDR_BLACKLIST");
	if (value) {
		char **strv;

		strv = transform_hwaddr_blacklist (value);
		g_object_set (s_wired, NM_SETTING_WIRED_MAC_ADDRESS_BLACKLIST, strv, NULL);
		g_strfreev (strv);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "KEY_MGMT");
	if (value) {
		if (!strcmp (value, "IEEE8021X")) {
			*s_8021x = fill_8021x (ifcfg, file, value, FALSE, error);
			if (!*s_8021x)
				goto error;
		} else {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Unknown wired KEY_MGMT type '%s'", value);
			goto error;
		}
		g_free (value);
	}

	parse_ethtool_options (ifcfg, s_wired,
	                       svGetValue (ifcfg, "ETHTOOL_OPTS", &value));
	g_free (value);

	return (NMSetting *) s_wired;

error:
	g_free (value);
	g_object_unref (s_wired);
	return NULL;
}

static NMConnection *
wired_connection_from_ifcfg (const char *file,
                             shvarFile *ifcfg,
                             GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *wired_setting = NULL;
	NMSetting8021x *s_8021x = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_WIRED_SETTING_NAME, NULL, NULL);
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	check_if_slave (ifcfg, (NMSettingConnection *) con_setting);
	nm_connection_add_setting (connection, con_setting);

	wired_setting = make_wired_setting (ifcfg, file, &s_8021x, error);
	if (!wired_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, wired_setting);

	if (s_8021x)
		nm_connection_add_setting (connection, NM_SETTING (s_8021x));

	return connection;
}

static gboolean
parse_infiniband_p_key (shvarFile *ifcfg,
                        int *out_p_key,
                        char **out_parent,
                        GError **error)
{
	char *device = NULL, *physdev = NULL, *pkey_id = NULL;
	char *ifname = NULL;
	int id;
	gboolean ret = FALSE;

	device = svGetValueString (ifcfg, "DEVICE");
	if (!device) {
		PARSE_WARNING ("InfiniBand connection specified PKEY but not DEVICE");
		goto done;
	}

	physdev = svGetValueString (ifcfg, "PHYSDEV");
	if (!physdev) {
		PARSE_WARNING ("InfiniBand connection specified PKEY but not PHYSDEV");
		goto done;
	}

	pkey_id = svGetValueString (ifcfg, "PKEY_ID");
	if (!pkey_id) {
		PARSE_WARNING ("InfiniBand connection specified PKEY but not PKEY_ID");
		goto done;
	}

	id = _nm_utils_ascii_str_to_int64 (pkey_id, 0, 0, 0xFFFF, -1);
	if (id == -1) {
		PARSE_WARNING ("invalid InfiniBand PKEY_ID '%s'", pkey_id);
		goto done;
	}
	id = (id | 0x8000);

	ifname = g_strdup_printf ("%s.%04x", physdev, (unsigned) id);
	if (strcmp (device, ifname) != 0) {
		PARSE_WARNING ("InfiniBand DEVICE (%s) does not match PHYSDEV+PKEY_ID (%s)",
		               device, ifname);
		goto done;
	}

	*out_p_key = id;
	*out_parent = g_strdup (physdev);
	ret = TRUE;

 done:
	g_free (device);
	g_free (physdev);
	g_free (pkey_id);
	g_free (ifname);

	if (!ret) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create InfiniBand setting.");
	}
	return ret;
}


static NMSetting *
make_infiniband_setting (shvarFile *ifcfg,
                         const char *file,
                         GError **error)
{
	NMSettingInfiniband *s_infiniband;
	char *value = NULL;

	s_infiniband = NM_SETTING_INFINIBAND (nm_setting_infiniband_new ());

	value = svGetValueString (ifcfg, "MTU");
	if (value) {
		int mtu;

		mtu = _nm_utils_ascii_str_to_int64 (value, 0, 0, 65535, -1);
		if (mtu >= 0)
			g_object_set (s_infiniband, NM_SETTING_INFINIBAND_MTU, (guint) mtu, NULL);
		else
			PARSE_WARNING ("invalid MTU '%s'", value);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "HWADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_infiniband, NM_SETTING_INFINIBAND_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	if (svGetValueBoolean (ifcfg, "CONNECTED_MODE", FALSE))
		g_object_set (s_infiniband, NM_SETTING_INFINIBAND_TRANSPORT_MODE, "connected", NULL);
	else
		g_object_set (s_infiniband, NM_SETTING_INFINIBAND_TRANSPORT_MODE, "datagram", NULL);

	if (svGetValueBoolean (ifcfg, "PKEY", FALSE)) {
		int p_key;
		char *parent;

		if (!parse_infiniband_p_key (ifcfg, &p_key, &parent, error)) {
			g_object_unref (s_infiniband);
			return NULL;
		}

		g_object_set (s_infiniband,
		              NM_SETTING_INFINIBAND_P_KEY, p_key,
		              NM_SETTING_INFINIBAND_PARENT, parent,
		              NULL);
	}

	return (NMSetting *) s_infiniband;
}

static NMConnection *
infiniband_connection_from_ifcfg (const char *file,
                                  shvarFile *ifcfg,
                                  GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *infiniband_setting = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_INFINIBAND_SETTING_NAME, NULL, NULL);
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	check_if_slave (ifcfg, (NMSettingConnection *) con_setting);
	nm_connection_add_setting (connection, con_setting);

	infiniband_setting = make_infiniband_setting (ifcfg, file, error);
	if (!infiniband_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, infiniband_setting);

	return connection;
}

static void
handle_bond_option (NMSettingBond *s_bond,
                    const char *key,
                    const char *value)
{
	char *sanitized = NULL, *j;
	const char *p = value;

	/* Remove any quotes or +/- from arp_ip_target */
	if (!g_strcmp0 (key, NM_SETTING_BOND_OPTION_ARP_IP_TARGET) && value && value[0]) {
		if (*p == '\'' || *p == '"')
			p++;
		j = sanitized = g_malloc0 (strlen (p) + 1);
		while (*p) {
			if (*p != '+' && *p != '-' && *p != '\'' && *p != '"')
				*j++ = *p;
			p++;
		}
	}

	if (!nm_setting_bond_add_option (s_bond, key, sanitized ? sanitized : value))
		PARSE_WARNING ("invalid bonding option '%s' = %s",
		               key, sanitized ? sanitized : value);
	g_free (sanitized);
}

static NMSetting *
make_bond_setting (shvarFile *ifcfg,
                   const char *file,
                   GError **error)
{
	NMSettingBond *s_bond;
	char *value;

	s_bond = NM_SETTING_BOND (nm_setting_bond_new ());

	value = svGetValueString (ifcfg, "DEVICE");
	if (!value || !strlen (value)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "mandatory DEVICE keyword missing");
		goto error;
	}
	g_free (value);

	value = svGetValueString (ifcfg, "BONDING_OPTS");
	if (value) {
		char **items, **iter;

		items = g_strsplit_set (value, " ", -1);
		for (iter = items; iter && *iter; iter++) {
			if (strlen (*iter)) {
				char **keys, *key, *val;

				keys = g_strsplit_set (*iter, "=", 2);
				if (keys && *keys) {
					key = *keys;
					val = *(keys + 1);
					if (val && strlen(key) && strlen(val))
						handle_bond_option (s_bond, key, val);
				}

				g_strfreev (keys);
			}
		}
		g_free (value);
		g_strfreev (items);
	}

	return (NMSetting *) s_bond;

error:
	g_object_unref (s_bond);
	return NULL;
}

static NMConnection *
bond_connection_from_ifcfg (const char *file,
                            shvarFile *ifcfg,
                            GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *bond_setting = NULL;
	NMSetting *wired_setting = NULL;
	NMSetting8021x *s_8021x = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_BOND_SETTING_NAME, NULL, _("Bond"));
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, con_setting);

	bond_setting = make_bond_setting (ifcfg, file, error);
	if (!bond_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, bond_setting);

	wired_setting = make_wired_setting (ifcfg, file, &s_8021x, error);
	if (!wired_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, wired_setting);

	if (s_8021x)
		nm_connection_add_setting (connection, NM_SETTING (s_8021x));

	return connection;
}

/* Check 'error' for errors. Missing config (NULL return value) is a valid case. */
static char *
read_team_config (shvarFile *ifcfg, const char *key, GError **error)
{
	gs_free_error GError *local_error = NULL;
	gs_free char *value = NULL;
	size_t l;

	value = svGetValueString (ifcfg, key);
	if (!value)
		return NULL;

	l = strlen (value);
	if (l > 1*1024*1024) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "%s too long (size %zd)", key, l);
		return NULL;
	}

	if (!nm_utils_is_json_object (value, &local_error)) {
		PARSE_WARNING ("ignoring invalid team configuration: %s", local_error->message);
		return NULL;
	}

	return g_steal_pointer (&value);
}

static NMSetting *
make_team_setting (shvarFile *ifcfg,
                   const char *file,
                   GError **error)
{
	NMSettingTeam *s_team;
	char *value;
	GError *local_err = NULL;

	s_team = NM_SETTING_TEAM (nm_setting_team_new ());

	value = svGetValueString (ifcfg, "DEVICE");
	if (!value || !strlen (value)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "mandatory DEVICE keyword missing");
		goto error;
	}
	g_free (value);

	value = read_team_config (ifcfg, "TEAM_CONFIG", &local_err);
	if (local_err) {
		g_propagate_error (error, local_err);
		goto error;
	}
	g_object_set (s_team, NM_SETTING_TEAM_CONFIG, value, NULL);
	g_free (value);

	return (NMSetting *) s_team;

error:
	g_object_unref (s_team);
	return NULL;
}

static NMConnection *
team_connection_from_ifcfg (const char *file,
                            shvarFile *ifcfg,
                            GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *team_setting = NULL;
	NMSetting *wired_setting = NULL;
	NMSetting8021x *s_8021x = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_TEAM_SETTING_NAME, NULL, _("Team"));
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, con_setting);

	team_setting = make_team_setting (ifcfg, file, error);
	if (!team_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, team_setting);

	wired_setting = make_wired_setting (ifcfg, file, &s_8021x, error);
	if (!wired_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, wired_setting);

	if (s_8021x)
		nm_connection_add_setting (connection, NM_SETTING (s_8021x));

	return connection;
}

typedef void (*BridgeOptFunc) (NMSetting *setting,
                               gboolean stp,
                               const char *key,
                               const char *value);

static void
handle_bridge_option (NMSetting *setting,
                      gboolean stp,
                      const char *key,
                      const char *value)
{
	guint32 u = 0;

	if (!strcmp (key, "priority")) {
		if (stp == FALSE) {
			PARSE_WARNING ("'priority' invalid when STP is disabled");
		} else if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_PRIORITY, u, NULL);
		else
			PARSE_WARNING ("invalid priority value '%s'", value);
	} else if (!strcmp (key, "hello_time")) {
		if (stp == FALSE) {
			PARSE_WARNING ("'hello_time' invalid when STP is disabled");
		} else if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_HELLO_TIME, u, NULL);
		else
			PARSE_WARNING ("invalid hello_time value '%s'", value);
	} else if (!strcmp (key, "max_age")) {
		if (stp == FALSE) {
			PARSE_WARNING ("'max_age' invalid when STP is disabled");
		} else if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_MAX_AGE, u, NULL);
		else
			PARSE_WARNING ("invalid max_age value '%s'", value);
	} else if (!strcmp (key, "ageing_time")) {
		if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_AGEING_TIME, u, NULL);
		else
			PARSE_WARNING ("invalid ageing_time value '%s'", value);
	} else if (!strcmp (key, "multicast_snooping")) {
		if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_MULTICAST_SNOOPING,
			              (gboolean) u, NULL);
		else
			PARSE_WARNING ("invalid multicast_snooping value '%s'", value);
	} else
			PARSE_WARNING ("unhandled bridge option '%s'", key);
}

static void
handle_bridging_opts (NMSetting *setting,
                      gboolean stp,
                      const char *value,
                      BridgeOptFunc func)
{
	char **items, **iter;

	items = g_strsplit_set (value, " ", -1);
	for (iter = items; iter && *iter; iter++) {
		if (strlen (*iter)) {
			char **keys, *key, *val;

			keys = g_strsplit_set (*iter, "=", 2);
			if (keys && *keys) {
				key = *keys;
				val = *(keys + 1);
				if (val && strlen(key) && strlen(val))
					func (setting, stp, key, val);
			}

			g_strfreev (keys);
		}
	}
	g_strfreev (items);
}

static NMSetting *
make_bridge_setting (shvarFile *ifcfg,
                     const char *file,
                     GError **error)
{
	NMSettingBridge *s_bridge;
	char *value;
	guint32 u;
	gboolean stp = FALSE;
	gboolean stp_set = FALSE;

	s_bridge = NM_SETTING_BRIDGE (nm_setting_bridge_new ());

	value = svGetValueString (ifcfg, "DEVICE");
	if (!value || !strlen (value)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "mandatory DEVICE keyword missing");
		goto error;
	}
	g_free (value);

	value = svGetValueString (ifcfg, "MACADDR");
	if (value) {
		value = g_strstrip (value);
		g_object_set (s_bridge, NM_SETTING_BRIDGE_MAC_ADDRESS, value, NULL);
		g_free (value);
	}

	value = svGetValueString (ifcfg, "STP");
	if (value) {
		if (!strcasecmp (value, "on") || !strcasecmp (value, "yes")) {
			g_object_set (s_bridge, NM_SETTING_BRIDGE_STP, TRUE, NULL);
			stp = TRUE;
			stp_set = TRUE;
		} else if (!strcasecmp (value, "off") || !strcasecmp (value, "no")) {
			g_object_set (s_bridge, NM_SETTING_BRIDGE_STP, FALSE, NULL);
			stp_set = TRUE;
		} else
			PARSE_WARNING ("invalid STP value '%s'", value);
		g_free (value);
	}

	if (!stp_set) {
		/* Missing or invalid STP property means "no" */
		g_object_set (s_bridge, NM_SETTING_BRIDGE_STP, FALSE, NULL);
	}

	value = svGetValueString (ifcfg, "DELAY");
	if (value) {
		if (stp) {
			if (get_uint (value, &u))
				g_object_set (s_bridge, NM_SETTING_BRIDGE_FORWARD_DELAY, u, NULL);
			else
				PARSE_WARNING ("invalid forward delay value '%s'", value);
		} else
			PARSE_WARNING ("DELAY invalid when STP is disabled");
		g_free (value);
	}

	value = svGetValueString (ifcfg, "BRIDGING_OPTS");
	if (value) {
		handle_bridging_opts (NM_SETTING (s_bridge), stp, value, handle_bridge_option);
		g_free (value);
	}

	return (NMSetting *) s_bridge;

error:
	g_object_unref (s_bridge);
	return NULL;
}

static NMConnection *
bridge_connection_from_ifcfg (const char *file,
                              shvarFile *ifcfg,
                              GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *bridge_setting = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_BRIDGE_SETTING_NAME, NULL, _("Bridge"));
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, con_setting);

	bridge_setting = make_bridge_setting (ifcfg, file, error);
	if (!bridge_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, bridge_setting);	

	return connection;
}

static void
handle_bridge_port_option (NMSetting *setting,
                           gboolean stp,
                           const char *key,
                           const char *value)
{
	guint32 u = 0;

	if (!strcmp (key, "priority")) {
		if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_PORT_PRIORITY, u, NULL);
		else
			PARSE_WARNING ("invalid priority value '%s'", value);
	} else if (!strcmp (key, "path_cost")) {
		if (get_uint (value, &u))
			g_object_set (setting, NM_SETTING_BRIDGE_PORT_PATH_COST, u, NULL);
		else
			PARSE_WARNING ("invalid path_cost value '%s'", value);
	} else if (!strcmp (key, "hairpin_mode")) {
		if (!strcasecmp (value, "on") || !strcasecmp (value, "yes") || !strcmp (value, "1"))
			g_object_set (setting, NM_SETTING_BRIDGE_PORT_HAIRPIN_MODE, TRUE, NULL);
		else if (!strcasecmp (value, "off") || !strcasecmp (value, "no"))
			g_object_set (setting, NM_SETTING_BRIDGE_PORT_HAIRPIN_MODE, FALSE, NULL);
		else
			PARSE_WARNING ("invalid hairpin_mode value '%s'", value);
	} else
			PARSE_WARNING ("unhandled bridge port option '%s'", key);
}

static NMSetting *
make_bridge_port_setting (shvarFile *ifcfg)
{
	NMSetting *s_port = NULL;
	char *value;

	g_return_val_if_fail (ifcfg != NULL, FALSE);

	value = svGetValueString (ifcfg, "BRIDGE_UUID");
	if (!value)
		value = svGetValueString (ifcfg, "BRIDGE");
	if (value) {
		g_free (value);

		s_port = nm_setting_bridge_port_new ();
		value = svGetValueString (ifcfg, "BRIDGING_OPTS");
		if (value)
			handle_bridging_opts (s_port, FALSE, value, handle_bridge_port_option);
		g_free (value);
	}

	return s_port;
}

static NMSetting *
make_team_port_setting (shvarFile *ifcfg)
{
	NMSetting *s_port = NULL;
	char *value;
	GError *error = NULL;

	value = read_team_config (ifcfg, "TEAM_PORT_CONFIG", &error);
	if (value) {
		s_port = nm_setting_team_port_new ();
		g_object_set (s_port, NM_SETTING_TEAM_PORT_CONFIG, value, NULL);
		g_free (value);
	} else if (error) {
		PARSE_WARNING ("%s", error->message);
		g_error_free (error);
	}

	return s_port;
}

static gboolean
is_bond_device (const char *name, shvarFile *parsed)
{
	g_return_val_if_fail (name != NULL, FALSE);
	g_return_val_if_fail (parsed != NULL, FALSE);

	if (svGetValueBoolean (parsed, "BONDING_MASTER", FALSE))
		return TRUE;

	return FALSE;
}

static gboolean
is_vlan_device (const char *name, shvarFile *parsed)
{
	g_return_val_if_fail (name != NULL, FALSE);
	g_return_val_if_fail (parsed != NULL, FALSE);

	if (svGetValueBoolean (parsed, "VLAN", FALSE))
		return TRUE;

	return FALSE;
}

static gboolean
is_wifi_device (const char *name, shvarFile *parsed)
{
	int ifindex;

	g_return_val_if_fail (name != NULL, FALSE);
	g_return_val_if_fail (parsed != NULL, FALSE);

	ifindex = nm_platform_link_get_ifindex (NM_PLATFORM_GET, name);
	if (ifindex == 0)
		return FALSE;

	return nm_platform_link_get_type (NM_PLATFORM_GET, ifindex) == NM_LINK_TYPE_WIFI;
}

static void
parse_prio_map_list (NMSettingVlan *s_vlan,
                     shvarFile *ifcfg,
                     const char *key,
                     NMVlanPriorityMap map)
{
	char *value;
	gchar **list = NULL, **iter;

	value = svGetValueString (ifcfg, key);
	if (!value)
		return;

	list = g_strsplit_set (value, ",", -1);
	g_free (value);

	for (iter = list; iter && *iter; iter++) {
		if (!*iter || !strchr (*iter, ':'))
			continue;

		if (!nm_setting_vlan_add_priority_str (s_vlan, map, *iter))
			PARSE_WARNING ("invalid %s priority map item '%s'", key, *iter);
	}
	g_strfreev (list);
}

static NMSetting *
make_vlan_setting (shvarFile *ifcfg,
                   const char *file,
                   GError **error)
{
	NMSettingVlan *s_vlan = NULL;
	char *value = NULL;
	char *iface_name = NULL;
	char *parent = NULL;
	const char *p = NULL;
	int vlan_id = -1;
	guint32 vlan_flags = 0;
	gint gvrp, reorder_hdr;

	value = svGetValueString (ifcfg, "VLAN_ID");
	if (value) {
		vlan_id = _nm_utils_ascii_str_to_int64 (value, 10, 0, 4095, -1);
		if (vlan_id == -1) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Invalid VLAN_ID '%s'", value);
			g_free (value);
			return NULL;
		}
		g_free (value);
	}

	/* Need DEVICE if we don't have a separate VLAN_ID property */
	iface_name = svGetValueString (ifcfg, "DEVICE");
	if (!iface_name && vlan_id < 0) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "Missing DEVICE property; cannot determine VLAN ID.");
		return NULL;
	}

	s_vlan = NM_SETTING_VLAN (nm_setting_vlan_new ());

	/* Parent interface from PHYSDEV takes precedence if it exists */
	parent = svGetValueString (ifcfg, "PHYSDEV");

	if (iface_name) {
		p = strchr (iface_name, '.');
		if (p) {
			/* eth0.43; PHYSDEV is assumed from it if unknown */
			if (!parent) {
				parent = g_strndup (iface_name, p - iface_name);
				if (g_str_has_prefix (parent, "vlan")) {
					/* Like initscripts, if no PHYSDEV and we get an obviously
					 * invalid parent interface from DEVICE, fail.
					 */
					g_free (parent);
					parent = NULL;
				}
			}
			p++;
		} else {
			/* format like vlan43; PHYSDEV must be set */
			if (g_str_has_prefix (iface_name, "vlan"))
				p = iface_name + 4;
		}

		if (p) {
			int device_vlan_id;

			/* Grab VLAN ID from interface name; this takes precedence over the
			 * separate VLAN_ID property for backwards compat.
			 */
			device_vlan_id = _nm_utils_ascii_str_to_int64 (p, 10, 0, 4095, -1);
			if (device_vlan_id != -1)
				vlan_id = device_vlan_id;
		}
	}

	if (vlan_id < 0) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "Failed to determine VLAN ID from DEVICE or VLAN_ID.");
		goto error;
	}
	g_object_set (s_vlan, NM_SETTING_VLAN_ID, vlan_id, NULL);

	if (parent == NULL) {
		g_set_error_literal (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "Failed to determine VLAN parent from DEVICE or PHYSDEV");
		goto error;
	}
	g_object_set (s_vlan, NM_SETTING_VLAN_PARENT, parent, NULL);
	g_clear_pointer (&parent, g_free);

	vlan_flags |= NM_VLAN_FLAG_REORDER_HEADERS;

	gvrp = svGetValueBoolean (ifcfg, "GVRP", -1);
	if (gvrp > 0)
		vlan_flags |= NM_VLAN_FLAG_GVRP;

	value = svGetValueString (ifcfg, "VLAN_FLAGS");
	if (value) {
		gs_strfreev char **strv = NULL;
		char **ptr;

		strv = g_strsplit_set (value, ", ", 0);

		for (ptr = strv; ptr && *ptr; ptr++) {
			if (nm_streq (*ptr, "GVRP") && gvrp == -1)
				vlan_flags |= NM_VLAN_FLAG_GVRP;
			if (nm_streq (*ptr, "LOOSE_BINDING"))
				vlan_flags |=  NM_VLAN_FLAG_LOOSE_BINDING;
			if (nm_streq (*ptr, "NO_REORDER_HDR"))
				vlan_flags &= ~NM_VLAN_FLAG_REORDER_HEADERS;
		}
	}

	reorder_hdr = svGetValueBoolean (ifcfg, "REORDER_HDR", -1);
	if (   reorder_hdr != -1
	    && reorder_hdr != NM_FLAGS_HAS (vlan_flags, NM_VLAN_FLAG_REORDER_HEADERS))
		PARSE_WARNING ("REORDER_HDR key is deprecated, use VLAN_FLAGS");

	if (svGetValueBoolean (ifcfg, "MVRP", FALSE))
		vlan_flags |= NM_VLAN_FLAG_MVRP;

	g_object_set (s_vlan, NM_SETTING_VLAN_FLAGS, vlan_flags, NULL);
	g_free (value);

	parse_prio_map_list (s_vlan, ifcfg, "VLAN_INGRESS_PRIORITY_MAP", NM_VLAN_INGRESS_MAP);
	parse_prio_map_list (s_vlan, ifcfg, "VLAN_EGRESS_PRIORITY_MAP", NM_VLAN_EGRESS_MAP);

	g_free (iface_name);

	return (NMSetting *) s_vlan;

error:
	g_free (parent);
	g_free (iface_name);
	g_object_unref (s_vlan);
	return NULL;
}

static NMConnection *
vlan_connection_from_ifcfg (const char *file,
                            shvarFile *ifcfg,
                            GError **error)
{
	NMConnection *connection = NULL;
	NMSetting *con_setting = NULL;
	NMSetting *wired_setting = NULL;
	NMSetting *vlan_setting = NULL;
	NMSetting8021x *s_8021x = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	g_return_val_if_fail (ifcfg != NULL, NULL);

	connection = nm_simple_connection_new ();

	con_setting = make_connection_setting (file, ifcfg, NM_SETTING_VLAN_SETTING_NAME, NULL, "Vlan");
	if (!con_setting) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Failed to create connection setting.");
		g_object_unref (connection);
		return NULL;
	}
	check_if_slave (ifcfg, (NMSettingConnection *) con_setting);
	nm_connection_add_setting (connection, con_setting);

	vlan_setting = make_vlan_setting (ifcfg, file, error);
	if (!vlan_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, vlan_setting);

	wired_setting = make_wired_setting (ifcfg, file, &s_8021x, error);
	if (!wired_setting) {
		g_object_unref (connection);
		return NULL;
	}
	nm_connection_add_setting (connection, wired_setting);

	if (s_8021x)
		nm_connection_add_setting (connection, NM_SETTING (s_8021x));

	return connection;
}

static NMConnection *
create_unhandled_connection (const char *filename, shvarFile *ifcfg,
                             const char *type, char **out_spec)
{
	NMConnection *connection;
	NMSetting *s_con;
	char *value;

	nm_assert (out_spec && !*out_spec);

	connection = nm_simple_connection_new ();

	/* Get NAME, UUID, etc. We need to set a connection type (generic) and add
	 * an empty type-specific setting as well, to make sure it passes
	 * nm_connection_verify() later.
	 */
	s_con = make_connection_setting (filename, ifcfg, NM_SETTING_GENERIC_SETTING_NAME,
	                                 NULL, NULL);
	nm_connection_add_setting (connection, s_con);

	nm_connection_add_setting (connection, nm_setting_generic_new ());

	/* Get a spec */
	value = svGetValueString (ifcfg, "HWADDR");
	if (value) {
		char *lower = g_ascii_strdown (value, -1);
		*out_spec = g_strdup_printf ("%s:mac:%s", type, lower);
		g_free (lower);
		g_free (value);
		return connection;
	}

	value = svGetValueString (ifcfg, "SUBCHANNELS");
	if (value) {
		*out_spec = g_strdup_printf ("%s:s390-subchannels:%s", type, value);
		g_free (value);
		return connection;
	}

	value = svGetValueString (ifcfg, "DEVICE");
	if (value) {
		*out_spec = g_strdup_printf ("%s:interface-name:%s", type, value);
		g_free (value);
		return connection;
	}

	g_object_unref (connection);
	return NULL;
}

char *
uuid_from_file (const char *filename)
{
	const char *ifcfg_name = NULL;
	shvarFile *ifcfg;
	char *uuid;

	g_return_val_if_fail (filename != NULL, NULL);

	ifcfg_name = utils_get_ifcfg_name (filename, TRUE);
	if (!ifcfg_name)
		return NULL;

	ifcfg = svOpenFile (filename, NULL);
	if (!ifcfg)
		return NULL;

	/* Try for a UUID key before falling back to hashing the file name */
	uuid = svGetValueString (ifcfg, "UUID");
	if (!uuid || !strlen (uuid)) {
		g_free (uuid);
		uuid = nm_utils_uuid_generate_from_string (svFileGetName (ifcfg), -1, NM_UTILS_UUID_TYPE_LEGACY, NULL);
	}

	svCloseFile (ifcfg);
	return uuid;
}

static void
check_dns_search_domains (shvarFile *ifcfg, NMSetting *s_ip4, NMSetting *s_ip6)
{
	if (!s_ip6)
		return;

	/* If there is no IPv4 config or it doesn't contain DNS searches,
	 * read DOMAIN and put the domains into IPv6.
	 */
	if (!s_ip4 || nm_setting_ip_config_get_num_dns_searches (NM_SETTING_IP_CONFIG (s_ip4)) == 0) {
		/* DNS searches */
		char *value = svGetValueString (ifcfg, "DOMAIN");
		if (value) {
			char **searches = g_strsplit (value, " ", 0);
			if (searches) {
				char **item;
				for (item = searches; *item; item++) {
					if (strlen (*item)) {
						if (!nm_setting_ip_config_add_dns_search (NM_SETTING_IP_CONFIG (s_ip6), *item))
							PARSE_WARNING ("duplicate DNS domain '%s'", *item);
					}
				}
				g_strfreev (searches);
			}
			g_free (value);
		}
	}
}

static NMConnection *
connection_from_file_full (const char *filename,
                           const char *network_file,  /* for unit tests only */
                           const char *test_type,     /* for unit tests only */
                           char **out_unhandled,
                           GError **error,
                           gboolean *out_ignore_error)
{
	NMConnection *connection = NULL;
	shvarFile *parsed;
	gs_free char *type = NULL;
	char *devtype, *bootproto;
	NMSetting *s_ip4, *s_ip6, *s_proxy, *s_port, *s_dcb = NULL;
	const char *ifcfg_name = NULL;

	g_return_val_if_fail (filename != NULL, NULL);
	g_return_val_if_fail (out_unhandled && !*out_unhandled, NULL);

	/* Non-NULL only for unit tests; normally use /etc/sysconfig/network */
	if (!network_file)
		network_file = SYSCONFDIR "/sysconfig/network";

	ifcfg_name = utils_get_ifcfg_name (filename, TRUE);
	if (!ifcfg_name) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Ignoring connection '%s' because it's not an ifcfg file.", filename);
		return NULL;
	}

	parsed = svOpenFile (filename, error);
	if (!parsed)
		return NULL;

	if (!svGetValueBoolean (parsed, "NM_CONTROLLED", TRUE)) {
		connection = create_unhandled_connection (filename, parsed, "unmanaged", out_unhandled);
		if (!connection)
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_FAILED,
			             "NM_CONTROLLED was false but device was not uniquely identified; device will be managed");
		goto done;
	}

	/* iBFT is handled by the iBFT settings plugin */
	bootproto = svGetValueString (parsed, "BOOTPROTO");
	if (bootproto && !g_ascii_strcasecmp (bootproto, "ibft")) {
		if (out_ignore_error)
			*out_ignore_error = TRUE;
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "Ignoring iBFT configuration");
		g_free (bootproto);
		goto done;
	}
	g_free (bootproto);

	devtype = svGetValueString (parsed, "DEVICETYPE");
	if (devtype) {
		if (!strcasecmp (devtype, TYPE_TEAM))
			type = g_strdup (TYPE_TEAM);
		else if (!strcasecmp (devtype, TYPE_TEAM_PORT))
			type = g_strdup (TYPE_ETHERNET);
		g_free (devtype);
	}
	if (!type) {
		gs_free char *t = NULL;

		/* Team and TeamPort types are also accepted by the mere
		 * presense of TEAM_CONFIG/TEAM_MASTER. They don't require
		 * DEVICETYPE. */
		t = svGetValueString (parsed, "TEAM_CONFIG");
		if (t)
			type = g_strdup (TYPE_TEAM);
	}

	if (!type)
		type = svGetValueString (parsed, "TYPE");

	if (!type) {
		gs_free char *tmp = NULL;
		char *device;

		if ((tmp = svGetValueString (parsed, "IPV6TUNNELIPV4"))) {
			if (out_ignore_error)
				*out_ignore_error = TRUE;
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Ignoring unsupported connection due to IPV6TUNNELIPV4");
			goto done;
		}

		device = svGetValueString (parsed, "DEVICE");
		if (!device) {
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "File '%s' had neither TYPE nor DEVICE keys.", filename);
			goto done;
		}
		g_assert (device[0]);

		if (!strcmp (device, "lo")) {
			if (out_ignore_error)
				*out_ignore_error = TRUE;
			g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
			             "Ignoring loopback device config.");
			g_free (device);
			goto done;
		}

		if (!test_type) {
			if (is_bond_device (device, parsed))
				type = g_strdup (TYPE_BOND);
			else if (is_vlan_device (device, parsed))
				type = g_strdup (TYPE_VLAN);
			else if (is_wifi_device (device, parsed))
				type = g_strdup (TYPE_WIRELESS);
			else {
				gs_free char *p_path = NULL;
				char *p_device;
				gsize i;

				/* network-functions detects DEVICETYPE based on the ifcfg-* name and the existence
				 * of a ifup script:
				 *    [ -z "$DEVICETYPE" ] && DEVICETYPE=$(echo ${DEVICE} | sed "s/[0-9]*$//")
				 * later...
				 *    OTHERSCRIPT="/etc/sysconfig/network-scripts/ifup-${DEVICETYPE}"
				 * */
#define IFUP_PATH_PREFIX "/etc/sysconfig/network-scripts/ifup-"
				i = strlen (device);
				p_path = g_malloc (NM_STRLEN (IFUP_PATH_PREFIX) + i + 1);
				p_device = &p_path[NM_STRLEN (IFUP_PATH_PREFIX)];
				memcpy (p_device, device, i + 1);

				/* strip trailing numbers */
				while (i >= 1) {
					i--;
					if (p_device[i] < '0' || p_device[i] > '9')
						break;
					p_device[i] = '\0';
				}

				if (nm_streq (p_device, "eth"))
					type = g_strdup (TYPE_ETHERNET);
				else if (nm_streq (p_device, "wireless"))
					type = g_strdup (TYPE_WIRELESS);
				else if (p_device[0]) {
					memcpy (p_path, IFUP_PATH_PREFIX, NM_STRLEN (IFUP_PATH_PREFIX));
					if (access (p_path, X_OK) == 0) {
						/* for all other types, this is not something we want to handle. */
						if (out_ignore_error)
							*out_ignore_error = TRUE;
						g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
						             "Ignore script for unknown device type which has a matching %s script",
						             p_path);
						goto done;
					}
				}

				if (!type)
					type = g_strdup (TYPE_ETHERNET);
			}
		} else {
			/* For the unit tests, there won't necessarily be any
			 * adapters of the connection's type in the system so the
			 * type can't be tested with ioctls.
			 */
			type = g_strdup (test_type);
		}

		g_free (device);
	} else {
		/* Check for IBM s390 CTC devices and call them Ethernet */
		if (g_strcmp0 (type, "CTC") == 0) {
			g_free (type);
			type = g_strdup (TYPE_ETHERNET);
		}
	}

	if (svGetValueBoolean (parsed, "BONDING_MASTER", FALSE) &&
	    strcasecmp (type, TYPE_BOND)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "BONDING_MASTER=yes key only allowed in TYPE=bond connections");
		goto done;
	}

	/* Construct the connection */
	if (!strcasecmp (type, TYPE_ETHERNET))
		connection = wired_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_WIRELESS))
		connection = wireless_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_INFINIBAND))
		connection = infiniband_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_BOND))
		connection = bond_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_TEAM))
		connection = team_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_VLAN))
		connection = vlan_connection_from_ifcfg (filename, parsed, error);
	else if (!strcasecmp (type, TYPE_BRIDGE))
		connection = bridge_connection_from_ifcfg (filename, parsed, error);
	else {
		connection = create_unhandled_connection (filename, parsed, "unrecognized", out_unhandled);
		if (!connection)
			PARSE_WARNING ("connection type was unrecognized but device was not uniquely identified; device may be managed");
		goto done;
	}

	if (!connection)
		goto done;

	s_ip6 = make_ip6_setting (parsed, network_file, error);
	if (!s_ip6) {
		g_object_unref (connection);
		connection = NULL;
		goto done;
	} else
		nm_connection_add_setting (connection, s_ip6);

	s_ip4 = make_ip4_setting (parsed, network_file, error);
	if (!s_ip4) {
		g_object_unref (connection);
		connection = NULL;
		goto done;
	} else {
		read_aliases (NM_SETTING_IP_CONFIG (s_ip4), filename);
		nm_connection_add_setting (connection, s_ip4);
	}

	/* There is only one DOMAIN variable and it is read and put to IPv4 config
	 * But if IPv4 is disabled or the config fails for some reason, we read
	 * DOMAIN and put the values into IPv6 config instead.
	 */
	check_dns_search_domains (parsed, s_ip4, s_ip6);

	s_proxy = make_proxy_setting (parsed, error);
	if (s_proxy)
		nm_connection_add_setting (connection, s_proxy);

	/* Bridge port? */
	s_port = make_bridge_port_setting (parsed);
	if (s_port)
		nm_connection_add_setting (connection, s_port);

	/* Team port? */
	s_port = make_team_port_setting (parsed);
	if (s_port)
		nm_connection_add_setting (connection, s_port);

	if (!make_dcb_setting (parsed, network_file, &s_dcb, error)) {
		g_object_unref (connection);
		connection = NULL;
		goto done;
	}
	if (s_dcb)
		nm_connection_add_setting (connection, s_dcb);

	if (!nm_connection_normalize (connection, NULL, NULL, error)) {
		g_object_unref (connection);
		connection = NULL;
	}

done:
	svCloseFile (parsed);
	return connection;
}

NMConnection *
connection_from_file (const char *filename,
                      char **out_unhandled,
                      GError **error,
                      gboolean *out_ignore_error)
{
	return connection_from_file_full (filename, NULL, NULL,
	                                  out_unhandled,
	                                  error,
	                                  out_ignore_error);
}

NMConnection *
connection_from_file_test (const char *filename,
                           const char *network_file,
                           const char *test_type,
                           char **out_unhandled,
                           GError **error)
{
	return connection_from_file_full (filename,
	                                  network_file,
	                                  test_type,
	                                  out_unhandled,
	                                  error,
	                                  NULL);
}

guint
devtimeout_from_file (const char *filename)
{
	shvarFile *ifcfg;
	char *devtimeout_str;
	guint devtimeout;

	g_return_val_if_fail (filename != NULL, 0);

	ifcfg = svOpenFile (filename, NULL);
	if (!ifcfg)
		return 0;

	devtimeout_str = svGetValueString (ifcfg, "DEVTIMEOUT");
	if (devtimeout_str) {
		devtimeout = _nm_utils_ascii_str_to_int64 (devtimeout_str, 10, 0, G_MAXUINT, 0);
		g_free (devtimeout_str);
	} else
		devtimeout = 0;

	svCloseFile (ifcfg);

	return devtimeout;
}
