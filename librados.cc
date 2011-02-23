// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
#include <pthread.h>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"
#include "mds/MDS.h"
#include "osd/OSDMap.h"
#include "osd/PGLS.h"

#include "msg/SimpleMessenger.h"

#include "common/ceph_argparse.h"
#include "common/Timer.h"
#include "common/common_init.h"

#include "mon/MonClient.h"

#include "osdc/Objecter.h"

#include "include/rados/librados.h"
#include "include/rados/librados.hpp"

#include "messages/MWatchNotify.h"

#define RADOS_LIST_MAX_ENTRIES 1024
#define DOUT_SUBSYS rados
#undef dout_prefix
#define dout_prefix *_dout << "librados: "


class RadosClient : public Dispatcher
{
  OSDMap osdmap;
  MonClient monclient;
  SimpleMessenger *messenger;

  bool _dispatch(Message *m);
  bool ms_dispatch(Message *m);

  bool ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new) {
    //dout(0) << "RadosClient::ms_get_authorizer type=" << dest_type << dendl;
    /* monitor authorization is being handled on different layer */
    if (dest_type == CEPH_ENTITY_TYPE_MON)
      return true;
    *authorizer = monclient.auth->build_authorizer(dest_type);
    return *authorizer != NULL;
  }
  void ms_handle_connect(Connection *con);
  bool ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con);


  Objecter *objecter;

  Mutex lock;
  Cond cond;
  SafeTimer timer;

public:
  RadosClient() : messenger(NULL), lock("radosclient"), timer(lock),
                  max_watch_cookie(0) {
    messenger = new SimpleMessenger();
  }

  ~RadosClient();
  int init();
  void shutdown();

  struct PoolCtx {
    RadosClient *client;
    int poolid;
    string name;
    snapid_t snap_seq;
    SnapContext snapc;
    uint64_t assert_ver;
    eversion_t last_objver;
    uint32_t notify_timeout;

    PoolCtx(RadosClient *c, int pid, const char *n, snapid_t s = CEPH_NOSNAP) :
      client(c), poolid(pid), name(n), snap_seq(s), assert_ver(0),
      notify_timeout(g_conf.client_notify_timeout) {}

    void set_snap(snapid_t s) {
      if (!s)
	s = CEPH_NOSNAP;
      snap_seq = s;
    }

    int set_snap_context(snapid_t seq, vector<snapid_t>& snaps) {
      SnapContext n;
      n.seq = seq;
      n.snaps = snaps;
      if (!n.is_valid())
	return -EINVAL;
      snapc = n;
      return 0;
    }
  };

  struct ListCtx {
    PoolCtx *ctx;
    Objecter::ListContext *lc;

    ListCtx(PoolCtx *c, Objecter::ListContext *l) : ctx(c), lc(l) {}
    ~ListCtx() {
      delete lc;
    }
  };


  int lookup_pool(const char *name) {
    int ret = osdmap.lookup_pg_pool_name(name);
    if (ret < 0)
      return -ENOENT;
    return ret;
  }

  // snaps
  int snap_list(PoolCtx *pool, vector<uint64_t> *snaps);
  int snap_lookup(PoolCtx *pool, const char *name, uint64_t *snapid);
  int snap_get_name(PoolCtx *pool, uint64_t snapid, std::string *s);
  int snap_get_stamp(PoolCtx *pool, uint64_t snapid, time_t *t);
  int snap_create(rados_pool_t pool, const char* snapname);
  int selfmanaged_snap_create(rados_pool_t pool, uint64_t *snapid);
  int snap_remove(rados_pool_t pool, const char* snapname);
  int snap_rollback_object(rados_pool_t pool, const object_t& oid,
		    const char* snapname);
  int selfmanaged_snap_remove(rados_pool_t pool, uint64_t snapid);
  int selfmanaged_snap_rollback_object(rados_pool_t pool, const object_t& oid,
                                       SnapContext& snapc, uint64_t snapid);

  // io
  int create(PoolCtx& pool, const object_t& oid, bool exclusive);
  int write(PoolCtx& pool, const object_t& oid, off_t off, bufferlist& bl, size_t len);
  int write_full(PoolCtx& pool, const object_t& oid, bufferlist& bl);
  int read(PoolCtx& pool, const object_t& oid, off_t off, bufferlist& bl, size_t len);
  int mapext(PoolCtx& pool, const object_t& oid, off_t off, size_t len, std::map<off_t,size_t>& m);
  int sparse_read(PoolCtx& pool, const object_t& oid, off_t off, size_t len, std::map<off_t,size_t>& m, bufferlist& bl);
  int remove(PoolCtx& pool, const object_t& oid);
  int stat(PoolCtx& pool, const object_t& oid, uint64_t *psize, time_t *pmtime);
  int trunc(PoolCtx& pool, const object_t& oid, size_t size);

  int tmap_update(PoolCtx& pool, const object_t& oid, bufferlist& cmdbl);
  int exec(PoolCtx& pool, const object_t& oid, const char *cls, const char *method, bufferlist& inbl, bufferlist& outbl);

  int getxattr(PoolCtx& pool, const object_t& oid, const char *name, bufferlist& bl);
  int setxattr(PoolCtx& pool, const object_t& oid, const char *name, bufferlist& bl);
  int getxattrs(PoolCtx& pool, const object_t& oid, map<string, bufferlist>& attrset);
  int rmxattr(PoolCtx& pool, const object_t& oid, const char *name);

  int list_pools(std::list<string>& ls);
  int get_pool_stats(std::list<string>& ls, map<string,pool_stat_t>& result);
  int get_fs_stats(ceph_statfs& result);

  int create_pool(string& name, unsigned long long auid=0, __u8 crush_rule=0);
  int delete_pool(rados_pool_t pool);
  int change_pool_auid(rados_pool_t pool, unsigned long long auid);

  int list(Objecter::ListContext *context, int max_entries);

  // --- aio ---
  struct AioCompletion {
    Mutex lock;
    Cond cond;
    int ref, rval;
    bool released;
    bool ack, safe;
    eversion_t objver;

    rados_callback_t callback_complete, callback_safe;
    void *callback_arg;

    // for read
    bufferlist bl, *pbl;
    char *buf;
    unsigned maxlen;

    AioCompletion() : lock("RadosClient::AioCompletion::lock"),
		      ref(1), rval(0), released(false), ack(false), safe(false), 
		      callback_complete(0), callback_safe(0), callback_arg(0),
		      pbl(0), buf(0), maxlen(0) { }

    int set_complete_callback(void *cb_arg, rados_callback_t cb) {
      lock.Lock();
      callback_complete = cb;
      callback_arg = cb_arg;
      lock.Unlock();
      return 0;
    }
    int set_safe_callback(void *cb_arg, rados_callback_t cb) {
      lock.Lock();
      callback_safe = cb;
      callback_arg = cb_arg;
      lock.Unlock();
      return 0;
    }
    int wait_for_complete() {
      lock.Lock();
      while (!ack)
	cond.Wait(lock);
      lock.Unlock();
      return 0;
    }
    int wait_for_safe() {
      lock.Lock();
      while (!safe)
	cond.Wait(lock);
      lock.Unlock();
      return 0;
    }
    int is_complete() {
      lock.Lock();
      int r = ack;
      lock.Unlock();
      return r;
    }
    int is_safe() {
      lock.Lock();
      int r = safe;
      lock.Unlock();
      return r;
    }
    int get_return_value() {
      lock.Lock();
      int r = rval;
      lock.Unlock();
      return r;
    }
    uint64_t get_version() {
      lock.Lock();
      eversion_t v = objver;
      lock.Unlock();
      return v.version;
    }

    void get() {
      lock.Lock();
      assert(ref > 0);
      ref++;
      lock.Unlock();
    }
    void release() {
      lock.Lock();
      assert(!released);
      released = true;
      put_unlock();
    }
    void put() {
      lock.Lock();
      put_unlock();
    }
    void put_unlock() {
      assert(ref > 0);
      int n = --ref;
      lock.Unlock();
      if (!n)
	delete this;
    }
  };

  struct C_aio_Ack : public Context {
    AioCompletion *c;
    void finish(int r) {
      c->lock.Lock();
      c->rval = r;
      c->ack = true;
      c->cond.Signal();

      if (c->buf && c->bl.length() > 0) {
	unsigned l = MIN(c->bl.length(), c->maxlen);
	c->bl.copy(0, l, c->buf);
	c->rval = c->bl.length();
      }
      if (c->pbl) {
	*c->pbl = c->bl;
      }

      if (c->callback_complete) {
	rados_callback_t cb = c->callback_complete;
	void *cb_arg = c->callback_arg;
	c->lock.Unlock();
	cb(c, cb_arg);
	c->lock.Lock();
      }

      c->put_unlock();
    }
    C_aio_Ack(AioCompletion *_c) : c(_c) {
      c->get();
    }
  };
  
  struct C_aio_sparse_read_Ack : public Context {
    AioCompletion *c;
    bufferlist *data_bl;
    std::map<off_t,size_t> *m;
    
    void finish(int r) {
      c->lock.Lock();
      c->rval = r;
      c->ack = true;
      c->cond.Signal();

      bufferlist::iterator iter = c->bl.begin();
      if (r >= 0) {
        ::decode(*m, iter);
        ::decode(*data_bl, iter);
      }

      if (c->callback_complete) {
	rados_callback_t cb = c->callback_complete;
	void *cb_arg = c->callback_arg;
	c->lock.Unlock();
	cb(c, cb_arg);
	c->lock.Lock();
      }

      c->put_unlock();
    }
    C_aio_sparse_read_Ack(AioCompletion *_c) : c(_c) {
      c->get();
    }
  };

  struct C_aio_Safe : public Context {
    AioCompletion *c;
    void finish(int r) {
      c->lock.Lock();
      if (!c->ack) {
	c->rval = r;
	c->ack = true;
      }
      c->safe = true;
      c->cond.Signal();

      if (c->callback_safe) {
	rados_callback_t cb = c->callback_safe;
	void *cb_arg = c->callback_arg;
	c->lock.Unlock();
	cb(c, cb_arg);
	c->lock.Lock();
      }

      c->put_unlock();
    }
    C_aio_Safe(AioCompletion *_c) : c(_c) {
      c->get();
    }
  };

  int aio_read(PoolCtx& pool, object_t oid, off_t off, bufferlist *pbl, size_t len,
	       AioCompletion *c);
  int aio_read(PoolCtx& pool, object_t oid, off_t off, char *buf, size_t len,
	       AioCompletion *c);

  int aio_sparse_read(PoolCtx& pool, const object_t oid, off_t off,
		      std::map<off_t,size_t> *m, bufferlist *data_bl, size_t len,
		      AioCompletion *c);

  int aio_write(PoolCtx& pool, object_t oid, off_t off, const bufferlist& bl, size_t len,
		AioCompletion *c);

  int aio_write_full(PoolCtx& pool, object_t oid, const bufferlist& bl,
		    AioCompletion *c);

  static AioCompletion *aio_create_completion() {
    return new AioCompletion;
  }
  static AioCompletion *aio_create_completion(void *cb_arg, rados_callback_t cb_complete, rados_callback_t cb_safe) {
    AioCompletion *c = new AioCompletion;
    if (cb_complete)
      c->set_complete_callback(cb_arg, cb_complete);
    if (cb_safe)
      c->set_safe_callback(cb_arg, cb_safe);
    return c;
  }


  // watch/notify
  struct WatchContext {
    PoolCtx pool_ctx;
    const object_t oid; 
    uint64_t cookie;
    uint64_t ver;
    librados::Rados::WatchCtx *ctx;
    ObjectOperation *op;
    uint64_t linger_id;

    WatchContext(PoolCtx& _pc, const object_t& _oc, librados::Rados::WatchCtx *_ctx,
                 ObjectOperation *_op) : pool_ctx(_pc), oid(_oc), ctx(_ctx), op(_op), linger_id(0) {}
    ~WatchContext() {
    }
    void notify(RadosClient *client, MWatchNotify *m) {
      ctx->notify(m->opcode, m->ver);
      if (m->opcode != WATCH_NOTIFY_COMPLETE) {
        client->_notify_ack(pool_ctx, oid, m->notify_id, m->ver);
      }
    }
  };

  struct C_NotifyComplete : public librados::Rados::WatchCtx {
    Mutex *lock;
    Cond *cond;
    bool *done;

    C_NotifyComplete(Mutex *_l, Cond *_c, bool *_d) : lock(_l), cond(_c), done(_d) {}

    void notify(uint8_t opcode, uint64_t ver) {
      if (opcode != WATCH_NOTIFY_COMPLETE)
        derr << "WARNING: C_NotifyComplete got response: opcode="
	     << (int)opcode << " ver=" << ver << dendl;
      *done = true;
      cond->Signal();
    }
  };

  uint64_t max_watch_cookie;
  map<uint64_t, WatchContext *> watchers;
 
  void set_sync_op_version(PoolCtx& pool, eversion_t& ver) {
      pool.last_objver = ver;
  }

  void register_watcher(PoolCtx& pool, const object_t& oid, librados::Rados::WatchCtx *ctx,
			ObjectOperation *op, uint64_t *cookie, WatchContext **pwc = NULL) {
    assert(lock.is_locked());
    WatchContext *wc = new WatchContext(pool, oid, ctx, op);
    *cookie = ++max_watch_cookie;
    watchers[*cookie] = wc;
    if (pwc)
      *pwc = wc;
  }

  void unregister_watcher(uint64_t cookie) {
    assert(lock.is_locked());
    map<uint64_t, WatchContext *>::iterator iter = watchers.find(cookie);
    if (iter != watchers.end()) {
      WatchContext *ctx = iter->second;
      if (ctx->linger_id)
        objecter->unregister_linger(ctx->linger_id);
      delete ctx;
      watchers.erase(iter);
    }
  }

  void watch_notify(MWatchNotify *m);

  int watch(PoolCtx& pool, const object_t& oid, uint64_t ver, uint64_t *cookie, librados::Rados::WatchCtx *ctx);
  int unwatch(PoolCtx& pool, const object_t& oid, uint64_t cookie);
  int notify(PoolCtx& pool, const object_t& oid, uint64_t ver);
  int _notify_ack(PoolCtx& pool, const object_t& oid, uint64_t notify_id, uint64_t ver);

  eversion_t last_version(PoolCtx& pool) {
    return pool.last_objver;
  }
  void set_assert_version(PoolCtx& pool, uint64_t ver) {
    pool.assert_ver = ver;
  }

  void set_notify_timeout(PoolCtx& pool, uint32_t timeout) {
    pool.notify_timeout = timeout;
  }
};

