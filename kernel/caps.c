#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "ceph_debug.h"

int ceph_debug_caps = -1;
#define DOUT_MASK DOUT_MASK_CAPS
#define DOUT_VAR ceph_debug_caps
#define DOUT_PREFIX "caps: "
#include "super.h"

#include "decode.h"
#include "messenger.h"


/*
 * Find ceph_cap for given mds, if any.
 *
 * Called with i_lock held.
 */
static struct ceph_cap *__get_cap_for_mds(struct inode *inode, int mds)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_cap *cap;
	struct rb_node *n = ci->i_caps.rb_node;

	while (n) {
		cap = rb_entry(n, struct ceph_cap, ci_node);
		if (mds < cap->mds)
			n = n->rb_left;
		else if (mds > cap->mds)
			n = n->rb_right;
		else
			return cap;
	}
	return NULL;
}

/*
 * Return id of any MDS with a cap, preferably WR|WRBUFFER|EXCL, else
 * -1.
 */
static int __ceph_get_cap_mds(struct ceph_inode_info *ci, u32 *mseq)
{
	struct ceph_cap *cap;
	int mds = -1;
	struct rb_node *p;

	/* prefer mds with WR|WRBUFFER|EXCL caps */
	for (p = rb_first(&ci->i_caps); p; p = rb_next(p)) {
		cap = rb_entry(p, struct ceph_cap, ci_node);
		mds = cap->mds;
		if (mseq)
			*mseq = cap->mseq;
		if (cap->issued & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER|CEPH_CAP_EXCL))
			break;
	}
	return mds;
}

int ceph_get_cap_mds(struct inode *inode)
{
	int mds;
	spin_lock(&inode->i_lock);
	mds = __ceph_get_cap_mds(ceph_inode(inode), NULL);
	spin_unlock(&inode->i_lock);
	return mds;
}

/*
 * Called under i_lock.
 */
static void __insert_cap_node(struct ceph_inode_info *ci,
			      struct ceph_cap *new)
{
	struct rb_node **p = &ci->i_caps.rb_node;
	struct rb_node *parent = NULL;
	struct ceph_cap *cap = NULL;

	while (*p) {
		parent = *p;
		cap = rb_entry(parent, struct ceph_cap, ci_node);
		if (new->mds < cap->mds)
			p = &(*p)->rb_left;
		else if (new->mds > cap->mds)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	rb_link_node(&new->ci_node, parent, p);
	rb_insert_color(&new->ci_node, &ci->i_caps);
}

/*
 * Add a capability under the given MDS session, after processing
 * the snapblob (to update the snap realm hierarchy).
 *
 * Bump i_count when adding it's first cap.
 *
 * Caller should hold session snap_rwsem, s_mutex.
 *
 * @fmode can be negative, in which case it is ignored.
 */
int ceph_add_cap(struct inode *inode,
		 struct ceph_mds_session *session,
		 int fmode, unsigned issued,
		 unsigned seq, unsigned mseq,
		 void *snapblob, int snapblob_len,
		 struct ceph_cap *new_cap)
{
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_cap *cap;
	struct ceph_snap_realm *realm;
	int mds = session->s_mds;
	int is_first = 0;

	realm = ceph_update_snap_trace(mdsc, snapblob, snapblob+snapblob_len,
				       false /* not a deletion */);

	dout(10, "add_cap on %p mds%d cap %d seq %d\n", inode,
	     session->s_mds, issued, seq);
retry:
	spin_lock(&inode->i_lock);
	cap = __get_cap_for_mds(inode, mds);
	if (!cap) {
		if (new_cap) {
			cap = new_cap;
			new_cap = NULL;
		} else {
			spin_unlock(&inode->i_lock);
			new_cap = kmalloc(sizeof(*cap), GFP_NOFS);
			if (new_cap == NULL) {
				ceph_put_snap_realm(mdsc, realm);
				return -ENOMEM;
			}
			goto retry;
		}

		cap->issued = cap->implemented = 0;
		cap->mds = mds;

		is_first = RB_EMPTY_ROOT(&ci->i_caps);  /* grab inode later */
		cap->ci = ci;
		__insert_cap_node(ci, cap);

		/* add to session cap list */
		cap->session = session;
		list_add(&cap->session_caps, &session->s_caps);
		session->s_nr_caps++;

		/* clear out old exporting info?  (i.e. on cap import) */
		if (ci->i_cap_exporting_mds == mds) {
			ci->i_cap_exporting_issued = 0;
			ci->i_cap_exporting_mseq = 0;
			ci->i_cap_exporting_mds = -1;
		}
	}
	if (!ci->i_snap_realm) {
		ci->i_snap_realm = realm;
		list_add(&ci->i_snap_realm_item, &realm->inodes_with_caps);
	} else {
		ceph_put_snap_realm(mdsc, realm);
	}

