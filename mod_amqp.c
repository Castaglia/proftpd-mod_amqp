/*
 * ProFTPD - mod_amqp
 * Copyright (c) 2017-2025 TJ Saunders
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
#ifdef HAVE_AMQP_SSL_SOCKET_H
# include <amqp_ssl_socket.h>
#endif

/* Newer versions of librabbitmq define these delivery mode values via enum. */
#ifndef HAVE_RABBITMQ_DELIVERY_MODE
# define AMQP_DELIVERY_NONPERSISTENT	1
# define AMQP_DELIVERY_PERSISTENT	2
#endif

#define AMQP_DEFAULT_APP_ID		"proftpd"
#define AMQP_DEFAULT_MESSAGE_TYPE	"log"
#define AMQP_DEFAULT_VHOST		"/"
#define AMQP_DEFAULT_USERNAME		"guest"
#define AMQP_DEFAULT_PASSWORD		"guest"
#define AMQP_DEFAULT_EXCHANGE		""

#define AMQP_DEFAULT_CONNECT_TIMEOUT_MS	2000

extern xaset_t *server_list;

/* From response.c */
extern pr_response_t *resp_list, *resp_err_list;

module amqp_module;

int amqp_logfd = -1;
pool *amqp_pool = NULL;
unsigned long amqp_opts = 0UL;

static int amqp_engine = FALSE;

/* XXX Wrap this in our own struct, for supporting other libs, too. */
static amqp_connection_state_t amqp_conn;
static amqp_channel_t amqp_chan;

static const char *amqp_app_id = NULL;
static const char *amqp_msg_expires = NULL;
static const char *amqp_msg_type = NULL;

/* AMQPOptions */
#define AMQP_OPT_PERSISTENT_DELIVERY	0x0001

static pr_table_t *jot_logfmt2json = NULL;
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

static void millis2timeval(struct timeval *tv, unsigned long millis) {
  tv->tv_sec = (millis / 1000);
  tv->tv_usec = (millis - (tv->tv_sec * 1000)) * 1000;
}

