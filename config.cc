// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "auth/Auth.h"
#include "auth/ExportControl.h"
#include "ceph_ver.h"
#include "common/BackTrace.h"
#include "common/Clock.h"
#include "common/ConfUtils.h"
#include "common/ProfLogger.h"
#include "common/common_init.h"
#include "common/dyn_snprintf.h"
#include "config.h"
#include "include/atomic.h"
#include "include/str_list.h"
#include "include/types.h"
#include "msg/msg_types.h"
#include "osd/osd_types.h"

#include <fcntl.h>
#include <fstream>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Don't use standard Ceph logging in this file.
 * We can't use logging until it's initialized, and a lot of the necessary
 * initialization happens here.
 */
#undef dout
#undef pdout
#undef derr
#undef generic_dout
#undef dendl

/* The Ceph configuration. */
md_config_t g_conf __attribute__((init_priority(103)));

/* These should be moved into md_config_t eventually, grrr */
static ConfFile *cf = NULL;
static ExportControl *ec = NULL;

atomic_t _num_threads(0);

// file layouts
struct ceph_file_layout g_default_file_layout = {
 fl_stripe_unit: init_le32(1<<22),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<22),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred : init_le32(-1),
 fl_pg_pool : init_le32(-1),
};

static void env_override(char **ceph_var, const char * const env_var)
{
  char *e = getenv(env_var);
  if (!e)
    return;
  if (*ceph_var)
    free(*ceph_var);
  *ceph_var = strdup(e);
}

void env_to_vec(std::vector<const char*>& args)
{
  char *p = getenv("CEPH_ARGS");
  if (!p) return;

  static char buf[1000];
  int len = MIN(strlen(p), sizeof(buf)-1);  // bleh.
  memcpy(buf, p, len);
  buf[len] = 0;
  //cout << "CEPH_ARGS='" << p << ";" << endl;

  p = buf;
  while (*p && p < buf + len) {
    char *e = p;
    while (*e && *e != ' ')
      e++;
    *e = 0;
    args.push_back(p);
    //cout << "arg " << p << std::endl;
    p = e+1;
  }
}

void env_to_deq(std::deque<const char*>& args)
{
  char *p = getenv("CEPH_ARGS");
  if (!p) return;

  static char buf[1000];
  int len = MIN(strlen(p), sizeof(buf)-1);  // bleh.
  memcpy(buf, p, len);
  buf[len] = 0;

  p = buf;
  while (*p && p < buf + len) {
    char *e = p;
    while (*e && *e != ' ')
      e++;
    *e = 0;
    args.push_back(p);
    p = e+1;
  }
}

void argv_to_vec(int argc, const char **argv,
                 std::vector<const char*>& args)
{
  for (int i=1; i<argc; i++)
    args.push_back(argv[i]);
}

void argv_to_deq(int argc, const char **argv,
                 std::deque<const char*>& args)
{
  for (int i=1; i<argc; i++)
    args.push_back(argv[i]);
}

void vec_to_argv(std::vector<const char*>& args,
                 int& argc, const char **&argv)
{
  const char *myname = "asdf";
  if (argc && argv)
    myname = argv[0];
  argv = (const char**)malloc(sizeof(char*) * argc);
  argc = 1;
  argv[0] = myname;

  for (unsigned i=0; i<args.size(); i++)
    argv[argc++] = args[i];
}

bool parse_ip_port_vec(const char *s, vector<entity_addr_t>& vec)
{
  const char *p = s;
  const char *end = p + strlen(p);
  while (p < end) {
    entity_addr_t a;
    //cout << " parse at '" << p << "'" << std::endl;
    if (!a.parse(p, &p)) {
      //dout(0) << " failed to parse address '" << p << "'" << dendl;
      return false;
    }
    //cout << " got " << a << ", rest is '" << p << "'" << std::endl;
    vec.push_back(a);
    while (*p == ',' || *p == ' ')
      p++;
  }
  return true;
}

void parse_config_option_string(string& s)
{
  char b[s.length()+1];
  strcpy(b, s.c_str());
  vector<const char*> nargs;
  char *p = b;
  while (*p) {
    nargs.push_back(p);
    while (*p && *p != ' ') p++;
    if (!*p)
      break;
    *p++ = 0;
    while (*p && *p == ' ') p++;
  }
  parse_config_options(nargs);
}

#define _STR(x) #x
#define STRINGIFY(x) _STR(x)

struct config_option {
  const char *section;
  const char *conf_name;
  const char *name;
  void *val_ptr;

  const char *def_str;
  long long def_longlong;
  double def_double;

  opt_type_t type;
  char char_option;  // if any
};

#define OPTION_OPT_STR(section, name, schar, type, def_val) \
       { STRINGIFY(section), NULL, STRINGIFY(name), \
	   &g_conf.name, def_val, 0, 0, type, schar }

#define OPTION_OPT_ADDR(section, name, schar, type, def_val) \
       { STRINGIFY(section), NULL, STRINGIFY(name), \
	   &g_conf.name, def_val, 0, 0, type, schar }

#define OPTION_OPT_LONGLONG(section, name, schar, type, def_val) \
       { STRINGIFY(section), NULL, STRINGIFY(name), \
	   &g_conf.name, 0, def_val, 0, type, schar }
#define OPTION_OPT_INT OPTION_OPT_LONGLONG
#define OPTION_OPT_BOOL OPTION_OPT_INT

#define OPTION_OPT_DOUBLE(section, name, schar, type, def_val) \
       { STRINGIFY(section), NULL, STRINGIFY(name), \
	   &g_conf.name, 0, 0, def_val, type, schar }
#define OPTION_OPT_FLOAT OPTION_OPT_DOUBLE

#define OPTION(name, schar, type, def_val) OPTION_##type("global", name, schar, type, def_val)

#define OPTION_ALT(section, conf_name, name, schar, type, def_val) \
       { STRINGIFY(section), NULL, STRINGIFY(conf_name), \
         &g_conf.name, STRINGIFY(def_val), type, schar }