	dout(10, "add_cap inode %p (%llx.%llx) cap %xh now %xh seq %d mds%d\n",
	     inode, ceph_vinop(inode), issued, issued|cap->issued, seq, mds);
	cap->issued |= issued;
	cap->implemented |= issued;
	cap->seq = seq;
	cap->mseq = mseq;
	cap->gen = session->s_cap_gen;
	if (fmode >= 0)
		__ceph_get_fmode(ci, fmode);
	spin_unlock(&inode->i_lock);
	if (is_first)
		igrab(inode);
	if (new_cap)
		kfree(new_cap);
	return 0;
}

/*
 * Return set of valid cap bits issued to us.  Note that caps time
 * out, and may be invalidated in bulk if the client session times out
 * and session->s_cap_gen is bumped.
 */
int __ceph_caps_issued(struct ceph_inode_info *ci, int *implemented)
{
	int have = ci->i_snap_caps;
	struct ceph_cap *cap;
	u32 gen;
	unsigned long ttl;
	struct rb_node *p;

	for (p = rb_first(&ci->i_caps); p; p = rb_next(p)) {
		cap = rb_entry(p, struct ceph_cap, ci_node);

		spin_lock(&cap->session->s_cap_lock);
		gen = cap->session->s_cap_gen;
		ttl = cap->session->s_cap_ttl;
		spin_unlock(&cap->session->s_cap_lock);

		if (cap->gen < gen || time_after_eq(jiffies, ttl)) {
			dout(30, "__ceph_caps_issued %p cap %p issued %d "
			     "but STALE (gen %u vs %u)\n", &ci->vfs_inode,
			     cap, cap->issued, cap->gen, gen);
			continue;
		}
		dout(30, "__ceph_caps_issued %p cap %p issued %d\n",
		     &ci->vfs_inode, cap, cap->issued);
		have |= cap->issued;
		if (implemented)
			*implemented |= cap->implemented;
	}
	return have;
}

/*
 * caller should hold i_lock, snap_rwsem, and session s_mutex.
 * returns true if this is the last cap.  if so, caller should iput.
 */
static int __ceph_remove_cap(struct ceph_cap *cap)
{
	struct ceph_mds_session *session = cap->session;
	struct ceph_inode_info *ci = cap->ci;
	struct ceph_mds_client *mdsc = &ceph_client(ci->vfs_inode.i_sb)->mdsc;

	dout(20, "__ceph_remove_cap %p from %p\n", cap, &ci->vfs_inode);

	/* remove from session list */
	list_del_init(&cap->session_caps);
	session->s_nr_caps--;

	/* remove from inode list */
	rb_erase(&cap->ci_node, &ci->i_caps);
	cap->session = NULL;

	kfree(cap);

	if (RB_EMPTY_ROOT(&ci->i_caps)) {
		list_del_init(&ci->i_snap_realm_item);
		ceph_put_snap_realm(mdsc, ci->i_snap_realm);
		ci->i_snap_realm = NULL;
		return 1;
	}
	return 0;
}

/*
 * caller should hold snap_rwsem and session s_mutex.
 */
void ceph_remove_cap(struct ceph_cap *cap)
{
	struct inode *inode = &cap->ci->vfs_inode;
	int was_last;

	spin_lock(&inode->i_lock);
	was_last = __ceph_remove_cap(cap);
	spin_unlock(&inode->i_lock);
	if (was_last)
		iput(inode);
}

/*
 *
 * (Re)queue cap at the end of the delayed cap release list.
 *
 * Caller holds i_lock
 *    -> we take mdsc->cap_delay_lock
 */
static void __cap_delay_requeue(struct ceph_mds_client *mdsc,
			      struct ceph_inode_info *ci)
{
	ci->i_hold_caps_until = round_jiffies(jiffies + HZ * 5);
	dout(10, "__cap_delay_requeue %p at %lu\n", &ci->vfs_inode,
	     ci->i_hold_caps_until);
	spin_lock(&mdsc->cap_delay_lock);
	if (!mdsc->stopping) {
		if (list_empty(&ci->i_cap_delay_list))
			igrab(&ci->vfs_inode);
		else
			list_del_init(&ci->i_cap_delay_list);
		list_add_tail(&ci->i_cap_delay_list, &mdsc->cap_delay_list);
	}
	spin_unlock(&mdsc->cap_delay_lock);
}

/*
 * Cancel delayed work on cap.
 * caller hold s_mutex, snap_rwsem.
 */
static void __cap_delay_cancel(struct ceph_mds_client *mdsc,
			       struct ceph_inode_info *ci)
{
	dout(10, "__cap_delay_cancel %p\n", &ci->vfs_inode);
	if (list_empty(&ci->i_cap_delay_list))
		return;
	spin_lock(&mdsc->cap_delay_lock);
	list_del_init(&ci->i_cap_delay_list);
	spin_unlock(&mdsc->cap_delay_lock);
	iput(&ci->vfs_inode);
}

/*
 * Build and send a cap message to the given MDS.
 *
 * Caller should be holding s_mutex.
 */
static void send_cap_msg(struct ceph_mds_client *mdsc, u64 ino, int op,
			 int caps, int wanted, u64 seq, u64 mseq,
			 u64 size, u64 max_size,
			 struct timespec *mtime, struct timespec *atime,
			 u64 time_warp_seq, u64 follows, int mds)
{
	struct ceph_mds_caps *fc;
	struct ceph_msg *msg;

	dout(10, "send_cap_msg %s %llx caps %d wanted %d seq %llu/%llu"
	     " follows %lld size %llu\n", ceph_cap_op_name(op), ino,
	     caps, wanted, seq, mseq, follows, size);

	msg = ceph_msg_new(CEPH_MSG_CLIENT_CAPS, sizeof(*fc), 0, 0, NULL);
	if (IS_ERR(msg))
		return;

	fc = msg->front.iov_base;

	memset(fc, 0, sizeof(*fc));

	fc->op = cpu_to_le32(op);
	fc->seq = cpu_to_le32(seq);
	fc->migrate_seq = cpu_to_le32(mseq);
	fc->caps = cpu_to_le32(caps);
	fc->wanted = cpu_to_le32(wanted);
	fc->ino = cpu_to_le64(ino);
	fc->size = cpu_to_le64(size);
	fc->max_size = cpu_to_le64(max_size);
	fc->snap_follows = cpu_to_le64(follows);
	if (mtime)
		ceph_encode_timespec(&fc->mtime, mtime);
	if (atime)
		ceph_encode_timespec(&fc->atime, atime);
	fc->time_warp_seq = cpu_to_le64(time_warp_seq);

	ceph_send_msg_mds(mdsc, msg, mds);
}

/*
 *
 * Send a cap msg on the given inode.  Make note of max_size
 * reported/requested from mds, revoked caps that have now been
 * implemented.
 *
 * Also, try to invalidate page cache if we are dropping RDCACHE.
 * Note that this will leave behind any locked pages... FIXME!
 *
 * called with i_lock, then drops it.
 * caller should hold snap_rwsem, s_mutex.
 */
static void __send_cap(struct ceph_mds_client *mdsc,
		struct ceph_mds_session *session,
		struct ceph_cap *cap,
		int used, int wanted) __releases(cap->ci->vfs_inode->i_lock)
{
	struct ceph_inode_info *ci = cap->ci;
	struct inode *inode = &ci->vfs_inode;
	int revoking = cap->implemented & ~cap->issued;
	int dropping = cap->issued & ~wanted;
	int keep;
	u64 seq, mseq, time_warp_seq, follows;
	u64 size, max_size;
	struct timespec mtime, atime;
	int wake = 0;
	int op = CEPH_CAP_OP_ACK;

	if (wanted == 0)
		op = CEPH_CAP_OP_RELEASE;

	dout(10, "__send_cap cap %p session %p %d -> %d\n", cap, cap->session,
	     cap->issued, cap->issued & wanted);
	cap->issued &= wanted;  /* drop bits we don't want */

	if (revoking && (revoking && used) == 0) {
		cap->implemented = cap->issued;
		/*
		 * Wake up any waiters on wanted -> needed transition.
		 * This is due to the weird transition from buffered
		 * to sync IO... we need to flush dirty pages _before_
		 * allowing sync writes to avoid reordering.
		 */
		wake = 1;
	}

	keep = cap->issued;
	seq = cap->seq;
	mseq = cap->mseq;
	size = inode->i_size;
	ci->i_reported_size = size;
	max_size = ci->i_wanted_max_size;
	ci->i_requested_max_size = max_size;
	mtime = inode->i_mtime;
	atime = inode->i_atime;
	time_warp_seq = ci->i_time_warp_seq;
	follows = ci->i_snap_realm->cached_context->seq;
	spin_unlock(&inode->i_lock);

	if (dropping & CEPH_CAP_RDCACHE) {
		/* invalidate what we can */
		dout(20, "invalidating pages on %p\n", inode);
		invalidate_mapping_pages(&inode->i_data, 0, -1);
	}

	send_cap_msg(mdsc, ceph_vino(inode).ino,
		     op, keep, wanted, seq, mseq,
		     size, max_size, &mtime, &atime, time_warp_seq,
		     follows, session->s_mds);

	if (wake)
		wake_up(&ci->i_cap_wq);
}


/*
 * When a snapshot is taken, clients accumulate "dirty" data on inodes
 * with capabilities in ceph_cap_snaps to describe the file state at
 * the time the snapshot was taken.  This must be flushed
 * asynchronously back to the MDS once sync writes complete and dirty
 * data is written out.
 *
 * Called under i_lock.  Takes s_mutex as needed.
 */
void __ceph_flush_snaps(struct ceph_inode_info *ci)
{
	struct inode *inode = &ci->vfs_inode;
	int mds;
	struct list_head *p;
	struct ceph_cap_snap *capsnap;
	int issued;
	u64 size;
	struct timespec mtime, atime, ctime;
	u64 time_warp_seq;
	u32 mseq;
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	struct ceph_mds_session *session = NULL; /* if session != NULL, we hold
						    session->s_mutex */
	u64 follows = 0;  /* keep track of how far we've gotten through the
			     i_cap_snaps list, and skip these entries next time
			     around to avoid an infinite loop */

	dout(10, "__flush_snaps %p\n", inode);
retry:
	list_for_each(p, &ci->i_cap_snaps) {
		capsnap = list_entry(p, struct ceph_cap_snap, ci_item);

		/* avoid an infiniute loop after retry */
		if (capsnap->follows <= follows)
			continue;
		/*
		 * we need to wait for sync writes to complete and for dirty
		 * pages to be written out.
		 */
		if (capsnap->dirty || capsnap->writing)
			continue;

		/* pick mds, take s_mutex */
		mds = __ceph_get_cap_mds(ci, &mseq);
		if (session && session->s_mds != mds) {
			dout(30, "oops, wrong session %p mutex\n", session);
			mutex_unlock(&session->s_mutex);
			ceph_put_mds_session(session);
			session = NULL;
		}
		if (!session) {
			spin_unlock(&inode->i_lock);
			mutex_lock(&mdsc->mutex);
			session = __ceph_get_mds_session(mdsc, mds);
			mutex_unlock(&mdsc->mutex);
			if (session) {
				dout(10, "inverting session/ino locks on %p\n",
				     session);
				mutex_lock(&session->s_mutex);
			}
			/*
			 * if session == NULL, we raced against a cap
			 * deletion.  retry, and we'll get a better
			 * @mds value next time.
			 */
			spin_lock(&inode->i_lock);
			goto retry;
		}

		follows = capsnap->follows;
		size = capsnap->size;
		atime = capsnap->atime;
		mtime = capsnap->mtime;
		ctime = capsnap->ctime;
		time_warp_seq = capsnap->time_warp_seq;
		issued = capsnap->issued;
		spin_unlock(&inode->i_lock);

		dout(10, "flush_snaps %p cap_snap %p follows %lld size %llu\n",
		     inode, capsnap, follows, size);
		send_cap_msg(mdsc, ceph_vino(inode).ino,
			     CEPH_CAP_OP_FLUSHSNAP, issued, 0, 0, mseq,
			     size, 0,
			     &mtime, &atime, time_warp_seq,
			     follows, mds);

		spin_lock(&inode->i_lock);
		goto retry;
	}

	if (session) {
		mutex_unlock(&session->s_mutex);
		ceph_put_mds_session(session);
	}
}

void ceph_flush_snaps(struct ceph_inode_info *ci)
{
	struct inode *inode = &ci->vfs_inode;

	spin_lock(&inode->i_lock);
	__ceph_flush_snaps(ci);
	spin_unlock(&inode->i_lock);
}


/*
 * Swiss army knife function to examine currently used, wanted versus
 * held caps.  Release, flush, ack revoked caps to mds as appropriate.
 *
 * @is_delayed indicates caller is delayed work and we should not
 * delay further.
 */
void ceph_check_caps(struct ceph_inode_info *ci, int is_delayed)
{
	struct ceph_client *client = ceph_inode_to_client(&ci->vfs_inode);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct inode *inode = &ci->vfs_inode;
	struct ceph_cap *cap;
	int wanted, used;
	struct ceph_mds_session *session = NULL;  /* if non-NULL, i hold s_mutex */
	int took_snap_rwsem = 0;             /* true if mdsc->snap_rwsem held */
	int revoking;
	int mds = -1;   /* keep track of how far we've gone through i_caps list
			   to avoid an infinite loop on retry */
	struct rb_node *p;

	spin_lock(&inode->i_lock);

	/* flush snaps first time around only */
	if (!list_empty(&ci->i_cap_snaps))
		__ceph_flush_snaps(ci);
	goto first;
retry:
	spin_lock(&inode->i_lock);
first:
	wanted = __ceph_caps_wanted(ci);
	used = __ceph_caps_used(ci);
	dout(10, "check_caps %p wanted %d used %d issued %d\n",
	     inode, wanted, used, __ceph_caps_issued(ci, NULL));

	if (!is_delayed)
		__cap_delay_requeue(mdsc, ci);
#if 0
	/* delay cap release for a bit? */
	if (time_after(jiffies, ci->i_hold_caps_until) &&
	    ci->rdcache_pending) {
		dout(30, "delaying cap release\n");
		__send_cap(mdsc, session, cap, used, wanted);
	}
#endif

	for (p = rb_first(&ci->i_caps); p; p = rb_next(p)) {
		cap = rb_entry(p, struct ceph_cap, ci_node);

		/* avoid looping forever */
		if (mds >= cap->mds)
			continue;

		/* NOTE: no side-effects allowed, until we take s_mutex */

		revoking = cap->implemented & ~cap->issued;
		if (revoking)
			dout(10, "mds%d revoking %d\n", cap->mds, revoking);

		/* request larger max_size from MDS? */
		if (ci->i_wanted_max_size > ci->i_max_size &&
		    ci->i_wanted_max_size > ci->i_requested_max_size)
			goto ack;

		/* approaching file_max? */
		if ((cap->issued & CEPH_CAP_WR) &&
		    (inode->i_size << 1) >= ci->i_max_size &&
		    (ci->i_reported_size << 1) < ci->i_max_size) {
			dout(10, "i_size approaching max_size\n");
			goto ack;
		}

		/* completed revocation? */
		if (revoking && (revoking & used) == 0) {
			dout(10, "completed revocation of %d\n",
			     cap->implemented & ~cap->issued);
			goto ack;
		}

		if ((cap->issued & ~wanted) == 0)
			continue;     /* nothing extra, all good */

		/* delay cap release for a bit? */
		if (time_before(jiffies, ci->i_hold_caps_until)) {
			dout(30, "delaying cap release\n");
			continue;
		}

ack:
		/* take snap_rwsem before session mutex */
		if (!took_snap_rwsem) {
			if (down_read_trylock(&mdsc->snap_rwsem) == 0) {
				dout(10, "inverting snap/in locks on %p\n",
				     inode);
				spin_unlock(&inode->i_lock);
				down_read(&mdsc->snap_rwsem);
				took_snap_rwsem = 1;
				goto retry;
			}
			took_snap_rwsem = 1;
		}
		if (session && session != cap->session) {
			dout(30, "oops, wrong session %p mutex\n", session);
			mutex_unlock(&session->s_mutex);
			session = NULL;
		}
		if (!session) {
			session = cap->session;
			if (mutex_trylock(&session->s_mutex) == 0) {
				dout(10, "inverting session/ino locks on %p\n",
				     session);
				spin_unlock(&inode->i_lock);
				mutex_lock(&session->s_mutex);
				goto retry;
			}
		}

		mds = cap->mds;  /* remember mds, so we don't repeat */

		/* __send_cap drops i_lock */
		__send_cap(mdsc, session, cap, used, wanted);

		goto retry; /* retake i_lock and restart our cap scan. */
	}

	/* okay */
	spin_unlock(&inode->i_lock);

	if (session)
		mutex_unlock(&session->s_mutex);
	if (took_snap_rwsem)
		up_read(&mdsc->snap_rwsem);
}


/*
 * Track references to capabilities we hold, so that we don't release
 * them to the MDS prematurely.
 *
 * Protected by i_lock.
 */
static void __take_cap_refs(struct ceph_inode_info *ci, int got)
{
	if (got & CEPH_CAP_RD)
		ci->i_rd_ref++;
	if (got & CEPH_CAP_RDCACHE)
		ci->i_rdcache_ref++;
	if (got & CEPH_CAP_WR)
		ci->i_wr_ref++;
	if (got & CEPH_CAP_WRBUFFER) {
		ci->i_wrbuffer_ref++;
		dout(30, "__take_cap_refs %p wrbuffer %d -> %d (?)\n",
		     &ci->vfs_inode, ci->i_wrbuffer_ref-1, ci->i_wrbuffer_ref);
	}
}

/*
 * Try to grab cap references.  Specify those refs we @want, and the
 * minimal set we @need.  Also include the larger offset we are writing
 * to (when applicable), and check against max_size here as well.
 * Note that caller is responsible for ensuring max_size increases are
 * requested from the MDS.
 */
int ceph_get_cap_refs(struct ceph_inode_info *ci, int need, int want, int *got,
		      loff_t endoff)
{
	struct inode *inode = &ci->vfs_inode;
	int ret = 0;
	int have, implemented;

	dout(30, "get_cap_refs %p need %d want %d\n", inode, need, want);
	spin_lock(&inode->i_lock);
	if (need & CEPH_CAP_WR) {
		if (endoff >= 0 && endoff > (loff_t)ci->i_max_size) {
			dout(20, "get_cap_refs %p endoff %llu > maxsize %llu\n",
			     inode, endoff, ci->i_max_size);
			goto sorry;
		}
		/*
		 * If a sync write is in progress, we must wait, so that we
		 * can get a final snapshot value for size+mtime.
		 */
		if (__ceph_have_pending_cap_snap(ci)) {
			dout(20, "get_cap_refs %p cap_snap_pending\n", inode);
			goto sorry;
		}
	}
	have = __ceph_caps_issued(ci, &implemented);
	/* HACK: force sync writes...
	have &= ~CEPH_CAP_WRBUFFER;
	implemented &= ~CEPH_CAP_WRBUFFER;
	*/
	if ((have & need) == need) {
		/*
		 * Look at (implemented & ~have & not) so that we keep waiting
		 * on transition from wanted -> needed caps.  This is needed
		 * for WRBUFFER|WR -> WR to avoid a new WR sync write from
		 * going before a prior buffered writeback happens.
		 */
		int not = want & ~(have & need);
		int revoking = implemented & ~have;
		dout(30, "get_cap_refs %p have %d but not %d (revoking %d)\n",
		     inode, have, not, revoking);
		if ((revoking & not) == 0) {
			*got = need | (have & want);
			__take_cap_refs(ci, *got);
			ret = 1;
		}
	} else {
		dout(30, "get_cap_refs %p have %d needed %d\n", inode,
		     have, need);
	}
sorry:
	spin_unlock(&inode->i_lock);
	dout(30, "get_cap_refs %p ret %d got %d\n", inode,
	     ret, *got);
	return ret;
}

/*
 * Release cap refs.
 *
 * If we released the last ref on any given cap, call ceph_check_caps
 * to release (or schedule a release).
 *
 * If we are releasing a WR cap (from a sync write), finalize any affected
 * cap_snap, and wake up any waiters.
 */
void ceph_put_cap_refs(struct ceph_inode_info *ci, int had)
{
	struct inode *inode = &ci->vfs_inode;
	int last = 0, flushsnaps = 0, wake = 0;
	struct ceph_cap_snap *capsnap;

	spin_lock(&inode->i_lock);
	if (had & CEPH_CAP_RD)
		if (--ci->i_rd_ref == 0)
			last++;
	if (had & CEPH_CAP_RDCACHE)
		if (--ci->i_rdcache_ref == 0)
			last++;
	if (had & CEPH_CAP_WRBUFFER) {
		if (--ci->i_wrbuffer_ref == 0)
			last++;
		dout(30, "put_cap_refs %p wrbuffer %d -> %d (?)\n",
		     inode, ci->i_wrbuffer_ref+1, ci->i_wrbuffer_ref);
	}
	if (had & CEPH_CAP_WR)
		if (--ci->i_wr_ref == 0) {
			last++;
			if (!list_empty(&ci->i_cap_snaps)) {
				capsnap = list_entry(ci->i_cap_snaps.next,
						     struct ceph_cap_snap,
						     ci_item);
				if (capsnap->writing) {
					capsnap->writing = 0;
					flushsnaps =
						__ceph_finish_cap_snap(ci,
								       capsnap);
					wake = 1;
				}
			}
		}
	spin_unlock(&inode->i_lock);

	dout(30, "put_cap_refs %p had %d %s\n", inode, had, last ? "last":"");

	if (last && !flushsnaps)
		ceph_check_caps(ci, 0);
	else if (flushsnaps)
		ceph_flush_snaps(ci);
	if (wake)
		wake_up(&ci->i_cap_wq);
}

/*
 * Release @nr WRBUFFER refs on dirty pages for the given @snapc snap
 * context.  Adjust per-snap dirty page accounting as appropriate.
 * Once all dirty data for a cap_snap is flushed, flush snapped file
 * metadata back to the MDS.  If we dropped the last ref, call
 * ceph_check_caps.
 */
void ceph_put_wrbuffer_cap_refs(struct ceph_inode_info *ci, int nr,
				struct ceph_snap_context *snapc)
{
	struct inode *inode = &ci->vfs_inode;
	int last = 0;
	int last_snap = 0;

