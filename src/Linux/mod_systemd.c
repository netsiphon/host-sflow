/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

/* with grateful reference to:
 * http://www.matthew.ath.cx/misc/dbus
 * https://www.freedesktop.org/wiki/Software/systemd/dbus/
 * https://github.com/brianmcgillion/DBus/blob/master/tools/dbus-monitor.c
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
#include <openssl/sha.h>

#include "hsflowd.h"
#include "cpu_utils.h"

  // limit the number of chars we will read from each line in /proc
#define MAX_PROC_LINELEN 256
#define MAX_PROC_TOKLEN 32

#define HSP_SYSTEMD_MAX_FNAME_LEN 255
#define HSP_SYSTEMD_MAX_STATS_LINELEN 512
#define HSP_SYSTEMD_WAIT_STARTUP 5

#define HSP_DBUS_TIMEOUT_mS 10000

#define HSP_SYSTEMD_SERVICE_REGEX "\\.service$"
#define HSP_SYSTEMD_SYSTEM_SLICE_REGEX "system\\.slice"

#define HSP_DBUS_MONITOR 0
  
  typedef void (*HSPDBusHandler)(EVMod *mod, DBusMessage *dbm, void *magic);
  
  typedef struct _HSPDBusRequest {
    int serial;
    HSPDBusHandler handler;
    void *magic;
    struct timespec send_time;
  } HSPDBusRequest;

  typedef struct _HSPUnitCounters {
    uint64_t rd_bytes;
    uint64_t wr_bytes;
    uint64_t cpu_total;
  } HSPUnitCounters;
    
  typedef struct _HSPDBusUnit {
    char *name;
    char *obj;
    char *cgroup;
    char uuid[16];
    UTHash *processes;
    bool marked:1;
    bool cpuAccounting:1;
    bool memoryAccounting:1;
    bool blockIOAccounting:1;
    HSPUnitCounters cntr;
  } HSPDBusUnit;

  typedef struct _HSPDBusProcess {
    pid_t pid;
    bool marked;
    HSPUnitCounters cntr;
    HSPUnitCounters last;
  } HSPDBusProcess;
  
  typedef struct _HSPVMState_SYSTEMD {
    HSPVMState vm; // superclass: must come first
    char *id;
  } HSPVMState_SYSTEMD;

  typedef struct _HSP_mod_SYSTEMD {
    DBusConnection *connection;
    DBusError error;
    UTHash *dbusRequests;
    uint32_t dbus_tx;
    uint32_t dbus_rx;
    UTHash *units;
    EVBus *pollBus;
    UTHash *vmsByUUID;
    UTHash *vmsByID;
    UTHash *pollActions;
    SFLCounters_sample_element vnodeElem;
    uint32_t countdownToResync;
    regex_t *service_regex;
    regex_t *system_slice_regex;
#ifdef HSP_DBUS_MONITOR
    bool subscribed;
#endif
    uint32_t page_size;
  } HSP_mod_SYSTEMD;

  /*_________________---------------------------__________________
    _________________     logging utils         __________________
    -----------------___________________________------------------
  */

  static void log_dbus_error(EVMod *mod, char *msg) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    if (dbus_error_is_set(&mdata->error))
      myLog(LOG_ERR, "SYSTEMD Error(%s) = %s", msg, mdata->error.message);
    else if(msg)
      myLog(LOG_ERR, "SYSTEMD Error(%s)", msg);
  }

  static const char *messageTypeStr(int mtype)  {
    switch (mtype) {
    case DBUS_MESSAGE_TYPE_SIGNAL: return "signal";
    case DBUS_MESSAGE_TYPE_METHOD_CALL: return "method_call";
    case DBUS_MESSAGE_TYPE_METHOD_RETURN: return "method_return";
    case DBUS_MESSAGE_TYPE_ERROR:  return "error";
    default: return "(unknown message type)";
    }
  }

  char *containerStr(HSPVMState_SYSTEMD *container, char *buf, int bufLen) {
    u_char uuidstr[100];
    printUUID((u_char *)container->vm.uuid, uuidstr, 100);
    snprintf(buf, bufLen, "uuid: %s id: %s",
	     container->vm.uuid,
	     container->id);
    return buf;
  }

  void containerHTPrint(UTHash *ht, char *prefix) {
    char buf[1024];
    HSPVMState_SYSTEMD *container;
    UTHASH_WALK(ht, container)
      myLog(LOG_INFO, "%s: %s", prefix, containerStr(container, buf, 1024));
  }

  /*_________________---------------------------__________________
    _________________   add and remove VM       __________________
    -----------------___________________________------------------
  */

  static void agentCB_getCounters_SYSTEMD_request(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
    EVMod *mod = (EVMod *)magic;
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSPVMState_SYSTEMD *container = (HSPVMState_SYSTEMD *)poller->userData;
    UTHashAdd(mdata->pollActions, container);
  }

  static void removeAndFreeVM_SYSTEMD(EVMod *mod, HSPVMState_SYSTEMD *container) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    if(getDebug()) {
      myLog(LOG_INFO, "removeAndFreeVM: removing service with dsIndex=%u", container->vm.dsIndex);
    }

    if(UTHashDel(mdata->vmsByID, container) == NULL) {
      myLog(LOG_ERR, "UTHashDel (vmsByID) failed: service %s", container->id);
      if(debug(1))
	containerHTPrint(mdata->vmsByID, "vmsByID");
    }

    if(UTHashDel(mdata->vmsByUUID, container) == NULL) {
      myLog(LOG_ERR, "UTHashDel (vmsByUUID) failed: service %s", container->id);
      if(debug(1))
	containerHTPrint(mdata->vmsByUUID, "vmsByUUID");
    }

    if(container->id) my_free(container->id);
    removeAndFreeVM(mod, &container->vm);
  }

  static HSPVMState_SYSTEMD *getContainer(EVMod *mod, HSPDBusUnit *unit, int create) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSPVMState_SYSTEMD cont = { .id = unit->name };
    HSPVMState_SYSTEMD *container = UTHashGet(mdata->vmsByID, &cont);
    if(container == NULL
       && create) {
      container = (HSPVMState_SYSTEMD *)getVM(mod, unit->uuid, YES, sizeof(HSPVMState_SYSTEMD), VMTYPE_SYSTEMD, agentCB_getCounters_SYSTEMD_request);
      assert(container != NULL);
      if(container) {
	container->id = my_strdup(unit->name);
	// add to collections
	UTHashAdd(mdata->vmsByID, container);
	UTHashAdd(mdata->vmsByUUID, container);
      }
    }
    return container;
  }

  /*_________________---------------------------__________________
    _________________    name_uuid              __________________
    -----------------___________________________------------------
  */

  static void uuidgen_type5(HSP *sp, u_char *uuid, char *name) {
    // Generate type 5 UUID (rfc 4122)
    SHA_CTX ctx;
    unsigned char sha_bits[SHA_DIGEST_LENGTH];
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, sp->uuid, 16); // use sp->uuid as "namespace UUID"
    SHA1_Update(&ctx, name, my_strlen(name));
    // also hash in agent IP address in case sp->uuid is missing or not unique
    SHA1_Update(&ctx,
		&sp->agentIP.address,
		(sp->agentIP.type == SFLADDRESSTYPE_IP_V6 ? 16 : 4));
    SHA1_Final(sha_bits, &ctx);
    // now generate a type-5 UUID according to the recipe here:
    // http://stackoverflow.com/questions/10867405/generating-v5-uuid-what-is-name-and-namespace
    // SHA1 Digest:   74738ff5 5367 e958 9aee 98fffdcd1876 94028007
    // UUID (v5):     74738ff5-5367-5958-9aee-98fffdcd1876
    //                          ^_low nibble is set to 5 to indicate type 5
    //                                   ^_first two bits set to 1 and 0, respectively
    memcpy(uuid, sha_bits, 16);
    uuid[6] &= 0x0F;
    uuid[6] |= 0x50;
    uuid[8] &= 0x3F;
    uuid[8] |= 0x80;
  }

  /*_________________---------------------------__________________
    _________________    HSPDBusUnit            __________________
    -----------------___________________________------------------
  */
  
  static HSPDBusUnit *HSPDBusUnitNew(EVMod *mod, char *name) {
    HSP *sp = (HSP *)EVROOTDATA(mod);
    HSPDBusUnit *unit = (HSPDBusUnit *)my_calloc(sizeof(HSPDBusUnit));
    unit->name = my_strdup(name);
    unit->processes = UTHASH_NEW(HSPDBusProcess, pid, UTHASH_DFLT);
    uuidgen_type5(sp, (u_char *)unit->uuid, unit->name);
    return unit;
  }

  static void HSPDBusUnitFree(HSPDBusUnit *unit) {
    if(unit->name) my_free(unit->name);
    if(unit->obj) my_free(unit->obj);
    if(unit->cgroup) my_free(unit->cgroup);
    HSPDBusProcess *process;
    UTHASH_WALK(unit->processes, process)
      my_free(process);
    UTHashFree(unit->processes);
    my_free(unit);
  }

  /*_________________---------------------------__________________
    _________________    printDbusElem          __________________
    -----------------___________________________------------------
  */

  static void indent(UTStrBuf *buf, int depth) {
    for(int ii = 0; ii < depth; ii++)
      UTStrBuf_append(buf, "  ");
  }