int RadosClient::init()
{
  // get monmap
  int ret = monclient.build_initial_monmap();
  if (ret < 0)
    return ret;

  assert_warn(messenger);
  if (!messenger)
    return -ENOMEM;

  dout(1) << "starting msgr at " << messenger->get_ms_addr() << dendl;

  messenger->register_entity(entity_name_t::CLIENT(-1));
  dout(1) << "starting objecter" << dendl;

  objecter = new Objecter(messenger, &monclient, &osdmap, lock, timer);
  if (!objecter)
    return -ENOMEM;
  objecter->set_balanced_budget();

  monclient.set_messenger(messenger);
  
  messenger->add_dispatcher_head(this);

  messenger->start(1);
  messenger->add_dispatcher_head(this);

  dout(1) << "setting wanted keys" << dendl;
  monclient.set_want_keys(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD);
  dout(1) << "calling monclient init" << dendl;
  monclient.init();

  int err = monclient.authenticate(g_conf.client_mount_timeout);
  if (err) {
    dout(0) << *g_conf.entity_name << " authentication error " << strerror(-err) << dendl;
    shutdown();
    return err;
  }
  messenger->set_myname(entity_name_t::CLIENT(monclient.get_global_id()));

  lock.Lock();

  timer.init();

  objecter->set_client_incarnation(0);
  objecter->init();
  monclient.renew_subs();

  while (osdmap.get_epoch() == 0) {
    dout(1) << "waiting for osdmap" << dendl;
    cond.Wait(lock);
  }
  lock.Unlock();

  dout(1) << "init done" << dendl;

  return 0;
}

void RadosClient::shutdown()
{
  lock.Lock();
  monclient.shutdown();
  objecter->shutdown();
  timer.shutdown();
  lock.Unlock();
  messenger->shutdown();
  messenger->wait();
  dout(1) << "shutdown" << dendl;
}

RadosClient::~RadosClient()
{
  if (messenger)
    messenger->destroy();
}


bool RadosClient::ms_dispatch(Message *m)
{
  lock.Lock();
  bool ret = _dispatch(m);
  lock.Unlock();
  return ret;
}

void RadosClient::ms_handle_connect(Connection *con)
{
  Mutex::Locker l(lock);
  objecter->ms_handle_connect(con);
}

bool RadosClient::ms_handle_reset(Connection *con)
{
  Mutex::Locker l(lock);
  objecter->ms_handle_reset(con);
  return false;
}

void RadosClient::ms_handle_remote_reset(Connection *con)
{
  Mutex::Locker l(lock);
  objecter->ms_handle_remote_reset(con);
}