	spin_lock(&inode->i_lock);
	ci->i_wrbuffer_ref -= nr;
	last = !ci->i_wrbuffer_ref;
	if (snapc == ci->i_snap_realm->cached_context) {
		ci->i_wrbuffer_ref_head -= nr;
		dout(30, "put_wrbuffer_cap_refs on %p head %d/%d -> %d/%d %s\n",
		     inode,
		     ci->i_wrbuffer_ref+nr, ci->i_wrbuffer_ref_head+nr,
		     ci->i_wrbuffer_ref, ci->i_wrbuffer_ref_head,
		     last ? " LAST":"");
	} else {
		struct list_head *p;
		struct ceph_cap_snap *capsnap = NULL;
		list_for_each(p, &ci->i_cap_snaps) {
			capsnap = list_entry(p, struct ceph_cap_snap, ci_item);
			if (capsnap->context == snapc) {
				capsnap->dirty -= nr;
				last_snap = !capsnap->dirty;
				break;
			}
		}
		BUG_ON(!capsnap);
		dout(30, "put_wrbuffer_cap_refs on %p cap_snap %p "
		     " snap %lld %d/%d -> %d/%d %s%s\n",
		     inode, capsnap, capsnap->context->seq,
		     ci->i_wrbuffer_ref+nr, capsnap->dirty + nr,
		     ci->i_wrbuffer_ref, capsnap->dirty,
		     last ? " (wrbuffer last)":"",
		     last_snap ? " (capsnap last)":"");
	}
	spin_unlock(&inode->i_lock);