static struct config_option config_optionsp[] = {
	OPTION(host, 0, OPT_STR, "localhost"),
	OPTION(public_addr, 0, OPT_ADDR, ""),
	OPTION(cluster_addr, 0, OPT_ADDR, ""),
	OPTION(num_client, 0, OPT_INT, 1),
	OPTION(monmap, 'M', OPT_STR, 0),
	OPTION(mon_host, 'm', OPT_STR, 0),
	OPTION(daemonize, 'd', OPT_BOOL, false),
        OPTION(tcmalloc_profiler_run, 0, OPT_BOOL, false),
        OPTION(profiler_allocation_interval, 0, OPT_INT, 1073741824),
        OPTION(profiler_highwater_interval, 0, OPT_INT, 104857600),
	OPTION(profiling_logger, 0, OPT_BOOL, false),
	OPTION(profiling_logger_interval, 0, OPT_INT, 1),
	OPTION(profiling_logger_calc_variance, 0, OPT_BOOL, false),
	OPTION(profiling_logger_subdir, 0, OPT_STR, 0),
	OPTION(profiling_logger_dir, 0, OPT_STR, "/var/log/ceph/stat"),
	OPTION(log_file, 0, OPT_STR, 0),
	OPTION(log_dir, 0, OPT_STR, "/var/log/ceph"),
	OPTION(log_sym_dir, 0, OPT_STR, 0),
	OPTION(log_sym_history, 0, OPT_INT, 10),
	OPTION(log_to_stderr, 0, OPT_INT, LOG_TO_STDERR_SOME),
	OPTION(log_to_syslog, 0, OPT_BOOL, false),
	OPTION(log_per_instance, 0, OPT_BOOL, false),
	OPTION(log_to_file, 0, OPT_BOOL, true),
	OPTION(clog_to_monitors, 0, OPT_BOOL, true),
	OPTION(clog_to_syslog, 0, OPT_BOOL, false),
	OPTION(pid_file, 0, OPT_STR, "/var/run/ceph/$type.$id.pid"),
	OPTION(conf, 'c', OPT_STR, "/etc/ceph/ceph.conf, ~/.ceph/config, ceph.conf"),
	OPTION(chdir, 0, OPT_STR, "/"),
	OPTION(kill_after, 0, OPT_INT, 0),
	OPTION(max_open_files, 0, OPT_LONGLONG, 0),
	OPTION(debug, 0, OPT_INT, 0),
	OPTION(debug_lockdep, 0, OPT_INT, 0),
	OPTION(debug_mds, 0, OPT_INT, 1),
	OPTION(debug_mds_balancer, 0, OPT_INT, 1),
	OPTION(debug_mds_log, 0, OPT_INT, 1),
	OPTION(debug_mds_log_expire, 0, OPT_INT, 1),
	OPTION(debug_mds_migrator, 0, OPT_INT, 1),
	OPTION(debug_buffer, 0, OPT_INT, 0),
	OPTION(debug_timer, 0, OPT_INT, 0),
	OPTION(debug_filer, 0, OPT_INT, 0),
	OPTION(debug_objecter, 0, OPT_INT, 0),
	OPTION(debug_rados, 0, OPT_INT, 0),
	OPTION(debug_rbd, 0, OPT_INT, 0),
	OPTION(debug_journaler, 0, OPT_INT, 0),
	OPTION(debug_objectcacher, 0, OPT_INT, 0),
	OPTION(debug_client, 0, OPT_INT, 0),
	OPTION(debug_osd, 0, OPT_INT, 0),
	OPTION(debug_ebofs, 0, OPT_INT, 1),
	OPTION(debug_filestore, 0, OPT_INT, 1),
	OPTION(debug_journal, 0, OPT_INT, 1),
	OPTION(debug_bdev, 0, OPT_INT, 1),         // block device
	OPTION(debug_ms, 0, OPT_INT, 0),
	OPTION(debug_mon, 0, OPT_INT, 1),
	OPTION(debug_monc, 0, OPT_INT, 0),
	OPTION(debug_paxos, 0, OPT_INT, 0),
	OPTION(debug_tp, 0, OPT_INT, 0),
	OPTION(debug_auth, 0, OPT_INT, 1),
	OPTION(debug_finisher, 0, OPT_INT, 1),
	OPTION(key, 0, OPT_STR, ""),
	OPTION(keyfile, 'K', OPT_STR, ""),
	OPTION(keyring, 'k', OPT_STR, "/etc/ceph/keyring,/etc/ceph/keyring.bin"),
	OPTION(ms_tcp_nodelay, 0, OPT_BOOL, true),
	OPTION(ms_initial_backoff, 0, OPT_DOUBLE, .2),
	OPTION(ms_max_backoff, 0, OPT_DOUBLE, 15.0),
	OPTION(ms_nocrc, 0, OPT_BOOL, false),
	OPTION(ms_die_on_bad_msg, 0, OPT_BOOL, false),
	OPTION(ms_dispatch_throttle_bytes, 0, OPT_INT, 100 << 20),
	OPTION(ms_bind_ipv6, 0, OPT_BOOL, false),
        OPTION(ms_rwthread_stack_bytes, 0, OPT_INT, 1024 << 10),
        OPTION(ms_tcp_read_timeout, 0, OPT_LONGLONG, 900),
	OPTION(ms_inject_socket_failures, 0, OPT_LONGLONG, 0),
	OPTION(mon_data, 0, OPT_STR, ""),
	OPTION(mon_tick_interval, 0, OPT_INT, 5),
	OPTION(mon_subscribe_interval, 0, OPT_DOUBLE, 300),
	OPTION(mon_osd_down_out_interval, 0, OPT_INT, 300), // seconds
	OPTION(mon_lease, 0, OPT_FLOAT, 5),		    // lease interval
	OPTION(mon_lease_renew_interval, 0, OPT_FLOAT, 3), // on leader, to renew the lease
	OPTION(mon_lease_ack_timeout, 0, OPT_FLOAT, 10.0), // on leader, if lease isn't acked by all peons
	OPTION(mon_clock_drift_allowed, 0, OPT_FLOAT, .010), // allowed clock drift between monitors
	OPTION(mon_clock_drift_warn_backoff, 0, OPT_FLOAT, 5), // exponential backoff for clock drift warnings
	OPTION(mon_accept_timeout, 0, OPT_FLOAT, 10.0),    // on leader, if paxos update isn't accepted
	OPTION(mon_pg_create_interval, 0, OPT_FLOAT, 30.0), // no more than every 30s
	OPTION(mon_globalid_prealloc, 0, OPT_INT, 100),   // how many globalids to prealloc
	OPTION(mon_osd_report_timeout, 0, OPT_INT, 900),    // grace period before declaring unresponsive OSDs dead
	OPTION(paxos_propose_interval, 0, OPT_DOUBLE, 1.0),  // gather updates for this long before proposing a map update
	OPTION(paxos_min_wait, 0, OPT_DOUBLE, 0.05),  // min time to gather updates for after period of inactivity
	OPTION(paxos_observer_timeout, 0, OPT_DOUBLE, 5*60), // gather updates for this long before proposing a map update
	OPTION(auth_supported, 0, OPT_STR, "none"),
	OPTION(auth_mon_ticket_ttl, 0, OPT_DOUBLE, 60*60*12),
	OPTION(auth_service_ticket_ttl, 0, OPT_DOUBLE, 60*60),
	OPTION(mon_client_hunt_interval, 0, OPT_DOUBLE, 3.0),   // try new mon every N seconds until we connect
	OPTION(mon_client_ping_interval, 0, OPT_DOUBLE, 10.0),  // ping every N seconds
	OPTION(client_cache_size, 0, OPT_INT, 16384),
	OPTION(client_cache_mid, 0, OPT_FLOAT, .75),
	OPTION(client_cache_stat_ttl, 0, OPT_INT, 0), // seconds until cached stat results become invalid
	OPTION(client_cache_readdir_ttl, 0, OPT_INT, 1),  // 1 second only
	OPTION(client_use_random_mds, 0, OPT_BOOL, false),
	OPTION(client_mount_timeout, 0, OPT_DOUBLE, 30.0),
	OPTION(client_unmount_timeout, 0, OPT_DOUBLE, 10.0),
	OPTION(client_tick_interval, 0, OPT_DOUBLE, 1.0),
	OPTION(client_trace, 0, OPT_STR, 0),
	OPTION(client_readahead_min, 0, OPT_LONGLONG, 128*1024),  // readahead at _least_ this much.
	OPTION(client_readahead_max_bytes, 0, OPT_LONGLONG, 0),  //8 * 1024*1024,
	OPTION(client_readahead_max_periods, 0, OPT_LONGLONG, 4),  // as multiple of file layout period (object size * num stripes)
	OPTION(client_snapdir, 0, OPT_STR, ".snap"),
	OPTION(client_mountpoint, 'r', OPT_STR, "/"),
	OPTION(client_notify_timeout, 0, OPT_INT, 10), // in seconds
	OPTION(client_oc, 0, OPT_BOOL, true),
	OPTION(client_oc_size, 0, OPT_INT, 1024*1024* 200),    // MB * n
	OPTION(client_oc_max_dirty, 0, OPT_INT, 1024*1024* 100),    // MB * n  (dirty OR tx.. bigish)
	OPTION(client_oc_target_dirty, 0, OPT_INT, 1024*1024* 8), // target dirty (keep this smallish)
	// note: the max amount of "in flight" dirty data is roughly (max - target)
	OPTION(client_oc_max_sync_write, 0, OPT_LONGLONG, 128*1024),   // sync writes >= this use wrlock
	OPTION(objecter_tick_interval, 0, OPT_DOUBLE, 5.0),
	OPTION(objecter_mon_retry_interval, 0, OPT_DOUBLE, 5.0),
	OPTION(objecter_timeout, 0, OPT_DOUBLE, 10.0),    // before we ask for a map
	OPTION(objecter_inflight_op_bytes, 0, OPT_LONGLONG, 1024*1024*100), //max in-flight data (both directions)
	OPTION(journaler_allow_split_entries, 0, OPT_BOOL, true),
	OPTION(journaler_safe, 0, OPT_BOOL, true),  // wait for COMMIT on journal writes
	OPTION(journaler_write_head_interval, 0, OPT_INT, 15),
	OPTION(journaler_cache, 0, OPT_BOOL, false), // cache writes for later readback
	OPTION(journaler_prefetch_periods, 0, OPT_INT, 50),   // * journal object size (1~MB? see above)
	OPTION(journaler_batch_interval, 0, OPT_DOUBLE, .001),   // seconds.. max add'l latency we artificially incur
	OPTION(journaler_batch_max, 0, OPT_LONGLONG, 0),  // max bytes we'll delay flushing; disable, for now....
	OPTION(mds_max_file_size, 0, OPT_LONGLONG, 1ULL << 40),
	OPTION(mds_cache_size, 0, OPT_INT, 100000),
	OPTION(mds_cache_mid, 0, OPT_FLOAT, .7),
	OPTION(mds_mem_max, 0, OPT_INT, 1048576),        // KB
	OPTION(mds_dir_commit_ratio, 0, OPT_FLOAT, .5),
	OPTION(mds_dir_max_commit_size, 0, OPT_INT, 90), // MB
	OPTION(mds_decay_halflife, 0, OPT_FLOAT, 5),
	OPTION(mds_beacon_interval, 0, OPT_FLOAT, 4),
	OPTION(mds_beacon_grace, 0, OPT_FLOAT, 15),
	OPTION(mds_blacklist_interval, 0, OPT_FLOAT, 24.0*60.0),  // how long to blacklist failed nodes
	OPTION(mds_session_timeout, 0, OPT_FLOAT, 60),    // cap bits and leases time out if client idle
	OPTION(mds_session_autoclose, 0, OPT_FLOAT, 300), // autoclose idle session
	OPTION(mds_reconnect_timeout, 0, OPT_FLOAT, 45),  // seconds to wait for clients during mds restart
							  //  make it (mds_session_timeout - mds_beacon_grace)
	OPTION(mds_tick_interval, 0, OPT_FLOAT, 5),
	OPTION(mds_dirstat_min_interval, 0, OPT_FLOAT, 1),    // try to avoid propagating more often than this
	OPTION(mds_scatter_nudge_interval, 0, OPT_FLOAT, 5),  // how quickly dirstat changes propagate up the hierarchy
	OPTION(mds_client_prealloc_inos, 0, OPT_INT, 1000),
	OPTION(mds_early_reply, 0, OPT_BOOL, true),
	OPTION(mds_use_tmap, 0, OPT_BOOL, true),        // use trivialmap for dir updates
	OPTION(mds_default_dir_hash, 0, OPT_INT, CEPH_STR_HASH_RJENKINS),
	OPTION(mds_log, 0, OPT_BOOL, true),
	OPTION(mds_log_unsafe, 0, OPT_BOOL, false),      // only wait for log sync, when it's mostly safe to do so
	OPTION(mds_log_skip_corrupt_events, 0, OPT_BOOL, false),
	OPTION(mds_log_max_events, 0, OPT_INT, -1),
	OPTION(mds_log_max_segments, 0, OPT_INT, 30),  // segment size defined by FileLayout, above
	OPTION(mds_log_max_expiring, 0, OPT_INT, 20),
	OPTION(mds_log_eopen_size, 0, OPT_INT, 100),   // # open inodes per log entry
	OPTION(mds_bal_sample_interval, 0, OPT_FLOAT, 3.0),  // every 5 seconds
	OPTION(mds_bal_replicate_threshold, 0, OPT_FLOAT, 8000),
	OPTION(mds_bal_unreplicate_threshold, 0, OPT_FLOAT, 0),
	OPTION(mds_bal_frag, 0, OPT_BOOL, false),
	OPTION(mds_bal_split_size, 0, OPT_INT, 10000),
	OPTION(mds_bal_split_rd, 0, OPT_FLOAT, 25000),
	OPTION(mds_bal_split_wr, 0, OPT_FLOAT, 10000),
	OPTION(mds_bal_split_bits, 0, OPT_INT, 3),
	OPTION(mds_bal_merge_size, 0, OPT_INT, 50),
	OPTION(mds_bal_merge_rd, 0, OPT_FLOAT, 1000),
	OPTION(mds_bal_merge_wr, 0, OPT_FLOAT, 1000),
	OPTION(mds_bal_interval, 0, OPT_INT, 10),           // seconds
	OPTION(mds_bal_fragment_interval, 0, OPT_INT, 5),      // seconds
	OPTION(mds_bal_idle_threshold, 0, OPT_FLOAT, 0),
	OPTION(mds_bal_max, 0, OPT_INT, -1),
	OPTION(mds_bal_max_until, 0, OPT_INT, -1),
	OPTION(mds_bal_mode, 0, OPT_INT, 0),
	OPTION(mds_bal_min_rebalance, 0, OPT_FLOAT, .1),  // must be this much above average before we export anything
	OPTION(mds_bal_min_start, 0, OPT_FLOAT, .2),      // if we need less than this, we don't do anything
	OPTION(mds_bal_need_min, 0, OPT_FLOAT, .8),       // take within this range of what we need
	OPTION(mds_bal_need_max, 0, OPT_FLOAT, 1.2),
	OPTION(mds_bal_midchunk, 0, OPT_FLOAT, .3),       // any sub bigger than this taken in full
	OPTION(mds_bal_minchunk, 0, OPT_FLOAT, .001),     // never take anything smaller than this
	OPTION(mds_bal_target_removal_min, 0, OPT_INT, 5), // min balance iterations before old target is removed
	OPTION(mds_bal_target_removal_max, 0, OPT_INT, 10), // max balance iterations before old target is removed
	OPTION(mds_replay_interval, 0, OPT_FLOAT, 1.0), // time to wait before starting replay again
	OPTION(mds_shutdown_check, 0, OPT_INT, 0),
	OPTION(mds_thrash_exports, 0, OPT_INT, 0),
	OPTION(mds_thrash_fragments, 0, OPT_INT, 0),
	OPTION(mds_dump_cache_on_map, 0, OPT_BOOL, false),
	OPTION(mds_dump_cache_after_rejoin, 0, OPT_BOOL, false),
	OPTION(mds_verify_scatter, 0, OPT_BOOL, false),
	OPTION(mds_debug_scatterstat, 0, OPT_BOOL, false),
	OPTION(mds_debug_frag, 0, OPT_BOOL, false),
	OPTION(mds_kill_mdstable_at, 0, OPT_INT, 0),
	OPTION(mds_kill_export_at, 0, OPT_INT, 0),
	OPTION(mds_kill_import_at, 0, OPT_INT, 0),
	OPTION(mds_kill_rename_at, 0, OPT_INT, 0),
	OPTION(mds_wipe_sessions, 0, OPT_BOOL, 0),
	OPTION(mds_wipe_ino_prealloc, 0, OPT_BOOL, 0),
	OPTION(mds_skip_ino, 0, OPT_INT, 0),
	OPTION(max_mds, 0, OPT_INT, 1),
	OPTION(mds_standby_for_name, 0, OPT_STR, ""),
	OPTION(mds_standby_for_rank, 0, OPT_INT, -1),
	OPTION(mds_standby_replay, 0, OPT_BOOL, false),
	OPTION(osd_data, 0, OPT_STR, ""),
	OPTION(osd_journal, 0, OPT_STR, ""),
	OPTION(osd_journal_size, 0, OPT_INT, 0),         // in mb
	OPTION(osd_max_write_size, 0, OPT_INT, 90),
	OPTION(osd_balance_reads, 0, OPT_BOOL, false),
	OPTION(osd_flash_crowd_iat_threshold, 0, OPT_INT, 0),
	OPTION(osd_flash_crowd_iat_alpha, 0, OPT_DOUBLE, 0.125),
	OPTION(osd_shed_reads, 0, OPT_INT, false),     // forward from primary to replica
	OPTION(osd_shed_reads_min_latency, 0, OPT_DOUBLE, .01),       // min local latency
	OPTION(osd_shed_reads_min_latency_diff, 0, OPT_DOUBLE, .01),  // min latency difference
	OPTION(osd_shed_reads_min_latency_ratio, 0, OPT_DOUBLE, 1.5),  // 1.2 == 20% higher than peer
	OPTION(osd_client_message_size_cap, 0, OPT_LONGLONG, 500*1024L*1024L), // default to 200MB client data allowed in-memory
	OPTION(osd_stat_refresh_interval, 0, OPT_DOUBLE, .5),
	OPTION(osd_pg_bits, 0, OPT_INT, 9),  // bits per osd
	OPTION(osd_pgp_bits, 0, OPT_INT, 6),  // bits per osd
	OPTION(osd_lpg_bits, 0, OPT_INT, 2),  // bits per osd
	OPTION(osd_pg_layout, 0, OPT_INT, CEPH_PG_LAYOUT_CRUSH),
	OPTION(osd_min_rep, 0, OPT_INT, 1),
	OPTION(osd_max_rep, 0, OPT_INT, 10),
	OPTION(osd_min_raid_width, 0, OPT_INT, 3),
	OPTION(osd_max_raid_width, 0, OPT_INT, 2),
	OPTION(osd_pool_default_crush_rule, 0, OPT_INT, 0),
	OPTION(osd_pool_default_size, 0, OPT_INT, 2),
	OPTION(osd_pool_default_pg_num, 0, OPT_INT, 8),
	OPTION(osd_pool_default_pgp_num, 0, OPT_INT, 8),
	OPTION(osd_op_threads, 0, OPT_INT, 2),    // 0 == no threading
	OPTION(osd_max_opq, 0, OPT_INT, 10),
	OPTION(osd_disk_threads, 0, OPT_INT, 1),
	OPTION(osd_recovery_threads, 0, OPT_INT, 1),
	OPTION(osd_age, 0, OPT_FLOAT, .8),
	OPTION(osd_age_time, 0, OPT_INT, 0),
	OPTION(osd_heartbeat_interval, 0, OPT_INT, 1),
	OPTION(osd_mon_heartbeat_interval, 0, OPT_INT, 30),  // if no peers, ping monitor
	OPTION(osd_heartbeat_grace, 0, OPT_INT, 20),
	OPTION(osd_mon_report_interval_max, 0, OPT_INT, 120),
	OPTION(osd_mon_report_interval_min, 0, OPT_INT, 5),  // pg stats, failures, up_thru, boot.
	OPTION(osd_min_down_reporters, 0, OPT_INT, 1),   // number of OSDs who need to report a down OSD for it to count
	OPTION(osd_min_down_reports, 0, OPT_INT, 3),     // number of times a down OSD must be reported for it to count
	OPTION(osd_replay_window, 0, OPT_INT, 45),
	OPTION(osd_preserve_trimmed_log, 0, OPT_BOOL, true),
	OPTION(osd_recovery_delay_start, 0, OPT_FLOAT, 15),
	OPTION(osd_recovery_max_active, 0, OPT_INT, 5),
	OPTION(osd_recovery_max_chunk, 0, OPT_LONGLONG, 1<<20),  // max size of push chunk
	OPTION(osd_recovery_forget_lost_objects, 0, OPT_BOOL, false),   // off for now
	OPTION(osd_max_scrubs, 0, OPT_INT, 1),
	OPTION(osd_scrub_load_threshold, 0, OPT_FLOAT, 0.5),
	OPTION(osd_scrub_min_interval, 0, OPT_FLOAT, 300),
	OPTION(osd_scrub_max_interval, 0, OPT_FLOAT, 60*60*24),   // once a day
	OPTION(osd_auto_weight, 0, OPT_BOOL, false),
	OPTION(osd_class_error_timeout, 0, OPT_DOUBLE, 60.0),  // seconds
	OPTION(osd_class_timeout, 0, OPT_DOUBLE, 60*60.0), // seconds
	OPTION(osd_class_tmp, 0, OPT_STR, "/var/lib/ceph/tmp"),
	OPTION(osd_check_for_log_corruption, 0, OPT_BOOL, false),
	OPTION(osd_use_stale_snap, 0, OPT_BOOL, false),
	OPTION(osd_max_notify_timeout, 0, OPT_INT, 30), // max notify timeout in seconds
	OPTION(filestore, 0, OPT_BOOL, false),
	OPTION(filestore_max_sync_interval, 0, OPT_DOUBLE, 5),    // seconds
	OPTION(filestore_min_sync_interval, 0, OPT_DOUBLE, .01),  // seconds
	OPTION(filestore_fake_attrs, 0, OPT_BOOL, false),
	OPTION(filestore_fake_collections, 0, OPT_BOOL, false),
	OPTION(filestore_dev, 0, OPT_STR, 0),
	OPTION(filestore_btrfs_trans, 0, OPT_BOOL, false),
	OPTION(filestore_btrfs_snap, 0, OPT_BOOL, true),
	OPTION(filestore_btrfs_clone_range, 0, OPT_BOOL, true),
	OPTION(filestore_fsync_flushes_journal_data, 0, OPT_BOOL, false),
	OPTION(filestore_flusher, 0, OPT_BOOL, true),
	OPTION(filestore_flusher_max_fds, 0, OPT_INT, 512),
	OPTION(filestore_sync_flush, 0, OPT_BOOL, false),
	OPTION(filestore_journal_parallel, 0, OPT_BOOL, false),
	OPTION(filestore_journal_writeahead, 0, OPT_BOOL, false),
	OPTION(filestore_journal_trailing, 0, OPT_BOOL, false),
	OPTION(filestore_queue_max_ops, 0, OPT_INT, 500),
	OPTION(filestore_queue_max_bytes, 0, OPT_INT, 100 << 20),
	OPTION(filestore_op_threads, 0, OPT_INT, 2),
	OPTION(filestore_commit_timeout, 0, OPT_FLOAT, 600),
	OPTION(ebofs, 0, OPT_BOOL, false),
	OPTION(ebofs_cloneable, 0, OPT_BOOL, true),
	OPTION(ebofs_verify, 0, OPT_BOOL, false),
	OPTION(ebofs_commit_ms, 0, OPT_INT, 200),       // 0 = no forced commit timeout (for debugging/tracing)
	OPTION(ebofs_oc_size, 0, OPT_INT, 10000),      // onode cache
	OPTION(ebofs_cc_size, 0, OPT_INT, 10000),      // cnode cache
	OPTION(ebofs_bc_size, 0, OPT_LONGLONG, 50*256), // 4k blocks, *256 for MB
	OPTION(ebofs_bc_max_dirty, 0, OPT_LONGLONG, 30*256), // before write() will block
	OPTION(ebofs_max_prefetch, 0, OPT_INT, 1000), // 4k blocks
	OPTION(ebofs_realloc, 0, OPT_BOOL, false),    // hrm, this can cause bad fragmentation, don't use!
	OPTION(ebofs_verify_csum_on_read, 0, OPT_BOOL, true),
	OPTION(journal_dio, 0, OPT_BOOL, true),
	OPTION(journal_block_align, 0, OPT_BOOL, true),
	OPTION(journal_max_write_bytes, 0, OPT_INT, 10 << 20),
	OPTION(journal_max_write_entries, 0, OPT_INT, 100),
	OPTION(journal_queue_max_ops, 0, OPT_INT, 500),
	OPTION(journal_queue_max_bytes, 0, OPT_INT, 100 << 20),
	OPTION(journal_align_min_size, 0, OPT_INT, 64 << 10),  // align data payloads >= this.
	OPTION(bdev_lock, 0, OPT_BOOL, true),
	OPTION(bdev_iothreads, 0, OPT_INT, 1),         // number of ios to queue with kernel
	OPTION(bdev_idle_kick_after_ms, 0, OPT_INT, 100),  // ms
	OPTION(bdev_el_fw_max_ms, 0, OPT_INT, 10000),      // restart elevator at least once every 1000 ms
	OPTION(bdev_el_bw_max_ms, 0, OPT_INT, 3000),       // restart elevator at least once every 300 ms
	OPTION(bdev_el_bidir, 0, OPT_BOOL, false),          // bidirectional elevator?
	OPTION(bdev_iov_max, 0, OPT_INT, 512),            // max # iov's to collect into a single readv()/writev() call
	OPTION(bdev_debug_check_io_overlap, 0, OPT_BOOL, true),  // [DEBUG] check for any pending io overlaps
	OPTION(bdev_fake_mb, 0, OPT_INT, 0),
	OPTION(bdev_fake_max_mb, 0, OPT_INT, 0),
};