bool RadosClient::_dispatch(Message *m)
{
  switch (m->get_type()) {
  // OSD
  case CEPH_MSG_OSD_OPREPLY:
    objecter->handle_osd_op_reply((class MOSDOpReply*)m);
    break;
  case CEPH_MSG_OSD_MAP:
    objecter->handle_osd_map((MOSDMap*)m);
    cond.Signal();
    break;
  case MSG_GETPOOLSTATSREPLY:
    objecter->handle_get_pool_stats_reply((MGetPoolStatsReply*)m);
    break;

  case CEPH_MSG_MDS_MAP:
    break;

  case CEPH_MSG_STATFS_REPLY:
    objecter->handle_fs_stats_reply((MStatfsReply*)m);
    break;

  case CEPH_MSG_POOLOP_REPLY:
    objecter->handle_pool_op_reply((MPoolOpReply*)m);
    break;

  case CEPH_MSG_WATCH_NOTIFY:
    watch_notify((MWatchNotify *)m);
    break;
  default:
    return false;
  }

  return true;
}

int RadosClient::list_pools(std::list<string>& v)
{
  Mutex::Locker l(lock);
  for (map<int,pg_pool_t>::const_iterator p = osdmap.get_pools().begin(); 
       p != osdmap.get_pools().end();
       p++)
    v.push_back(osdmap.get_pool_name(p->first));
  return 0;
}

int RadosClient::get_pool_stats(std::list<string>& pools, map<string,pool_stat_t>& result)
{
  Mutex mylock("RadosClient::get_pool_stats::mylock");
  Cond cond;
  bool done;

  lock.Lock();
  objecter->get_pool_stats(pools, &result, new C_SafeCond(&mylock, &cond, &done));
  lock.Unlock();
  
  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  return 0;
}

