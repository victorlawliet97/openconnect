/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Author: Dan Lenski <dlenski@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#include <errno.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "openconnect-internal.h"

void gpst_common_headers(struct openconnect_info *vpninfo, struct oc_text_buf *buf)
{
	http_common_headers(vpninfo, buf);
}

/* our "auth form" is just a static combination of username and password */
static struct oc_auth_form *gp_auth_form(struct openconnect_info *vpninfo)
{
	static struct oc_form_opt password = {.type=OC_FORM_OPT_PASSWORD, .name=(char *)"password", .label=(char *)"Password: "};
	static struct oc_form_opt username = {.next=&password, .type=OC_FORM_OPT_TEXT, .name=(char *)"username", .label=(char *)"Username: "};
	static struct oc_auth_form form = {.opts=&username, .message=(char *)"Please enter your username and password." };

	if (vpninfo->token_mode!=OC_TOKEN_MODE_NONE)
		password.type = OC_FORM_OPT_TOKEN;

	return &form;
}

/* Return value:
 *  < 0, on error
 *  = 0, on success; *form is populated
 */
struct gp_login_arg { const char *opt; int save:1; int show:1; int warn_missing:1; int err_missing:1; const char *check; };
static const struct gp_login_arg gp_login_args[] = {
    [1] = { .opt="authcookie", .save=1, .err_missing=1 },
    [3] = { .opt="portal", .save=1, .warn_missing=1 },
    [4] = { .opt="user", .save=1, .err_missing=1 },
    [5] = { .opt="authentication source", .show=1 },
    [7] = { .opt="domain", .save=1, .warn_missing=1 },
    [12] = { .opt="connection-type", .err_missing=1, .check="tunnel" },
    [14] = { .opt="clientVer", .err_missing=1, .check="4100" },
    [15] = { .opt="preferred-ip", .save=1 },
};
const int gp_login_nargs = (sizeof(gp_login_args)/sizeof(*gp_login_args));

static int parse_login_xml(struct openconnect_info *vpninfo, xmlNode *xml_node)
{
	struct oc_text_buf *cookie = buf_alloc();
	const char *value = NULL;
	const struct gp_login_arg *arg;

	if (!xmlnode_is_named(xml_node, "jnlp"))
		goto err_out;

	xml_node = xml_node->children;
	if (!xmlnode_is_named(xml_node, "application-desc"))
		goto err_out;

	xml_node = xml_node->children;
	for (arg=gp_login_args; xml_node && arg<gp_login_args+gp_login_nargs; xml_node=xml_node->next, arg++) {
		if (!xmlnode_is_named(xml_node, "argument"))
			goto err_out;

		if (!arg->opt)
			continue;

		value = (const char *)xmlNodeGetContent(xml_node);
		if (value && (!strlen(value) || !strcmp(value, "(null)"))) {
			free((void *)value);
			value = NULL;
		}

		if (arg->check && (value==NULL || strcmp(value, arg->check))) {
			vpn_progress(vpninfo, arg->err_missing ? PRG_ERR : PRG_DEBUG,
						 _("GlobalProtect login returned %s=%s (expected %s)\n"), arg->opt, value, arg->check);
			if (arg->err_missing) goto err_out;
		} else if ((arg->err_missing || arg->warn_missing) && value==NULL) {
			vpn_progress(vpninfo, arg->err_missing ? PRG_ERR : PRG_DEBUG,
						 _("GlobalProtect login returned empty %s\n"), arg->opt);
			if (arg->err_missing) goto err_out;
		} else if (value && arg->show) {
			vpn_progress(vpninfo, PRG_INFO,
						 _("GlobalProtect login returned %s=%s\n"), arg->opt, value);
		}

		if (value && arg->save)
			append_opt(cookie, arg->opt, value);
		free((void *)value);
	}

	vpninfo->cookie = strdup(cookie->data);
	buf_free(cookie);
	return 0;

err_out:
	free((void *)value);
	buf_free(cookie);
	return -EINVAL;
}