bool conf_set_conf_val(void *field, opt_type_t type, const char *val)
{
  switch (type) {
  case OPT_BOOL:
    if (strcasecmp(val, "false") == 0)
      *(bool *)field = false;
    else if (strcasecmp(val, "true") == 0)
      *(bool *)field = true;
    else
      *(bool *)field = (bool)atoi(val);
    break;
  case OPT_INT:
    *(int *)field = atoi(val);
    break;
  case OPT_LONGLONG:
    *(long long *)field = atoll(val);
    break;
  case OPT_STR:
    if (val)
      *(char **)field = strdup(val);
    else
      *(char **)field = NULL;
    break;
  case OPT_FLOAT:
    *(float *)field = atof(val);
    break;
  case OPT_DOUBLE:
    *(double *)field = strtod(val, NULL);
    break;
  case OPT_ADDR:
    ((entity_addr_t *)field)->parse(val);
    break;
  default:
    return false;
  }

  return true;
}

bool conf_set_conf_val(void *field, opt_type_t type, const char *val, long long intval, double doubleval)
{
  switch (type) {
  case OPT_BOOL:
    *(bool *)field = intval;
    break;
  case OPT_INT:
    *(int *)field = intval;
    break;
  case OPT_LONGLONG:
    *(long long *)field = intval;
    break;
  case OPT_STR:
    if (val) {
      *(char **)field = strdup(val);
    } else {
      *(char **)field = NULL;
    }
    break;
  case OPT_FLOAT:
    *(float *)field = doubleval;
    break;
  case OPT_DOUBLE:
    *(double *)field = doubleval;
    break;
  case OPT_ADDR:
    ((entity_addr_t *)field)->parse(val);
    break;
  default:
    return false;
  }

  return true;
}