#define PRINT_DBUS_VAR(it,type,format,buf) do {	\
    type val;					\
    dbus_message_iter_get_basic(it, &val);	\
    UTStrBuf_printf(buf, format, val);		\
} while(0)
  
  static void printDBusElem(DBusMessageIter *it, UTStrBuf *buf, bool ind, int depth, char *suffix) {
    if(ind) indent(buf, depth);
    int atype = dbus_message_iter_get_arg_type(it);
    switch(atype) {
    case DBUS_TYPE_INVALID: break;
    case DBUS_TYPE_STRING: PRINT_DBUS_VAR(it, char *, "\"%s\"", buf); break;
    case DBUS_TYPE_OBJECT_PATH: PRINT_DBUS_VAR(it, char *, "obj=%s", buf); break;
    case DBUS_TYPE_BYTE: PRINT_DBUS_VAR(it, uint8_t, "0x%02x", buf); break;
    case DBUS_TYPE_INT16: PRINT_DBUS_VAR(it, int16_t, "%d", buf); break;
    case DBUS_TYPE_INT32: PRINT_DBUS_VAR(it, int32_t, "%d", buf); break;
    case DBUS_TYPE_INT64: PRINT_DBUS_VAR(it, int64_t, "%"PRId64, buf); break;
    case DBUS_TYPE_UINT16: PRINT_DBUS_VAR(it, uint16_t, "%u", buf); break;
    case DBUS_TYPE_UINT32: PRINT_DBUS_VAR(it, uint32_t, "%u", buf); break;
    case DBUS_TYPE_UINT64: PRINT_DBUS_VAR(it, uint64_t, "%"PRIu64, buf); break;
    case DBUS_TYPE_DOUBLE: PRINT_DBUS_VAR(it, double, "%f", buf); break;
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
      printDBusElem(&sub, buf, NO, depth+1, ")");
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
	do printDBusElem(&sub, buf, YES, depth+1, ",\n");
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
	printDBusElem(&sub, buf, NO, depth+1, " => ");
	dbus_message_iter_next(&sub);
	printDBusElem(&sub, buf, NO, depth+1, NULL);
      }
      while (dbus_message_iter_next(&sub));
      break;
    }
    case DBUS_TYPE_STRUCT: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      UTStrBuf_printf(buf, "struct {\n");
      do printDBusElem(&sub, buf, YES, depth+1, ",\n");
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
    _________________    printDbusMessage       __________________
    -----------------___________________________------------------
  */

  static void printDBusMessage(EVMod *mod, DBusMessage *msg) {
    int mtype = dbus_message_get_type(msg);
    const char *src = dbus_message_get_sender(msg);
    const char *dst = dbus_message_get_destination(msg);
    UTStrBuf *buf = UTStrBuf_new();
    UTStrBuf_printf(buf, "SYSTEMD %s->%s %s(",
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
      do printDBusElem(&iterator, buf, YES, 1, "\n");
      while (dbus_message_iter_next(&iterator));
    }
    UTStrBuf_append(buf, "}\n");
    myDebug(1, "SYSTEMD message: %s", buf->buf);
    UTStrBuf_free(buf);
  }
  
  /*________________---------------------------__________________
    ________________    deltaProcessCPU        __________________
    ----------------___________________________------------------
  */
  
  static uint64_t readProcessCPU(EVMod *mod, HSPDBusProcess *process) {
    // HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    uint64_t cpu_total = 0;
    // compare with the reading of /proc/stat in readCpuCounters.c 
    char path[HSP_SYSTEMD_MAX_FNAME_LEN+1];
    sprintf(path, "/proc/%u/stat", process->pid);
    FILE *statFile = fopen(path, "r");
    if(statFile == NULL) {
      myDebug(2, "cannot open %s : %s", path, strerror(errno));
    }
    else {
      char line[MAX_PROC_LINELEN];
      if(fgets(line, MAX_PROC_LINELEN, statFile)) {
	char *p = line;
	char buf[MAX_PROC_TOKLEN];
	int tok = 0;
	while(parseNextTok(&p, " ", NO, 0, NO, buf, MAX_PROC_TOKLEN)) {
	  switch(++tok) {
	  case 14: // utime
	  case 15: // stime
	  case 16: // cutime
	  case 17: // cstime
	    cpu_total += strtoll(buf, NULL, 0);
	  }
	}
      }
      fclose(statFile);
    }
    // accumulate delta
    if(process->last.cpu_total)
      process->cntr.cpu_total += cpu_total - process->last.cpu_total;
    process->last.cpu_total = cpu_total;
    return process->cntr.cpu_total;
  }
  
  /*________________---------------------------__________________
    ________________   accumulateProcessCPU    __________________
    ----------------___________________________------------------
  */
  
  static uint64_t accumulateProcessCPU(EVMod *mod, HSPDBusUnit *unit) {
    HSPDBusProcess *process;
    uint64_t unit_total = 0;
    UTHASH_WALK(unit->processes, process) {
      unit_total += readProcessCPU(mod, process);
    }
    unit->cntr.cpu_total = unit_total;
    return unit->cntr.cpu_total;
  }
  
  /*________________---------------------------__________________
    ________________    readProcessRAM         __________________
    ----------------___________________________------------------
  */
  
  static uint64_t readProcessRAM(EVMod *mod, HSPDBusProcess *process) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    uint64_t rss = 0;
    char path[HSP_SYSTEMD_MAX_FNAME_LEN+1];
    sprintf(path, "/proc/%u/statm", process->pid);
    FILE *statFile = fopen(path, "r");
    if(statFile == NULL) {
      myDebug(2, "cannot open %s : %s", path, strerror(errno));
    }
    else {
      char line[MAX_PROC_LINELEN];
      if(fgets(line, MAX_PROC_LINELEN, statFile)) {
	char *p = line;
	char buf[MAX_PROC_TOKLEN];
	int tok = 0;
	while(parseNextTok(&p, " ", NO, 0, NO, buf, MAX_PROC_TOKLEN)) {
	  switch(++tok) {
	  case 2: // resident
	    rss += strtoll(buf, NULL, 0);
	  }
	}
      }
      fclose(statFile);
    }
    return rss * mdata->page_size;
  }
  
  /*________________---------------------------__________________
    ________________   accumulateProcessRAM    __________________
    ----------------___________________________------------------
  */
  
  static uint64_t accumulateProcessRAM(EVMod *mod, HSPDBusUnit *unit) {
    uint64_t rss = 0;
    HSPDBusProcess *process;
    UTHASH_WALK(unit->processes, process) {
      rss += readProcessRAM(mod, process);
    }
    return rss;
  }
  
  /*________________---------------------------__________________
    ________________    readProcessIO          __________________
    ----------------___________________________------------------
  */
  
  static bool readProcessIO(EVMod *mod, HSPDBusProcess *process, SFLHost_vrt_dsk_counters *dskio) {
    int found = NO;
    uint64_t rd_bytes = 0;
    uint64_t wr_bytes = 0;
    char path[HSP_SYSTEMD_MAX_FNAME_LEN+1];
    sprintf(path, "/proc/%u/io", process->pid);
    FILE *statFile = fopen(path, "r");
    if(statFile == NULL) {
      myDebug(2, "cannot open %s : %s", path, strerror(errno));
    }
    else {
      found = YES;
      char line[MAX_PROC_LINELEN];
      while(fgets(line, MAX_PROC_LINELEN, statFile)) {
	char var[MAX_PROC_TOKLEN];
	uint64_t val64;
	if(sscanf(line, "%s %"SCNu64, var, &val64) == 2) {
	  if(!strcmp(var, "read_bytes:")
	     || !strcmp(var, "rchar:"))
	    rd_bytes += val64;
	  else if(!strcmp(var, "write_bytes:")
		  || !strcmp(var, "wchar:"))
	    wr_bytes += val64;
	}
      }
      fclose(statFile);
    }
    // accumulate deltas
    if(process->last.rd_bytes) process->cntr.rd_bytes += rd_bytes - process->last.rd_bytes;
    process->last.rd_bytes = rd_bytes;
    if(process->last.wr_bytes) process->cntr.wr_bytes += wr_bytes - process->last.wr_bytes;
    process->last.wr_bytes = wr_bytes;
    // feed sflow struct
    dskio->rd_bytes += process->cntr.rd_bytes;
    dskio->wr_bytes += process->cntr.wr_bytes;
    return found;
  }
  
  /*________________---------------------------__________________
    ________________   accumulateProcessIO     __________________
    ----------------___________________________------------------
  */
  
  static bool accumulateProcessIO(EVMod *mod, HSPDBusUnit *unit, SFLHost_vrt_dsk_counters *dskio) {
    bool gotData = NO;
    HSPDBusProcess *process;
    UTHASH_WALK(unit->processes, process) {
      gotData |= readProcessIO(mod, process, dskio);
    }
    return gotData;
  }

  /*_________________---------------------------__________________
    _________________     readCgroupCounters    __________________
    -----------------___________________________------------------
  */

  static bool readCgroupCounters(EVMod *mod, char *acct, char *cgroup, char *fname, int nvals, HSPNameVal *nameVals, bool multi) {
    int found = 0;
    char statsFileName[HSP_SYSTEMD_MAX_FNAME_LEN+1];
    snprintf(statsFileName, HSP_SYSTEMD_MAX_FNAME_LEN, "/sys/fs/cgroup/%s%s/%s", acct, cgroup, fname);
    FILE *statsFile = fopen(statsFileName, "r");
    if(statsFile == NULL) {
      myDebug(2, "cannot open %s : %s", statsFileName, strerror(errno));
    }
    else {
      char line[HSP_SYSTEMD_MAX_STATS_LINELEN];
      char var[HSP_SYSTEMD_MAX_STATS_LINELEN];
      uint64_t val64;
      char *fmt = multi ?
	"%*s %s %"SCNu64 :
	"%s %"SCNu64 ;
      while(fgets(line, HSP_SYSTEMD_MAX_STATS_LINELEN, statsFile)) {
	if(found == nvals && !multi) break;
	if(sscanf(line, fmt, var, &val64) == 2) {
	  for(int ii = 0; ii < nvals; ii++) {
	    char *nm = nameVals[ii].nv_name;
	    if(nm == NULL) break; // null name is double-check
	    if(strcmp(var, nm) == 0)  {
	      nameVals[ii].nv_found = YES;
	      nameVals[ii].nv_val64 += val64;
	      found++;
	    }
	  }
        }
      }
      fclose(statsFile);
    }
    return (found > 0);
  }
  
  /*________________---------------------------__________________
    ________________   getCounters_SYSTEMD     __________________
    ----------------___________________________------------------
  */
  
  static void getCounters_SYSTEMD(EVMod *mod, HSPVMState_SYSTEMD *container)
  {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    HSPDBusUnit search = { .name = container->id };
    HSPDBusUnit *unit = UTHashGet(mdata->units, &search);
    if(unit == NULL
       || unit->cgroup == NULL
       || UTHashN(unit->processes) == 0) {
      removeAndFreeVM_SYSTEMD(mod, container);
      return;
    }
    
    SFL_COUNTERS_SAMPLE_TYPE cs = { 0 };
    HSPVMState *vm = (HSPVMState *)&container->vm;
    // host ID
    SFLCounters_sample_element hidElem = { 0 };
    hidElem.tag = SFLCOUNTERS_HOST_HID;
    hidElem.counterBlock.host_hid.hostname.str = container->id;
    hidElem.counterBlock.host_hid.hostname.len = my_strlen(container->id);
    memcpy(hidElem.counterBlock.host_hid.uuid, vm->uuid, 16);

    // we can show the same OS attributes as the parent
    hidElem.counterBlock.host_hid.machine_type = sp->machine_type;
    hidElem.counterBlock.host_hid.os_name = SFLOS_linux;
    hidElem.counterBlock.host_hid.os_release.str = sp->os_release;
    hidElem.counterBlock.host_hid.os_release.len = my_strlen(sp->os_release);
    SFLADD_ELEMENT(&cs, &hidElem);

    // host parent
    SFLCounters_sample_element parElem = { 0 };
    parElem.tag = SFLCOUNTERS_HOST_PAR;
    parElem.counterBlock.host_par.dsClass = SFL_DSCLASS_PHYSICAL_ENTITY;
    parElem.counterBlock.host_par.dsIndex = HSP_DEFAULT_PHYSICAL_DSINDEX;
    SFLADD_ELEMENT(&cs, &parElem);

    // TODO: can we gather NIO stats by PID?  It looks like maybe not.  The numbers
    // in /proc/<pid>/net/dev are the same as in /proc/net/dev.  mod_docker gets the
    // numbers because veth devices are used to connect between network namespaces
    // but with no cgroup network accounting we would have to follow the links in
    // /proc/<pid>/fd/* and accumuate the list of socket inodes,  then query those
    // inodes for the counters -- assuming that sockets are not shared between processes
    // in different cgroups.  The fact that sockets can appear and disappear in a
    // short timeframe makes this hard to deal with accurately.  Collecting the listen
    // ports (SAPs) for a service would make more sense.  Then that list could be
    // exported,  and packet-samples could be annotated with service name if they
    // were to or from one of those SAPs.
    // VM Net I/O
    // SFLCounters_sample_element nioElem = { 0 };
    // nioElem.tag = SFLCOUNTERS_HOST_VRT_NIO;
    // accumulateProcessNIO(mod, unit, (SFLHost_nio_counters *)&nioElem.counterBlock.host_vrt_nio);
    // SFLADD_ELEMENT(&cs, &nioElem);

    // VM cpu counters [ref xenstat.c]
    SFLCounters_sample_element cpuElem = { 0 };
    cpuElem.tag = SFLCOUNTERS_HOST_VRT_CPU;
    cpuElem.counterBlock.host_vrt_cpu.nrVirtCpu = 0;
    SFL_UNDEF_COUNTER(cpuElem.counterBlock.host_vrt_cpu.cpuTime);

    // map service state into SFLVirDomainState. We will stop
    // reporting counters when a unit is not loaded or active,
    // so this will always be "running":
    enum SFLVirDomainState virState = SFL_VIR_DOMAIN_RUNNING;
    cpuElem.counterBlock.host_vrt_cpu.state = virState;

    uint64_t cpu_total = 0;
    if(unit->cpuAccounting) {
      HSPNameVal cpuVals[] = {
	{ "user",0,0 },
	{ "system",0,0},
	{ NULL,0,0},
      };
      if(readCgroupCounters(mod, "cpuacct", unit->cgroup, "cpuacct.stat", 2, cpuVals, NO)) {
	if(cpuVals[0].nv_found) cpu_total += cpuVals[0].nv_val64;
	if(cpuVals[1].nv_found) cpu_total += cpuVals[1].nv_val64;
      }
    }
    if(cpu_total == 0) {
      cpu_total = accumulateProcessCPU(mod, unit);
    }
    cpuElem.counterBlock.host_vrt_cpu.cpuTime = (uint32_t)(JIFFY_TO_MS(cpu_total));
    SFLADD_ELEMENT(&cs, &cpuElem);

    SFLCounters_sample_element memElem = { 0 };
    memElem.tag = SFLCOUNTERS_HOST_VRT_MEM;
    uint64_t rss = 0;
    if(unit->memoryAccounting) {
      HSPNameVal memVals[] = {
	{ "rss",0,0 },
	{ NULL,0,0},
      };
      if(readCgroupCounters(mod, "memory", unit->cgroup, "memory.stat", 2, memVals, NO)) {
	if(memVals[0].nv_found) rss += memVals[0].nv_val64;
      }
    }
    if(rss == 0) {
      rss = accumulateProcessRAM(mod, unit);
    }
    memElem.counterBlock.host_vrt_mem.memory = rss;
    // TODO: get max memory (from DBUS?)
    // memElem.counterBlock.host_vrt_mem.maxMemory = maxMem;
    SFLADD_ELEMENT(&cs, &memElem);

    // VM disk I/O counters
    SFLCounters_sample_element dskElem = { 0 };
    dskElem.tag = SFLCOUNTERS_HOST_VRT_DSK;
    if(unit->blockIOAccounting) {
      HSPNameVal dskValsB[] = {
	{ "Read",0,0 },
	{ "Write",0,0},
	{ NULL,0,0},
      };
      if(readCgroupCounters(mod, "blkio", unit->cgroup, "blkio.io_service_bytes_recursive", 2, dskValsB, YES)) {
	if(dskValsB[0].nv_found) {
	  dskElem.counterBlock.host_vrt_dsk.rd_bytes += dskValsB[0].nv_val64;
	}
	if(dskValsB[1].nv_found) {
	  dskElem.counterBlock.host_vrt_dsk.wr_bytes += dskValsB[1].nv_val64;
	}
      }
      
      HSPNameVal dskValsO[] = {
	{ "Read",0,0 },
	{ "Write",0,0},
	{ NULL,0,0},
      };
      
      if(readCgroupCounters(mod, "blkio", unit->cgroup, "blkio.io_serviced_recursive", 2, dskValsO, YES)) {
	if(dskValsO[0].nv_found) {
	  dskElem.counterBlock.host_vrt_dsk.rd_req += dskValsO[0].nv_val64;
	}
	if(dskValsO[1].nv_found) {
	  dskElem.counterBlock.host_vrt_dsk.wr_req += dskValsO[1].nv_val64;
	}
      }
    }
    else {
      // This requires root privileges to be retained, so don't even try
      // unless we are still root:
      if(getuid() == 0)
	accumulateProcessIO(mod, unit, &dskElem.counterBlock.host_vrt_dsk);
    }
    // TODO: can we fill in capacity, allocation and available?
    SFLADD_ELEMENT(&cs, &dskElem);

    SEMLOCK_DO(sp->sync_agent) {
      sfl_poller_writeCountersSample(vm->poller, &cs);
      sp->counterSampleQueued = YES;
      sp->telemetry[HSP_TELEMETRY_COUNTER_SAMPLES]++;
    }
  }

  /*_________________---------------------------__________________
    _________________     dbusMethod            __________________
    -----------------___________________________------------------
  */