	if (last) {
		ceph_check_caps(ci, 0);
	} else if (last_snap) {
		ceph_flush_snaps(ci);
		wake_up(&ci->i_cap_wq);
	}
}



/*
 * Handle a cap GRANT message from the MDS.  (Note that a GRANT may
 * actually be a revocation if it specifies a smaller cap set.)
 *
 * caller holds s_mutex.  NOT snap_rwsem.
 * return value:
 *  0 - ok
 *  1 - send the msg back to mds
 */
static int handle_cap_grant(struct inode *inode, struct ceph_mds_caps *grant,
			    struct ceph_mds_session *session)
{
	struct ceph_cap *cap;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int mds = session->s_mds;
	int seq = le32_to_cpu(grant->seq);
	int newcaps = le32_to_cpu(grant->caps);
	int used;
	int issued; /* to me, before */
	int wanted;
	int reply = 0;
	u64 size = le64_to_cpu(grant->size);
	u64 max_size = le64_to_cpu(grant->max_size);
	struct timespec mtime, atime, ctime;
	int wake = 0;
	int writeback = 0;
	int invalidate = 0;
	int tried_invalidate = 0;
	u32 inv_gen = 0;
	int ret;

	dout(10, "handle_cap_grant inode %p ci %p mds%d seq %d\n",
	     inode, ci, mds, seq);
	dout(10, " size %llu max_size %llu, i_size %llu\n", size, max_size,
		inode->i_size);
start:
	spin_lock(&inode->i_lock);