static int parse_portal_xml(struct openconnect_info *vpninfo, xmlNode *xml_node)
{
	static struct oc_auth_form form = {.message=(char *)"Please select GlobalProtect gateway." };

	xmlNode *x;
	struct oc_form_opt_select *opt;
	int max_choices = 0, selection = 0;

	opt = calloc(1, sizeof(*opt));
	if (!opt)
		return -ENOMEM;
	opt->form.type = OC_FORM_OPT_SELECT;
	opt->form.name = strdup("GlobalProtect gateway selection");
	opt->form.label = strdup("GlobalProtect gateway");

	if (!xmlnode_is_named(xml_node, "policy"))
		goto err_out;

	/* Look for these XML nodes, which list the gateways: policy/gateways/external/list/entry */
	for (xml_node = xml_node->children; xml_node; xml_node=xml_node->next) {
		if (xmlnode_is_named(xml_node, "gateways")) {
			for (xml_node = xml_node->children; xml_node; xml_node=xml_node->next) {
				if (xmlnode_is_named(xml_node, "external")) {
					for (xml_node = xml_node->children; xml_node; xml_node=xml_node->next) {
						if (xmlnode_is_named(xml_node, "list")) {
							/* first, count the number of gateways */
							for (x = xml_node->children; x; x=x->next)
								if (xmlnode_is_named(x, "entry"))
									max_choices++;

							opt->choices = calloc(1, max_choices * sizeof(struct oc_choice *));
							if (!opt->choices) {
								free_opt((struct oc_form_opt *)opt);
								return -ENOMEM;
							}

							for (xml_node = xml_node->children; xml_node; xml_node=xml_node->next) {
								if (xmlnode_is_named(xml_node, "entry")) {
									struct oc_choice *choice = calloc(1, sizeof(*choice));
									if (!choice) {
										free_opt((struct oc_form_opt *)opt);
										return -ENOMEM;
									}

									xmlnode_get_prop(xml_node, "name", &choice->name);

									for (x = xml_node->children; x; x=x->next) {
										if (xmlnode_is_named(x, "description"))
											choice->label = (char *)xmlNodeGetContent(x);
										if (xmlnode_is_named(x, "priority")) {
											const char *p = (const char *)xmlNodeGetContent(x);
											if (p && !strcmp(p, "1"))
												selection = opt->nr_choices;
											free((void *)p);
										}
									}

									opt->choices[opt->nr_choices++] = choice;
								}
							}

							form.opts = (struct oc_form_opt *)opt;

							/* process static auth form (gateway selection) */
							if (process_auth_form(vpninfo, &form))
								goto err_out;

							free(vpninfo->hostname);
							vpninfo->hostname = strdup(opt->form._value);
							free_opt((struct oc_form_opt *)opt);
							return 0;
						}
					}
				}
			}
		}
	}

err_out:
	free_opt((struct oc_form_opt *)opt);
	return -EINVAL;
}

