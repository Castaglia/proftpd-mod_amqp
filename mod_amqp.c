/*
 * ProFTPD - mod_amqp
 * Copyright (c) 2017 TJ Saunders
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Archive: mod_amqp.a$
 * $Libraries: -lrabbitmq$
 */

#include "mod_amqp.h"
#include "jot.h"
#include <amqp.h>
#include <amqp_tcp_socket.h>

#define AMQP_DEFAULT_VHOST		"/"
#define AMQP_DEFAULT_USERNAME		"guest"
#define AMQP_DEFAULT_PASSWORD		"guest"

extern xaset_t *server_list;

/* From response.c */
extern pr_response_t *resp_list, *resp_err_list;

module amqp_module;

int amqp_logfd = -1;
pool *amqp_pool = NULL;

static int amqp_engine = FALSE;
static unsigned long amqp_opts = 0UL;

/* XXX Wrap this in our own struct, for supporting other libs, too. */
static amqp_connection_state_t amqp_conn;
static amqp_channel_t amqp_chan;

/* AMQPOptions */
#define AMQP_OPT_WITH_MESSAGE_ID	0x0001
#define AMQP_OPT_WITH_TIMESTAMP		0x0002
#define AMQP_OPT_WITH_USER_ID		0x0004
#define AMQP_OPT_WITH_APP_ID		0x0008

static const char *trace_channel = "amqp";

/* Necessary function prototypes. */
static int amqp_sess_init(void);

static void amqp_trace_reply(pool *p, amqp_rpc_reply_t reply,
    const char *text) {

  switch (reply.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      break;

    case AMQP_RESPONSE_NONE:
      pr_trace_msg(trace_channel, 3, "%s: missing RPC reply type in response",
        text);
      break;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      pr_trace_msg(trace_channel, 3, "%s: %s", text,
        amqp_error_string2(reply.library_error));
      break;

    case AMQP_RESPONSE_SERVER_EXCEPTION: {
      switch (reply.reply.id) {
        case AMQP_CONNECTION_CLOSE_METHOD: {
          amqp_connection_close_t *meth;

          meth = reply.reply.decoded;
          pr_trace_msg(trace_channel, 3,
            "%s: connection exception '%.*s' (%uh)", text,
            (int) meth->reply_text.len, (char *) meth->reply_text.bytes,
            meth->reply_code);
          break;
        }

        case AMQP_CHANNEL_CLOSE_METHOD: {
          amqp_channel_close_t *meth;

          meth = reply.reply.decoded;
          pr_trace_msg(trace_channel, 3,
            "%s: channel exception '%.*s' (%uh)", text,
            (int) meth->reply_text.len, (char *) meth->reply_text.bytes,
            meth->reply_code);
          break;
        }

        default:
          pr_trace_msg(trace_channel, 3,
            "%s: unknown broker error: method %s (0x%08x)", text,
            amqp_method_name(reply.reply.id), reply.reply.id);
      }

      break;
    }

    default:
      pr_trace_msg(trace_channel, 3, "%s: unknown reply-type: %d", text,
        reply.reply_type);
  }
}