static bool conf_reset_val(void *field, opt_type_t type)
{
  switch (type) {
  case OPT_BOOL:
    *(bool *)field = 0;
    break;
  case OPT_INT:
    *(int *)field = 0;
    break;
  case OPT_LONGLONG:
    *(long long *)field = 0;
    break;
  case OPT_STR:
      *(char **)field = NULL;
    break;
  case OPT_FLOAT:
    *(float *)field = 0;
    break;
  case OPT_DOUBLE:
    *(double *)field = 0;
    break;
  case OPT_ADDR:
    *(entity_addr_t *)field = entity_addr_t();
    break;
  default:
    return false;
  }

  return true;
}

static void set_conf_name(config_option *opt)
{
  char *newsection = (char *)opt->section;
  char *newconf = (char *)opt->name;
  int i;

  if (opt->section[0] == 0) {
    newsection = strdup("global");
  }

  if (strncmp(newsection, opt->name, strlen(newsection)) == 0) {
    /* if key starts with the name of the section, remove name of the section
       unless key equals to it */

    if (strcmp(newsection, opt->name) == 0)
      goto done;

    newconf = strdup(&opt->name[strlen(newsection)+1]);
  } else {
    newconf = strdup(opt->name);
  }

  i = 0;
  while (newconf[i]) {
    if (newconf[i] == '_')
      newconf[i] = ' ';

    ++i;
  }

  done:
    opt->section = newsection;
    opt->conf_name = (const char *)newconf;
}