int RadosClient::get_fs_stats(ceph_statfs& stats)
{
  Mutex mylock ("RadosClient::get_fs_stats::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->get_fs_stats(stats, new C_SafeCond(&mylock, &cond, &done));
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();

  return 0;
}


// SNAPS

int RadosClient::snap_create( rados_pool_t pool, const char *snapName)
{
  int reply;
  int poolID = ((PoolCtx *)pool)->poolid;
  string sName(snapName);

  Mutex mylock ("RadosClient::snap_create::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->create_pool_snap(poolID,
			     sName,
			     new C_SafeCond(&mylock, &cond, &done, &reply));
  lock.Unlock();

  mylock.Lock();
  while(!done) cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

int RadosClient::selfmanaged_snap_create(rados_pool_t pool, uint64_t *psnapid)
{
  int reply;
  int poolID = ((PoolCtx *)pool)->poolid;

  Mutex mylock("RadosClient::selfmanaged_snap_create::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  snapid_t snapid;
  objecter->allocate_selfmanaged_snap(poolID, &snapid,
				      new C_SafeCond(&mylock, &cond, &done, &reply));
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();
  if (reply == 0)
    *psnapid = snapid;
  return reply;
}

int RadosClient::snap_remove(rados_pool_t pool, const char *snapName)
{
  int reply;
  int poolID = ((PoolCtx *)pool)->poolid;
  string sName(snapName);

  Mutex mylock ("RadosClient::snap_remove::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->delete_pool_snap(poolID,
			     sName,
			     new C_SafeCond(&mylock, &cond, &done, &reply));
  lock.Unlock();

  mylock.Lock();
  while(!done) cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

int RadosClient::selfmanaged_snap_rollback_object(rados_pool_t pool,
				      const object_t& oid, SnapContext& snapc,
                                      uint64_t snapid)
{
  int reply;
  PoolCtx* ctx = (PoolCtx *) pool;

  object_locator_t oloc(ctx->poolid);

  Mutex mylock("RadosClient::snap_rollback::mylock");
  Cond cond;
  bool done;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &reply);

  lock.Lock();
  objecter->rollback_object(oid, oloc, snapc, snapid,
		     g_clock.now(), onack, NULL);
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

int RadosClient::snap_rollback_object(rados_pool_t pool_,
				      const object_t& oid, const char *snapName)
{
  PoolCtx* pool = (PoolCtx *) pool_;
  string sName(snapName);

  snapid_t snap;
  const map<int, pg_pool_t>& pools = objecter->osdmap->get_pools();
  const pg_pool_t& pg_pool = pools.find(pool->poolid)->second;
  map<snapid_t, pool_snap_info_t>::const_iterator p;
  for (p = pg_pool.snaps.begin();
       p != pg_pool.snaps.end();
       ++p) {
    if (p->second.name == snapName) {
      snap = p->first;
      break;
    }
  }
  if (p == pg_pool.snaps.end()) return -ENOENT;

  return selfmanaged_snap_rollback_object(pool_, oid, pool->snapc, snap);
}

int RadosClient::selfmanaged_snap_remove(rados_pool_t pool, uint64_t snapid)
{
  int reply;
  int poolID = ((PoolCtx *)pool)->poolid;

  Mutex mylock("RadosClient::selfmanaged_snap_remove::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->delete_selfmanaged_snap(poolID, snapid_t(snapid),
				      new C_SafeCond(&mylock, &cond, &done, &reply));
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();
  return (int)reply;
}

int RadosClient::create_pool(string& name, unsigned long long auid,
			     __u8 crush_rule)
{
  int reply;

  Mutex mylock ("RadosClient::pool_create::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->create_pool(name,
			new C_SafeCond(&mylock, &cond, &done, &reply),
			auid, crush_rule);
  lock.Unlock();

  mylock.Lock();
  while(!done)
    cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

int RadosClient::delete_pool(rados_pool_t pool)
{
  int reply;

  int poolID = ((PoolCtx *)pool)->poolid;

  Mutex mylock("RadosClient::delete_pool::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->delete_pool(poolID, new C_SafeCond(&mylock, &cond, &done, &reply));
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

/**
 * Attempt to change a pool's associated auid "owner." Requires that you
 * have write permission on both the current and new auid.
 * pool: reference to the pool to change.
 * auid: the auid you wish the pool to have.
 * Returns: 0 on success, or -ERROR# on failure.
 */
int RadosClient::change_pool_auid(rados_pool_t pool, unsigned long long auid)
{
  int reply;

  int poolID = ((PoolCtx *)pool)->poolid;

  Mutex mylock("RadosClient::change_pool_auid::mylock");
  Cond cond;
  bool done;
  lock.Lock();
  objecter->change_pool_auid(poolID,
			     new C_SafeCond(&mylock, &cond, &done, &reply),
			     auid);
  lock.Unlock();

  mylock.Lock();
  while (!done) cond.Wait(mylock);
  mylock.Unlock();
  return reply;
}

int RadosClient::snap_list(PoolCtx *pool, vector<uint64_t> *snaps)
{
  Mutex::Locker l(lock);
  const pg_pool_t *pi = objecter->osdmap->get_pg_pool(pool->poolid);
  for (map<snapid_t,pool_snap_info_t>::const_iterator p = pi->snaps.begin();
       p != pi->snaps.end();
       p++)
    snaps->push_back(p->first);
  return 0;
}

int RadosClient::snap_lookup(PoolCtx *pool, const char *name, uint64_t *snapid)
{
  Mutex::Locker l(lock);
  const pg_pool_t *pi = objecter->osdmap->get_pg_pool(pool->poolid);
  for (map<snapid_t,pool_snap_info_t>::const_iterator p = pi->snaps.begin();
       p != pi->snaps.end();
       p++) {
    if (p->second.name == name) {
      *snapid = p->first;
      return 0;
    }
  }
  return -ENOENT;
}

int RadosClient::snap_get_name(PoolCtx *pool, uint64_t snapid, std::string *s)
{
  Mutex::Locker l(lock);
  const pg_pool_t *pi = objecter->osdmap->get_pg_pool(pool->poolid);
  map<snapid_t,pool_snap_info_t>::const_iterator p = pi->snaps.find(snapid);
  if (p == pi->snaps.end())
    return -ENOENT;
  *s = p->second.name.c_str();
  return 0;
}

int RadosClient::snap_get_stamp(PoolCtx *pool, uint64_t snapid, time_t *t)
{
  Mutex::Locker l(lock);
  const pg_pool_t *pi = objecter->osdmap->get_pg_pool(pool->poolid);
  map<snapid_t,pool_snap_info_t>::const_iterator p = pi->snaps.find(snapid);
  if (p == pi->snaps.end())
    return -ENOENT;
  *t = p->second.stamp.sec();
  return 0;
}


// IO

int RadosClient::list(Objecter::ListContext *context, int max_entries)
{
  Cond cond;
  bool done;
  int r = 0;
  object_t oid;
  Mutex mylock("RadosClient::list::mylock");

  if (context->at_end)
    return 0;

  context->max_entries = max_entries;

  lock.Lock();
  objecter->list_objects(context, new C_SafeCond(&mylock, &cond, &done, &r));
  lock.Unlock();

  mylock.Lock();
  while(!done)
    cond.Wait(mylock);
  mylock.Unlock();

  return r;
}

int RadosClient::create(PoolCtx& pool, const object_t& oid, bool exclusive)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::create::mylock");
  Cond cond;
  bool done;
  int r;
  eversion_t ver;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->create(oid, oloc,
		  pool.snapc, ut, 0, (exclusive ? CEPH_OSD_OP_FLAG_EXCL : 0),
		  onack, NULL, &ver);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::write(PoolCtx& pool, const object_t& oid, off_t off, bufferlist& bl, size_t len)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::write::mylock");
  Cond cond;
  bool done;
  int r;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->write(oid, oloc,
		  off, len, pool.snapc, bl, ut, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  if (r < 0)
    return r;

  return len;
}

int RadosClient::write_full(PoolCtx& pool, const object_t& oid, bufferlist& bl)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::write_full::mylock");
  Cond cond;
  bool done;
  int r;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);

  eversion_t ver;
  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->write_full(oid, oloc,
		  pool.snapc, bl, ut, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::aio_read(PoolCtx& pool, const object_t oid, off_t off, bufferlist *pbl, size_t len,
			  AioCompletion *c)
{
 
  Context *onack = new C_aio_Ack(c);
  eversion_t ver;

  c->pbl = pbl;

  Mutex::Locker l(lock);
  object_locator_t oloc(pool.poolid);
  objecter->read(oid, oloc,
		 off, len, pool.snap_seq, &c->bl, 0,
		 onack, &c->objver);
  return 0;
}

int RadosClient::aio_read(PoolCtx& pool, const object_t oid, off_t off, char *buf, size_t len,
			  AioCompletion *c)
{
  Context *onack = new C_aio_Ack(c);

  c->buf = buf;
  c->maxlen = len;

  Mutex::Locker l(lock);
  object_locator_t oloc(pool.poolid);
  objecter->read(oid, oloc,
		 off, len, pool.snap_seq, &c->bl, 0,
		 onack, &c->objver);

  return 0;
}

int RadosClient::aio_sparse_read(PoolCtx& pool, const object_t oid, off_t off,
			  std::map<off_t,size_t> *m, bufferlist *data_bl, size_t len,
			  AioCompletion *c)
{
 
  C_aio_sparse_read_Ack *onack = new C_aio_sparse_read_Ack(c);
  onack->m = m;
  onack->data_bl = data_bl;
  eversion_t ver;

  c->pbl = NULL;

  Mutex::Locker l(lock);
  object_locator_t oloc(pool.poolid);
  objecter->sparse_read(oid, oloc,
		 off, len, pool.snap_seq, &c->bl, 0,
		 onack);
  return 0;
}

int RadosClient::aio_write(PoolCtx& pool, const object_t oid, off_t off, const bufferlist& bl, size_t len,
			   AioCompletion *c)
{
  SnapContext snapc;
  utime_t ut = g_clock.now();

  Context *onack = new C_aio_Ack(c);
  Context *onsafe = new C_aio_Safe(c);

  Mutex::Locker l(lock);
  object_locator_t oloc(pool.poolid);
  objecter->write(oid, oloc,
		  off, len, pool.snapc, bl, ut, 0,
		  onack, onsafe, &c->objver);

  return 0;
}

int RadosClient::aio_write_full(PoolCtx& pool, const object_t oid, const bufferlist& bl,
			       AioCompletion *c)
{
  SnapContext snapc;
  utime_t ut = g_clock.now();

  Context *onack = new C_aio_Ack(c);
  Context *onsafe = new C_aio_Safe(c);

  Mutex::Locker l(lock);
  object_locator_t oloc(pool.poolid);
  objecter->write_full(oid, oloc,
		  pool.snapc, bl, ut, 0,
		  onack, onsafe, &c->objver);

  return 0;
}

int RadosClient::remove(PoolCtx& pool, const object_t& oid)
{
  SnapContext snapc;
  utime_t ut = g_clock.now();

  Mutex mylock("RadosClient::remove::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->remove(oid, oloc,
		  snapc, ut, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::trunc(PoolCtx& pool, const object_t& oid, size_t size)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::write_full::mylock");
  Cond cond;
  bool done;
  int r;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->trunc(oid, oloc,
		  pool.snapc, ut, 0,
		  size, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::tmap_update(PoolCtx& pool, const object_t& oid, bufferlist& cmdbl)
{
  utime_t ut = g_clock.now();

  Mutex mylock("RadosClient::tmap_update::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  bufferlist outbl;

  lock.Lock();
  SnapContext snapc;
  object_locator_t oloc(pool.poolid);
  ObjectOperation wr;
  if (pool.assert_ver) {
    wr.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  wr.tmap_update(cmdbl);
  objecter->mutate(oid, oloc, wr, snapc, ut, 0, onack, NULL, &ver);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}


int RadosClient::exec(PoolCtx& pool, const object_t& oid, const char *cls, const char *method,
		      bufferlist& inbl, bufferlist& outbl)
{
  utime_t ut = g_clock.now();

  Mutex mylock("RadosClient::exec::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;


  lock.Lock();
  object_locator_t oloc(pool.poolid);
  ObjectOperation rd;
  if (pool.assert_ver) {
    rd.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  rd.call(cls, method, inbl);
  objecter->read(oid, oloc, rd, pool.snap_seq, &outbl, 0, onack, &ver);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::read(PoolCtx& pool, const object_t& oid, off_t off, bufferlist& bl, size_t len)
{
  SnapContext snapc;

  Mutex mylock("RadosClient::read::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->read(oid, oloc,
	      off, len, pool.snap_seq, &bl, 0,
              onack, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();
  dout(10) << "Objecter returned from read r=" << r << dendl;

  set_sync_op_version(pool, ver);

  if (r < 0)
    return r;

  if (bl.length() < len) {
    dout(10) << "Returned length " << bl.length()
	     << " less than original length "<< len << dendl;
  }

  return bl.length();
}

int RadosClient::mapext(PoolCtx& pool, const object_t& oid, off_t off, size_t len, std::map<off_t,size_t>& m)
{
  SnapContext snapc;
  bufferlist bl;

  Mutex mylock("RadosClient::read::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->mapext(oid, oloc,
	      off, len, pool.snap_seq, &bl, 0,
              onack);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();
  dout(10) << "Objecter returned from read r=" << r << dendl;

  if (r < 0)
    return r;

  bufferlist::iterator iter = bl.begin();
  ::decode(m, iter);

  return m.size();
}

int RadosClient::sparse_read(PoolCtx& pool, const object_t& oid, off_t off, size_t len,
                             std::map<off_t,size_t>& m, bufferlist& data_bl)
{
  SnapContext snapc;
  bufferlist bl;

  Mutex mylock("RadosClient::read::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->sparse_read(oid, oloc,
	      off, len, pool.snap_seq, &bl, 0,
              onack);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();
  dout(10) << "Objecter returned from read r=" << r << dendl;

  if (r < 0)
    return r;

  bufferlist::iterator iter = bl.begin();
  ::decode(m, iter);
  ::decode(data_bl, iter);

  return m.size();
}

int RadosClient::stat(PoolCtx& pool, const object_t& oid, uint64_t *psize, time_t *pmtime)
{
  SnapContext snapc;

  Mutex mylock("RadosClient::stat::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  uint64_t size;
  utime_t mtime;
  eversion_t ver;

  if (!psize)
    psize = &size;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->stat(oid, oloc,
	      pool.snap_seq, psize, &mtime, 0,
              onack, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();
  dout(10) << "Objecter returned from stat" << dendl;

  if (r >= 0 && pmtime) {
    *pmtime = mtime.sec();
  }

  set_sync_op_version(pool, ver);

  return r;
}

int RadosClient::getxattr(PoolCtx& pool, const object_t& oid, const char *name, bufferlist& bl)
{
  SnapContext snapc;

  Mutex mylock("RadosClient::getxattr::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->getxattr(oid, oloc,
	      name, pool.snap_seq, &bl, 0,
              onack, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();
  dout(10) << "Objecter returned from getxattr" << dendl;

  set_sync_op_version(pool, ver);

  if (r < 0)
    return r;

  return bl.length();
}

int RadosClient::rmxattr(PoolCtx& pool, const object_t& oid, const char *name)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::rmxattr::mylock");
  Cond cond;
  bool done;
  int r;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  object_locator_t oloc(pool.poolid);

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  lock.Lock();
  objecter->removexattr(oid, oloc, name,
		  pool.snapc, ut, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  if (r < 0)
    return r;

  return 0;
}

int RadosClient::setxattr(PoolCtx& pool, const object_t& oid, const char *name, bufferlist& bl)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::setxattr::mylock");
  Cond cond;
  bool done;
  int r;

  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  lock.Lock();
  object_locator_t oloc(pool.poolid);
  objecter->setxattr(oid, oloc, name,
		  pool.snapc, bl, ut, 0,
		  onack, NULL, &ver, pop);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  if (r < 0)
    return r;

  return bl.length();
}

int RadosClient::getxattrs(PoolCtx& pool, const object_t& oid, map<std::string, bufferlist>& attrset)
{
  utime_t ut = g_clock.now();

  /* can't write to a snapshot */
  if (pool.snap_seq != CEPH_NOSNAP)
    return -EINVAL;

  Mutex mylock("RadosClient::getexattrs::mylock");
  Cond cond;
  bool done;
  int r;
  eversion_t ver;

  ObjectOperation op, *pop = NULL;
  if (pool.assert_ver) {
    op.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
    pop = &op;
  }
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);

  lock.Lock();
  object_locator_t oloc(pool.poolid);
  map<string, bufferlist> aset;
  objecter->getxattrs(oid, oloc, pool.snap_seq,
		      aset,
		      0, onack, &ver, pop);
  lock.Unlock();

  attrset.clear();


  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  for (map<string,bufferlist>::iterator p = aset.begin(); p != aset.end(); p++) {
    dout(10) << "RadosClient::getxattrs: xattr=" << p->first << dendl;
    attrset[p->first.c_str()] = p->second;
  }

  set_sync_op_version(pool, ver);

  return r;
}

void RadosClient::watch_notify(MWatchNotify *m)
{
  assert(lock.is_locked());
  WatchContext *wc = NULL;
  map<uint64_t, WatchContext *>::iterator iter = watchers.find(m->cookie);
  if (iter != watchers.end())
    wc = iter->second;

  if (!wc)
    return;

  wc->notify(this, m);
}

int RadosClient::watch(PoolCtx& pool, const object_t& oid, uint64_t ver, uint64_t *cookie, librados::Rados::WatchCtx *ctx)
{
  utime_t ut = g_clock.now();

  ObjectOperation *rd = new ObjectOperation();
  if (!rd)
    return -ENOMEM;

  Mutex mylock("RadosClient::watch::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t objver;

  lock.Lock();

  WatchContext *wc;
  register_watcher(pool, oid, ctx, rd, cookie, &wc);

  object_locator_t oloc(pool.poolid);

  if (pool.assert_ver) {
    rd->assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  rd->watch(*cookie, ver, 1);
  bufferlist bl;
  wc->linger_id = objecter->linger(oid, oloc, *rd, pool.snap_seq, bl, NULL, 0, onack, NULL, &objver);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, objver);

  if (r < 0) {
    lock.Lock();
    unregister_watcher(*cookie);
    lock.Unlock();
  }

  return r;
}


/* this is called with RadosClient::lock held */
int RadosClient::_notify_ack(PoolCtx& pool, const object_t& oid, uint64_t notify_id, uint64_t ver)
{
  utime_t ut = g_clock.now();

  Mutex mylock("RadosClient::watch::mylock");
  Cond cond;
  eversion_t objver;

  object_locator_t oloc(pool.poolid);
  ObjectOperation rd;
  if (pool.assert_ver) {
    rd.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  rd.notify_ack(notify_id, ver);
  objecter->read(oid, oloc, rd, pool.snap_seq, NULL, 0, 0, 0);

  return 0;
}

int RadosClient::unwatch(PoolCtx& pool, const object_t& oid, uint64_t cookie)
{
  utime_t ut = g_clock.now();
  bufferlist inbl, outbl;

  Mutex mylock("RadosClient::watch::mylock");
  Cond cond;
  bool done;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t ver;
  lock.Lock();

  unregister_watcher(cookie);

  object_locator_t oloc(pool.poolid);
  ObjectOperation rd;
  if (pool.assert_ver) {
    rd.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  rd.watch(cookie, 0, 0);
  objecter->read(oid, oloc, rd, pool.snap_seq, &outbl, 0, onack, &ver);
  lock.Unlock();

  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  set_sync_op_version(pool, ver);

  return r;
}// ---------------------------------------------

int RadosClient::notify(PoolCtx& pool, const object_t& oid, uint64_t ver)
{
  utime_t ut = g_clock.now();
  bufferlist inbl, outbl;

  Mutex mylock("RadosClient::notify::mylock");
  Mutex mylock_all("RadosClient::notify::mylock_all");
  Cond cond, cond_all;
  bool done, done_all;
  int r;
  Context *onack = new C_SafeCond(&mylock, &cond, &done, &r);
  eversion_t objver;
  uint64_t cookie;
  C_NotifyComplete *ctx = new C_NotifyComplete(&mylock_all, &cond_all, &done_all);
  ObjectOperation rd;

  object_locator_t oloc(pool.poolid);
  if (pool.assert_ver) {
    rd.assert_version(pool.assert_ver);
    pool.assert_ver = 0;
  }
  lock.Lock();
  register_watcher(pool, oid, ctx, &rd, &cookie);
  uint32_t prot_ver = 1;
  uint32_t timeout = pool.notify_timeout;
  ::encode(prot_ver, inbl);
  ::encode(timeout, inbl);
  rd.notify(cookie, ver, inbl);
  objecter->read(oid, oloc, rd, pool.snap_seq, &outbl, 0, onack, &objver);
  lock.Unlock();

  mylock_all.Lock();
  mylock.Lock();
  while (!done)
    cond.Wait(mylock);
  mylock.Unlock();

  while (!done_all)
    cond_all.Wait(mylock_all);
  mylock_all.Unlock();
  
  lock.Lock();
  unregister_watcher(cookie);
  lock.Unlock();

  set_sync_op_version(pool, objver);
  delete ctx;

  return r;
}

namespace librados {

Rados::Rados() : client(NULL)
{
}

Rados::Rados(pool_t pool)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  client = ctx->client;
}

Rados::~Rados()
{
  if (client)
    delete client;
  client = NULL;
}

int Rados::initialize(int argc, const char *argv[])
{
 vector<const char*> args;

  if (argc && argv) {
    argv_to_vec(argc, argv, args);
    env_to_vec(args);
  }
  common_set_defaults(false);
  common_init(args, "librados", STARTUP_FLAG_INIT_KEYS);

  client = new RadosClient();
  return client->init();
}

void Rados::shutdown()
{
  if (!client)
    return;
  client->shutdown();
}

void Rados::version(int *major, int *minor, int *extra)
{
  rados_version(major, minor, extra);
}

int Rados::pool_list(std::list<string>& v)
{
  if (!client)
    return -EINVAL;
  return client->list_pools(v);
}

int Rados::pool_get_stats(std::list<string>& v, std::map<string,pool_stat_t>& result)
{
  if (!client)
    return -EINVAL;
  map<string,::pool_stat_t> rawresult;
  int r = client->get_pool_stats(v, rawresult);
  for (map<string,::pool_stat_t>::iterator p = rawresult.begin();
       p != rawresult.end();
       p++) {
    pool_stat_t& v = result[p->first];
    v.num_kb = p->second.num_kb;
    v.num_bytes = p->second.num_bytes;
    v.num_objects = p->second.num_objects;
    v.num_object_clones = p->second.num_object_clones;
    v.num_object_copies = p->second.num_object_copies;
    v.num_objects_missing_on_primary = p->second.num_objects_missing_on_primary;
    v.num_objects_unfound = p->second.num_objects_unfound;
    v.num_objects_degraded = p->second.num_objects_degraded;
    v.num_rd = p->second.num_rd;
    v.num_rd_kb = p->second.num_rd_kb;
    v.num_wr = p->second.num_wr;
    v.num_wr_kb = p->second.num_wr_kb;
  }
  return r;
}

int Rados::create_pool(const char *name, uint64_t auid, __u8 crush_rule)
{
  string str(name);
  if (!client)
    return -EINVAL;
  return client->create_pool(str, auid, crush_rule);
}

int Rados::delete_pool(rados_pool_t pool)
{
  if (!client) return -EINVAL;
  return client->delete_pool(pool);
}

int Rados::change_pool_auid(rados_pool_t pool, uint64_t auid)
{
  if (!client) return -EINVAL;
  return client->change_pool_auid(pool, auid);
}

int Rados::fs_get_stats(statfs_t& result)
{
  if (!client)
    return -EINVAL;
  ceph_statfs stats;
  int r = client->get_fs_stats(stats);
  result.kb = stats.kb;
  result.kb_used = stats.kb_used;
  result.kb_avail = stats.kb_avail;
  result.num_objects = stats.num_objects;
  return r;
}

int Rados::objects_list_open(pool_t pool, Rados::ListCtx *ctx)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *p = (RadosClient::PoolCtx *)pool;
  Objecter::ListContext *h = new Objecter::ListContext;
  h->pool_id = p->poolid;
  h->pool_snap_seq = p->snap_seq;
  ctx->ctx = (void *)h;
  return 0;
}

int Rados::objects_list_more(Rados::ListCtx& ctx, int max, std::list<string>& entries)
{
  if (!client)
    return -EINVAL;

  Objecter::ListContext *h = (Objecter::ListContext *)ctx.ctx;
  h->list.clear();
  int r = client->list(h, max);
  while (!h->list.empty()) {
    entries.push_back(h->list.front().name.c_str());
    h->list.pop_front();
  }
  if (ctx.extra_info)
    ctx.extra_info->append(h->extra_info);
  return r;
}

void Rados::objects_list_close(Rados::ListCtx& ctx)
{
  if (!client)
    return;
  Objecter::ListContext *h = (Objecter::ListContext *)ctx.ctx;
  delete h;
}

void Rados::list_filter(Rados::ListCtx& ctx, bufferlist& filter, bufferlist *extra_info)
{
  Objecter::ListContext *h = (Objecter::ListContext *)ctx.ctx;
  h->filter = filter;
  ctx.extra_info = extra_info;
}

uint64_t Rados::get_last_version(rados_pool_t pool)
{
  if (!client)
    return 0;

  eversion_t ver = client->last_version(*(RadosClient::PoolCtx *)pool);
  return ver.version;
}


int Rados::create(rados_pool_t pool, const string& o, bool exclusive)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->create(*(RadosClient::PoolCtx *)pool, oid, exclusive);
}

int Rados::write(rados_pool_t pool, const string& o, off_t off, bufferlist& bl, size_t len)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->write(*(RadosClient::PoolCtx *)pool, oid, off, bl, len);
}

int Rados::write_full(rados_pool_t pool, const string& o, bufferlist& bl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->write_full(*(RadosClient::PoolCtx *)pool, oid, bl);
}

int Rados::trunc(rados_pool_t pool, const string& o, size_t size)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->trunc(*(RadosClient::PoolCtx *)pool, oid, size);
}

int Rados::remove(rados_pool_t pool, const string& o)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->remove(*(RadosClient::PoolCtx *)pool, oid);
}

int Rados::read(rados_pool_t pool, const string& o, off_t off, bufferlist& bl, size_t len)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->read(*(RadosClient::PoolCtx *)pool, oid, off, bl, len);
}

int Rados::mapext(rados_pool_t pool, const string& o, off_t off, size_t len, std::map<off_t, size_t>& m)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return ((RadosClient *)client)->mapext(*(RadosClient::PoolCtx *)pool, oid, off, len, m);
}

int Rados::sparse_read(rados_pool_t pool, const string& o, off_t off, size_t len, std::map<off_t, size_t>& m, bufferlist& bl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return ((RadosClient *)client)->sparse_read(*(RadosClient::PoolCtx *)pool, oid, off, len, m, bl);
}

int Rados::getxattr(rados_pool_t pool, const string& o, const char *name, bufferlist& bl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->getxattr(*(RadosClient::PoolCtx *)pool, oid, name, bl);
}

int Rados::getxattrs(rados_pool_t pool, const string& o, map<std::string, bufferlist>& attrset)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->getxattrs(*(RadosClient::PoolCtx *)pool, oid, attrset);
}

int Rados::setxattr(rados_pool_t pool, const string& o, const char *name, bufferlist& bl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->setxattr(*(RadosClient::PoolCtx *)pool, oid, name, bl);
}

int Rados::rmxattr(rados_pool_t pool, const string& o, const char *name)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->rmxattr(*(RadosClient::PoolCtx *)pool, oid, name);
}

int Rados::stat(rados_pool_t pool, const string& o, uint64_t *psize, time_t *pmtime)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->stat(*(RadosClient::PoolCtx *)pool, oid, psize, pmtime);
}

int Rados::tmap_update(rados_pool_t pool, const string& o, bufferlist& cmdbl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->tmap_update(*(RadosClient::PoolCtx *)pool, oid, cmdbl);
}
int Rados::exec(rados_pool_t pool, const string& o, const char *cls, const char *method,
		bufferlist& inbl, bufferlist& outbl)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->exec(*(RadosClient::PoolCtx *)pool, oid, cls, method, inbl, outbl);
}

int Rados::lookup_pool(const char *name)
{
  return client->lookup_pool(name);
}

int Rados::open_pool(const char *name, rados_pool_t *pool)
{
  if (!client)
    return -EINVAL;
  int poolid = client->lookup_pool(name);
  if (poolid >= 0) {
    RadosClient::PoolCtx *ctx = new RadosClient::PoolCtx(client, poolid, name, CEPH_NOSNAP);
    if (!ctx)
      return -ENOMEM;

    *pool = (rados_pool_t)ctx;
    return 0;
  }
  return poolid;
}

int Rados::close_pool(rados_pool_t pool)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  delete ctx;
  return 0;
}

// SNAPS

int Rados::snap_create(rados_pool_t pool, const char *snapname) {
  if (!client) return -EINVAL;
  return client->snap_create(pool, snapname);
}

int Rados::snap_remove(rados_pool_t pool, const char *snapname) {
  if (!client) return -EINVAL;
  return client->snap_remove(pool, snapname);
}

int Rados::snap_rollback_object(rados_pool_t pool, const std::string& oid,
				const char *snapname) {
  if (!client) return -EINVAL;
  return client->snap_rollback_object(pool, oid, snapname);
}

int Rados::selfmanaged_snap_create(rados_pool_t pool, uint64_t *snapid)
{
  if (!client) return -EINVAL;
  return client->selfmanaged_snap_create(pool, snapid);
}

int Rados::selfmanaged_snap_remove(rados_pool_t pool,
				   uint64_t snapid)
{
  if (!client) return -EINVAL;
  return client->selfmanaged_snap_remove(pool, snapid);
}

int Rados::selfmanaged_snap_rollback_object(rados_pool_t pool,
                                const std::string& oid,
                                SnapContext& snapc, uint64_t snapid)
{
  ::SnapContext sn;
  if (!client) return -EINVAL;
  sn.seq = snapc.seq;
  sn.snaps.clear();
  vector<snap_t>::iterator iter = snapc.snaps.begin();
  for (; iter != snapc.snaps.end(); ++iter) {
    sn.snaps.push_back(*iter);
  }
  return client->selfmanaged_snap_rollback_object(pool, oid, sn, snapid);
}

void Rados::set_snap(rados_pool_t pool, snap_t seq)
{
  if (!client)
    return;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  ctx->set_snap(seq);
}

int Rados::set_snap_context(rados_pool_t pool, snap_t seq, vector<snap_t>& snaps)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (unsigned i=0; i<snaps.size(); i++)
    snv[i] = snaps[i];
  return ctx->set_snap_context(seq, snv);
}

int Rados::snap_list(rados_pool_t pool, vector<snap_t> *snaps)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return client->snap_list(ctx, snaps);
}

int Rados::snap_lookup(rados_pool_t pool, const char *name, snap_t *snapid)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return client->snap_lookup(ctx, name, snapid);
}

int Rados::snap_get_name(rados_pool_t pool, snap_t snapid, std::string *s)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return client->snap_get_name(ctx, snapid, s);
}
int Rados::snap_get_stamp(rados_pool_t pool, snap_t snapid, time_t *t)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return client->snap_get_stamp(ctx, snapid, t);
}

// AIO
int Rados::aio_read(rados_pool_t pool, const string& oid, off_t off, bufferlist *pbl, size_t len,
		    Rados::AioCompletion *c)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  RadosClient::AioCompletion *pc = (RadosClient::AioCompletion *)c->pc;
  int r = client->aio_read(*ctx, oid, off, pbl, len, pc);
  return r;
}

int Rados::aio_sparse_read(pool_t pool, const std::string& oid, off_t off,
		    std::map<off_t,size_t> *m, bufferlist *data_bl, size_t len,
		    AioCompletion *c)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  RadosClient::AioCompletion *pc = (RadosClient::AioCompletion *)c->pc;
  int r = client->aio_sparse_read(*ctx, oid, off, m, data_bl, len, pc);
  return r;
}

int Rados::aio_write(rados_pool_t pool, const string& oid, off_t off, const bufferlist& bl, size_t len,
		     AioCompletion *c)
{
  if (!client)
    return -EINVAL;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  RadosClient::AioCompletion *pc = (RadosClient::AioCompletion *)c->pc;
  int r = client->aio_write(*ctx, oid, off, bl, len, pc);
  return r;
}

Rados::AioCompletion *Rados::aio_create_completion()
{
  if (!client)
    return NULL;
  RadosClient::AioCompletion *c = client->aio_create_completion();
  return new AioCompletion(c);
}

Rados::AioCompletion *Rados::aio_create_completion(void *cb_arg, callback_t cb_complete, callback_t cb_safe)
{
  if (!client)
    return NULL;
  RadosClient::AioCompletion *c = client->aio_create_completion(cb_arg, cb_complete, cb_safe);
  return new AioCompletion(c);
}

int Rados::AioCompletion::set_complete_callback(void *cb_arg, rados_callback_t cb)
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->set_complete_callback(cb_arg, cb);
}
int Rados::AioCompletion::set_safe_callback(void *cb_arg, rados_callback_t cb)
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->set_safe_callback(cb_arg, cb);
}
int Rados::AioCompletion::wait_for_complete()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->wait_for_complete();
}
int Rados::AioCompletion::wait_for_safe()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->wait_for_safe();
}
bool Rados::AioCompletion::is_complete()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->is_complete();
}
bool Rados::AioCompletion::is_safe()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->is_safe();
}
int Rados::AioCompletion::get_return_value()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->get_return_value();
}
int Rados::AioCompletion::get_version()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  return c->get_version();
}
void Rados::AioCompletion::release()
{
  RadosClient::AioCompletion *c = (RadosClient::AioCompletion *)pc;
  c->release();
}