	/* do we have this cap? */
	cap = __get_cap_for_mds(inode, mds);
	if (!cap) {
		/*
		 * then ignore.  never reply to cap messages out of turn,
		 * or we'll be mixing up different instances of caps on the
		 * same inode, and confuse the mds.
		 */
		dout(10, "no cap on %p ino %llx.%llx from mds%d, ignoring\n",
		     inode, ci->i_vino.ino, ci->i_vino.snap, mds);
		goto out;
	}
	dout(10, " cap %p\n", cap);
	cap->gen = session->s_cap_gen;

	if (((cap->issued & ~newcaps) & CEPH_CAP_RDCACHE)
	    && !ci->i_wrbuffer_ref){
		dout(10, "RDCACHE invalidation\n");
		if (!tried_invalidate) {
			inv_gen = ci->i_rdcache_gen;
			spin_unlock(&inode->i_lock);

			tried_invalidate = 1;
			ret = invalidate_inode_pages2(&inode->i_data);
			ret = -EBUSY; /* FIXME debug only! */
			if (ret < 0)
				invalidate = 1;
			goto start;
		} else {
			if (ci->i_rdcache_gen != inv_gen) /* was there a race? */
				invalidate = 1;
		}
	}

	if ((cap->issued & ~newcaps) & CEPH_CAP_RDCACHE & __ceph_caps_issued(ci, 0)) {
		if (!ci->i_rdcache_pending)
			ci->i_rdcache_gen++;
		else
			invalidate = 0; /* ok, we're already taking care of it */
	}