static int amqp_open_conn(pool *p, config_rec *c,
    unsigned long connect_millis) {
  register unsigned int i;
  int connected = FALSE;
  array_header *addrs;
  amqp_socket_t *sock;
  amqp_channel_t chan;
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

  if (c->argc == 10) {
    sock = amqp_ssl_socket_new(amqp_conn);

  } else {
    sock = amqp_tcp_socket_new(amqp_conn);
  }

  if (sock == NULL) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "error allocating memory for AMQP socket");
    (void) amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = ENOMEM;
    return -1;
  }

  if (c->argc == 10) {
    const char *ssl_cert = NULL, *ssl_key = NULL, *ssl_ca = NULL;
    int res, ssl_verify = -1, ssl_verify_hostname = -1, ssl_verify_peer = -1;

    ssl_cert = c->argv[4];
    ssl_key = c->argv[5];
    ssl_ca = c->argv[6];
    ssl_verify = *((int *) c->argv[7]);
    ssl_verify_hostname = *((int *) c->argv[8]);
    ssl_verify_peer = *((int *) c->argv[9]);

    if (ssl_cert != NULL &&
        ssl_key != NULL) {
      pr_trace_msg(trace_channel, 12,
        "using SSL client certificate '%s', key '%s'", ssl_cert, ssl_key);
      res = amqp_ssl_socket_set_key(sock, ssl_cert, ssl_key);
      if (res != AMQP_STATUS_OK) {
        pr_trace_msg(trace_channel, 3,
          "error setting SSL client certificate: %s", amqp_error_string2(res));
      }
    }

    if (ssl_ca != NULL) {
      pr_trace_msg(trace_channel, 12, "using SSL CA '%s'", ssl_ca);
      res = amqp_ssl_socket_set_cacert(sock, ssl_ca);
      if (res != AMQP_STATUS_OK) {
        pr_trace_msg(trace_channel, 3,
          "error setting SSL CA: %s", amqp_error_string2(res));
      }
    }

    if (ssl_verify_hostname >= 0 ||
        ssl_verify_peer >= 0) {

#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME
      if (ssl_verify_hostname >= 0) {
        pr_trace_msg(trace_channel, 12, "%s SSL hostname verification",
          ssl_verify_hostname ? "enabling" : "disabling");
        amqp_ssl_socket_set_verify_hostname(sock, ssl_verify_hostname);
      }
#endif

#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER
      if (ssl_verify_peer >= 0) {
        pr_trace_msg(trace_channel, 12, "%s SSL peer verification",
          ssl_verify_peer ? "enabling" : "disabling");
        amqp_ssl_socket_set_verify_peer(sock, ssl_verify_peer);
      }
#endif

#if !defined(HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME) && \
    !defined(HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER)
      pr_trace_msg(trace_channel, 12, "%s SSL verification",
        ssl_verify_peer ? "enabling" : "disabling");
      amqp_ssl_socket_set_verify(sock, ssl_verify_peer);
#endif

    } else {
      pr_trace_msg(trace_channel, 12, "%s SSL verification",
        ssl_verify ? "enabling" : "disabling");

#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME
      amqp_ssl_socket_set_verify_hostname(sock, ssl_verify);
#endif

#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER
      amqp_ssl_socket_set_verify_peer(sock, ssl_verify);
#endif

#if !defined(HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME) && \
    !defined(HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER)
      amqp_ssl_socket_set_verify(sock, ssl_verify);
#endif
    }
  }

  addrs = c->argv[0];

  for (i = 0; i < addrs->nelts; i++) {
    int res, xerrno;
    const pr_netaddr_t *addr;
    struct timeval tv;

    pr_signals_handle();

    addr = ((pr_netaddr_t **) addrs->elts)[i];
    ip_addr = pr_netaddr_get_ipstr(addr);
    port = ntohs(pr_netaddr_get_port(addr));
    millis2timeval(&tv, connect_millis);

    pr_trace_msg(trace_channel, 12, "connecting to %s:%u", ip_addr, port);
    res = amqp_socket_open_noblock(sock, ip_addr, port, &tv);
    xerrno = errno;

    switch (res) {
      case AMQP_STATUS_OK:
        connected = TRUE;
        break;

      case AMQP_STATUS_SOCKET_ERROR:
        pr_trace_msg(trace_channel, 2, "error connecting to %s:%u: %s (%s)",
          ip_addr, port, amqp_error_string2(res), strerror(xerrno));
        break;

      default:
        pr_trace_msg(trace_channel, 2, "error connecting to %s:%u: %s", ip_addr,
          port, amqp_error_string2(res));
        break;
    }

    if (connected == TRUE) {
      break;
    }
  }

  if (connected == FALSE) {
    (void) amqp_destroy_connection(amqp_conn);
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

    (void) amqp_connection_close(amqp_conn, AMQP_NOT_ALLOWED);
    (void) amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = EPERM;
    return -1;
  }

  pr_trace_msg(trace_channel, 12,
    "authenticated to broker %s:%u with vhost '%s', username '%s'", ip_addr,
    port, vhost, username);

  chan = 1;
  amqp_channel_open(amqp_conn, chan);
  reply = amqp_get_rpc_reply(amqp_conn);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_trace_reply(p, reply, "channel opening error");

    (void) amqp_connection_close(amqp_conn, AMQP_CHANNEL_ERROR);
    (void) amqp_destroy_connection(amqp_conn);
    amqp_conn = NULL;

    errno = EPERM;
    return -1;
  }

  amqp_chan = chan;
  return 0;
}