// watch/notify
int Rados::watch(pool_t pool, const string& o,
                 uint64_t ver, uint64_t *cookie, Rados::WatchCtx *ctx)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->watch(*(RadosClient::PoolCtx *)pool, oid, ver, cookie, ctx);
}

int Rados::unwatch(pool_t pool, const string& o, uint64_t handle)
{
  uint64_t cookie = handle;
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->unwatch(*(RadosClient::PoolCtx *)pool, oid, cookie);
}

int Rados::notify(pool_t pool, const string& o, uint64_t ver)
{
  if (!client)
    return -EINVAL;
  object_t oid(o);
  return client->notify(*(RadosClient::PoolCtx *)pool, oid, ver);
}

void Rados::set_assert_version(pool_t pool, uint64_t ver)
{
  if (!client)
    return;
  client->set_assert_version(*(RadosClient::PoolCtx *)pool, ver);
}

void Rados::set_notify_timeout(pool_t pool, uint32_t timeout)
{
  if (!client)
    return;
  client->set_notify_timeout(*(RadosClient::PoolCtx *)pool, timeout);
}
}

// ---------------------------------------------

static Mutex rados_init_mutex("rados_init");
static int rados_initialized = 0;

extern "C" int rados_init(rados_t *pcluster) 
{
  int ret = 0;

  rados_init_mutex.Lock();
  if (!rados_initialized) {
    // parse environment
    vector<const char*> args;
    env_to_vec(args);

    common_set_defaults(false);
    common_init(args, "librados", STARTUP_FLAG_INIT_KEYS);

    ++rados_initialized;
  }
  rados_init_mutex.Unlock();

  RadosClient *radosp = new RadosClient;
  ret = radosp->init();
  if (ret < 0) {
    delete radosp;
  } else {
    *pcluster = (void *)radosp;
  }
  return ret;
}