	if (invalidate && !(ci->i_rdcache_pending))
		ci->i_rdcache_pending = 1;


	dout(10, "invalidate=%d ci->i_rdcache_pending=%d gen=%d\n", invalidate, ci->i_rdcache_pending, ci->i_rdcache_gen);


	/* size/ctime/mtime/atime? */
	issued = __ceph_caps_issued(ci, NULL);
	ceph_decode_timespec(&mtime, &grant->mtime);
	ceph_decode_timespec(&atime, &grant->atime);
	ceph_decode_timespec(&ctime, &grant->ctime);
	ceph_fill_file_bits(inode, issued,
			    le64_to_cpu(grant->truncate_seq), size,
			    le64_to_cpu(grant->time_warp_seq), &ctime, &mtime,
			    &atime);

	/* max size increase? */
	if (max_size != ci->i_max_size) {
		dout(10, "max_size %lld -> %llu\n", ci->i_max_size, max_size);
		ci->i_max_size = max_size;
		if (max_size >= ci->i_wanted_max_size) {
			ci->i_wanted_max_size = 0;  /* reset */
			ci->i_requested_max_size = 0;
		}
		wake = 1;
	}

	/* check cap bits */
	wanted = __ceph_caps_wanted(ci);
	used = __ceph_caps_used(ci);
	dout(10, " my wanted = %d, used = %d\n", wanted, used);
	if (wanted != le32_to_cpu(grant->wanted)) {
		dout(10, "mds wanted %d -> %d\n", le32_to_cpu(grant->wanted),
		     wanted);
		grant->wanted = cpu_to_le32(wanted);
	}

	cap->seq = seq;

	/* file layout may have changed */
	ci->i_layout = grant->layout;

	/* revocation? */
	if (cap->issued & ~newcaps) {
		dout(10, "revocation: %d -> %d\n", cap->issued, newcaps);
		if ((used & ~newcaps) & CEPH_CAP_WRBUFFER) {
			writeback = 1; /* will delay ack */
		} else if (!invalidate) {
			/*
			 * we're not using revoked caps.. ack now.
			 * re-use incoming message.
			 */
			cap->implemented = newcaps;

			grant->size = cpu_to_le64(inode->i_size);
			grant->max_size = 0;  /* don't re-request */
			ceph_encode_timespec(&grant->mtime, &inode->i_mtime);
			ceph_encode_timespec(&grant->atime, &inode->i_atime);
			grant->time_warp_seq = cpu_to_le64(ci->i_time_warp_seq);
			grant->snap_follows =
			     cpu_to_le64(ci->i_snap_realm->cached_context->seq);
			reply = 1;
			wake = 1;
		}
		cap->issued = newcaps;
		goto out;
	}

	/* grant or no-op */
	if (cap->issued == newcaps) {
		dout(10, "caps unchanged: %d -> %d\n", cap->issued, newcaps);
	} else {
		dout(10, "grant: %d -> %d\n", cap->issued, newcaps);
		cap->issued = newcaps;
		cap->implemented |= newcaps;    /* add bits only, to
						 * avoid stepping on a
						 * pending revocation */
		wake = 1;
	}

out:
	spin_unlock(&inode->i_lock);
	if (writeback) {
		/*
		 * queue inode for writeback: we can't actually call
		 * filemap_write_and_wait, etc. from message handler
		 * context.
		 */
		dout(10, "queueing %p for writeback\n", inode);
		ceph_queue_writeback(inode);
	}
	if (invalidate) {
		dout(10, "queueing %p for page invalidation\n", inode);
		ceph_queue_page_invalidation(inode);
	}
	if (wake)
		wake_up(&ci->i_cap_wq);
	return reply;
}


/*
 * Handle RELEASE from MDS.  That means we can throw away our cap
 * state as the MDS has fully flushed that metadata to disk.
 */
static void handle_cap_released(struct inode *inode,
				struct ceph_mds_caps *m,
				struct ceph_mds_session *session)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int seq = le32_to_cpu(m->seq);
	int removed_last;
	struct ceph_cap *cap;

	dout(10, "handle_cap_released inode %p ci %p mds%d seq %d\n", inode, ci,
	     session->s_mds, seq);

	spin_lock(&inode->i_lock);
	cap = __get_cap_for_mds(inode, session->s_mds);
	BUG_ON(!cap);
	removed_last = __ceph_remove_cap(cap);
	if (removed_last)
		__cap_delay_cancel(&ceph_inode_to_client(inode)->mdsc, ci);
	spin_unlock(&inode->i_lock);
	if (removed_last)
		iput(inode);
}


