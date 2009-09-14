/*
   SSSD

   sss_useradd

   Copyright (C) Jakub Hrozek <jhrozek@redhat.com>        2009
   Copyright (C) Simo Sorce <ssorce@redhat.com>           2009

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

#include <stdio.h>
#include <stdlib.h>
#include <talloc.h>
#include <popt.h>
#include <errno.h>
#include <unistd.h>

#include "util/util.h"
#include "db/sysdb.h"
#include "tools/tools_util.h"
#include "util/sssd-i18n.h"

/* Default settings for user attributes */
#define CONFDB_DFL_SECTION "config/user_defaults"

#define DFL_SHELL_ATTR     "defaultShell"
#define DFL_BASEDIR_ATTR   "baseDirectory"

#define DFL_SHELL_VAL      "/bin/bash"
#define DFL_BASEDIR_VAL    "/home"

static void get_gid_callback(void *ptr, int error, struct ldb_result *res)
{
    struct tools_ctx *tctx = talloc_get_type(ptr, struct tools_ctx);

    if (error) {
        tctx->error = error;
        return;
    }

    switch (res->count) {
    case 0:
        tctx->error = ENOENT;
        break;

    case 1:
        tctx->octx->gid = ldb_msg_find_attr_as_uint(res->msgs[0], SYSDB_GIDNUM, 0);
        if (tctx->octx->gid == 0) {
            tctx->error = ERANGE;
        }
        break;

    default:
        tctx->error = EFAULT;
        break;
    }
}

/* Returns a gid for a given groupname. If a numerical gid
 * is given, returns that as integer (rationale: shadow-utils)
 * On error, returns -EINVAL
 */
static int get_gid(struct tools_ctx *tctx, const char *groupname)
{
    char *end_ptr;
    int ret;

    errno = 0;
    tctx->octx->gid = strtoul(groupname, &end_ptr, 10);
    if (groupname == '\0' || *end_ptr != '\0' ||
        errno != 0 || tctx->octx->gid == 0) {
        /* Does not look like a gid - find the group name */

        ret = sysdb_getgrnam(tctx->octx, tctx->sysdb,
                             tctx->octx->domain, groupname,
                             get_gid_callback, tctx);
        if (ret != EOK) {
            DEBUG(1, ("sysdb_getgrnam failed: %d\n", ret));
            goto done;
        }

        tctx->error = EOK;
        tctx->octx->gid = 0;
        while ((tctx->error == EOK) && (tctx->octx->gid == 0)) {
            tevent_loop_once(tctx->ev);
        }

        if (tctx->error) {
            DEBUG(1, ("sysdb_getgrnam failed: %d\n", ret));
            goto done;
        }
    }

done:
    return ret;
}

static void add_user_transaction(struct tevent_req *req)
{
    int ret;
    struct tools_ctx *tctx = tevent_req_callback_data(req,
                                                struct tools_ctx);
    struct tevent_req *subreq;

    ret = sysdb_transaction_recv(req, tctx, &tctx->handle);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }
    talloc_zfree(req);

    /* useradd */
    ret = useradd(tctx, tctx->ev,
                  tctx->sysdb, tctx->handle, tctx->octx);
    if (ret != EOK) {
        goto fail;
    }

    subreq = sysdb_transaction_commit_send(tctx, tctx->ev, tctx->handle);
    if (!subreq) {
        ret = ENOMEM;
        goto fail;
    }
    tevent_req_set_callback(subreq, tools_transaction_done, tctx);
    return;

fail:
    /* free transaction and signal error */
    talloc_zfree(tctx->handle);
    tctx->transaction_done = true;
    tctx->error = ret;
}

