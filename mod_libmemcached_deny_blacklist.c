#include "conf.h"
#include "libmemcached/memcached.h"
#include "libmemcached/memcached_util.h"

#include <stdbool.h>
#include <utmp.h>

module libmemcached_deny_blacklist_module;

#define MODULE_NAME libmemcached_deny_blacklist_module.name

/* rw */
static bool ignore_memcached_down = true;
static bool is_set_server = false;
static memcached_st *memcached_deny_blacklist_mmc = NULL;

#ifdef DEBUG
static int walk_table(const void *key_data,
                      void *value_data,
                      size_t value_datasz,
                      void *user_data) {
    pr_log_debug(DEBUG2, "%s %s => %s\n", MODULE_NAME, (char *)key_data, (char *)value_datasz);
    return 0;
}
#endif

static void lmd_cleanup() {
    if(memcached_deny_blacklist_mmc) {
        memcached_free(memcached_deny_blacklist_mmc);
        memcached_deny_blacklist_mmc = NULL;
    }
}

static void lmd_postparse_ev(const void *event_data, void *user_data) {
    memcached_stat_st *unused;
    memcached_return rc;

    unused = memcached_stat(memcached_deny_blacklist_mmc, NULL, &rc);
    if(rc != MEMCACHED_SUCCESS) {
        pr_log_pri(PR_LOG_WARNING,
            "%s: Failed connect to memcached."
            "Please check memcached is alive", MODULE_NAME);

        if(ignore_memcached_down){
            return;
        }else {
            exit(1);
        }
    }
}

static bool is_allowed(cmd_rec *cmd, pr_netaddr_t *na) {
    int i;
    config_rec *c;
    array_header *allowed_acls;

    c = find_config(cmd->server->conf, CONF_PARAM, "LMDBAllow", FALSE);
    if(NULL == c)
        return false;

    allowed_acls = c->argv[0];
    if(NULL == allowed_acls) {
        pr_log_auth(PR_LOG_ERR,
          "%s: pr_table_t is NULL. something fatal", MODULE_NAME);
        return false;
    }

#ifdef DEBUG
    pr_table_do(allowed_acls, walk_table, NULL, 0);
#endif

    pr_netacl_t **elts = allowed_acls->elts;
    for (i = 0; i < allowed_acls->nelts; i++) {
        pr_netacl_t *acl = elts[i];
        if(pr_netacl_match(acl, na) == 1) {
            pr_log_auth(PR_LOG_INFO,
                "%s: client IP matched with LMDBAllow '%s'. Skip last process",
                        MODULE_NAME, pr_netacl_get_str(cmd->tmp_pool, acl));
            return true;
        }
    }

    return false;
}

static void lmd_restart_ev(const void *event_data, void *user_data) {
    if(memcached_deny_blacklist_mmc){
        memcached_free(memcached_deny_blacklist_mmc);
        memcached_deny_blacklist_mmc = NULL;
    }
    /* restartの前にmodule-unloadが呼ばれるのかな? */
    pr_log_debug(DEBUG5, "%s at core.module-unload", MODULE_NAME);
}

static int lmd_init(void) {
    memcached_deny_blacklist_mmc = memcached_create(NULL);
    if(!memcached_deny_blacklist_mmc) {
        pr_log_pri(PR_LOG_ERR, "Fatal %s: Out of memory", MODULE_NAME);
        exit(1);
    }

    pr_event_register(&libmemcached_deny_blacklist_module,
        "core.postparse", lmd_postparse_ev, NULL);

    pr_event_register(&libmemcached_deny_blacklist_module,
         "core.module-unload", lmd_restart_ev, NULL);

/*
    pr_event_register(&libmemcached_deny_blacklist_module,
         "core.connect", lmd_connect_ev, NULL);
*/

    return 0;
}

MODRET set_lmd_ignore_memcached_down(cmd_rec *cmd) {
  int ignore = -1;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  ignore = get_boolean(cmd, 1);
  if (ignore == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  if(ignore == TRUE) {
      ignore_memcached_down = true;
  }

  return PR_HANDLED(cmd);
}

MODRET add_lmd_allow_from(cmd_rec *cmd) {
    config_rec *c;
    int i;
    array_header *allowed_acls;

    if(cmd->argc < 2 )
        CONF_ERROR(cmd, "argument missing");

    CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL);

    /* argv => LMDMemcachedHost 127.0.0.1 192.168.0.1 ... */
    c = find_config(main_server->conf, CONF_PARAM, "LMDBAllow", FALSE);
    if(c && c->argv[0]) {
        allowed_acls = c->argv[0];
    } else {
        c = add_config_param(cmd->argv[0], 0, NULL);
        c->argv[0] = allowed_acls =
          make_array(cmd->server->pool, 0, sizeof(char *));
    }

    for(i=1; i < cmd->argc; i++) {
        char *entry = cmd->argv[i];
        if (strcasecmp(entry, "all") == 0 ||
            strcasecmp(entry, "none") == 0) {
            break;
        }
        pr_netacl_t *acl = pr_netacl_create(cmd->server->pool, entry);
        *((pr_netacl_t **) push_array(allowed_acls)) = acl;
        pr_log_debug(DEBUG2,
            "%s: add LMDBAllow[%d] %s", MODULE_NAME, i, entry);
    }

    return PR_HANDLED(cmd);
}