/*
 * Handle FLUSHEDSNAP.  MDS has flushed snap data to disk and we can
 * throw away our cap_snap.
 *
 * Caller hold s_mutex, snap_rwsem.
 */
static void handle_cap_flushedsnap(struct inode *inode,
				   struct ceph_mds_caps *m,
				   struct ceph_mds_session *session)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	u64 follows = le64_to_cpu(m->snap_follows);
	struct list_head *p;
	struct ceph_cap_snap *capsnap;

	dout(10, "handle_cap_flushedsnap inode %p ci %p mds%d follows %lld\n",
	     inode, ci, session->s_mds, follows);

	spin_lock(&inode->i_lock);
	list_for_each(p, &ci->i_cap_snaps) {
		capsnap = list_entry(p, struct ceph_cap_snap, ci_item);
		if (capsnap->follows == follows) {
			WARN_ON(capsnap->dirty || capsnap->writing);
			dout(10, " removing cap_snap %p follows %lld\n",
			     capsnap, follows);
			ceph_put_snap_context(capsnap->context);
			list_del(&capsnap->ci_item);
			kfree(capsnap);
			break;
		}
	}
	spin_unlock(&inode->i_lock);
}


/*
 * Handle TRUNC from MDS, indicating file truncation.
 *
 * caller hold s_mutex, NOT snap_rwsem.
 */
static void handle_cap_trunc(struct inode *inode,
			     struct ceph_mds_caps *trunc,
			     struct ceph_mds_session *session)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int mds = session->s_mds;
	int seq = le32_to_cpu(trunc->seq);
	u64 size = le64_to_cpu(trunc->size);
	int queue_trunc = 0;

	dout(10, "handle_cap_trunc inode %p ci %p mds%d seq %d\n", inode, ci,
	     mds, seq);

	/*
	 * vmtruncate lazily; we can't block on i_mutex in the message
	 * handler path, or we deadlock against osd op replies needed
	 * to complete the writes holding i_lock.  vmtruncate will
	 * also block on page locks held by writes...
	 *
	 * if its an expansion, and there is no truncate pending, we
	 * don't need to truncate.
	 */
	spin_lock(&inode->i_lock);
	if (ci->i_vmtruncate_to < 0 && size > inode->i_size) {
		dout(10, "clean fwd truncate, no vmtruncate needed\n");
	} else if (ci->i_vmtruncate_to >= 0 && size >= ci->i_vmtruncate_to) {
		dout(10, "trunc to %lld < %lld already queued\n",
		     ci->i_vmtruncate_to, size);
	} else {
		/* we need to trunc even smaller */
		dout(10, "queueing trunc %lld -> %lld\n", inode->i_size, size);
		ci->i_vmtruncate_to = size;
		queue_trunc = 1;
	}
	i_size_write(inode, size);
	ci->i_reported_size = size;
	spin_unlock(&inode->i_lock);

	if (queue_trunc)
		queue_work(ceph_client(inode->i_sb)->trunc_wq,
			   &ci->i_vmtruncate_work);
}

/*
 * Handle EXPORT from MDS.  Cap is being migrated _from_ this mds to a
 * different one.  If we are the most recent migration we've seen (as
 * indicated by mseq), make note of the migrating cap bits for the
 * duration (until we see the corresponding IMPORT).
 *
 * caller holds s_mutex, snap_rwsem
 */
static void handle_cap_export(struct inode *inode, struct ceph_mds_caps *ex,
			      struct ceph_mds_session *session)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int mds = session->s_mds;
	unsigned mseq = le32_to_cpu(ex->migrate_seq);
	struct ceph_cap *cap = NULL, *t;
	struct rb_node *p;
	int was_last = 0;
	int remember = 1;

	dout(10, "handle_cap_export inode %p ci %p mds%d mseq %d\n",
	     inode, ci, mds, mseq);

	spin_lock(&inode->i_lock);

	/* make sure we haven't seen a higher mseq */
	for (p = rb_first(&ci->i_caps); p; p = rb_next(p)) {
		t = rb_entry(p, struct ceph_cap, ci_node);
		if (t->mseq > mseq) {
			dout(10, " higher mseq on cap from mds%d\n",
			     t->session->s_mds);
			remember = 0;
		}
		if (t->session->s_mds == mds)
			cap = t;
	}

	if (cap) {
		if (remember) {
			/* make note */
			ci->i_cap_exporting_mds = mds;
			ci->i_cap_exporting_mseq = mseq;
			ci->i_cap_exporting_issued = cap->issued;
		}
		was_last = __ceph_remove_cap(cap);
	} else {
		WARN_ON(!cap);
	}

	spin_unlock(&inode->i_lock);
	if (was_last)
		iput(inode);
}

/*
 * Handle cap IMPORT.  If there are temp bits from an older EXPORT,
 * clean them up.
 *
 * caller holds s_mutex, snap_rwsem
 */
static void handle_cap_import(struct inode *inode, struct ceph_mds_caps *im,
			      struct ceph_mds_session *session,
			      void *snaptrace, int snaptrace_len)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int mds = session->s_mds;
	unsigned issued = le32_to_cpu(im->caps);
	unsigned seq = le32_to_cpu(im->seq);
	unsigned mseq = le32_to_cpu(im->migrate_seq);

	if (ci->i_cap_exporting_mds >= 0 &&
	    ci->i_cap_exporting_mseq < mseq) {
		dout(10, "handle_cap_import inode %p ci %p mds%d mseq %d"
		     " - cleared exporting from mds%d\n",
		     inode, ci, mds, mseq,
		     ci->i_cap_exporting_mds);
		ci->i_cap_exporting_issued = 0;
		ci->i_cap_exporting_mseq = 0;
		ci->i_cap_exporting_mds = -1;
	} else {
		dout(10, "handle_cap_import inode %p ci %p mds%d mseq %d\n",
		     inode, ci, mds, mseq);
	}

	ceph_add_cap(inode, session, -1, issued, seq, mseq,
		     snaptrace, snaptrace_len, NULL);
}


/*
 * Handle a CEPH_CAPS message from the MDS.
 *
 * Identify the appropriate session, inode, and call the right handler
 * based on the cap op.  Take read or write lock on snap_rwsem as
 * appropriate.
 */