static int gpst_login(struct openconnect_info *vpninfo, int portal)
{
	int result;

	struct oc_form_opt *opt;
	struct oc_auth_form *form = gp_auth_form(vpninfo);
	struct oc_text_buf *request_body = buf_alloc();
	const char *request_body_type = "application/x-www-form-urlencoded";
	const char *method = "POST";
	char *xml_buf=NULL, *orig_path, *orig_ua;

#ifdef HAVE_LIBSTOKEN
	/* Step 1: Unlock software token (if applicable) */
	if (vpninfo->token_mode == OC_TOKEN_MODE_STOKEN) {
		result = prepare_stoken(vpninfo);
		if (result)
			goto out;
	}
#endif

	/* Ask the user to fill in the auth form; repeat as necessary */
	do {
		/* process static auth form (username and password) */
		result = process_auth_form(vpninfo, form);
		if (result)
			goto out;

	redo_gateway:
		buf_truncate(request_body);

		/* generate token code if specified */
		result = do_gen_tokencode(vpninfo, form);
		if (result) {
			vpn_progress(vpninfo, PRG_ERR, _("Failed to generate OTP tokencode; disabling token\n"));
			vpninfo->token_bypassed = 1;
			goto out;
		}

		/* submit gateway login (ssl-vpn/login.esp) or portal config (global-protect/getconfig.esp) request */
		buf_append(request_body, "jnlpReady=jnlpReady&ok=Login&direct=yes&clientVer=4100&prot=https:");
		append_opt(request_body, "server", vpninfo->hostname);
		append_opt(request_body, "computer", vpninfo->localname);
		for (opt=form->opts; opt; opt=opt->next) {
			if (!strcmp(opt->name, "username"))
				append_opt(request_body, "user", opt->_value);
			else if (!strcmp(opt->name, "password"))
				append_opt(request_body, "passwd", opt->_value);
		}

		orig_path = vpninfo->urlpath;
		orig_ua = vpninfo->useragent;
		vpninfo->useragent = (char *)"PAN GlobalProtect";
		vpninfo->urlpath = strdup(portal ? "global-protect/getconfig.esp" : "ssl-vpn/login.esp");
		result = do_https_request(vpninfo, method, request_body_type, request_body,
					  &xml_buf, 0);
		free(vpninfo->urlpath);
		vpninfo->urlpath = orig_path;
		vpninfo->useragent = orig_ua;

		if (portal) {
			result = gpst_xml_or_error(vpninfo, result, xml_buf, parse_portal_xml);
			if (result == 0) {
				portal = 0;
				goto redo_gateway;
			}
		} else {
			result = gpst_xml_or_error(vpninfo, result, xml_buf, parse_login_xml);
		}
	}
	/* repeat on invalid username or password */
	while (result == -512);

out:
	buf_free(request_body);
	free(xml_buf);
	return result;
}

int gpst_obtain_cookie(struct openconnect_info *vpninfo)
{
	int result;

	if (vpninfo->urlpath && !strncmp(vpninfo->urlpath, "global-protect", 14)) {
		/* assume the server is a portal */
		return gpst_login(vpninfo, 1);
	} else if (vpninfo->urlpath && !strncmp(vpninfo->urlpath, "ssl-vpn", 7)) {
		/* assume the server is a gateway */
		return gpst_login(vpninfo, 0);
	} else {
		/* first try handling it as a gateway, then a portal */
		result = gpst_login(vpninfo, 0);
		if (result == -EEXIST) {
			result = gpst_login(vpninfo, 1);
			if (result == -EEXIST)
				vpn_progress(vpninfo, PRG_ERR, _("Server is neither a GlobalProtect portal nor a gateway.\n"));
		}
		return result;
	}
}

int gpst_bye(struct openconnect_info *vpninfo, const char *reason)
{
	char *orig_path, *orig_ua;
	int result;
	struct oc_text_buf *request_body = buf_alloc();
	const char *request_body_type = "application/x-www-form-urlencoded";
	const char *method = "POST";
	char *xml_buf=NULL;

	/* submit logout request */
	append_opt(request_body, "computer", vpninfo->localname);
	buf_append(request_body, "&%s", vpninfo->cookie);

	/* We need to close and reopen the HTTPS connection (to kill
	 * the tunnel session) and submit a new HTTPS request to
	 * logout.
	 */
	orig_path = vpninfo->urlpath;
	orig_ua = vpninfo->useragent;
	vpninfo->useragent = (char *)"PAN GlobalProtect";
	vpninfo->urlpath = strdup("ssl-vpn/logout.esp");
	openconnect_close_https(vpninfo, 0);
	result = do_https_request(vpninfo, method, request_body_type, request_body,
				  &xml_buf, 0);
	free(vpninfo->urlpath);
	vpninfo->urlpath = orig_path;
	vpninfo->useragent = orig_ua;

	/* logout.esp returns HTTP status 200 and <response status="success"> when
	 * successful, and all manner of malformed junk when unsuccessful.
         */
	result = gpst_xml_or_error(vpninfo, result, xml_buf, NULL);
	if (result < 0)
		vpn_progress(vpninfo, PRG_ERR, _("Logout failed.\n"));
	else
		vpn_progress(vpninfo, PRG_INFO, _("Logout successful\n"));

	buf_free(request_body);
	free(xml_buf);
	return result;
}