extern "C" void rados_release(rados_t cluster)
{
  RadosClient *radosp = (RadosClient *)cluster;
  radosp->shutdown();
  delete radosp;
}

extern "C" void rados_version(int *major, int *minor, int *extra)
{
  if (major)
    *major = LIBRADOS_VER_MAJOR;
  if (minor)
    *minor = LIBRADOS_VER_MINOR;
  if (extra)
    *extra = LIBRADOS_VER_EXTRA;
}


// -- config --
extern "C" int rados_conf_parse_argv(rados_t cluster, int argc, const char **argv)
{
  vector<const char*> args;

  argv_to_vec(argc, argv, args);
  common_init(args, "librados", STARTUP_FLAG_INIT_KEYS);

  return 0;
}


extern "C" int rados_conf_read_file(rados_t cluster, const char *path)
{
  return 0;
}

extern "C" int rados_conf_set(rados_t cluster, const char *option, const char *value)
{
  return 0;
}

extern "C" const char *rados_conf_get(rados_t cluster, const char *option)
{
  return 0;
}



extern "C" int rados_pool_lookup(rados_t cluster, const char *name)
{
  RadosClient *radosp = (RadosClient *)cluster;
  return radosp->lookup_pool(name);
}

extern "C" int rados_pool_open(rados_t cluster, const char *name, rados_pool_t *pool)
{
  RadosClient *radosp = (RadosClient *)cluster;
  int poolid = radosp->lookup_pool(name);
  if (poolid >= 0) {
    RadosClient::PoolCtx *ctx = new RadosClient::PoolCtx(radosp, poolid, name, CEPH_NOSNAP);
    if (!ctx)
      return -ENOMEM;
    *pool = ctx;
    return 0;
  }
  return poolid;
}