#define HSP_dbusMethod_endargs DBUS_TYPE_INVALID,NULL
  
  static void dbusMethod(EVMod *mod, HSPDBusHandler reqCB, void *magic, char *target, char  *obj, char *interface, char *method, ...) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    DBusMessage *msg = dbus_message_new_method_call(target, obj, interface, method);
    if(msg == NULL) {
      log_dbus_error(mod, "dbus_message_new_method_call");
      return;
    }
    // append arguments
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    va_list args;
    va_start(args, method);
    for(;;) {
      int type = va_arg(args, int);
      void *arg = va_arg(args, void *);
      if(type == DBUS_TYPE_INVALID || arg == NULL) break;
      dbus_message_iter_append_basic(&iter, type, &arg);
    }
    va_end(args);
    // send the message
    uint32_t serial = 0;
    if(!dbus_connection_send(mdata->connection, msg, &serial)) {
      log_dbus_error(mod, "dbus_connection_send");
    }
    else {
      myDebug(1, "SYSTEMD dbus method %s serial=%u", method, serial);
      // register the handler
      HSPDBusRequest *req = (HSPDBusRequest *)my_calloc(sizeof(HSPDBusRequest));
      req->serial = serial;
      req->handler = reqCB;
      req->magic = magic;
      EVClockMono(&req->send_time);
      UTHashAdd(mdata->dbusRequests, req);
      mdata->dbus_tx++;
    }
    dbus_message_unref(msg);
  }

  /*_________________---------------------------__________________
    _________________    getDbusProperty        __________________
    -----------------___________________________------------------
  */
  static void getDbusProperty(EVMod *mod, HSPDBusUnit *unit, HSPDBusHandler reqCB, char *property) {
    dbusMethod(mod,
	       reqCB,
	       unit,
	       "org.freedesktop.systemd1",
	       unit->obj,
	       "org.freedesktop.DBus.Properties",
	       "Get",
	       DBUS_TYPE_STRING,
	       "org.freedesktop.systemd1.Service",
	       DBUS_TYPE_STRING,
	       property,
	       HSP_dbusMethod_endargs);
  }

  /*_________________---------------------------__________________
    _________________     db_get, db_next       __________________
    -----------------___________________________------------------
    When decoding a particular method response we know what we are
    willing to accept,  so the parsing is much simpler.  Because the
    iterator starts with the first element already "loaded" and
    libdbus exits if we try to walk off the end of an array, we have
    to be careful how we walk.  Patterns that work are:

    do { if(db_get(it,...)) {...} } while(db_next(it));

    or:

    if(db_get(it...) && db_get_next(it,...) && db_get_next(it, ...))

    or the DB_WALK() macro can be used to walk over a sequence of the same type
    and stop when a different type is found or the iterator is done.
  */

  static bool db_get(DBusMessageIter *it, int expected_type, DBusBasicValue *val) {
    int atype = dbus_message_iter_get_arg_type(it);
    if(atype == DBUS_TYPE_VARIANT) {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      return db_get(&sub, expected_type, val);
    }
    bool expected = (atype == expected_type);
    if(expected
       && val)
      dbus_message_iter_get_basic(it, val);
    return expected;
  }

  static bool db_next(DBusMessageIter *it) {
    return dbus_message_iter_next(it);
  }

  static bool db_get_next(DBusMessageIter *it, int expected_type, DBusBasicValue *val) {
    return db_next(it) && db_get(it, expected_type, val);
  }