static int amqp_open_conn(pool *p, config_rec *c) {
  register unsigned int i;
  int connected = FALSE;
  array_header *addrs;
  amqp_socket_t *sock;
  amqp_rpc_reply_t reply;
  const char *ip_addr, *vhost, *username = NULL, *password = NULL;
  unsigned int port;

  amqp_conn = amqp_new_connection();
  if (amqp_conn == NULL) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "error allocating memory for RabbitMQ connection");
    errno = ENOMEM;
    return -1;
  }

  sock = amqp_tcp_socket_new(amqp_conn);
  if (sock == NULL) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "error allocating memory for AMQP socket");
    amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = ENOMEM;
    return -1;
  }

  addrs = c->argv[0];

  for (i = 0; i < addrs->nelts; i++) {
    int res;
    const pr_netaddr_t *addr;

    pr_signals_handle();

    addr = ((pr_netaddr_t **) addrs->elts)[i];
    ip_addr = pr_netaddr_get_ipstr(addr);
    port = ntohs(pr_netaddr_get_port(addr));

    pr_trace_msg(trace_channel, 12, "connecting to %s:%u", ip_addr, port);
    res = amqp_socket_open(sock, ip_addr, port);
    if (res == AMQP_STATUS_OK) {
      connected = TRUE;
      break;
    }

    pr_trace_msg(trace_channel, 6, "error connecting to %s:%u: %s", ip_addr,
      port, amqp_error_string2(res));
  }

  if (connected == FALSE) {
    amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = ENOENT;
    return -1;
  }

  vhost = c->argv[1];
  if (vhost == NULL) {
    vhost = AMQP_DEFAULT_VHOST;
  }

  username = c->argv[2];
  if (username == NULL) {
    username = AMQP_DEFAULT_USERNAME;
  }

  password = c->argv[3];
  if (password == NULL) {
    password = AMQP_DEFAULT_PASSWORD;
  }

  pr_trace_msg(trace_channel, 12,
    "logging into %s:%u with vhost '%s', username '%s'", ip_addr, port,
    vhost, username);
  reply = amqp_login(amqp_conn, vhost, AMQP_DEFAULT_MAX_CHANNELS,
    AMQP_DEFAULT_FRAME_SIZE, AMQP_DEFAULT_HEARTBEAT, AMQP_SASL_METHOD_PLAIN,
    username, password);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_trace_reply(p, reply, "broker login error");

    amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = EPERM;
    return -1;
  }

  pr_trace_msg(trace_channel, 12,
    "authenticated to broker %s:%u with vhost '%s', username '%s'", ip_addr,
    port, vhost, username);
  return 0;
}

static int amqp_close_conn(pool *p) {
  int res, reason = AMQP_REPLY_SUCCESS;
  amqp_rpc_reply_t reply;

  reply = amqp_channel_close(amqp_conn, amqp_chan, reason);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_trace_reply(p, reply, "channel close error");
  }

  reply = amqp_connection_close(amqp_conn, reason);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_trace_reply(p, reply, "connection close error");
  }

  res = amqp_destroy_connection(amqp_conn);
  if (res != AMQP_STATUS_OK) {
    pr_trace_msg(trace_channel, 3, "connection destroy error: %s",
      amqp_error_string2(res));
  }

  amqp_conn = NULL;
  amqp_chan = 0;

  return 0;
}

/* Configuration handlers
 */

/* usage: AMQPEngine on|off */
MODRET set_amqpengine(cmd_rec *cmd) {
  int engine = 1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: AMQPLog path|"none" */
MODRET set_amqplog(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: AMQPLogOnEvent events log-fmt [exchange] */
MODRET set_amqplogonevent(cmd_rec *cmd) {
  config_rec *c, *logfmt_config;
  const char *fmt_name, *rules;
  char *exchange = NULL;
  unsigned char *log_fmt = NULL;
  pr_jot_filters_t *jot_filters;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL);

  c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);

  rules = cmd->argv[1];
  jot_filters = pr_jot_filters_create(c->pool, rules,
    PR_JOT_FILTER_TYPE_COMMANDS_WITH_CLASSES,
    PR_JOT_FILTER_FL_ALL_INCL_ALL);
  if (jot_filters == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use events '", rules,
      "': ", strerror(errno), NULL));
  }

  fmt_name = cmd->argv[2];

  if (cmd->argc == 4) {
    exchange = cmd->argv[3];
  }

  /* Make sure that the given LogFormat name is known. */
  logfmt_config = find_config(cmd->server->conf, CONF_PARAM, "LogFormat",
    FALSE);
  while (logfmt_config != NULL) {
    pr_signals_handle();

    if (strcmp(fmt_name, logfmt_config->argv[0]) == 0) {
      log_fmt = logfmt_config->argv[1];
      break;
    }

    logfmt_config = find_config_next(logfmt_config, logfmt_config->next,
      CONF_PARAM, "LogFormat", FALSE);
  }

  if (log_fmt == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "no LogFormat '", fmt_name,
      "' configured", NULL));
  }

  c->argv[0] = jot_filters;
  c->argv[1] = pstrdup(c->pool, fmt_name);
  c->argv[2] = log_fmt;
  c->argv[3] = exchange;

  return PR_HANDLED(cmd);
}

/* usage: AMQPOptions ... */
MODRET set_amqpoptions(cmd_rec *cmd) {
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "WithMessageID") == 0) {
      opts |= AMQP_OPT_WITH_MESSAGE_ID;

    } else if (strcmp(cmd->argv[i], "WithTimestamp") == 0) {
      opts |= AMQP_OPT_WITH_TIMESTAMP;

    } else if (strcmp(cmd->argv[i], "WithUserID") == 0) {
      opts |= AMQP_OPT_WITH_USER_ID;

    } else if (strcmp(cmd->argv[i], "WithAppID") == 0) {
      opts |= AMQP_OPT_WITH_APP_ID;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown AMQPOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
}