extern "C" int rados_pool_close(rados_pool_t pool)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  delete ctx;
  return 0;
}

extern "C" int rados_pool_stat(rados_pool_t pool, struct rados_pool_stat_t *stats)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  list<string> ls;
  ls.push_back(ctx->name);
  map<string, ::pool_stat_t> rawresult;

  int err = ctx->client->get_pool_stats(ls, rawresult);
  if (err)
    return err;

  ::pool_stat_t& r = rawresult[ctx->name];
  stats->num_kb = r.num_kb;
  stats->num_bytes = r.num_bytes;
  stats->num_objects = r.num_objects;
  stats->num_object_clones = r.num_object_clones;
  stats->num_object_copies = r.num_object_copies;
  stats->num_objects_missing_on_primary = r.num_objects_missing_on_primary;
  stats->num_objects_unfound = r.num_objects_unfound;
  stats->num_objects_degraded = r.num_objects_degraded;
  stats->num_rd = r.num_rd;
  stats->num_rd_kb = r.num_rd_kb;
  stats->num_wr = r.num_wr;
  stats->num_wr_kb = r.num_wr_kb;
  return 0;
}


extern "C" void rados_snap_set_read(rados_pool_t pool, rados_snap_t seq)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  ctx->set_snap((snapid_t)seq);
}

extern "C" int rados_snap_set_write_context(rados_pool_t pool, rados_snap_t seq,
				       rados_snap_t *snaps, int num_snaps)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  vector<snapid_t> snv;
  snv.resize(num_snaps);
  for (int i=0; i<num_snaps; i++)
    snv[i] = (snapid_t)snaps[i];
  return ctx->set_snap_context((snapid_t)seq, snv);
}

extern "C" int rados_write(rados_pool_t pool, const char *o, off_t off, const char *buf, size_t len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist bl;
  bl.append(buf, len);
  return ctx->client->write(*ctx, oid, off, bl, len);
}

extern "C" int rados_write_full(rados_pool_t pool, const char *o, off_t off, const char *buf, size_t len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist bl;
  bl.append(buf, len);
  return ctx->client->write_full(*ctx, oid, bl);
}

extern "C" int rados_trunc(rados_pool_t pool, const char *o, size_t size)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->trunc(*ctx, oid, size);
}

extern "C" int rados_remove(rados_pool_t pool, const char *o)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->remove(*ctx, oid);
}

extern "C" int rados_read(rados_pool_t pool, const char *o, off_t off, char *buf, size_t len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  int ret;
  object_t oid(o);

  bufferlist bl;
  bufferptr bp = buffer::create_static(len, buf);
  bl.push_back(bp);
  
  ret = ctx->client->read(*ctx, oid, off, bl, len);
  if (ret >= 0) {
    if (bl.length() > len)
      return -ERANGE;
    if (bl.c_str() != buf)
      bl.copy(0, bl.length(), buf);
    ret = bl.length();    // hrm :/
  }

  return ret;
}

extern "C" uint64_t rados_get_last_version(rados_pool_t pool)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  eversion_t ver = ctx->client->last_version(*ctx);
  return ver.version;
}

extern "C" int rados_pool_create(rados_t cluster, const char *name)
{
  RadosClient *radosp = (RadosClient *)cluster;
  string sname(name);
  return radosp->create_pool(sname);
}

extern "C" int rados_pool_create_with_auid(rados_t cluster, const char *name, uint64_t auid)
{
  RadosClient *radosp = (RadosClient *)cluster;
  string sname(name);
  return radosp->create_pool(sname, auid);
}