#define DB_WALK(it, atype, val)  for(bool _more = YES; _more && db_get((it), (atype), (val)); _more = db_next(it))

  /*_________________---------------------------__________________
    _________________   handler_<property>      __________________
    -----------------___________________________------------------
  */

  static void handler_cpuAccounting(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSPDBusUnit *unit = (HSPDBusUnit *)magic;
    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      DBusBasicValue val;
      if(db_get(&it, DBUS_TYPE_BOOLEAN, &val)) {
	myDebug(1, "UNIT CPUAccounting %u", val.bool_val);
	unit->cpuAccounting = val.bool_val;
      }
    }
  }

  static void handler_memoryAccounting(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSPDBusUnit *unit = (HSPDBusUnit *)magic;
    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      DBusBasicValue val;
      if(db_get(&it, DBUS_TYPE_BOOLEAN, &val)) {
	myDebug(1, "UNIT memoryAccounting %u", val.bool_val);
	unit->memoryAccounting = val.bool_val;
      }
    }
  }

  static void handler_blockIOAccounting(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSPDBusUnit *unit = (HSPDBusUnit *)magic;
    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      DBusBasicValue val;
      if(db_get(&it, DBUS_TYPE_BOOLEAN, &val)) {
	myDebug(1, "UNIT BlockIOAccounting %u", val.bool_val);
	unit->blockIOAccounting = val.bool_val;
      }
    }
  }

  /*_________________---------------------------__________________
    _________________   handler_controlGroup    __________________
    -----------------___________________________------------------
  */

  static void handler_controlGroup(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSPDBusUnit *unit = (HSPDBusUnit *)magic;
    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      DBusBasicValue val;
      if(db_get(&it, DBUS_TYPE_STRING, &val)
	 && val.str
	 && my_strlen(val.str)
	 && regexec(mdata->system_slice_regex, val.str, 0, NULL, 0) == 0) {
	myDebug(1, "UNIT CGROUP[cgroup=\"%s\"]", val.str);
	unit->cgroup = my_strdup(val.str);
	// read the process ids
	
	// mark and sweep - mark
	HSPDBusProcess *process;
	UTHASH_WALK(unit->processes, process)
	  process->marked = YES;
	
	char path[HSP_SYSTEMD_MAX_FNAME_LEN+1];
	sprintf(path, "/sys/fs/cgroup/systemd/%s/cgroup.procs", val.str);
	FILE *pidsFile = fopen(path, "r");
	if(pidsFile == NULL) {
	  myDebug(2, "cannot open %s : %s", path, strerror(errno));
	}
	else {
	  char line[MAX_PROC_LINELEN];
	  uint64_t pid64;
	  while(fgets(line, MAX_PROC_LINELEN, pidsFile)) {
	    if(sscanf(line, "%"SCNu64, &pid64) == 1) {
	      myDebug(1, "got PID=%"PRIu64, pid64);
	      HSPDBusProcess search = { .pid = pid64 };
	      process = UTHashGet(unit->processes, &search);
	      if(process)
		process->marked = NO;
	      else {		
		HSPDBusProcess *process = (HSPDBusProcess *)my_calloc(sizeof(HSPDBusProcess));
		process->pid = pid64;
		UTHashAdd(unit->processes, process);
	      }
	    }
	  }
	  fclose(pidsFile);

	  if(UTHashN(unit->processes)) {
	    // mark and sweep - sweep
	    UTHASH_WALK(unit->processes, process)
	      if(process->marked)
		if(UTHashDel(unit->processes, process))
		  my_free(process);
	    // find or allocate the container
	    getContainer(mod, unit, YES);
	    getDbusProperty(mod, unit, handler_cpuAccounting, "CPUAccounting");
	    getDbusProperty(mod, unit, handler_memoryAccounting, "MemoryAccounting");
	    getDbusProperty(mod, unit, handler_blockIOAccounting, "BlockIOAccounting");
	    // TODO: could try and get "MemoryCurrent" and "CPUUsageNSec" here, but since they
	    // are usually not limited,  these numbers are usually == (uint64_t)-1.  So
	    // we have to get the numbers from the cgroup accounting (if enabled) or fall
	    // back on getting the numbers from each process.
	  }
	}
      }
    }
  }

  /*_________________---------------------------__________________
    _________________   handler_getUnit         __________________
    -----------------___________________________------------------
  */

  static void handler_getUnit(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSPDBusUnit *unit = (HSPDBusUnit *)magic;
    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      DBusBasicValue val;
      if(db_get(&it, DBUS_TYPE_OBJECT_PATH, &val)
	 && val.str) {
	unit->obj = my_strdup(val.str);
	myDebug(1, "UNIT OBJ[obj=\"%s\"]", val.str);
	dbusMethod(mod,
		   handler_controlGroup,
		   unit,
		   "org.freedesktop.systemd1",
		   unit->obj,
		   "org.freedesktop.DBus.Properties",
		   "Get",
		   DBUS_TYPE_STRING,
		   "org.freedesktop.systemd1.Service",
		   DBUS_TYPE_STRING,
		   "ControlGroup",
		   HSP_dbusMethod_endargs);
      }
    }
  }
  
  /*_________________---------------------------__________________
    _________________   handler_listUnits       __________________
    -----------------___________________________------------------

    expect array of units, where each unit is a struct with ssssssouso
    {
    char *unit_name;
    char *unit_descr;
    char *load_state;
    char *active_state;
    char *sub_state;
    char *following;
    char *obj_path;
    uint32_t job_queued;
    char *job_type;
    char *job_obj_path;
    }
  */

  static void handler_listUnits(EVMod *mod, DBusMessage *dbm, void *magic) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSPDBusUnit *unit;

    // mark and sweep - mark here
    UTHASH_WALK(mdata->units, unit)  unit->marked = YES;

    DBusMessageIter it;
    if(dbus_message_iter_init(dbm, &it)) {
      if(db_get(&it, DBUS_TYPE_ARRAY, NULL)) {
	DBusMessageIter it_unit;
	dbus_message_iter_recurse(&it, &it_unit);
	DB_WALK(&it_unit, DBUS_TYPE_STRUCT, NULL) {
	  DBusMessageIter it_field;
	  dbus_message_iter_recurse(&it_unit, &it_field);
	  DBusBasicValue nm, ds, ls, as;
	  if(db_get(&it_field,  DBUS_TYPE_STRING, &nm)
	     && db_get_next(&it_field, DBUS_TYPE_STRING, &ds)
	     && db_get_next(&it_field, DBUS_TYPE_STRING, &ls)
	     && db_get_next(&it_field, DBUS_TYPE_STRING, &as)) {
	    if(nm.str
	       && my_strlen(nm.str)
	       && my_strequal(ls.str, "loaded")
	       && my_strequal(as.str, "active")
	       && regexec(mdata->service_regex, nm.str, 0, NULL, 0) == 0) {
	      HSPDBusUnit search = { .name = nm.str };
	      unit = UTHashGet(mdata->units, &search);
	      if(unit) {
		unit->marked = NO;
	      }
	      else {
		unit = HSPDBusUnitNew(mod, nm.str);
		UTHashAdd(mdata->units, unit);
	      }
	      myDebug(1, "UNIT[name=\"%s\" descr=\"%s\" load=\"%s\" active=\"%s\"]", nm.str, ds.str, ls.str, as.str);
	      dbusMethod(mod,
			 handler_getUnit,
			 unit,
			 "org.freedesktop.systemd1",
			 "/org/freedesktop/systemd1",
			 "org.freedesktop.systemd1.Manager",
			 "GetUnit",
			 DBUS_TYPE_STRING,
			 nm.str,
			 HSP_dbusMethod_endargs);
	    }
	  }
	}
      }
    }
    // mark and sweep - sweep here
    UTHASH_WALK(mdata->units, unit) {
      if(unit->marked) {
	UTHashDel(mdata->units, unit);
	HSPDBusUnitFree(unit);
      }
    }
  }

  /*_________________---------------------------__________________
    _________________   dbusSynchronize         __________________
    -----------------___________________________------------------
  */

  static void dbusSynchronize(EVMod *mod) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;