static int amqp_close_conn(pool *p) {
  int res, reason = AMQP_REPLY_SUCCESS;
  amqp_rpc_reply_t reply;

  if (amqp_conn == NULL) {
    return 0;
  }

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

static int amqp_send_msg(pool *p, const char *exchange, const char *routing_key,
    char *payload, size_t payload_len) {
  amqp_basic_properties_t props;
  int mandatory, immediate, res;

  props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG;
  props.content_type = amqp_cstring_bytes("application/json");

  props._flags |= AMQP_BASIC_TYPE_FLAG;
  props.type = amqp_cstring_bytes(amqp_msg_type);

  /* Note: RabbitMQ expects/implements this as a value in milliseconds:
   *  https://www.rabbitmq.com/ttl.html
   */
  if (amqp_msg_expires != NULL) {
    props._flags |= AMQP_BASIC_EXPIRATION_FLAG;
    props.expiration = amqp_cstring_bytes(amqp_msg_expires);
  }

  props._flags |= AMQP_BASIC_DELIVERY_MODE_FLAG;
  if (amqp_opts & AMQP_OPT_PERSISTENT_DELIVERY) {
    props.delivery_mode = AMQP_DELIVERY_PERSISTENT;

  } else {
    props.delivery_mode = AMQP_DELIVERY_NONPERSISTENT;
  }

  /* Note: While it might be tempting to use AMQP_BASIC_USER_ID_FLAG,
   * it will not work as desired.  RabbitMQ will verify this user ID against
   * that used for the connection, which is not what we want.
   */

  props._flags |= AMQP_BASIC_TIMESTAMP_FLAG;
  props.timestamp = time(NULL);

  props._flags |= AMQP_BASIC_APP_ID_FLAG;
  props.app_id = amqp_cstring_bytes(amqp_app_id);

  /* From:
   *   https://www.rabbitmq.com/amqp-0-9-1-reference.html
   *
   * mandatory
   *
   *   This flag tells the server how to react if the message cannot be routed     *   to a queue. If this flag is set, the server will return an unroutable
   *   message with a Return method. If this flag is zero, the server silently
   *   drops the message.
   *
   * immediate
   *
   *  This flag tells the server how to react if the message cannot be routed
   *  to a queue consumer immediately. If this flag is set, the server will
   *  return an undeliverable message with a Return method. If this flag is
   *  zero, the server will queue the message, but with no guarantee that it
   *  will ever be consumed.
   */

  mandatory = TRUE;
  immediate = FALSE;

  pr_trace_msg(trace_channel, 17,
    "publishing AMQP message: channel %u, exchange '%s', routing key '%s'",
    (unsigned int) amqp_chan, exchange, routing_key);
  res = amqp_basic_publish(amqp_conn, amqp_chan,
    amqp_cstring_bytes(exchange),
    amqp_cstring_bytes(routing_key),
    mandatory,
    immediate,
    &props,
    amqp_cstring_bytes(payload));
  if (res != AMQP_STATUS_OK) {
    pr_trace_msg(trace_channel, 3, "message publish error: %s",
      amqp_error_string2(res));
    errno = EIO;
    return -1;
  }

  return 0;
}

/* Logging */

struct amqp_buffer {
  char *ptr, *buf;
  size_t bufsz, buflen;
};

static void amqp_buffer_append_text(struct amqp_buffer *log, const char *text,
    size_t text_len) {
  if (text == NULL ||
      text_len == 0) {
    return;
  }

  if (text_len > log->buflen) {
    text_len = log->buflen;
  }

  pr_trace_msg(trace_channel, 19, "appending text '%.*s' (%lu) to buffer",
    (int) text_len, text, (unsigned long) text_len);
  memcpy(log->buf, text, text_len);
  log->buf += text_len;
  log->buflen -= text_len;
}

static int resolve_on_meta(pool *p, pr_jot_ctx_t *jot_ctx,
    unsigned char logfmt_id, const char *jot_hint, const void *val) {
  struct amqp_buffer *log;

  log = jot_ctx->log;
  if (log->buflen > 0) {
    const char *text = NULL;
    size_t text_len = 0;
    char buf[1024];

    switch (logfmt_id) {
      case LOGFMT_META_MICROSECS: {
        unsigned long num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%06lu", num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_MILLISECS: {
        unsigned long num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%03lu", num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_LOCAL_PORT:
      case LOGFMT_META_REMOTE_PORT:
      case LOGFMT_META_RESPONSE_CODE: {
        int num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%d", num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_UID: {
        uid_t uid;

        uid = *((double *) val);
        text = pr_uid2str(p, uid);
        break;
      }

      case LOGFMT_META_GID: {
        gid_t gid;

        gid = *((double *) val);
        text = pr_gid2str(p, gid);
        break;
      }

      case LOGFMT_META_BYTES_SENT:
      case LOGFMT_META_FILE_OFFSET:
      case LOGFMT_META_FILE_SIZE:
      case LOGFMT_META_RAW_BYTES_IN:
      case LOGFMT_META_RAW_BYTES_OUT:
      case LOGFMT_META_RESPONSE_MS:
      case LOGFMT_META_XFER_MS: {
        off_t num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%" PR_LU, (pr_off_t) num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_EPOCH:
      case LOGFMT_META_PID: {
        unsigned long num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%lu", num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_FILE_MODIFIED: {
        int truth;

        truth = *((int *) val);
        text = truth ? "true" : "false";
        break;
      }

      case LOGFMT_META_SECONDS: {
        float num;

        num = *((double *) val);
        text_len = pr_snprintf(buf, sizeof(buf)-1, "%0.3f", num);
        buf[text_len] = '\0';
        text = buf;
        break;
      }

      case LOGFMT_META_ANON_PASS:
      case LOGFMT_META_BASENAME:
      case LOGFMT_META_CLASS:
      case LOGFMT_META_CMD_PARAMS:
      case LOGFMT_META_COMMAND:
      case LOGFMT_META_DIR_NAME:
      case LOGFMT_META_DIR_PATH:
      case LOGFMT_META_ENV_VAR:
      case LOGFMT_META_EOS_REASON:
      case LOGFMT_META_FILENAME:
      case LOGFMT_META_GROUP:
      case LOGFMT_META_IDENT_USER:
      case LOGFMT_META_ISO8601:
      case LOGFMT_META_LOCAL_FQDN:
      case LOGFMT_META_LOCAL_IP:
      case LOGFMT_META_LOCAL_NAME:
      case LOGFMT_META_METHOD:
      case LOGFMT_META_NOTE_VAR:
      case LOGFMT_META_ORIGINAL_USER:
      case LOGFMT_META_PROTOCOL:
      case LOGFMT_META_REMOTE_HOST:
      case LOGFMT_META_REMOTE_IP:
      case LOGFMT_META_RENAME_FROM:
      case LOGFMT_META_RESPONSE_STR:
      case LOGFMT_META_TIME:
      case LOGFMT_META_USER:
      case LOGFMT_META_VERSION:
      case LOGFMT_META_VHOST_IP:
      case LOGFMT_META_XFER_FAILURE:
      case LOGFMT_META_XFER_PATH:
      case LOGFMT_META_XFER_STATUS:
      case LOGFMT_META_XFER_TYPE:
      default:
        text = val;
    }

    if (text != NULL &&
        text_len == 0) {
      text_len = strlen(text);
    }

    amqp_buffer_append_text(log, text, text_len);
  }

  return 0;
}

static int resolve_on_other(pool *p, pr_jot_ctx_t *jot_ctx, unsigned char *text,
    size_t text_len) {
  struct amqp_buffer *log;

  log = jot_ctx->log;
  amqp_buffer_append_text(log, (const char *) text, text_len);
  return 0;
}

static void log_event(amqp_channel_t chan, config_rec *c, cmd_rec *cmd) {
  pool *tmp_pool;
  int res;
  pr_jot_ctx_t *jot_ctx;
  pr_jot_filters_t *jot_filters;
  pr_json_object_t *json;
  const char *fmt_name = NULL;
  char *payload = NULL;
  size_t payload_len = 0;
  unsigned char *log_fmt, *exchange_fmt, *routing_fmt;

  jot_filters = c->argv[0];
  fmt_name = c->argv[1];
  log_fmt = c->argv[2];
  exchange_fmt = c->argv[3];
  routing_fmt = c->argv[4];

  if (jot_filters == NULL ||
      fmt_name == NULL ||
      log_fmt == NULL) {
    return;
  }

  tmp_pool = make_sub_pool(cmd->tmp_pool);
  jot_ctx = pcalloc(tmp_pool, sizeof(pr_jot_ctx_t));
  json = pr_json_object_alloc(tmp_pool);
  jot_ctx->log = json;
  jot_ctx->user_data = jot_logfmt2json;

  res = pr_jot_resolve_logfmt(tmp_pool, cmd, jot_filters, log_fmt, jot_ctx,
    pr_jot_on_json, NULL, NULL);
  if (res == 0) {
    payload = pr_json_object_to_text(tmp_pool, json, "");
    payload_len = strlen(payload);
    pr_trace_msg(trace_channel, 8, "generated JSON payload for %s: %.*s",
      (char *) cmd->argv[0], (int) payload_len, payload);

  } else {
    /* EPERM indicates that the message was filtered. */
    if (errno != EPERM) {
      (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
        "error generating JSON formatted log message: %s", strerror(errno));
    }

    payload = NULL;
    payload_len = 0;
  }

  pr_json_object_free(json);

  if (payload_len > 0) {
    const char *exchange, *routing_key;

    if (exchange_fmt != NULL) {
      struct amqp_buffer *rb;
      char exchange_buf[1024];

      rb = pcalloc(tmp_pool, sizeof(struct amqp_buffer));
      rb->bufsz = rb->buflen = sizeof(exchange_buf)-1;
      rb->ptr = rb->buf = exchange_buf;

      jot_ctx->log = rb;

      res = pr_jot_resolve_logfmt(tmp_pool, cmd, NULL, exchange_fmt, jot_ctx,
        resolve_on_meta, NULL, resolve_on_other);
      if (res == 0) {
        size_t exchange_buflen;

        exchange_buflen = rb->bufsz - rb->buflen;
        exchange = pstrndup(tmp_pool, exchange_buf, exchange_buflen);

      } else {
        (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
          "error resolving AMQP exchange format '%s': %s", exchange_fmt,
          strerror(errno));
        exchange = AMQP_DEFAULT_EXCHANGE;
      }

    } else {
      exchange = AMQP_DEFAULT_EXCHANGE;
    }

    if (routing_fmt != NULL) {
      struct amqp_buffer *rb;
      char routing_buf[1024];

      rb = pcalloc(tmp_pool, sizeof(struct amqp_buffer));
      rb->bufsz = rb->buflen = sizeof(routing_buf)-1;
      rb->ptr = rb->buf = routing_buf;

      jot_ctx->log = rb;

      res = pr_jot_resolve_logfmt(tmp_pool, cmd, NULL, routing_fmt, jot_ctx,
        resolve_on_meta, NULL, resolve_on_other);
      if (res == 0) {
        size_t routing_buflen;

        routing_buflen = rb->bufsz - rb->buflen;
        routing_key = pstrndup(tmp_pool, routing_buf, routing_buflen);

      } else {
        (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
          "error resolving AMQP routing key format '%s': %s", routing_fmt,
          strerror(errno));
        routing_key = fmt_name;
      }

    } else {
      routing_key = fmt_name;
    }

    res = amqp_send_msg(cmd->tmp_pool, exchange, routing_key, payload,
      payload_len);
    if (res < 0) {
      (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
        "error publishing log message to '%s#%s': %s", exchange, routing_key,
        strerror(errno));

    } else {
      pr_trace_msg(trace_channel, 17, "published log message to '%s#%s'",
        exchange, routing_key);
    }
  }

  destroy_pool(tmp_pool);
}

static void log_events(cmd_rec *cmd) {
  config_rec *c;

  c = find_config(CURRENT_CONF, CONF_PARAM, "AMQPLogOnEvent", FALSE);
  while (c != NULL) {
    pr_signals_handle();

    log_event(amqp_chan, c, cmd);
    c = find_config_next(c, c->next, CONF_PARAM, "AMQPLogOnEvent", FALSE);
  }
}

/* Configuration handlers
 */

/* usage: AMQPApplicationID name */
MODRET set_amqpapplicationid(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

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

static unsigned char *parse_jotted_key(pool *p, const char *text) {
  int res;
  pool *tmp_pool;
  pr_jot_ctx_t *jot_ctx;
  pr_jot_parsed_t *jot_parsed;
  unsigned char fmtbuf[1024], *fmt = NULL;
  size_t fmtlen;

  tmp_pool = make_sub_pool(p);
  jot_ctx = pcalloc(tmp_pool, sizeof(pr_jot_ctx_t));
  jot_parsed = pcalloc(tmp_pool, sizeof(pr_jot_parsed_t));
  jot_parsed->bufsz = jot_parsed->buflen = sizeof(fmtbuf)-1;
  jot_parsed->ptr = jot_parsed->buf = fmtbuf;

  jot_ctx->log = jot_parsed;

  res = pr_jot_parse_logfmt(tmp_pool, text, jot_ctx,
    pr_jot_parse_on_meta, pr_jot_parse_on_unknown, pr_jot_parse_on_other, 0);
  destroy_pool(tmp_pool);

  if (res < 0) {
    return NULL;
  }

  fmtlen = jot_parsed->bufsz - jot_parsed->buflen;
  fmtbuf[fmtlen] = '\0';
  fmt = (unsigned char *) pstrndup(p, (char *) fmtbuf, fmtlen);
  return fmt;
}

/* usage: AMQPLogOnEvent "none"|events log-fmt [exchange ...] [routing ...] */
MODRET set_amqplogonevent(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c, *logfmt_config;
  const char *fmt_name, *rules;
  char *exchange = NULL, *routing_key = NULL;
  unsigned char *log_fmt = NULL, *exchange_fmt = NULL, *routing_fmt = NULL;
  pr_jot_filters_t *jot_filters;

  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL|CONF_ANON|CONF_DIR);

  if (cmd->argc < 3 ||
      cmd->argc > 7) {

    if (cmd->argc == 2 &&
        strcasecmp(cmd->argv[1], "none") == 0) {
       c = add_config_param(cmd->argv[0], 5, NULL, NULL, NULL, NULL, NULL);
       c->flags |= CF_MERGEDOWN;
       return PR_HANDLED(cmd);
    }

    CONF_ERROR(cmd, "wrong number of parameters");
  }

  if ((cmd->argc - 3) % 2 != 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  c = add_config_param(cmd->argv[0], 5, NULL, NULL, NULL, NULL, NULL);

  rules = cmd->argv[1];
  jot_filters = pr_jot_filters_create(c->pool, rules,
    PR_JOT_FILTER_TYPE_COMMANDS_WITH_CLASSES,
    PR_JOT_FILTER_FL_ALL_INCL_ALL);
  if (jot_filters == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use events '", rules,
      "': ", strerror(errno), NULL));
  }

  fmt_name = cmd->argv[2];

  for (i = 3; i < cmd->argc; i += 2) {
    if (strcmp(cmd->argv[i], "exchange") == 0) {
      exchange = cmd->argv[i+1];

    } else if (strcmp(cmd->argv[i], "routing") == 0) {
      routing_key = cmd->argv[i+1];

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        ": unknown AMQPLogOnEvent parameter: ", cmd->argv[i], NULL));
    }
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

  if (exchange != NULL) {
    exchange_fmt = parse_jotted_key(c->pool, exchange);
    if (exchange_fmt == NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "error parsing AMQPLogOnEvent parameter '", exchange, "': ",
        strerror(errno), NULL));
    }
  }

  if (routing_key != NULL) {
    routing_fmt = parse_jotted_key(c->pool, routing_key);
    if (routing_fmt == NULL) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "error parsing AMQPLogEvent parameter '", routing_key, "': ",
        strerror(errno), NULL));
    }
  }

  c->argv[0] = jot_filters;
  c->argv[1] = pstrdup(c->pool, fmt_name);
  c->argv[2] = log_fmt;
  c->argv[3] = exchange_fmt;
  c->argv[4] = routing_fmt;

  return PR_HANDLED(cmd);
}