int main(int argc, const char **argv)
{
    uid_t pc_uid = 0;
    const char *pc_group = NULL;
    const char *pc_gecos = NULL;
    const char *pc_home = NULL;
    char *pc_shell = NULL;
    int pc_debug = 0;
    const char *pc_username = NULL;
    struct poptOption long_options[] = {
        POPT_AUTOHELP
        { "debug", '\0', POPT_ARG_INT | POPT_ARGFLAG_DOC_HIDDEN, &pc_debug, 0, _("The debug level to run with"), NULL },
        { "uid",   'u', POPT_ARG_INT, &pc_uid, 0, _("The UID of the user"), NULL },
        { "gid",   'g', POPT_ARG_STRING, &pc_group, 0, _("The GID or group name of the user"), NULL },
        { "gecos", 'c', POPT_ARG_STRING, &pc_gecos, 0, _("The comment string"), NULL },
        { "home",  'h', POPT_ARG_STRING, &pc_home, 0, _("Home directory"), NULL },
        { "shell", 's', POPT_ARG_STRING, &pc_shell, 0, _("Login shell"), NULL },
        { "groups", 'G', POPT_ARG_STRING, NULL, 'G', _("Groups"), NULL },
        POPT_TABLEEND
    };
    poptContext pc = NULL;
    struct tevent_req *req;
    struct tools_ctx *tctx = NULL;
    char *groups = NULL;
    int ret;

    debug_prg_name = argv[0];

    ret = set_locale();
    if (ret != EOK) {
        DEBUG(1, ("set_locale failed (%d): %s\n", ret, strerror(ret)));
        ERROR("Error setting the locale\n");
        ret = EXIT_FAILURE;
        goto fini;
    }

    /* parse parameters */
    pc = poptGetContext(NULL, argc, argv, long_options, 0);
    poptSetOtherOptionHelp(pc, "USERNAME");
    while ((ret = poptGetNextOpt(pc)) > 0) {
        if (ret == 'G') {
            groups = poptGetOptArg(pc);
            if (!groups) {
                ret = -1;
                break;
            }
        }
    }

    debug_level = pc_debug;

    if (ret != -1) {
        usage(pc, poptStrerror(ret));
        ret = EXIT_FAILURE;
        goto fini;
    }

    /* username is an argument without --option */
    pc_username = poptGetArg(pc);
    if (pc_username == NULL) {
        usage(pc, (_("Specify user to add\n")));
        ret = EXIT_FAILURE;
        goto fini;
    }

    CHECK_ROOT(ret, debug_prg_name);

    ret = init_sss_tools(&tctx);
    if (ret != EOK) {
        DEBUG(1, ("init_sss_tools failed (%d): %s\n", ret, strerror(ret)));
        ERROR("Error initializing the tools\n");
        ret = EXIT_FAILURE;
        goto fini;
    }

    /* if the domain was not given as part of FQDN, default to local domain */
    ret = parse_name_domain(tctx, pc_username);
    if (ret != EOK) {
        ERROR("Cannot get domain information\n");
        ret = EXIT_FAILURE;
        goto fini;
    }

    if (groups) {
        ret = parse_groups(tctx, groups, &tctx->octx->addgroups);
        if (ret != EOK) {
            DEBUG(1, ("Cannot parse groups to add the user to\n"));
            ERROR("Internal error while parsing parameters\n");
            goto fini;
        }
    }

    /* Same as shadow-utils useradd, -g can specify gid or group name */
    if (pc_group != NULL) {
        ret = get_gid(tctx, pc_group);
        if (ret != EOK) {
            ERROR("Cannot get group information for the user\n");
            ret = EXIT_FAILURE;
            goto fini;
        }
    }

    tctx->octx->uid = pc_uid;

    /*
     * Fills in defaults for ops_ctx user did not specify.
     */
    ret = useradd_defaults(tctx, tctx->confdb, tctx->octx,
                           pc_gecos, pc_home, pc_shell);
    if (ret != EOK) {
        ERROR("Cannot set default values\n");
        ret = EXIT_FAILURE;
        goto fini;
    }

    /* arguments processed, go on to actual work */
    if (id_in_range(tctx->octx->uid, tctx->octx->domain) != EOK) {
        ERROR("The selected UID is outside the allowed range\n");
        ret = EXIT_FAILURE;
        goto fini;
    }

    /* useradd */
    req = sysdb_transaction_send(tctx->octx, tctx->ev, tctx->sysdb);
    if (!req) {
        DEBUG(1, ("Could not start transaction (%d)[%s]\n", ret, strerror(ret)));
        ERROR("Transaction error. Could not modify user.\n");
        ret = EXIT_FAILURE;
        goto fini;
    }
    tevent_req_set_callback(req, add_user_transaction, tctx);

    while (!tctx->transaction_done) {
        tevent_loop_once(tctx->ev);
    }

    if (tctx->error) {
        ret = tctx->error;
        switch (ret) {
            case EEXIST:
                ERROR("A user with the same name or UID already exists\n");
                break;

            default:
                DEBUG(1, ("sysdb operation failed (%d)[%s]\n",
                          ret, strerror(ret)));
                ERROR("Transaction error. Could not add user.\n");
                break;
        }
        ret = EXIT_FAILURE;
        goto fini;
    }

    ret = EXIT_SUCCESS;

fini:
    poptFreeContext(pc);
    talloc_free(tctx);
    free(groups);
    exit(ret);
}