void ceph_handle_caps(struct ceph_mds_client *mdsc,
		      struct ceph_msg *msg)
{
	struct super_block *sb = mdsc->client->sb;
	struct ceph_mds_session *session;
	struct inode *inode;
	struct ceph_mds_caps *h;
	int mds = le32_to_cpu(msg->hdr.src.name.num);
	int op;
	u32 seq;
	struct ceph_vino vino;
	u64 size, max_size;
	int check_caps = 0;

	dout(10, "handle_caps from mds%d\n", mds);

	/* decode */
	if (msg->front.iov_len < sizeof(*h))
		goto bad;
	h = msg->front.iov_base;
	op = le32_to_cpu(h->op);
	vino.ino = le64_to_cpu(h->ino);
	vino.snap = CEPH_NOSNAP;
	seq = le32_to_cpu(h->seq);
	size = le64_to_cpu(h->size);
	max_size = le64_to_cpu(h->max_size);

	/* find session */
	mutex_lock(&mdsc->mutex);
	session = __ceph_get_mds_session(mdsc, mds);
	if (session)
		down_write(&mdsc->snap_rwsem);
	mutex_unlock(&mdsc->mutex);
	if (!session) {
		dout(10, "WTF, got cap but no session for mds%d\n", mds);
		return;
	}

	mutex_lock(&session->s_mutex);
	session->s_seq++;
	dout(20, " mds%d seq %lld\n", session->s_mds, session->s_seq);

	/* lookup ino */
	inode = ceph_find_inode(sb, vino);
	dout(20, " op %s ino %llx inode %p\n", ceph_cap_op_name(op), vino.ino,
	     inode);
	if (!inode) {
		dout(10, " i don't have ino %llx, sending release\n", vino.ino);
		send_cap_msg(mdsc, vino.ino, CEPH_CAP_OP_RELEASE, 0, 0, seq,
			     size, 0, 0, NULL, NULL, 0, 0, mds);
		goto no_inode;
	}

	switch (op) {
	case CEPH_CAP_OP_GRANT:
		up_write(&mdsc->snap_rwsem);
		if (handle_cap_grant(inode, h, session) == 1) {
			dout(10, " sending reply back to mds%d\n", mds);
			ceph_msg_get(msg);
			ceph_send_msg_mds(mdsc, msg, mds);
		}
		break;

	case CEPH_CAP_OP_TRUNC:
		up_write(&mdsc->snap_rwsem);
		handle_cap_trunc(inode, h, session);
		break;

	case CEPH_CAP_OP_RELEASED:
		handle_cap_released(inode, h, session);
		up_write(&mdsc->snap_rwsem);
		break;

	case CEPH_CAP_OP_FLUSHEDSNAP:
		handle_cap_flushedsnap(inode, h, session);
		up_write(&mdsc->snap_rwsem);
		break;

	case CEPH_CAP_OP_EXPORT:
		handle_cap_export(inode, h, session);
		up_write(&mdsc->snap_rwsem);
		break;

	case CEPH_CAP_OP_IMPORT:
		handle_cap_import(inode, h, session,
				  msg->front.iov_base + sizeof(*h),
				  le32_to_cpu(h->snap_trace_len));
		up_write(&mdsc->snap_rwsem);
		check_caps = 1; /* we may have sent a RELEASE to the old auth */
		break;

	default:
		up_write(&mdsc->snap_rwsem);
		derr(10, " unknown cap op %d %s\n", op, ceph_cap_op_name(op));
	}

no_inode:
	mutex_unlock(&session->s_mutex);
	ceph_put_mds_session(session);

	if (check_caps)
		ceph_check_caps(ceph_inode(inode), 1);
	if (inode)
		iput(inode);
	return;

bad:
	derr(10, "corrupt caps message\n");
	return;
}


/*
 * Delayed work handler to process end of delayed cap release LRU list.
 */
void ceph_check_delayed_caps(struct ceph_mds_client *mdsc)
{
	struct ceph_inode_info *ci;

	dout(10, "check_delayed_caps\n");
	while (1) {
		spin_lock(&mdsc->cap_delay_lock);
		if (list_empty(&mdsc->cap_delay_list))
			break;
		ci = list_first_entry(&mdsc->cap_delay_list,
				      struct ceph_inode_info,
				      i_cap_delay_list);
		if (time_before(jiffies, ci->i_hold_caps_until))
			break;
		list_del_init(&ci->i_cap_delay_list);
		spin_unlock(&mdsc->cap_delay_lock);
		dout(10, "check_delayed_caps on %p\n", &ci->vfs_inode);
		ceph_check_caps(ci, 1);
		iput(&ci->vfs_inode);
	}
	spin_unlock(&mdsc->cap_delay_lock);
}


/*
 * Force a flush of any snap_caps and write caps we hold.
 *
 * Caller holds snap_rwsem, s_mutex.
 */
void ceph_flush_write_caps(struct ceph_mds_client *mdsc,
			   struct ceph_mds_session *session)
{
	struct list_head *p, *n;

	dout(10, "flush_write_caps mds%d\n", session->s_mds);
	list_for_each_safe (p, n, &session->s_caps) {
		struct ceph_cap *cap =
			list_entry(p, struct ceph_cap, session_caps);
		struct inode *inode = &cap->ci->vfs_inode;
		int used, wanted;

		spin_lock(&inode->i_lock);

		if (!list_empty(&cap->ci->i_cap_snaps))
			__ceph_flush_snaps(cap->ci);

		if ((cap->implemented & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) == 0) {
			spin_unlock(&inode->i_lock);
			continue;
		}

		used = __ceph_caps_used(cap->ci);
		wanted = __ceph_caps_wanted(cap->ci);
		if (used || wanted) {
			derr(0, "residual caps on %p u %d w %d s=%llu wrb=%d\n",
			     inode, used, wanted, inode->i_size,
			     cap->ci->i_wrbuffer_ref);
			used = wanted = 0;
		}

		/* __send_cap drops i_lock */
		__send_cap(mdsc, session, cap, used, wanted);
	}
}