/* usage: AMQPMessageExpires millis */
MODRET set_amqpmessageexpires(cmd_rec *cmd) {
  unsigned long expires = 0;
  char *ptr = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  expires = strtoul(cmd->argv[1], &ptr, 10);
  if (ptr && *ptr) {
    CONF_ERROR(cmd, "invalid parameter");
  }

  /* The librabbitmq API for message expiration uses string, surprisingly;
   * we thus just copy the given parameter.
   */
  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: AMQPMessageType name */
MODRET set_amqpmessagetype(cmd_rec *cmd) {
  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  (void) add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
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
    if (strcmp(cmd->argv[i], "PersistentDelivery") == 0) {
      opts |= AMQP_OPT_PERSISTENT_DELIVERY;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown AMQPOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
}

/* usage: AMQPServer host[:port] [vhost] [username] [password]
 *   [ssl-cert:<path>] [ssl-key:<path>] [ssl-ca:/path] [ssl-verify:false]
 *   [ssl-verify-hostname:false] [ssl-verify-peer:false]
 */
MODRET set_amqpserver(cmd_rec *cmd) {
  register unsigned int i;
  config_rec *c;
  char *server, *vhost = NULL, *username = NULL, *password = NULL, *ptr;
  char *ssl_cert = NULL, *ssl_key = NULL, *ssl_ca = NULL;
  size_t server_len;
  int port = AMQP_PROTOCOL_PORT;
  int ssl_verify = -1, ssl_verify_hostname = -1, ssl_verify_peer = -1;
  const pr_netaddr_t *addr = NULL;
  array_header *addrs = NULL;

  if (cmd->argc < 2 ||
      cmd->argc > 11) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

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
      /* What if the colon is the first character, i.e. the hostname is
       * missing completely?
       */
      if (server == ptr) {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "malformed host: ", server,
          NULL));
      }

      *ptr = '\0';
      port = atoi(ptr + 1);
    }
  }

  if (cmd->argc >= 3) {
    vhost = cmd->argv[2];
    if (strcmp(vhost, "") == 0) {
      vhost = NULL;
    }
  }

  if (cmd->argc >= 4) {
    username = cmd->argv[3];
    if (strcmp(username, "") == 0) {
      username = NULL;
    }
  }

  if (cmd->argc >= 5) {
    password = cmd->argv[4];
    if (strcmp(password, "") == 0) {
      password = NULL;
    }
  }

  if (cmd->argc >= 6) {
    for (i = 5; i < cmd->argc; i++) {
      if (strncmp(cmd->argv[i], "ssl-cert:", 9) == 0) {
        char *path;

        path = cmd->argv[i];

        /* Advance past the "ssl-cert:" prefix. */
        path += 9;

        /* Check the file exists! */
        if (file_exists2(cmd->tmp_pool, path) == TRUE) {
          ssl_cert = path;

        } else {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL certificate '%s': %s", (char *) cmd->argv[0], path,
            strerror(ENOENT));
        }

      } else if (strncmp(cmd->argv[i], "ssl-key:", 8) == 0) {
        char *path;

        path = cmd->argv[i];

        /* Advance past the "ssl-key:" prefix. */
        path += 8;

        /* Check the file exists! */
        if (file_exists2(cmd->tmp_pool, path) == TRUE) {
          ssl_key = path;

        } else {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL certificate key '%s': %s", (char *) cmd->argv[0], path,
            strerror(ENOENT));
        }

      } else if (strncmp(cmd->argv[i], "ssl-ca:", 7) == 0) {
        char *path;

        path = cmd->argv[i];

        /* Advance past the "ssl-ca:" prefix. */
        path += 7;

        /* Check the file exists! */
        if (file_exists2(cmd->tmp_pool, path) == TRUE) {
          ssl_ca = path;

        } else {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL CA '%s': %s", (char *) cmd->argv[0], path,
            strerror(ENOENT));
        }

      } else if (strncmp(cmd->argv[i], "ssl-verify:", 11) == 0) {
        char *verify;
        int res;

        verify = cmd->argv[i];

        /* Advance past the "ssl-verify:" prefix. */
        verify += 11;

        res = pr_str_is_boolean(verify);
        if (res < 0) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL verification '%s': %s", (char *) cmd->argv[0], verify,
            strerror(errno));

        } else {
          ssl_verify = res;
        }

      } else if (strncmp(cmd->argv[i], "ssl-verify-hostname:", 20) == 0) {
#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME
        char *verify;
        int res;

        verify = cmd->argv[i];

        /* Advance past the "ssl-verify-hostname:" prefix. */
        verify += 20;

        res = pr_str_is_boolean(verify);
        if (res < 0) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL hostname verification '%s': %s", (char *) cmd->argv[0],
            verify, strerror(errno));

        } else {
          ssl_verify_hostname = res;
        }