static int def_conf_to_str(config_option *opt, char *buf, int len)
{
  int ret = 0;

  switch (opt->type) {
  case OPT_INT:
  case OPT_BOOL:
    ret = snprintf(buf, len, "%d", (int)opt->def_longlong);
    break;
  case OPT_LONGLONG:
    ret = snprintf(buf, len, "%lld", opt->def_longlong);
    break;
  case OPT_STR:
  case OPT_ADDR:
    ret = snprintf(buf, len, "%s", opt->def_str);
    break;
  case OPT_FLOAT:
  case OPT_DOUBLE:
    ret = snprintf(buf, len, "%f", opt->def_double);
    break;
  default:
    break;
  }
  return ret;
}

int ceph_def_conf_by_name(const char *name, char *buf, int buflen)
{
  char *newname = strdup(name);
  int len = strlen(name);
  config_option *opt;
  int ret = 0;
  int i;

  for (i = 0; i < len; i++) {
    if (newname[i] == ' ')
      newname[i] = '_';
  }

  len = sizeof(config_optionsp)/sizeof(config_option);

  for (i = 0; i < len; i++) {
    opt = &config_optionsp[i];
    if (strcmp(opt->name, newname) == 0) {
      ret = def_conf_to_str(opt, buf, buflen);
      break;
    }
  }
  free(newname);
  return ret;
}

