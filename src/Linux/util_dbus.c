/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <linux/types.h>
#include <sys/prctl.h>
#include <sched.h>
#include <dbus/dbus.h>

#include "util.h"
#include "util_dbus.h"

  /*_________________---------------------------__________________
    _________________    utils for DBUS         __________________
    -----------------___________________________------------------
  */

  static const char *messageTypeStr(int mtype)  {
    switch (mtype) {
    case DBUS_MESSAGE_TYPE_SIGNAL: return "signal";
    case DBUS_MESSAGE_TYPE_METHOD_CALL: return "method_call";
    case DBUS_MESSAGE_TYPE_METHOD_RETURN: return "method_return";
    case DBUS_MESSAGE_TYPE_ERROR:  return "error";
    default: return "(unknown message type)";
    }
  }

  /*_________________---------------------------__________________
    _________________    parseDbusElem          __________________
    -----------------___________________________------------------
  */

  static void indent(UTStrBuf *buf, int depth) {
    for(int ii = 0; ii < depth; ii++)
      UTStrBuf_append(buf, "  ");
  }

#define PARSE_DBUS_VAR(it,type,format,buf) do {	\
    type val;					\
    dbus_message_iter_get_basic(it, &val);	\
    UTStrBuf_printf(buf, format, val);		\
} while(0)

  void parseDBusElem(DBusMessageIter *it, UTStrBuf *buf, bool ind, int depth, char *suffix) {
    if(ind) indent(buf, depth);
    int atype = dbus_message_iter_get_arg_type(it);
    switch(atype) {
    case DBUS_TYPE_INVALID: break;
    case DBUS_TYPE_STRING: PARSE_DBUS_VAR(it, char *, "\"%s\"", buf); break;
    case DBUS_TYPE_OBJECT_PATH: PARSE_DBUS_VAR(it, char *, "obj=%s", buf); break;
    case DBUS_TYPE_BYTE: PARSE_DBUS_VAR(it, uint8_t, "0x%02x", buf); break;
    case DBUS_TYPE_INT16: PARSE_DBUS_VAR(it, int16_t, "%d", buf); break;
    case DBUS_TYPE_INT32: PARSE_DBUS_VAR(it, int32_t, "%d", buf); break;
    case DBUS_TYPE_INT64: PARSE_DBUS_VAR(it, int64_t, "%"PRId64, buf); break;
    case DBUS_TYPE_UINT16: PARSE_DBUS_VAR(it, uint16_t, "%u", buf); break;
    case DBUS_TYPE_UINT32: PARSE_DBUS_VAR(it, uint32_t, "%u", buf); break;
    case DBUS_TYPE_UINT64: PARSE_DBUS_VAR(it, uint64_t, "%"PRIu64, buf); break;
    case DBUS_TYPE_DOUBLE: PARSE_DBUS_VAR(it, double, "%f", buf); break;
    case DBUS_TYPE_BOOLEAN: {
      dbus_bool_t val;
      dbus_message_iter_get_basic(it, &val);
      UTStrBuf_printf(buf, "%s", val ? "true":"false");
      break;
    }
    case DBUS_TYPE_VARIANT: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      UTStrBuf_printf(buf, "(");
      parseDBusElem(&sub, buf, NO, depth+1, ")");
      break;
    }
    case DBUS_TYPE_ARRAY: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      // handle empty array
      int elemType = dbus_message_iter_get_arg_type(&sub);
      if(elemType == DBUS_TYPE_INVALID) {
	UTStrBuf_printf(buf, "[]");
      }
      else {
	UTStrBuf_printf(buf, "[\n");
	do parseDBusElem(&sub, buf, YES, depth+1, ",\n");
	while (dbus_message_iter_next(&sub));
	indent(buf, depth);
	UTStrBuf_printf(buf, "]");
      }
      break;
    }
    case DBUS_TYPE_DICT_ENTRY: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      // iterate over key-value pairs (usually only one pair)
      do {
	parseDBusElem(&sub, buf, NO, depth+1, " => ");
	dbus_message_iter_next(&sub);
	parseDBusElem(&sub, buf, NO, depth+1, NULL);
      }
      while (dbus_message_iter_next(&sub));
      break;
    }
    case DBUS_TYPE_STRUCT: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      UTStrBuf_printf(buf, "struct {\n");
      do parseDBusElem(&sub, buf, YES, depth+1, ",\n");
      while (dbus_message_iter_next(&sub));
      indent(buf, depth);
      UTStrBuf_printf(buf, "}");
      break;
    }
    default:
      UTStrBuf_printf(buf, "unknown-type=%d", atype);
      break;
    }
    if(suffix) UTStrBuf_append(buf, suffix);
  }


  /*_________________---------------------------__________________
    _________________    parseDbusMessage       __________________
    -----------------___________________________------------------
  */

  void parseDBusMessage(DBusMessage *msg) {
    myLog(LOG_INFO, "DBUS: dbusCB got message");
    int mtype = dbus_message_get_type(msg);
    const char *src = dbus_message_get_sender(msg);
    const char *dst = dbus_message_get_destination(msg);
    UTStrBuf *buf = UTStrBuf_new();
    UTStrBuf_printf(buf, "DBUS %s->%s %s(",
		    src?:"<no src>",
		    dst?:"<no dst>",
		    messageTypeStr(mtype));
    UTStrBuf_printf(buf, "(");
    switch(mtype) {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
    case DBUS_MESSAGE_TYPE_SIGNAL:
      UTStrBuf_printf(buf, "serial=%u,path=%s,interface=%s,member=%s",
		      dbus_message_get_serial(msg),
		      dbus_message_get_path(msg),
		      dbus_message_get_interface(msg),
		      dbus_message_get_member(msg));
      break;
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
      UTStrBuf_printf(buf, "reply_serial=%u",
		      dbus_message_get_reply_serial(msg));
      break;
    case DBUS_MESSAGE_TYPE_ERROR:
      UTStrBuf_printf(buf, "error_name=%s,reply_serial=%u",
		      dbus_message_get_error_name(msg),
		      dbus_message_get_reply_serial(msg));
      break;
    default:
      break;
    }
    UTStrBuf_printf(buf, ") {");
    DBusMessageIter iterator;
    if(dbus_message_iter_init(msg, &iterator)) {
      do parseDBusElem(&iterator, buf, YES, 1, "\n");
      while (dbus_message_iter_next(&iterator));
    }
    UTStrBuf_append(buf, "}\n");
    myDebug(1, "DBUS message: %s", buf->buf);
    UTStrBuf_free(buf);
  }


#if defined(__cplusplus)
} /* extern "C" */
#endif