extern "C" int rados_pool_create_with_crush_rule(rados_t cluster, const char *name,
						 __u8 crush_rule)
{
  RadosClient *radosp = (RadosClient *)cluster;
  string sname(name);
  return radosp->create_pool(sname, 0, crush_rule);
}

extern "C" int rados_pool_create_with_all(rados_t cluster, const char *name, uint64_t auid,
					  __u8 crush_rule)
{
  RadosClient *radosp = (RadosClient *)cluster;
  string sname(name);
  return radosp->create_pool(sname, auid, crush_rule);
}

extern "C" int rados_pool_delete(rados_pool_t pool)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->delete_pool(ctx);
}

extern "C" int rados_pool_change_auid(rados_pool_t pool, uint64_t auid)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->change_pool_auid(ctx, auid);
}

// snaps

extern "C" int rados_snap_create(rados_pool_t pool, const char *snapname)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->snap_create(ctx, snapname);
}

extern "C" int rados_snap_remove(rados_pool_t pool, const char *snapname)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->snap_remove(ctx, snapname);
}

extern "C" int rados_snap_rollback_object(rados_pool_t pool,
					  const char *oid,
					  const char *snapname)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->snap_rollback_object(ctx, oid, snapname);
}

extern "C" int rados_selfmanaged_snap_create(rados_pool_t pool,
					     uint64_t *snapid)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->selfmanaged_snap_create(ctx, snapid);
}

extern "C" int rados_selfmanaged_snap_remove(rados_pool_t pool,
					     uint64_t snapid)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->selfmanaged_snap_remove(ctx, snapid);
}

extern "C" int rados_snap_list(rados_pool_t pool, rados_snap_t *snaps, int maxlen)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  vector<uint64_t> snapvec;
  int r = ctx->client->snap_list(ctx, &snapvec);
  if (r < 0)
    return r;
  if ((int)snapvec.size() <= maxlen) {
    for (unsigned i=0; i<snapvec.size(); i++)
      snaps[i] = snapvec[i];
    return snapvec.size();
  }
  return -ERANGE;
}

extern "C" int rados_snap_lookup(rados_pool_t pool, const char *name, rados_snap_t *id)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  return ctx->client->snap_lookup(ctx, name, (uint64_t *)id);
}

extern "C" int rados_snap_get_name(rados_pool_t pool, rados_snap_t id, char *name, int maxlen)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  std::string sname;
  int r = ctx->client->snap_get_name(ctx, id, &sname);
  if (r < 0)
    return r;
  if ((int)sname.length() >= maxlen)
    return -ERANGE;
  strncpy(name, sname.c_str(), maxlen);
  return 0;
}


extern "C" int rados_getxattr(rados_pool_t pool, const char *o, const char *name, char *buf, size_t len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  int ret;
  object_t oid(o);
  bufferlist bl;
  ret = ctx->client->getxattr(*ctx, oid, name, bl);
  if (ret >= 0) {
    if (bl.length() > len)
      return -ERANGE;
    bl.copy(0, bl.length(), buf);
    ret = bl.length();
  }

  return ret;
}

extern "C" int rados_setxattr(rados_pool_t pool, const char *o, const char *name, const char *buf, size_t len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist bl;
  bl.append(buf, len);
  return ctx->client->setxattr(*ctx, oid, name, bl);
}

extern "C" int rados_rmxattr(rados_pool_t pool, const char *o, const char *name)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->rmxattr(*ctx, oid, name);
}

extern "C" int rados_stat(rados_pool_t pool, const char *o, uint64_t *psize, time_t *pmtime)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->stat(*ctx, oid, psize, pmtime);
}

extern "C" int rados_tmap_update(rados_pool_t pool, const char *o, const char *cmdbuf, size_t cmdbuflen)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist cmdbl;
  cmdbl.append(cmdbuf, cmdbuflen);
  return ctx->client->tmap_update(*ctx, oid, cmdbl);
}

extern "C" int rados_exec(rados_pool_t pool, const char *o, const char *cls, const char *method,
                         const char *inbuf, size_t in_len, char *buf, size_t out_len)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist inbl, outbl;
  int ret;
  inbl.append(inbuf, in_len);
  ret = ctx->client->exec(*ctx, oid, cls, method, inbl, outbl);
  if (ret >= 0) {
    if (outbl.length()) {
      if (outbl.length() > out_len)
	return -ERANGE;
      outbl.copy(0, outbl.length(), buf);
      ret = outbl.length();   // hrm :/
    }
  }
  return ret;
}

/* list objects */

extern "C" int rados_objects_list_open(rados_pool_t pool, rados_list_ctx_t *listh)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  Objecter::ListContext *h = new Objecter::ListContext;
  h->pool_id = ctx->poolid;
  h->pool_snap_seq = ctx->snap_seq;
  *listh = (void *)new RadosClient::ListCtx(ctx, h);
  return 0;
}

extern "C" void rados_objects_list_close(rados_list_ctx_t h)
{
  RadosClient::ListCtx *lh = (RadosClient::ListCtx *)h;
  delete lh;
}

extern "C" int rados_objects_list_next(rados_list_ctx_t listctx, const char **entry)
{
  RadosClient::ListCtx *lh = (RadosClient::ListCtx *)listctx;
  Objecter::ListContext *h = lh->lc;
  int ret;

  // if the list is non-empty, this method has been called before
  if (!h->list.empty())
    // so let's kill the previously-returned object
    h->list.pop_front();
  
  if (h->list.empty()) {
    ret = lh->ctx->client->list(lh->lc, RADOS_LIST_MAX_ENTRIES);
    if (!h->list.size())
      return -ENOENT;
  }

  *entry = h->list.front().name.c_str();
  return 0;
}



// -------------------------
// aio

extern "C" int rados_aio_create_completion(void *cb_arg, rados_callback_t cb_complete, rados_callback_t cb_safe,
					   rados_completion_t *pc)
{
  *pc = RadosClient::aio_create_completion(cb_arg, cb_complete, cb_safe);
  return 0;
}

extern "C" int rados_aio_wait_for_complete(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->wait_for_complete();
}

extern "C" int rados_aio_wait_for_safe(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->wait_for_safe();
}

extern "C" int rados_aio_is_complete(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->is_complete();
}

extern "C" int rados_aio_is_safe(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->is_safe();
}

extern "C" int rados_aio_get_return_value(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->get_return_value();
}

extern "C" uint64_t rados_aio_get_version(rados_completion_t c)
{
  return ((RadosClient::AioCompletion *)c)->get_version();
}

extern "C" void rados_aio_release(rados_completion_t c)
{
  ((RadosClient::AioCompletion *)c)->put();
}

extern "C" int rados_aio_read(rados_pool_t pool, const char *o,
			       off_t off, char *buf, size_t len,
			       rados_completion_t completion)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->aio_read(*ctx, oid, off, buf, len, (RadosClient::AioCompletion*)completion);
}

extern "C" int rados_aio_write(rados_pool_t pool, const char *o,
			       off_t off, const char *buf, size_t len,
			       rados_completion_t completion)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist bl;
  bl.append(buf, len);
  return ctx->client->aio_write(*ctx, oid, off, bl, len, (RadosClient::AioCompletion*)completion);
}

extern "C" int rados_aio_write_full(rados_pool_t pool, const char *o,
			       off_t off, const char *buf, size_t len,
			       rados_completion_t completion)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  bufferlist bl;
  bl.append(buf, len);
  return ctx->client->aio_write_full(*ctx, oid, bl, (RadosClient::AioCompletion*)completion);
}

struct C_WatchCB : public librados::Rados::WatchCtx {
  rados_watchcb_t wcb;
  void *arg;
  C_WatchCB(rados_watchcb_t _wcb, void *_arg) : wcb(_wcb), arg(_arg) {}
  void notify(uint8_t opcode, uint64_t ver) {
    wcb(opcode, ver, arg);
  }
};

int rados_watch(rados_pool_t pool, const char *o, uint64_t ver, uint64_t *handle,
                rados_watchcb_t watchcb, void *arg)
{
  uint64_t *cookie = handle;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  C_WatchCB *wc = new C_WatchCB(watchcb, arg);
  return ctx->client->watch(*ctx, oid, ver, cookie, wc);
}

int rados_unwatch(rados_pool_t pool, const char *o, uint64_t handle)
{
  uint64_t cookie = handle;
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->unwatch(*ctx, oid, cookie);
}

int rados_notify(rados_pool_t pool, const char *o, uint64_t ver)
{
  RadosClient::PoolCtx *ctx = (RadosClient::PoolCtx *)pool;
  object_t oid(o);
  return ctx->client->notify(*ctx, oid, ver);
}