static bool cmd_is_char(const char *cmd)
{
	return ((cmd[0] == '-') &&
		cmd[1] && !cmd[2]);
}

bool conf_cmd_equals(const char *cmd, const char *opt, char char_opt, unsigned int *val_pos)
{
	unsigned int i;
	unsigned int len = strlen(opt);

	*val_pos = 0;

	if (!*cmd)
		return false;

	if (char_opt && cmd_is_char(cmd))
		return (char_opt == cmd[1]);

	if ((cmd[0] != '-') || (cmd[1] != '-'))
		return false;

	for (i=0; i<len; i++) {
		if ((opt[i] == '_') || (opt[i] == '-')) {
			switch (cmd[i+2]) {
			case '-':
			case '_':
				continue;
			default:
				break;
			}
		}

		if (cmd[i+2] != opt[i])
			return false;
	}

	if (cmd[i+2] == '=')
		*val_pos = i+3;
	else if (cmd[i+2])
		return false;

	return true;
}

static bool get_var(const char *str, int pos, char *var_name, int len, int *new_pos)
{
  int bracket = (str[pos] == '{');
  int out_pos = 0;

  if (bracket) {
    pos++;
  }

  while (str[pos] &&
	((bracket && str[pos] != '}') ||
	 isalnum(str[pos]) ||
         str[pos] == '_')) {
	var_name[out_pos] = str[pos];

	out_pos	++;
	if (out_pos == len)
		return false;
	pos++;
  }

  var_name[out_pos] = '\0';


  if (bracket && (str[pos] == '}'))
	pos++;

  *new_pos = pos;

  return true;
}

static const char *var_val(char *var_name)
{
	const char *val;

	if (strcmp(var_name, "type")==0)
		return g_conf.type;
	if (strcmp(var_name, "id")==0)
		return g_conf.id;
	if (strcmp(var_name, "num")==0)
		return g_conf.id;
	if (strcmp(var_name, "name")==0)
		return g_conf.name;
	if (strcmp(var_name, "host")==0)
		return g_conf.host;

	val = getenv(var_name);
	if (!val)
		val = "";

	return val;
}