MODRET add_lmd_allow_user(cmd_rec *cmd) {
    config_rec *c;
    int i;
    pr_table_t *explicit_users;

    if(cmd->argc < 2)
        CONF_ERROR(cmd, "missing argument");

    CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL);

    /* argv => LMDBAllowedUser nobody nobody1 nobody2 */
    c = find_config(main_server->conf, CONF_PARAM, "LMDBAllowedUser", FALSE);
    if(c && c->argv[0]) {
        explicit_users = c->argv[0];
    } else {
        c = add_config_param(cmd->argv[0], 0, NULL);
        c->argv[0] = explicit_users = pr_table_alloc(main_server->pool, 0);
    }

    for(i=1; i < cmd->argc; i++) {
        const char *account = pstrdup(main_server->pool, cmd->argv[i]);
        if(pr_table_exists(explicit_users, account) > 0) {
            pr_log_debug(DEBUG2,
                "%s: %s is already registerd", MODULE_NAME, account);
            continue;
        }

        if(pr_table_add_dup(explicit_users, account, "y", 0) < 0){
            pr_log_pri(PR_LOG_ERR,
                "%s: failed pr_table_add_dup(): %s",
                 MODULE_NAME, strerror(errno));
            exit(1);
        }
        pr_log_debug(DEBUG2,
            "%s: add LMDBAllowedUser[%d] %s", MODULE_NAME, i, account);
    }

    return PR_HANDLED(cmd);
}

MODRET add_lmd_allow_user_regex(cmd_rec *cmd) {
    array_header *list;
    pr_regex_t *pre;
    int i, res;
    config_rec *c;

    if(cmd->argc < 2)
        CONF_ERROR(cmd, "missing argument");
    CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL);

    /* argv => LMDBAllowUserRegex ^test */
    c = find_config(cmd->server->conf, CONF_PARAM, "LMDBAllowedUserRegex", FALSE);
    if(c && c->argv[0]) {
        list = c->argv[0];
    } else {
        c = add_config_param(cmd->argv[0], 0, NULL);
        c->argv[0] = list = make_array(cmd->server->pool, 0, sizeof(regex_t *));
    }

    for(i=1; i < cmd->argc; i++) {
        pre = pr_regexp_alloc(&libmemcached_deny_blacklist_module);
        res = pr_regexp_compile(pre, cmd->argv[i], REG_NOSUB);
        if (res != 0) {
            char errstr[200] = {'\0'};
            pr_regexp_error(res, pre, errstr, sizeof(errstr));
            pr_regexp_free(NULL, pre);
            CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", cmd->argv[i], "' failed "
               "regex compilation: ", errstr, NULL));
        }
        *((pr_regex_t **) push_array(list)) = pre;
        pr_log_debug(DEBUG2,
            "%s: add LMDBAllowedUserRegex[%d] %s", MODULE_NAME, i, cmd->argv[i]);
    }

    return PR_HANDLED(cmd);
}

MODRET add_lmd_memcached_host(cmd_rec *cmd) {
    int i;
    memcached_return rc;
    memcached_server_st *server = NULL;

    if(cmd->argc < 2 )
        CONF_ERROR(cmd, "argument missing");

    CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL);

    /* NOTICE: i = 1 */
    for(i=1; i < cmd->argc; i++) {
        const char *arg = cmd->argv[i];
        server = memcached_servers_parse(arg);
        rc = memcached_server_push(memcached_deny_blacklist_mmc, server);
        if(rc != MEMCACHED_SUCCESS){
            pr_log_auth(PR_LOG_ERR,
              "Fatal %s: failed memcached_strerror(): %s",
              MODULE_NAME, memcached_strerror(memcached_deny_blacklist_mmc, rc));
            exit(1);
        }
        pr_log_debug(DEBUG2,
            "%s: add memcached server %s", MODULE_NAME, arg);
    }
    is_set_server = true;
    return PR_HANDLED(cmd);
}

/* todo */
static int lmd_timeout_callback(CALLBACK_FRAME) {
    pr_log_auth(PR_LOG_WARNING,
        "%s: memcached timeout", MODULE_NAME);
    return 0;
}