#if HSP_DBUS_MONITOR
    if(!mdata->subscribed) {
      mdata->subscribed = YES;
      dbusMethod(mod,
		 NULL,
		 NULL,
		 "org.freedesktop.systemd1",
		 "/org/freedesktop/systemd1",
		 "org.freedesktop.systemd1.Manager",
		 "Subscribe",
		 HSP_dbusMethod_endargs);
    }
#endif

    if(UTHashN(mdata->dbusRequests)) {
      myDebug(1, "SYSTEMD: dbusSynchronize - outstanding requests=%u", UTHashN(mdata->dbusRequests));
      struct timespec now;
      EVClockMono(&now);
      HSPDBusRequest *req;
      UTHASH_WALK(mdata->dbusRequests, req) {
	int delay_mS = EVTimeDiff_mS(&req->send_time, &now);
	if(delay_mS > HSP_DBUS_TIMEOUT_mS) {
	  myLog(LOG_ERR, "SYSTEMD dbus request timeout (serial=%u, delay_mS=%d)", req->serial, delay_mS);
	  UTHashDel(mdata->dbusRequests, req);
	  my_free(req);
	}
      }
    }
    else {
      // kick off a unit discovery sweep
      dbusMethod(mod,
		 handler_listUnits,
		 NULL,
		 "org.freedesktop.systemd1",
		 "/org/freedesktop/systemd1",
		 "org.freedesktop.systemd1.Manager",
		 "ListUnits",
		 HSP_dbusMethod_endargs);
    }
  }


  /*_________________---------------------------__________________
    _________________    evt_config_first       __________________
    -----------------___________________________------------------
  */

  static void evt_config_first(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    mdata->countdownToResync = HSP_SYSTEMD_WAIT_STARTUP;
  }

  /*_________________---------------------------__________________
    _________________    tick,tock              __________________
    -----------------___________________________------------------
  */

  static void evt_tick(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(mdata->countdownToResync) {
      if(--mdata->countdownToResync == 0) {
    	dbusSynchronize(mod);
	mdata->countdownToResync = sp->systemd.refreshVMListSecs ?: sp->refreshVMListSecs;
      }
    }
  }

  static void evt_tock(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    // now we can execute pollActions without holding on to the semaphore
    HSPVMState_SYSTEMD *container;
    UTHASH_WALK(mdata->pollActions, container) {
      getCounters_SYSTEMD(mod, container);
    }
    UTHashReset(mdata->pollActions);
  }

  // obtaining a selectable file-descriptor from libdbus is not as easy
  // as it ought to be, so it turns out that polling with
  // dbus_connection_read_write_dispatch() is the easiest way to drive
  // the bus (so that method calls are asychronous and monitoring with filters
  // can be layered on top if required).
  // In most cases a single poll is enough to propagate the message through one
  // way or the other, but when we ask to "ListUnits" it actually takes
  // about 20 polls before the data finally starts to appear for us in the
  // dbusCB filter callback.  (I think that means a single poll of
  // dbux_connection_read_write_dispatch() will sometimes only trigger
  // a single socket read() operation with a limited size buffer.)
  // We could spin tightly as long as their is an outstanding request, but
  // it seems safer to do this polling on deciTick and allow that it might
  // take a second or two of extra time before a call such as ListUnits delivers
  // results.  Better to accept extra latency than risk going into a busy loop.
  // If we see progress in terms of messages send or received, then we
  // keep spinning, so a flurry of short method calls will complete quickly.
  
  static void evt_deci(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    bool dbpoll = (UTHashN(mdata->dbusRequests) > 0);
#if HSP_DBUS_MONITOR
    dbpoll = YES
#endif
    if(dbpoll) {
      myDebug(2, "SYSTEMD deci - outstanding=%u tx=%u rx=%u", UTHashN(mdata->dbusRequests), mdata->dbus_tx, mdata->dbus_rx);
      uint32_t curr_tx = mdata->dbus_tx;
      uint32_t curr_rx = mdata->dbus_rx;
      for(;;) {
	// keep iterating here as long as visible progress is made
	dbus_connection_read_write_dispatch(mdata->connection, 0);
	if(curr_tx == mdata->dbus_tx &&
	   curr_rx == mdata->dbus_rx)
	  break;
	curr_tx = mdata->dbus_tx;
	curr_rx = mdata->dbus_rx;
      }
    }
  }

  /*_________________---------------------------__________________
    _________________   host counter sample     __________________
    -----------------___________________________------------------
  */

  static void evt_host_cs(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    SFL_COUNTERS_SAMPLE_TYPE *cs = *(SFL_COUNTERS_SAMPLE_TYPE **)data;
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->kvm.kvm
       || sp->docker.docker) {
      // TODO: clean this up.  Some kind of priority scheme?
      // requestVNodeRole(mod, SYSTEMD_VNODE_PRIORITY);
      // ...
      // if(hasVNodeRole(mod)) { }
      // then use in mod_kvm, mod_xen, mod_docker and here.
      return;
    }

    memset(&mdata->vnodeElem, 0, sizeof(mdata->vnodeElem));
    mdata->vnodeElem.tag = SFLCOUNTERS_HOST_VRT_NODE;
    mdata->vnodeElem.counterBlock.host_vrt_node.mhz = sp->cpu_mhz;
    mdata->vnodeElem.counterBlock.host_vrt_node.cpus = sp->cpu_cores;
    mdata->vnodeElem.counterBlock.host_vrt_node.num_domains = UTHashN(mdata->vmsByID);
    mdata->vnodeElem.counterBlock.host_vrt_node.memory = sp->mem_total;
    mdata->vnodeElem.counterBlock.host_vrt_node.memory_free = sp->mem_free;
    SFLADD_ELEMENT(cs, &mdata->vnodeElem);
  }

  /*_________________---------------------------__________________
    _________________         evt_final         __________________
    -----------------___________________________------------------
  */

  static void evt_final(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    if(mdata->connection) {
      dbus_connection_close(mdata->connection);
      mdata->connection = NULL;
    }
  }
  
  /*_________________---------------------------__________________
    _________________       dbusCB              __________________
    -----------------___________________________------------------
  */