#define MAX_LINE 256
#define MAX_VAR_LEN 32

char *conf_post_process_val(const char *val)
{
  char var_name[MAX_VAR_LEN];
  char *buf;
  int i=0;
  size_t out_pos = 0;
  size_t max_line = MAX_LINE;

  buf = (char *)malloc(max_line);
  *buf = 0;

  while (val[i]) {
    if (val[i] == '$') {
	if (get_var(val, i+1, var_name, MAX_VAR_LEN, &i)) {
		out_pos = dyn_snprintf(&buf, &max_line, 2, "%s%s", buf, var_val(var_name));
	} else {
	  ++i;
	}
    } else {
	if (out_pos == max_line - 1) {
		max_line *= 2;
		buf = (char *)realloc(buf, max_line);
	}
	buf[out_pos] = val[i];
	buf[out_pos + 1] = '\0';
	++out_pos;
	++i;
    }
  }

  buf[out_pos] = '\0';

  return buf;
}

#define OPT_READ_TYPE(ret, section, var, type, out, def) \
do { \
  if (def) \
    ret = cf->read(section, var, (type *)out, *(type *)def); \
  else \
    ret = cf->read(section, var, (type *)out, 0); \
} while (0)

int conf_read_key_ext(const char *conf_name, const char *conf_alt_name, const char *conf_type,
		      const char *alt_section, const char *key, opt_type_t type, void *out, void *def,
		      bool free_old_val)
{
  int s;
  int ret;
  char *tmp = 0;
  for (s=0; s<5; s++) {
    const char *section;

    switch (s) {
      case 0:
          section = conf_name;
          if (section)
            break;
      case 1:
          section = conf_alt_name;
          if (section)
            break;
      case 2:
	    s = 2;
            section = conf_type;
            if (section)
              break;
      case 3:
	    s = 3;
            section = alt_section;
	    if (section)
	      break;
      default:
	    s = 4;
	    section = "global";
    }

    switch (type) {
    case OPT_STR:
      if (free_old_val)
        tmp = *(char **)out;
      OPT_READ_TYPE(ret, section, key, char *, out, def);
      if (free_old_val &&
          *(char **)out != tmp)
          free(tmp);
      break;
    case OPT_BOOL:
      OPT_READ_TYPE(ret, section, key, bool, out, def);
      break;
    case OPT_LONGLONG:
      OPT_READ_TYPE(ret, section, key, long long, out, def);
      break;
    case OPT_INT:
      OPT_READ_TYPE(ret, section, key, int, out, def);
      break;
    case OPT_FLOAT:
      OPT_READ_TYPE(ret, section, key, float, out, def);
      break;
    case OPT_DOUBLE:
      OPT_READ_TYPE(ret, section, key, double, out, def);
      break;
    case OPT_ADDR:
      ret = cf->read(section, key, &tmp, (char *)def);
      if (*tmp == *((char *)def)) {
          ret = 0;
      }
      else {
        ret = 1;
      }
      if ((1 == ret) &&!(((entity_addr_t*)out)->parse(tmp))) {
        cerr << "Addr " << tmp << " failed to parse! Shutting down" << std::endl;
        exit(1);
      }
      break;
    case OPT_U32:
      OPT_READ_TYPE(ret, section, key, uint32_t, out, def);
      break;
    default:
	ret = 0;
        break;
    }

    if (ret)
	break;
  }

  return ret;
}

int conf_read_key(const char *alt_section, const char *key, opt_type_t type, void *out, void *def, bool free_old_val)
{
	return conf_read_key_ext(g_conf.name, g_conf.alt_name, g_conf.type,
				 alt_section, key, type, out, def, free_old_val);
}

bool parse_config_file(ConfFile *cf, bool auto_update)
{
  int opt_len = sizeof(config_optionsp)/sizeof(config_option);

  cf->set_post_process_func(conf_post_process_val);
  if (!cf->parse())
	return false;

  for (int i=0; i<opt_len; i++) {
      config_option *opt = &config_optionsp[i];
      conf_read_key(NULL, opt->conf_name, opt->type, opt->val_ptr, opt->val_ptr, true);
  }
  conf_read_key(NULL, "lockdep", OPT_INT, &g_lockdep, &g_lockdep, false);

  return true;
}

bool is_bool_param(const char *param)
{
	return ((strcasecmp(param, "true")==0) || (strcasecmp(param, "false")==0));
}