#endif /* HAVE_AMQP_SSL_SOCKET_SET_VERIFY_HOSTNAME */

      } else if (strncmp(cmd->argv[i], "ssl-verify-peer:", 16) == 0) {
#ifdef HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER
        char *verify;
        int res;

        verify = cmd->argv[i];

        /* Advance past the "ssl-verify-peer:" prefix. */
        verify += 16;

        res = pr_str_is_boolean(verify);
        if (res < 0) {
          pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
            ": %s: SSL peer verification '%s': %s", (char *) cmd->argv[0],
            verify, strerror(errno));

        } else {
          ssl_verify_peer = res;
        }
#endif /* HAVE_AMQP_SSL_SOCKET_SET_VERIFY_PEER */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
          "unknown AMQPServer parameter: ", cmd->argv[i], NULL));
      }
    }
  }

  /* Instead of stashing host/port here, consider using netaddrs, so that
   * we do the DNS resolution ONCE, and then iterate through all IPs at connect
   * time.
   *
   * Means a restart is needed to re-resolve the DNS name; not necessarily a
   * bad thing.
   */

  /* Note that we indicate, to the amqp_open_conn() function, whether to use
   * SSL or not based on the number of args in this config_rec.
   */
  if (ssl_cert != NULL ||
      ssl_key != NULL ||
      ssl_ca != NULL ||
      ssl_verify != -1 ||
      ssl_verify_hostname != -1 ||
      ssl_verify_peer != -1) {
    c = add_config_param(cmd->argv[0], 10, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL);

  } else {
    /* No SSL parameters used. */
    c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);
  }

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

  if (c->argc == 10) {
    c->argv[4] = pstrdup(c->pool, ssl_cert);
    c->argv[5] = pstrdup(c->pool, ssl_key);
    c->argv[6] = pstrdup(c->pool, ssl_ca);
    c->argv[7] = palloc(c->pool, sizeof(int));
    *((int *) c->argv[7]) = ssl_verify;
    c->argv[8] = palloc(c->pool, sizeof(int));
    *((int *) c->argv[8]) = ssl_verify_hostname;
    c->argv[9] = palloc(c->pool, sizeof(int));
    *((int *) c->argv[9]) = ssl_verify_peer;
  }

  return PR_HANDLED(cmd);
}