static DBusHandlerResult dbusCB(DBusConnection *connection, DBusMessage *message, void *user_data)
{
  EVMod *mod = user_data;
  HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
  mdata->dbus_rx++;

  if(debug(2))
    printDBusMessage(mod, message);
  
  if(dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    int serial = dbus_message_get_reply_serial(message);
    HSPDBusRequest search = { .serial = serial };
    HSPDBusRequest *req = UTHashDelKey(mdata->dbusRequests, &search);
    if(req) {
      if(debug(2)) {
	struct timespec now;
	EVClockMono(&now);
	myLog(LOG_INFO, "serial=%u response_mS=%d",
	      req->serial,
	      EVTimeDiff_mS(&req->send_time, &now));
      }
      if(req->handler)
	(*req->handler)(mod, message, req->magic);
      my_free(req);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  }
  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
  
  /*_________________---------------------------__________________
    _________________    addMatch               __________________
    -----------------___________________________------------------
  */
#if HSP_DBUS_MONITOR
  static void addMatch(EVMod *mod, char *rule) {
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    dbus_bus_add_match(mdata->connection, rule, &mdata->error);
    if(dbus_error_is_set(&mdata->error)) {
      myLog(LOG_ERR, "SYSTEMD: addMatch() error adding <%s>", rule);
      log_dbus_error(mod, "dbus_bus_add_match");
    }
  }
#endif
  
  /*_________________---------------------------__________________
    _________________    module init            __________________
    -----------------___________________________------------------
  */

  void mod_systemd(EVMod *mod) {
    mod->data = my_calloc(sizeof(HSP_mod_SYSTEMD));
    HSP_mod_SYSTEMD *mdata = (HSP_mod_SYSTEMD *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->systemd.dropPriv == NO)
      retainRootRequest(mod, "needed to read /proc/<pid>/io (if cgroup BlockIOAccounting is off).");

    // get page size for scaling memory pages->bytes
#if defined(PAGESIZE)
    mdata->page_size = PAGESIZE;
#elif defined(PAGE_SIZE)
    mdata->page_size = PAGE_SIZE;
#else
    mdata->page_size = sysconf(_SC_PAGE_SIZE);
#endif

    // this mod operates entirely on the pollBus thread
    mdata->pollBus = EVGetBus(mod, HSPBUS_POLL, YES);
      
    mdata->vmsByUUID = UTHASH_NEW(HSPVMState_SYSTEMD, vm.uuid, UTHASH_DFLT);
    mdata->vmsByID = UTHASH_NEW(HSPVMState_SYSTEMD, id, UTHASH_SKEY);
    mdata->pollActions = UTHASH_NEW(HSPVMState_SYSTEMD, id, UTHASH_IDTY);
    mdata->dbusRequests = UTHASH_NEW(HSPDBusRequest, serial, UTHASH_DFLT);
    mdata->units = UTHASH_NEW(HSPDBusUnit, name, UTHASH_SKEY);

    mdata->service_regex = UTRegexCompile(HSP_SYSTEMD_SERVICE_REGEX);
    mdata->system_slice_regex = UTRegexCompile(HSP_SYSTEMD_SYSTEM_SLICE_REGEX);

    dbus_error_init(&mdata->error);
    if((mdata->connection = dbus_bus_get(DBUS_BUS_SYSTEM, &mdata->error)) == NULL) {
      myLog(LOG_ERR, "dbus_bug_get error");
      return;
    }

#if HSP_DBUS_MONITOR    
    /* TODO: possible eavesdropping if we want to detect service start/stop asynchronously */
    /* addMatch(mod, "eavesdrop=true,type='signal'"); */
    /* addMatch(mod, "eavesdrop=true,type='method_call'"); */
    /* addMatch(mod, "eavesdrop=true,type='method_return'"); */
    /* addMatch(mod, "eavesdrop=true,type='error'"); */
#endif

    // register dispatch callback
    if(!dbus_connection_add_filter(mdata->connection, dbusCB, mod, NULL)) {
      log_dbus_error(mod, "dbus_connection_add_filter");
      return;
    }

    // request name
    dbus_bus_request_name(mdata->connection, "org.sflow.hsflowd.modsystemd", DBUS_NAME_FLAG_REPLACE_EXISTING, &mdata->error);
    if(dbus_error_is_set(&mdata->error)) {
      log_dbus_error(mod, "dbus_bus_request_name");
    }

    // connection OK - so register call-backs
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_TICK), evt_tick);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_DECI), evt_deci);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_TOCK), evt_tock);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_HOST_COUNTER_SAMPLE), evt_host_cs);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_CONFIG_FIRST), evt_config_first);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_FINAL), evt_final);
  }

#if defined(__cplusplus)
} /* extern "C" */
#endif