void parse_startup_config_options(std::vector<const char*>& args,
			  const char *module_type, int flags)
{
  bool show_config = false;
  DEFINE_CONF_VARS(NULL);
  std::vector<const char *> nargs;
  bool conf_specified = false;
  bool force_fg_logging = !!(flags & STARTUP_FLAG_FORCE_FG_LOGGING);

  if (!g_conf.type)
    g_conf.type = (char *)"";

  bool isdaemon = g_conf.daemonize;

  FOR_EACH_ARG(args) {
    if (CONF_ARG_EQ("version", 'v')) {
      cout << "ceph version " << VERSION << " (commit:" << STRINGIFY(CEPH_GIT_VER) << ")" << std::endl;
      _exit(0);
    } else if (CONF_ARG_EQ("conf", 'c')) {
	CONF_SAFE_SET_ARG_VAL(&g_conf.conf, OPT_STR);
	conf_specified = true;
    } else if (CONF_ARG_EQ("monmap", 'M')) {
	CONF_SAFE_SET_ARG_VAL(&g_conf.monmap, OPT_STR);
    } else if (CONF_ARG_EQ("show_conf", 'S')) {
      show_config = true;
    } else if (isdaemon && CONF_ARG_EQ("bind", 0)) {
      g_conf.public_addr.parse(args[++i]);
    } else if (CONF_ARG_EQ("nodaemon", 'D')) {
      g_conf.daemonize = false;
      force_fg_logging = true;
    } else if (CONF_ARG_EQ("foreground", 'f')) {
      g_conf.daemonize = false;
      force_fg_logging = false;
    } else if (isdaemon && (CONF_ARG_EQ("id", 'i') || CONF_ARG_EQ("name", 'n'))) {
      free(g_conf.id);
      CONF_SAFE_SET_ARG_VAL(&g_conf.id, OPT_STR);
    } else if (!isdaemon && (CONF_ARG_EQ("id", 'I') || CONF_ARG_EQ("name", 'n'))) {
      free(g_conf.id);
      CONF_SAFE_SET_ARG_VAL(&g_conf.id, OPT_STR);
    } else {
      nargs.push_back(args[i]);
    }
  }
  args.swap(nargs);
  nargs.clear();

  if (module_type) {
    g_conf.type = strdup(module_type);
    // is it "type.name"?
    const char *dot = strchr(g_conf.id, '.');
    if (dot) {
      int tlen = dot - g_conf.id;
      g_conf.type = (char *)malloc(tlen + 1);
      memcpy(g_conf.type, g_conf.id, tlen);
      g_conf.type[tlen] = 0;
      char *new_g_conf_id = strdup(dot + 1);
      free(g_conf.id);
      g_conf.id = new_g_conf_id;
    }

    int len = strlen(g_conf.type) + strlen(g_conf.id) + 2;
    g_conf.name = (char *)malloc(len);
    snprintf(g_conf.name, len, "%s.%s", g_conf.type, g_conf.id);
    g_conf.alt_name = (char *)malloc(len - 1);
    snprintf(g_conf.alt_name, len - 1, "%s%s", module_type, g_conf.id);
  }

  g_conf.entity_name = new EntityName;
  assert(g_conf.entity_name);

  g_conf.entity_name->from_type_id(g_conf.type, g_conf.id);

  if (cf) {
    delete cf;
    cf = NULL;
  }

  // do post_process substitutions
  int len = sizeof(config_optionsp)/sizeof(config_option);
  for (int i = 0; i<len; i++) {
    config_option *opt = &config_optionsp[i];
    if (opt->type == OPT_STR && opt->val_ptr) {
      if (*(char**)opt->val_ptr) {
	*(char **)opt->val_ptr = conf_post_process_val(*(char **)opt->val_ptr);
      }
    }
  }

  if (!conf_specified)
    env_override(&g_conf.conf, "CEPH_CONF");

  // open new conf
  string fn = g_conf.conf;
  list<string> ls;
  get_str_list(fn, ls);
  bool read_conf = false;
  for (list<string>::iterator p = ls.begin(); p != ls.end(); p++) {
    cf = new ConfFile(p->c_str());
    read_conf = parse_config_file(cf, true);
    if (read_conf)
      break;
    delete cf;
    cf = NULL;
  }

  if (conf_specified && !read_conf) {
    cerr << "error reading config file(s) " << g_conf.conf << std::endl;
    exit(1);
  }

  if (g_conf.log_to_syslog || g_conf.clog_to_syslog) {
    closelog();
    // It's ok if g_conf.name is NULL here.
    openlog(g_conf.name, LOG_ODELAY | LOG_PID, LOG_USER);
  }

  if (force_fg_logging)
    set_foreground_logging();

  {
    // In the long term, it would be best to ensure that we read ceph.conf
    // before initializing dout(). For now, just force a reopen here with the
    // configuration we have just read.
    Mutex::Locker l(_dout_lock);
    _dout_open_log(false);
  }

  if (!cf)
    return;

  if (show_config) {
    cf->dump();
    exit(0);
  }

  if (!ec) {
    ec = new ExportControl();
  }

  ec->load(cf);
}

void generic_usage(bool is_server)
{
  cout << "   -c ceph.conf or --conf=ceph.conf\n";
  cout << "        get options from given conf file\n";
  cout << "   -D   run in foreground.\n";
  cout << "   -f   run in foreground. Show all log messages on stdout.\n";
  if (is_server) {
    cout << "   --debug_ms N\n";
    cout << "        set message debug level (e.g. 1)\n";
  }
}

void generic_server_usage()
{
  generic_usage(true);
  exit(1);
}
void generic_client_usage()
{
  generic_usage(false);
  exit(1);
}

ConfFile *conf_get_conf_file()
{
  return cf;
}

ExportControl *conf_get_export_control()
{
  return ec;
}

void parse_config_options(std::vector<const char*>& args)
{
  int opt_len = sizeof(config_optionsp)/sizeof(config_option);
  DEFINE_CONF_VARS(NULL);

  std::vector<const char*> nargs;
  FOR_EACH_ARG(args) {
    int optn;

    for (optn = 0; optn < opt_len; optn++) {
      if (CONF_ARG_EQ("lockdep", '\0')) {
	CONF_SAFE_SET_ARG_VAL(&g_lockdep, OPT_INT);
      } else if (CONF_ARG_EQ(config_optionsp[optn].name,
	    config_optionsp[optn].char_option)) {
        if (__isarg || val_pos || config_optionsp[optn].type == OPT_BOOL)
	    CONF_SAFE_SET_ARG_VAL(config_optionsp[optn].val_ptr, config_optionsp[optn].type);
        else
          continue;
      } else {
        continue;
      }
      break;
    }

    if (optn == opt_len)
        nargs.push_back(args[i]);
  }

  env_override(&g_conf.keyring, "CEPH_KEYRING");
  args = nargs;
}

bool ceph_resolve_file_search(string& filename_list, string& result)
{
  list<string> ls;
  get_str_list(filename_list, ls);

  list<string>::iterator iter;
  for (iter = ls.begin(); iter != ls.end(); ++iter) {
    int fd = ::open(iter->c_str(), O_RDONLY);
    if (fd < 0)
      continue;

    close(fd);
    result = *iter;
    return true;
  }

  return false;
}

md_config_t::md_config_t()
{
	//
	// Note: because our md_config_t structure is a global, the memory used to
	// store it will start out zeroed. So there is no need to manually initialize
	// everything to 0 here.
	//
	// However, it's good practice to add your new config option to config_optionsp
	// so that its default value is explicit rather than implicit.
	//

  int len = sizeof(config_optionsp)/sizeof(config_option);
  int i;
  config_option *opt;

  for (i = 0; i<len; i++) {
    opt = &config_optionsp[i];
    if (opt->val_ptr) {
      conf_reset_val(opt->val_ptr, opt->type);
    }
    if (!conf_set_conf_val(opt->val_ptr,
			   opt->type,
			   opt->def_str,
			   opt->def_longlong,
			   opt->def_double)) {
			std::ostringstream oss;
			oss << "error initializing g_conf value num " << i;
			assert(oss.str().c_str() == 0);
    }

    set_conf_name(opt);
  }

  g_conf.id = strdup("admin");
}

md_config_t::~md_config_t()
{
  int len = sizeof(config_optionsp)/sizeof(config_option);
  int i;
  config_option *opt;

  for (i = 0; i<len; i++) {
    opt = &config_optionsp[i];
    if (opt->type == OPT_STR) {
      free(*(char **)opt->val_ptr);
    }
    free((void *)opt->conf_name);
  }

  delete cf;
  cf = NULL;
}