/* usage: AMQPServer host[:port] [vhost] [username] [password] */
MODRET set_amqpserver(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c;
  char *server, *vhost = NULL, *username = NULL, *password = NULL, *ptr;
  size_t server_len;
  int port = AMQP_PROTOCOL_PORT;
  const pr_netaddr_t *addr;
  array_header *addrs;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  server = pstrdup(cmd->tmp_pool, cmd->argv[1]);
  server_len = strlen(server);

  ptr = strrchr(server, ':');
  if (ptr != NULL) {
    /* We also need to check for IPv6 addresses, e.g. "[::1]" or "[::1]:5672",
     * before assuming that the text following our discovered ':' is indeed
     * a port number.
     */

    if (*server == '[') {
      if (*(ptr-1) == ']') {
        /* We have an IPv6 address with an explicit port number. */
        server = pstrndup(cmd->tmp_pool, server + 1, (ptr - 1) - (server + 1));
        *ptr = '\0';
        port = atoi(ptr + 1);

      } else if (server[server_len-1] == ']') {
        /* We have an IPv6 address without an explicit port number. */
        server = pstrndup(cmd->tmp_pool, server + 1, server_len - 2);
        port = AMQP_PROTOCOL_PORT;
      }

    } else {
      *ptr = '\0';
      port = atoi(ptr + 1);
    }
  }

  if (cmd->argc == 3) {
    vhost = cmd->argv[2];
    if (strcmp(vhost, "") == 0) {
      vhost = NULL;
    }
  }

  if (cmd->argc == 4) {
    username = cmd->argv[3];
    if (strcmp(username, "") == 0) {
      username = NULL;
    }
  }

  if (cmd->argc == 5) {
    password = cmd->argv[4];
    if (strcmp(password, "") == 0) {
      password = NULL;
    }
  }

  /* Instead of stashing host/port here, consider using netaddrs, so that
   * we do the DNS resolution ONCE, and then iterate through all IPs at connect
   * time.
   *
   * Means a restart is needed to re-resolve the DNS name; not necessarily a
   * bad thing.
   */

  c = add_config_param(cmd->argv[0], 5, NULL, NULL, NULL, NULL, NULL);

  addr = pr_netaddr_get_addr(c->pool, server, &addrs);
  if (addr == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to resolve '", server, "': ",
      strerror(errno), NULL));
  }

  if (addrs == NULL) {
    addrs = make_array(c->pool, 1, sizeof(pr_netaddr_t *));
  }
 
  *((pr_netaddr_t **) push_array(addrs)) = (pr_netaddr_t *) addr;

  for (i = 0; i < addrs->nelts; i++) {
    pr_netaddr_t *addl_addr;

    addl_addr = ((pr_netaddr_t **) addrs->elts)[i];
    pr_netaddr_set_port(addl_addr, htons(port));
  }

  c->argv[0] = addrs;
  c->argv[1] = pstrdup(c->pool, vhost);
  c->argv[2] = pstrdup(c->pool, username);
  c->argv[3] = pstrdup(c->pool, password);

  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET amqp_log_any(cmd_rec *cmd) {
  if (amqp_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  return PR_DECLINED(cmd);
}

/* Event handlers
 */

static void amqp_exit_ev(const void *event_data, void *user_data) {
  (void) amqp_close_conn(session.pool);

  if (amqp_logfd >= 0) {
    (void) close(amqp_logfd);
    amqp_logfd = -1;
  }
}

#if defined(PR_SHARED_MODULE)
static void amqp_mod_unload_ev(const void *event_data, void *user_data) {
  if (strncmp((const char *) event_data, "mod_amqp.c", 12) == 0) {
    /* Unregister ourselves from all events. */
    pr_event_unregister(&amqp_module, NULL, NULL);
  }
}
#endif

static void amqp_sess_reinit_ev(const void *event_data, void *user_data) {
  int res;

  /* A HOST command changed the main_server pointer; reinitialize ourselves. */

  pr_event_unregister(&amqp_module, "core.exit", amqp_exit_ev);
  pr_event_unregister(&amqp_module, "core.session-reinit", amqp_sess_reinit_ev);

  (void) close(amqp_logfd);
  amqp_logfd = -1;

  amqp_engine = FALSE;
  amqp_opts = 0UL;

  res = amqp_sess_init();
  if (res < 0) {
    pr_session_disconnect(&amqp_module, PR_SESS_DISCONNECT_SESSION_INIT_FAILED,
      NULL);
  }
}

/* XXX Do we want to support any Controls/ftpctl actions? */

/* Initialization routines
 */

static int amqp_init(void) {
#if defined(PR_SHARED_MODULE)
  pr_event_register(&amqp_module, "core.module-unload", amqp_mod_unload_ev,
    NULL);
#endif

/* XXX Log the different discovered library versions here */
  pr_log_debug(DEBUG2, MOD_AMQP_VERSION ": using %s", amqp_version());
  return 0;
}

static int amqp_sess_init(void) {
  config_rec *c;
  int res, connected = FALSE;

  /* We have to register our HOST handler here, even if AMQPEngine is off,
   * as the current vhost may be disabled BUT the requested vhost may be
   * enabled.
   */
  pr_event_register(&amqp_module, "core.session-reinit", amqp_sess_reinit_ev,
    NULL);

  c = find_config(main_server->conf, CONF_PARAM, "AMQPEngine", FALSE);
  if (c != NULL) {
    amqp_engine = *((int *) c->argv[0]);
  }

  if (amqp_engine == FALSE) {
    return 0;
  }

  pr_event_register(&amqp_module, "core.exit", amqp_exit_ev, NULL);

  c = find_config(main_server->conf, CONF_PARAM, "AMQPLog", FALSE);
  if (c != NULL) {
    char *logname;

    logname = c->argv[0];

    if (strncasecmp(logname, "none", 5) != 0) {
      int xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(logname, &amqp_logfd, PR_LOG_SYSTEM_MODE);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        if (res == -1) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": notice: unable to open AMQPLog '%s': %s", logname,
            strerror(xerrno));

        } else if (res == PR_LOG_WRITABLE_DIR) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": notice: unable to open AMQPLog '%s': parent directory is "
            "world-writable", logname);

        } else if (res == PR_LOG_SYMLINK) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": notice: unable to open AMQPLog '%s': cannot log to a "
            "symlink", logname);
        }
      }
    }
  }

  amqp_pool = make_sub_pool(session.pool);
  pr_pool_tag(amqp_pool, MOD_AMQP_VERSION);

  c = find_config(main_server->conf, CONF_PARAM, "AMQPOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    amqp_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "AMQPOptions", FALSE);
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPServer", FALSE);
  if (c == NULL) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "no AMQPServer configured");
  }

  while (c != NULL) {
    pr_signals_handle();

    res = amqp_open_conn(amqp_pool, c);
    if (res == 0) {
      connected = TRUE;
      break;
    }

    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "error connecting to AMQP server: %s", strerror(errno));

    c = find_config_next(c, c->next, CONF_PARAM, "AMQPServer", FALSE);
  }

  if (connected == FALSE) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "unable to connect to AMQP server, disabling module");

    amqp_engine = FALSE;

    destroy_pool(amqp_pool);
    amqp_pool = NULL;

    (void) close(amqp_logfd);
    amqp_logfd = -1;
  }

  return 0;
}

/* Module API tables
 */

static conftable amqp_conftab[] = {
  { "AMQPEngine",		set_amqpengine,		NULL },
  { "AMQPLog",			set_amqplog,		NULL },
  { "AMQPLogOnEvent",		set_amqplogonevent,	NULL },
  { "AMQPOptions",		set_amqpoptions,	NULL },
  { "AMQPServer",		set_amqpserver,		NULL },

  { NULL }
};

static cmdtable amqp_cmdtab[] = {
  { LOG_CMD,	C_ANY,	G_NONE,	amqp_log_any,	FALSE, FALSE },
  { LOG_CMD_ERR,C_ANY,	G_NONE,	amqp_log_any,	FALSE, FALSE },

  { 0, NULL }
};

module amqp_module = {
  /* Always NULL */
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "amqp",

  /* Module configuration handler table */
  amqp_conftab,

  /* Module command handler table */
  amqp_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  amqp_init,

  /* Session initialization */
  amqp_sess_init,

  /* Module version */
  MOD_AMQP_VERSION
};
