/*
    SSSD

    The main logic of exporting IPA SUDO rules into native LDAP SUDO format.

    Authors:
        Michal Šrubař <mmsrubar@gmail.com>

    Copyright (C) 2014 Michal Šrubař

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "db/sysdb_sudo.h"
#include "providers/ldap/sdap_async.h"
#include "providers/ldap/sdap_async_sudo.h"
#include "providers/ldap/sdap.h"
#include "providers/ipa/ipa_async_sudo.h"
#include "providers/ipa/ipa_common.h"
#include "providers/ipa/ipa_sudo_export.h"
#include "providers/ipa/ipa_sudo_cmd.h"
#include "providers/ipa/ipa_sudo.h"

static errno_t ipa_sudo_get_cmds_retry(struct tevent_req *req);
static void ipa_sudo_get_cmds_connect_done(struct tevent_req *subreq);
static void ipa_sudo_load_ipa_cmds_process(struct tevent_req *subreq);

// FIXME: rename state
struct ipa_sudo_get_cmds_state {

    struct be_ctx *be_ctx;
    struct sdap_id_op *sdap_op;
    struct sysdb_ctx *sysdb;
    struct tevent_context *ev;
    struct sdap_id_conn_cache *conn_cache;
    struct sdap_options *opts;
    struct tevent_req *req;     /* req from sdap_sudo_load_sudoers_send */

    const char *sysdb_filter;   /* sysdb delete filter */
    const char *filter;
    const char *basedn;
    const char **attrs;
    int scope;
    int timeout;

    int dp_error;
    int error;

    struct sudo_rules *rules;
    struct sysdb_attrs **tmp;
};

struct tevent_req *
ipa_sudo_get_cmds_send(TALLOC_CTX *mem,
                       struct sysdb_attrs **ipa_rules,
                       int ipa_rules_count,
                       struct be_ctx *be_ctx,
                       struct sdap_id_conn_cache *conn_cache,
                       struct sdap_options *opts)
{
    struct ipa_sudo_get_cmds_state *state;
    struct tevent_req *req;
    errno_t ret = EOK;
    errno_t cmds_ret = EOK;

    print_rules("IPA sudoers(ipa_sudo_get_cmds_send):", ipa_rules, ipa_rules_count);
    // FIXME:
    // REVIEW(FEEDBACK) - state by se mel jmenovat stejne jako _send, tj tady
    // ipa_sudo_get_cmds_state a v kodu by mel byt v .c souboru hned
    // pred _send()
    //
    // V pavlovem modulu jsem ty definice state struktur musel vynest do header
    // souboru, abych je mohl pouzit (providers/ldap/sdap_async_sudo.h), je to 
    // ok nebo to musim prepsat tak, abych vse co z tech struktur pouzivam
    // predaval pres parametry?
    //
    //req = tevent_req_create(mem, &state, struct ipa_sudo_get_cmds_state);
    req = tevent_req_create(mem, &state, struct ipa_sudo_get_cmds_state);
    if (!req) {
        return NULL;
    }

    state->be_ctx = be_ctx;
    state->sysdb = be_ctx->domain->sysdb;
    state->ev = be_ctx->ev;
    state->conn_cache = conn_cache;
    state->opts = opts;
    state->sdap_op = NULL;
    state->scope = LDAP_SCOPE_SUBTREE;
    /* req from sdap_sudo_load_sudoers_send, probably not needed anymore*/
    //state->req = req_sdap;  
    state->dp_error = DP_ERR_OK;
    state->error = EOK;
    state->basedn = talloc_strdup(state, IPA_SUDO_CMDS_BASEDN);
    state->rules = talloc_zero(state, struct sudo_rules);
    state->timeout = dp_opt_get_int(opts->basic, SDAP_SEARCH_TIMEOUT);
    state->filter = NULL;

    if (state->basedn == NULL) {
        ret = ENOMEM;
        goto immediately;
    }
 
    if (state->rules == NULL) {
        ret = ENOMEM;
        goto immediately;
    }

    cmds_ret = ipa_sudo_build_cmds_filter(state, state->sysdb, ipa_rules, 
                                          ipa_rules_count, &(state->filter));
    if (cmds_ret != EOK && cmds_ret != ENOENT) {
        goto immediately;
    }

    ret = ipa_sudo_export_sudoers(state, state->sysdb,
                                  ipa_rules, 
                                  ipa_rules_count, 
                                  &(state->rules->sudoers),
                                  &(state->rules->sudoers_count),
                                  &(state->rules->cmds_index),
                                  req);
    //print_rules(state->rules->sudoers, state->rules->sudoers_count);
    if (ret != EOK || cmds_ret == ENOENT) {
        /* if building cmds filter returned ENOENT then we don't need to
         * download any ipa sudo commands */
        goto immediately;
    }

    ret = ipa_sudo_get_cmds_retry(req);
    if (ret == EOK) {
        /* connection req sent successfully, we can return without finishing
         * this request */
        return req;
    }

immediately:
    if (ret == EOK) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
    tevent_req_post(req, state->ev);

    return req;
}