/* usage: AMQPTimeout connect-millis */
MODRET set_amqptimeout(cmd_rec *cmd) {
  config_rec *c;
  unsigned long connect_millis;
  char *ptr = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  connect_millis = strtoul(cmd->argv[1], &ptr, 10);
  if (ptr && *ptr) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "badly formatted connect timeout value: ", cmd->argv[1], NULL));
  }

  if (connect_millis < 1) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "connect timeout must be greater than zero: ", cmd->argv[1], NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = connect_millis;

  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET amqp_log_any(cmd_rec *cmd) {
  if (amqp_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  log_events(cmd);
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
  amqp_app_id = NULL;
  amqp_msg_expires = NULL;
  amqp_msg_type = NULL;

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

  if (amqp_version_number() != AMQP_VERSION) {
    pr_log_pri(PR_LOG_NOTICE, MOD_AMQP_VERSION
      ": compiled against '%s', but running using '%s'",
      AMQP_VERSION_STRING, amqp_version());

  } else {
    pr_log_debug(DEBUG2, MOD_AMQP_VERSION ": using %s", amqp_version());
  }

  return 0;
}

static int amqp_sess_init(void) {
  config_rec *c;
  int res, connected = FALSE;
  unsigned long connect_millis;

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

  c = find_config(main_server->conf, CONF_PARAM, "AMQPTimeout", FALSE);
  if (c != NULL) {
    connect_millis = *((unsigned long *) c->argv[0]);

  } else {
    connect_millis = AMQP_DEFAULT_CONNECT_TIMEOUT_MS;
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPServer", FALSE);
  if (c == NULL) {
    (void) pr_log_writefile(amqp_logfd, MOD_AMQP_VERSION,
      "no AMQPServer configured, disabling module");

    amqp_engine = FALSE;

    (void) close(amqp_logfd);
    amqp_logfd = -1;

    return 0;
  }

  amqp_pool = make_sub_pool(session.pool);
  pr_pool_tag(amqp_pool, MOD_AMQP_VERSION);

  while (c != NULL) {
    pr_signals_handle();

    res = amqp_open_conn(amqp_pool, c, connect_millis);
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
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPApplicationID", FALSE);
  if (c != NULL) {
    amqp_app_id = c->argv[0];

  } else {
    amqp_app_id = AMQP_DEFAULT_APP_ID;
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPMessageExpires", FALSE);
  if (c != NULL) {
    amqp_msg_expires = c->argv[0];
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPMessageType", FALSE);
  if (c != NULL) {
    amqp_msg_type = c->argv[0];

  } else {
    amqp_msg_type = AMQP_DEFAULT_MESSAGE_TYPE;
  }

  c = find_config(main_server->conf, CONF_PARAM, "AMQPOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    amqp_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "AMQPOptions", FALSE);
  }

  jot_logfmt2json = pr_jot_get_logfmt2json(amqp_pool);
  return 0;
}

/* Module API tables
 */

static conftable amqp_conftab[] = {
  { "AMQPApplicationID",	set_amqpapplicationid,	NULL },
  { "AMQPEngine",		set_amqpengine,		NULL },
  { "AMQPLog",			set_amqplog,		NULL },
  { "AMQPLogOnEvent",		set_amqplogonevent,	NULL },
  { "AMQPMessageExpires",	set_amqpmessageexpires,	NULL },
  { "AMQPMessageType",		set_amqpmessagetype,	NULL },
  { "AMQPOptions",		set_amqpoptions,	NULL },
  { "AMQPServer",		set_amqpserver,		NULL },
  { "AMQPTimeout",		set_amqptimeout,	NULL },

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