static bool is_cache_exits(memcached_st *mmc,
                           const char *key) {
    int timer_id;
    memcached_return rc;
    char *cached_value;
    size_t value_len;
    uint32_t flag;

    /* todo */
    timer_id = pr_timer_add(3, -1, NULL, lmd_timeout_callback, "memcached_get");
    cached_value = memcached_get(mmc, key, strlen(key), &value_len, &flag, &rc);
    pr_timer_remove(timer_id, NULL);

    /* no cache */
    if(MEMCACHED_NOTFOUND == rc)
        return false;

    /* failed by other reason */
    if(MEMCACHED_SUCCESS  != rc &&
       MEMCACHED_NOTFOUND != rc) {

      pr_log_auth(PR_LOG_NOTICE,
        "%s: failed memcached_get() %s. but IGNORE",
         MODULE_NAME, memcached_strerror(mmc, rc));
      return false;
    }

    /* cache not fond */
    if(NULL == cached_value)
        return false;

    /* something wrong */
    if(0 == value_len)
        return false;

    free(cached_value);
    return true;
}

static bool is_allowed_user(cmd_rec *cmd, const char *account) {
    config_rec *c;

    /* ハッシュテーブルにアカウントがあるか否か */
    c = find_config(cmd->server->conf, CONF_PARAM, "LMDBAllowedUser", FALSE);
    if(c && c->argv[0]) {
        pr_table_t *explicit_users = c->argv[0];
        if(pr_table_exists(explicit_users, account) > 0 ) {
            pr_log_debug(DEBUG2,
                "%s: '%s' match with LMDBAllowedUser", MODULE_NAME, account);
            return true;
        }
    }

    /* 正規表現にマッチするか否か */
    c = find_config(cmd->server->conf, CONF_PARAM, "LMDBAllowedUserRegex", FALSE);
    if(c && c->argv[0]) {
        int i;
        array_header *regex_list = c->argv[0];
        regex_t ** elts = regex_list->elts;

        for (i = 0; i < regex_list->nelts; i++) {
            regex_t *preg = elts[i];
            if(regexec(preg, account, 0, NULL, 0) == 0) {
                pr_log_debug(DEBUG2,
                    "%s: '%s' match with LMDBAllowedUserRegex", MODULE_NAME, account);
                return true;
            }
        }
    }

    return false;
}

MODRET lmd_deny_blacklist_post_pass(cmd_rec *cmd) {
    /*
      mod_authを通過するまでは session.userは空の様子
      const char *account  = session.user;
    */
    const char *account   = NULL;
    const char *remote_ip = NULL;

    /* return IP unless found hostname */
    account = get_param_ptr(cmd->server->conf, "UserName", FALSE);
    remote_ip = pr_netaddr_get_ipstr(pr_netaddr_get_sess_remote_addr());

    if(false == is_set_server) {
        pr_log_auth(PR_LOG_WARNING, "%s: memcached_server not set", MODULE_NAME);
        lmd_cleanup();
        return PR_DECLINED(cmd);
    }

    if(is_allowed_user(cmd, account) == true) {
        pr_log_auth(PR_LOG_NOTICE,
           "%s: '%s' is allowed to login. skip last process", MODULE_NAME, account);
        lmd_cleanup();
        return PR_DECLINED(cmd);
    }

    /* allow explicily */
    if(is_allowed(cmd, session.c->remote_addr) == true) {
        return PR_DECLINED(cmd);
    }

    /* check whether account is registerd in blacklist or not */
    if(is_cache_exits(memcached_deny_blacklist_mmc, account) == true) {
        pr_log_auth(PR_LOG_NOTICE,
            "%s: denied '%s@%s'. Account found in blacklist(memcached)",
                 MODULE_NAME, account, remote_ip);
        pr_response_send(R_530, _("Login denied temporary (Account found in blacklist)"));
        end_login(0);
    }

    /* check whether remote IP is registerd in blacklist or not */
    if(is_cache_exits(memcached_deny_blacklist_mmc, remote_ip) == true) {
        pr_log_auth(PR_LOG_NOTICE,
            "%s: denied '%s@%s'. IP found in blacklist(memcached)",
                 MODULE_NAME, account, remote_ip);
        pr_response_send(R_530, _("Login denied temporary (IP found in blacklist)"));
        end_login(0);
    }

    pr_log_debug(DEBUG2,
            "%s: not found in blaclist. '%s@%s' is allowed to Login",
                 MODULE_NAME, account, remote_ip);

    lmd_cleanup();
    return PR_DECLINED(cmd);
}

static conftable lmd_deny_blacklist_conftab[] = {
    { "LMDBAllowedUser",      add_lmd_allow_user,       NULL },
    { "LMDBAllowedUserRegex", add_lmd_allow_user_regex, NULL },
    { "LMDBMemcachedHost",    add_lmd_memcached_host,   NULL },
    { "LMDBAllow",            add_lmd_allow_from,       NULL },
    { NULL }
};
 
static cmdtable lmd_deny_blacklist_cmdtab[] = {
    { POST_CMD, C_USER, G_NONE, lmd_deny_blacklist_post_pass, FALSE, FALSE, CL_AUTH },
    { 0, NULL }
};

module libmemcached_deny_blacklist_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "libmemcached_deny_blacklist",

  /* Module configuration directive table */
  lmd_deny_blacklist_conftab,

  /* Module command handler table */
  lmd_deny_blacklist_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  lmd_init ,

  /* Session initialization function */
  NULL,
};