static errno_t ipa_sudo_get_cmds_retry(struct tevent_req *req)
{
    struct ipa_sudo_get_cmds_state *state;
    struct tevent_req *subreq;
    errno_t ret = EOK;

    state = tevent_req_data(req, struct ipa_sudo_get_cmds_state);

    if (be_is_offline(state->be_ctx)) {
        state->dp_error = DP_ERR_OFFLINE;
        state->error = EAGAIN;
        return EOK;
    }

    if (state->sdap_op == NULL) {
        state->sdap_op = sdap_id_op_create(state, 
                state->conn_cache);
        if (state->sdap_op == NULL) {
            DEBUG(SSSDBG_CRIT_FAILURE, ("sdap_id_op_create() failed\n"));
            state->dp_error = DP_ERR_FATAL;
            state->error = EIO;
            return EIO;
        }
    }

    subreq = sdap_id_op_connect_send(state->sdap_op, state, &ret);
    if (subreq == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("sdap_id_op_connect_send() failed: %d(%s)\n", ret, strerror(ret)));
        talloc_zfree(state->sdap_op);
        state->dp_error = DP_ERR_FATAL;
        state->error = ret;
        return ret;
    }

    tevent_req_set_callback(subreq, ipa_sudo_get_cmds_connect_done, req);

    return ret;
}

static void ipa_sudo_get_cmds_connect_done(struct tevent_req *subreq)
{
    struct ipa_sudo_get_cmds_state *state;
    struct tevent_req *req;
    int dp_error;
    int ret;

    /* req from ipa_sudo_get_cmds_send */
    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct ipa_sudo_get_cmds_state);

    ret = sdap_id_op_connect_recv(subreq, &dp_error);
    talloc_zfree(subreq);

    if (dp_error == DP_ERR_OFFLINE) {
        talloc_zfree(state->sdap_op);
        state->dp_error = DP_ERR_OFFLINE;
        state->error = EAGAIN;
        tevent_req_done(req);
        return;
    } else if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("IPA SUDO LDAP connection failed - %s\n", strerror(ret)));
        goto fail;
    }

    DEBUG(SSSDBG_TRACE_FUNC, ("IPA SUDO LDAP connection successful\n"));

    struct sdap_attr_map *map = state->opts->ipa_sudocmds_map;

    /* create attrs from map */
    ret = build_attrs_from_map(state, map, SDAP_OPTS_SUDO_CMD, NULL, &state->attrs, NULL);
    if (ret != EOK) {
        goto fail;
    }

    DEBUG(SSSDBG_TRACE_FUNC, ("Searching for IPA SUDO commands\n"));
    printf("timeout: %d\n", state->timeout);

    subreq = sdap_get_generic_send(state,
                                   state->ev,
                                   state->opts,
                                   sdap_id_op_handle(state->sdap_op),
                                   state->basedn,
                                   state->scope,
                                   state->filter,
                                   state->attrs,
                                   map,
                                   SDAP_OPTS_SUDO_CMD,
                                   state->timeout,
                                   true);
    if (subreq == NULL) {
        goto fail;
    }

    tevent_req_set_callback(subreq, ipa_sudo_load_ipa_cmds_process, req);

fail:
    state->error = ret;
    tevent_req_error(req, ret);
}

static void ipa_sudo_load_ipa_cmds_process(struct tevent_req *subreq)
{
    struct ipa_sudo_get_cmds_state *state;
    struct sysdb_attrs **attrs;
    struct tevent_req *req;
    size_t count;
    int ret;

    /* req from ipa_sudo_get_cmds_send */
    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct ipa_sudo_get_cmds_state);
 
    DEBUG(SSSDBG_TRACE_FUNC,
          ("Receiving commands for IPA SUDO rules with base [%s]\n",
           state->basedn));

    /* get IPA sudo commands */
    ret = sdap_get_generic_recv(subreq, state, &count, &attrs);
    talloc_zfree(subreq);
    if (ret) {
        goto fail;
    }

    /* if we don't have any rules but downloaded some commands then something
     * went wrong! */
    if (state->rules->sudoers_count == 0 || state->rules->sudoers == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE,
            ("We got some ipa sudo commands but we have no sudo rules\n"));
        goto fail;
    }

    print_rules("IPA sudo commands:", attrs, count);
    ret = ipa_sudo_export_cmds(state, 
                               state->rules->sudoers, 
                               state->rules->sudoers_count,
                               state->rules->cmds_index, 
                               attrs, count);
    if (ret != EOK) {
        goto fail;
    }

    DEBUG(SSSDBG_TRACE_FUNC, ("All IPA SUDO rules successfully exported into "
                              "native LDAP SUDO scheme. Giving control back to "
                              "the LDAP SUDO Provider.\n"));
    print_rules("Exported IPA sudoers:", state->rules->sudoers, state->rules->sudoers_count);

fail:
    if (ret == EOK) {
        /* ipa_sudo_get_cmds_send */
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
}

int ipa_sudo_get_cmds_recv(struct tevent_req *req,
                           TALLOC_CTX *mem_ctx,
                           size_t *reply_count,
                           struct sysdb_attrs ***reply)
{
    struct ipa_sudo_get_cmds_state *ipa_state;

    ipa_state = tevent_req_data(req, struct ipa_sudo_get_cmds_state);

    *reply_count = ipa_state->rules->sudoers_count;
    *reply = talloc_steal(mem_ctx, ipa_state->rules->sudoers);

    return EOK;
}
