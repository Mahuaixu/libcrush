#include <linux/module.h>
#include <linux/parser.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/rwsem.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/version.h>

/* debug levels; defined in super.h */

/*
 * global debug value.
 *  0 = quiet.
 *
 * if the per-file debug level >= 0, then that overrides this  global
 * debug level.
 */
int ceph_debug = 1;

/*
 * if true, send output to KERN_INFO (console) instead of KERN_DEBUG.
 */
int ceph_debug_console;

/* for this file */
int ceph_debug_super = -1;

#define DOUT_VAR ceph_debug_super
#define DOUT_PREFIX "super: "
#include "super.h"

#include <linux/statfs.h>
#include "mon_client.h"

void ceph_dispatch(void *p, struct ceph_msg *msg);
void ceph_peer_reset(void *p, struct ceph_entity_name *peer_name);


/*
 * super ops
 */

static int ceph_write_inode(struct inode *inode, int unused)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	if (memcmp(&ci->i_old_atime, &inode->i_atime, sizeof(struct timeval))) {
		dout(30, "ceph_write_inode %llx.%llx .. atime updated\n",
		     ceph_vinop(inode));
		/* eventually push this async to mds ... */
	}
	return 0;
}

static void ceph_put_super(struct super_block *s)
{
	struct ceph_client *cl = ceph_client(s);
	int rc;
	int seconds = 15;

	dout(30, "put_super\n");
	ceph_mdsc_close_sessions(&cl->mdsc);
	ceph_monc_request_umount(&cl->monc);

	rc = wait_event_timeout(cl->mount_wq,
				(cl->mount_state == CEPH_MOUNT_UNMOUNTED),
				seconds*HZ);
	if (rc == 0)
		derr(0, "umount timed out after %d seconds\n", seconds);

	return;
}

static int ceph_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct ceph_client *client = ceph_inode_to_client(dentry->d_inode);
	struct ceph_statfs st;
	int err;

	dout(30, "ceph_statfs\n");
	err = ceph_monc_do_statfs(&client->monc, &st);
	if (err < 0)
		return err;

	/* fill in kstatfs */
	buf->f_type = CEPH_SUPER_MAGIC;  /* ?? */
	buf->f_bsize = 1 << CEPH_BLOCK_SHIFT;     /* 1 MB */
	buf->f_blocks = st.f_total >> (CEPH_BLOCK_SHIFT-10);
	buf->f_bfree = st.f_free >> (CEPH_BLOCK_SHIFT-10);
	buf->f_bavail = st.f_avail >> (CEPH_BLOCK_SHIFT-10);
	buf->f_files = st.f_objects;
	buf->f_ffree = -1;
	/* fsid? */
	buf->f_namelen = PATH_MAX;
	buf->f_frsize = 4096;

	return 0;
}


static int ceph_syncfs(struct super_block *sb, int wait)
{
	dout(10, "sync_fs %d\n", wait);
	return 0;
}


/**
 * ceph_show_options - Show mount options in /proc/mounts
 * @m: seq_file to write to
 * @mnt: mount descriptor
 */
static int ceph_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct ceph_client *client = ceph_sb_to_client(mnt->mnt_sb);
	struct ceph_mount_args *args = &client->mount_args;

	if (ceph_debug != 0)
		seq_printf(m, ",debug=%d", ceph_debug);
	if (args->flags & CEPH_MOUNT_FSID)
		seq_printf(m, ",fsidmajor=%llu,fsidminor%llu",
			   args->fsid.major, args->fsid.minor);
	if (args->flags & CEPH_MOUNT_NOSHARE)
		seq_puts(m, ",noshare");

	if (args->flags & CEPH_MOUNT_DIRSTAT)
		seq_puts(m, ",dirstat");
	else
		seq_puts(m, ",nodirstat");
	if (args->flags & CEPH_MOUNT_RBYTES)
		seq_puts(m, ",rbytes");
	else
		seq_puts(m, ",norbytes");
	return 0;
}


/*
 * inode cache
 */
static struct kmem_cache *ceph_inode_cachep;

static struct inode *ceph_alloc_inode(struct super_block *sb)
{
	struct ceph_inode_info *ci;
	int i;

	ci = kmem_cache_alloc(ceph_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	dout(10, "alloc_inode %p vfsi %p\n", ci, &ci->vfs_inode);

	ci->i_version = 0;
	ci->i_time_warp_seq = 0;
	ci->i_symlink = 0;

	ci->i_lease_session = 0;
	ci->i_lease_mask = 0;
	ci->i_lease_ttl = 0;
	INIT_LIST_HEAD(&ci->i_lease_item);

	ci->i_fragtree = RB_ROOT;

	ci->i_xattr_len = 0;
	ci->i_xattr_data = 0;

	ci->i_caps = RB_ROOT;
	for (i = 0; i < STATIC_CAPS; i++)
		ci->i_static_caps[i].mds = -1;
	for (i = 0; i < CEPH_FILE_MODE_NUM; i++)
		ci->i_nr_by_mode[i] = 0;
	init_waitqueue_head(&ci->i_cap_wq);

	ci->i_wanted_max_size = 0;
	ci->i_requested_max_size = 0;

	ci->i_cap_exporting_mds = 0;
	ci->i_cap_exporting_mseq = 0;
	ci->i_cap_exporting_issued = 0;
	ci->i_snap_caps = 0;

	ci->i_rd_ref = ci->i_rdcache_ref = 0;
	ci->i_wr_ref = 0;
	atomic_set(&ci->i_wrbuffer_ref, 0);
	ci->i_hold_caps_until = 0;
	INIT_LIST_HEAD(&ci->i_cap_delay_list);

	ci->i_snaprealm = 0;

	INIT_WORK(&ci->i_wb_work, ceph_inode_writeback);

	ci->i_vmtruncate_to = -1;
	INIT_WORK(&ci->i_vmtruncate_work, ceph_vmtruncate_work);

	return &ci->vfs_inode;
}

static void ceph_destroy_inode(struct inode *inode)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_inode_frag *frag;
	struct rb_node *n;
	
	dout(30, "destroy_inode %p ino %llx.%llx\n", inode, ceph_vinop(inode));
	kfree(ci->i_symlink);
	while ((n = rb_first(&ci->i_fragtree)) != 0) {
		frag = rb_entry(n, struct ceph_inode_frag, node);
		rb_erase(n, &ci->i_fragtree);
		kfree(frag);
	}
	kfree(ci->i_xattr_data);
	kmem_cache_free(ceph_inode_cachep, ci);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26) 
static void init_once(void *foo)
#else
static void init_once(struct kmem_cache *cachep, void *foo)
#endif
{
	struct ceph_inode_info *ci = foo;
	dout(10, "init_once on %p\n", &ci->vfs_inode);
	inode_init_once(&ci->vfs_inode);
}

static int init_inodecache(void)
{
	ceph_inode_cachep = kmem_cache_create("ceph_inode_cache",
					      sizeof(struct ceph_inode_info),
					      0, (SLAB_RECLAIM_ACCOUNT|
						  SLAB_MEM_SPREAD),
					      init_once);
	if (ceph_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(ceph_inode_cachep);
}

static const struct super_operations ceph_super_ops = {
	.alloc_inode	= ceph_alloc_inode,
	.destroy_inode	= ceph_destroy_inode,
	.write_inode    = ceph_write_inode,
	.sync_fs        = ceph_syncfs,
	.put_super	= ceph_put_super,
	.show_options   = ceph_show_options,
	.statfs		= ceph_statfs,
};



/*
 * the monitor responds to monmap to indicate mount success.
 * (or, someday, to indicate a change in the monitor cluster?)
 */
static void handle_monmap(struct ceph_client *client, struct ceph_msg *msg)
{
	int err;
	int first = (client->monc.monmap->epoch == 0);
	void *new;

	dout(2, "handle_monmap had epoch %d\n", client->monc.monmap->epoch);
	new = ceph_monmap_decode(msg->front.iov_base,
				 msg->front.iov_base + msg->front.iov_len);
	if (IS_ERR(new)) {
		err = PTR_ERR(new);
		derr(0, "problem decoding monmap, %d\n", err);
		return;
	}
	kfree(client->monc.monmap);
	client->monc.monmap = new;

	if (first) {
		char name[10];
		client->whoami = le32_to_cpu(msg->hdr.dst.name.num);
		client->msgr->inst.name = msg->hdr.dst.name;
		sprintf(name, "client%d", client->whoami);
		dout(1, "i am %s, fsid is %llx.%llx\n", name,
		     le64_to_cpu(client->monc.monmap->fsid.major),
		     le64_to_cpu(client->monc.monmap->fsid.minor));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
		client->client_kobj = kobject_create_and_add(name, ceph_kobj);
		//client->fsid_kobj = kobject_create_and_add("fsid", 
		//client->client_kobj);
#endif
	}
}



const char *ceph_msg_type_name(int type)
{
	switch (type) {
	case CEPH_MSG_SHUTDOWN: return "shutdown";
	case CEPH_MSG_PING: return "ping";
	case CEPH_MSG_PING_ACK: return "ping_ack";
	case CEPH_MSG_MON_MAP: return "mon_map";
	case CEPH_MSG_MON_GET_MAP: return "mon_get_map";
	case CEPH_MSG_CLIENT_MOUNT: return "client_mount";
	case CEPH_MSG_CLIENT_UNMOUNT: return "client_unmount";
	case CEPH_MSG_STATFS: return "statfs";
	case CEPH_MSG_STATFS_REPLY: return "statfs_reply";
	case CEPH_MSG_MDS_GETMAP: return "mds_getmap";
	case CEPH_MSG_MDS_MAP: return "mds_map";
	case CEPH_MSG_CLIENT_SESSION: return "client_session";
	case CEPH_MSG_CLIENT_RECONNECT: return "client_reconnect";
	case CEPH_MSG_CLIENT_REQUEST: return "client_request";
	case CEPH_MSG_CLIENT_REQUEST_FORWARD: return "client_request_forward";
	case CEPH_MSG_CLIENT_REPLY: return "client_reply";
	case CEPH_MSG_CLIENT_CAPS: return "client_caps";
	case CEPH_MSG_CLIENT_SNAP: return "client_snap";
	case CEPH_MSG_CLIENT_LEASE: return "client_lease";
	case CEPH_MSG_OSD_GETMAP: return "osd_getmap";
	case CEPH_MSG_OSD_MAP: return "osd_map";
	case CEPH_MSG_OSD_OP: return "osd_op";
	case CEPH_MSG_OSD_OPREPLY: return "osd_opreply";
	}
	return "unknown";
}

void ceph_peer_reset(void *p, struct ceph_entity_name *peer_name)
{
	struct ceph_client *client = p;

	dout(30, "ceph_peer_reset peer_name = %s%d\n", ENTITY_NAME(*peer_name));

	/* write me */
}




/*
 * mount options
 */

enum {
	Opt_fsidmajor,
	Opt_fsidminor,
	Opt_debug,
	Opt_debug_console,
	Opt_debug_msgr,
	Opt_debug_mdsc,
	Opt_debug_osdc,
	Opt_debug_addr,
	Opt_debug_inode,
	Opt_debug_snap,
	Opt_debug_ioctl,
	Opt_debug_caps,
	Opt_monport,
	Opt_port,
	Opt_wsize,
	Opt_osdtimeout,
	Opt_mount_attempts,
	/* int args above */
	Opt_ip,
	Opt_unsafewrites,
	Opt_dirstat,
	Opt_nodirstat,
	Opt_rbytes,
	Opt_norbytes,
};

static match_table_t arg_tokens = {
	{Opt_fsidmajor, "fsidmajor=%ld"},
	{Opt_fsidminor, "fsidminor=%ld"},
	{Opt_debug, "debug=%d"},
	{Opt_debug_msgr, "debug_msgr=%d"},
	{Opt_debug_mdsc, "debug_mdsc=%d"},
	{Opt_debug_osdc, "debug_osdc=%d"},
	{Opt_debug_addr, "debug_addr=%d"},
	{Opt_debug_inode, "debug_inode=%d"},
	{Opt_debug_snap, "debug_snap=%d"},
	{Opt_debug_ioctl, "debug_ioctl=%d"},
	{Opt_debug_caps, "debug_caps=%d"},
	{Opt_monport, "monport=%d"},
	{Opt_port, "port=%d"},
	{Opt_wsize, "wsize=%d"},
	{Opt_osdtimeout, "osdtimeout=%d"},
	{Opt_mount_attempts, "mount_attempts=%d"},
	/* int args above */
	{Opt_ip, "ip=%s"},
	{Opt_debug_console, "debug_console"},
	{Opt_unsafewrites, "unsafewrites"},
	{Opt_dirstat, "dirstat"},
	{Opt_nodirstat, "nodirstat"},
	{Opt_rbytes, "rbytes"},
	{Opt_norbytes, "norbytes"},
	{-1, NULL}
};

/*
 * FIXME: add error checking to ip parsing
 */
static int parse_ip(const char *c, int len, struct ceph_entity_addr *addr)
{
	int i;
	int v;
	unsigned ip = 0;
	const char *p = c;

	dout(15, "parse_ip on '%s' len %d\n", c, len);
	for (i = 0; *p && i < 4; i++) {
		v = 0;
		while (*p && *p != '.' && p < c+len) {
			if (*p < '0' || *p > '9')
				goto bad;
			v = (v * 10) + (*p - '0');
			p++;
		}
		ip = (ip << 8) + v;
		if (!*p)
			break;
		p++;
	}
	if (p < c+len)
		goto bad;

	*(__be32 *)&addr->ipaddr.sin_addr.s_addr = htonl(ip);
	dout(15, "parse_ip got %u.%u.%u.%u\n",
	     ip >> 24, (ip >> 16) & 0xff,
	     (ip >> 8) & 0xff, ip & 0xff);
	return 0;

bad:
	derr(1, "parse_ip bad ip '%s'\n", c);
	return -EINVAL;
}

static int parse_mount_args(int flags, char *options, const char *dev_name,
			    struct ceph_mount_args *args, const char **path)
{
	char *c;
	int len, err;
	substring_t argstr[MAX_OPT_ARGS];

	dout(15, "parse_mount_args dev_name '%s'\n", dev_name);
	memset(args, 0, sizeof(*args));

	/* defaults */
	args->sb_flags = flags;
	args->flags = CEPH_MOUNT_DEFAULT;
	args->osd_timeout = 5;  /* seconds */
	args->mount_attempts = 2;  /* 2 attempts */
	args->snapdir_name = ".snap";

	/* ip1[,ip2...]:/server/path */
	c = strchr(dev_name, ':');
	if (c == NULL)
		return -EINVAL;

	/* get mon ip */
	/* er, just one for now. later, comma-separate... */
	len = c - dev_name;
	err = parse_ip(dev_name, len, &args->mon_addr[0]);
	if (err < 0)
		return err;
	args->mon_addr[0].ipaddr.sin_family = AF_INET;
	args->mon_addr[0].ipaddr.sin_port = htons(CEPH_MON_PORT);
	args->mon_addr[0].erank = 0;
	args->mon_addr[0].nonce = 0;
	args->num_mon = 1;

	/* path on server */
	c++;
	while (*c == '/') c++;  /* remove leading '/'(s) */
	*path = c;

	dout(15, "server path '%s'\n", *path);

	/* parse mount options */
	while ((c = strsep(&options, ",")) != NULL) {
		int token, intval, ret, i;
		if (!*c)
			continue;
		token = match_token(c, arg_tokens, argstr);
		if (token < 0) {
			derr(0, "bad mount option at '%s'\n", c);
			return -EINVAL;

		}
		if (token < Opt_ip) {
			ret = match_int(&argstr[0], &intval);
			if (ret < 0) {
				dout(0, "bad mount arg, not int\n");
				continue;
			}
			dout(30, "got token %d intval %d\n", token, intval);
		}
		switch (token) {
		case Opt_fsidmajor:
			args->fsid.major = intval;
			break;
		case Opt_fsidminor:
			args->fsid.minor = intval;
			break;
		case Opt_monport:
			dout(25, "parse_mount_args monport=%d\n", intval);
			for (i = 0; i < args->num_mon; i++)
				args->mon_addr[i].ipaddr.sin_port =
					htons(intval);
			break;
		case Opt_port:
			args->my_addr.ipaddr.sin_port = htons(intval);
			break;
		case Opt_ip:
			err = parse_ip(argstr[0].from,
				       argstr[0].to-argstr[0].from,
				       &args->my_addr);
			if (err < 0)
				return err;
			args->flags |= CEPH_MOUNT_MYIP;
			break;

			/* debug levels */
		case Opt_debug:
			ceph_debug = intval;
			break;
		case Opt_debug_msgr:
			ceph_debug_msgr = intval;
			break;
		case Opt_debug_mdsc:
			ceph_debug_mdsc = intval;
			break;
		case Opt_debug_osdc:
			ceph_debug_osdc = intval;
			break;
		case Opt_debug_addr:
			ceph_debug_addr = intval;
			break;
		case Opt_debug_inode:
			ceph_debug_inode = intval;
			break;
		case Opt_debug_snap:
			ceph_debug_snap = intval;
			break;
		case Opt_debug_ioctl:
			ceph_debug_ioctl = intval;
			break;
		case Opt_debug_caps:
			ceph_debug_caps = intval;
			break;
		case Opt_debug_console:
			ceph_debug_console = 1;
			break;

			/* misc */
		case Opt_wsize:
			args->wsize = intval;
			break;
		case Opt_osdtimeout:
			args->osd_timeout = intval;
			break;
		case Opt_mount_attempts:
			args->mount_attempts = intval;
			break;
		case Opt_unsafewrites:
			args->flags |= CEPH_MOUNT_UNSAFE_WRITES;
			break;

		case Opt_dirstat:
			args->flags |= CEPH_MOUNT_DIRSTAT;
			break;
		case Opt_nodirstat:
			args->flags &= ~CEPH_MOUNT_DIRSTAT;
			break;
		case Opt_rbytes:
			args->flags |= CEPH_MOUNT_RBYTES;
			break;
		case Opt_norbytes:
			args->flags &= ~CEPH_MOUNT_RBYTES;
			break;

		default:
			BUG_ON(token);
		}
	}

	return 0;
}

/*
 * create a fresh client instance
 */
struct ceph_client *ceph_create_client(void)
{
	struct ceph_client *client;
	int err = -ENOMEM;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (client == NULL)
		return ERR_PTR(-ENOMEM);

	mutex_init(&client->mount_mutex);

	init_waitqueue_head(&client->mount_wq);
	spin_lock_init(&client->sb_lock);

	client->sb = 0;
	client->mount_state = CEPH_MOUNT_MOUNTING;
	client->whoami = -1;

	client->msgr = 0;

	client->wb_wq = create_workqueue("ceph-writeback");
	if (client->wb_wq == 0)
		goto fail;
	client->trunc_wq = create_workqueue("ceph-trunc");
	if (client->trunc_wq == 0)
		goto fail;

	/* subsystems */
	err = ceph_monc_init(&client->monc, client);
	if (err < 0)
		return ERR_PTR(err);
	ceph_mdsc_init(&client->mdsc, client);
	ceph_osdc_init(&client->osdc, client);

	return client;

fail:
	return ERR_PTR(-ENOMEM);
}

void ceph_destroy_client(struct ceph_client *client)
{
	dout(10, "destroy_client %p\n", client);

	/* unmount */
	/* ... */

	ceph_mdsc_stop(&client->mdsc);
	ceph_monc_stop(&client->monc);
	ceph_osdc_stop(&client->osdc);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	if (client->client_kobj)
		kobject_put(client->client_kobj);
#endif
	if (client->wb_wq)
		destroy_workqueue(client->wb_wq);
	if (client->trunc_wq)
		destroy_workqueue(client->trunc_wq);
	if (client->msgr)
		ceph_messenger_destroy(client->msgr);
	kfree(client);
	dout(10, "destroy_client %p done\n", client);
}

static int have_all_maps(struct ceph_client *client)
{
	return client->osdc.osdmap && client->osdc.osdmap->epoch &&
		client->monc.monmap && client->monc.monmap->epoch &&
		client->mdsc.mdsmap && client->mdsc.mdsmap->m_epoch;
}

static struct dentry *open_root_dentry(struct ceph_client *client,
				       const char *path)
{
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req = 0;
	struct ceph_mds_request_head *reqhead;
	int err;
	struct dentry *root;

	/* open dir */
	dout(30, "open_root_inode opening '%s'\n", path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_OPEN,
				       1, path, 0, 0,
				       NULL, USE_ANY_MDS);
	if (IS_ERR(req))
		return ERR_PTR(PTR_ERR(req));
	req->r_expects_cap = 1;
	reqhead = req->r_request->front.iov_base;
	reqhead->args.open.flags = O_DIRECTORY;
	reqhead->args.open.mode = 0;
	err = ceph_mdsc_do_request(mdsc, req);
	if (err == 0) {
		root = req->r_last_dentry;
		dget(root);
		dout(30, "open_root_inode success, root dentry is %p\n", root);
	} else
		root = ERR_PTR(err);
	ceph_mdsc_put_request(req);
	return root;
}

/*
 * mount: join the ceph cluster.
 */
int ceph_mount(struct ceph_client *client, struct vfsmount *mnt,
	       const char *path)
{
	struct ceph_entity_addr *myaddr = 0;
	struct ceph_msg *mount_msg;
	struct dentry *root;
	int err;
	int attempts = client->mount_args.mount_attempts;
	int which;
	char r;

	dout(10, "mount start\n");
	mutex_lock(&client->mount_mutex);

	/* messenger */
	if (client->msgr == NULL) {
		if (client->mount_args.flags & CEPH_MOUNT_MYIP)
			myaddr = &client->mount_args.my_addr;
		client->msgr = ceph_messenger_create(myaddr);
		if (IS_ERR(client->msgr)) {
			err = PTR_ERR(client->msgr);
			client->msgr = 0;
			goto out;
		}
		client->msgr->parent = client;
		client->msgr->dispatch = ceph_dispatch;
		client->msgr->prepare_pages = ceph_osdc_prepare_pages;
		client->msgr->peer_reset = ceph_peer_reset;
	}
	
	while (!have_all_maps(client)) {
		err = -EIO;
		if (attempts == 0)
			goto out;
		dout(10, "mount sending mount request, %d attempts left\n",
		     attempts--);
		get_random_bytes(&r, 1);
		which = r % client->mount_args.num_mon;
		mount_msg = ceph_msg_new(CEPH_MSG_CLIENT_MOUNT, 0, 0, 0, 0);
		if (IS_ERR(mount_msg)) {
			err = PTR_ERR(mount_msg);
			goto out;
		}
		mount_msg->hdr.dst.name.type =
			cpu_to_le32(CEPH_ENTITY_TYPE_MON);
		mount_msg->hdr.dst.name.num = cpu_to_le32(which);
		mount_msg->hdr.dst.addr = client->mount_args.mon_addr[which];

		ceph_msg_send(client->msgr, mount_msg, 0);
		dout(10, "mount from mon%d, %d attempts left\n",
		     which, attempts);

		/* wait */
		dout(10, "mount sent mount request, waiting for maps\n");
		err = wait_event_interruptible_timeout(client->mount_wq,
						       have_all_maps(client),
						       6*HZ);
		dout(10, "mount wait got %d\n", err);
		if (err == -EINTR)
			goto out;
	}

	dout(30, "mount opening base mountpoint\n");
	root = open_root_dentry(client, path);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto out;
	}
	mnt->mnt_root = root;
	mnt->mnt_sb = client->sb;
	client->mount_state = CEPH_MOUNT_MOUNTED;
	dout(10, "mount success\n");
	err = 0;

out:
	mutex_unlock(&client->mount_mutex);
	return err;
}


/*
 * dispatch -- called with incoming messages.
 *
 * should be fast and non-blocking, as it is called with locks held.
 */
void ceph_dispatch(void *p, struct ceph_msg *msg)
{
	struct ceph_client *client = p;
	int had;
	int type = le32_to_cpu(msg->hdr.type);

	/* deliver the message */
	switch (type) {
		/* me */
	case CEPH_MSG_MON_MAP:
		had = client->monc.monmap->epoch ? 1:0;
		handle_monmap(client, msg);
		if (!had && client->monc.monmap->epoch && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;

		/* mon client */
	case CEPH_MSG_STATFS_REPLY:
		ceph_monc_handle_statfs_reply(&client->monc, msg);
		break;
	case CEPH_MSG_CLIENT_UNMOUNT:
		ceph_monc_handle_umount(&client->monc, msg);
		break;

		/* mds client */
	case CEPH_MSG_MDS_MAP:
		had = client->mdsc.mdsmap ? 1:0;
		ceph_mdsc_handle_map(&client->mdsc, msg);
		if (!had && client->mdsc.mdsmap && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;
	case CEPH_MSG_CLIENT_SESSION:
		ceph_mdsc_handle_session(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REPLY:
		ceph_mdsc_handle_reply(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REQUEST_FORWARD:
		ceph_mdsc_handle_forward(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_CAPS:
		ceph_handle_caps(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_SNAP:
		ceph_mdsc_handle_snap(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_LEASE:
		ceph_mdsc_handle_lease(&client->mdsc, msg);
		break;

		/* osd client */
	case CEPH_MSG_OSD_MAP:
		had = client->osdc.osdmap ? 1:0;
		ceph_osdc_handle_map(&client->osdc, msg);
		if (!had && client->osdc.osdmap && have_all_maps(client))
			wake_up(&client->mount_wq);
		break;
	case CEPH_MSG_OSD_OPREPLY:
		ceph_osdc_handle_reply(&client->osdc, msg);
		break;

	default:
		derr(0, "received unknown message type %d\n", type);
	}

	ceph_msg_put(msg);
}


static int ceph_set_super(struct super_block *s, void *data)
{
	struct ceph_client *client = data;
	int ret;

	dout(10, "set_super %p data %p\n", s, data);

	s->s_flags = client->mount_args.sb_flags;
	s->s_maxbytes = min((u64)MAX_LFS_FILESIZE, CEPH_FILE_MAX_SIZE);

	s->s_fs_info = client;
	client->sb = s;

	/* fill sbinfo */
	s->s_op = &ceph_super_ops;
	s->s_export_op = &ceph_export_ops;

	/* set time granularity */
	s->s_time_gran = 1000;  /* 1000 ns == 1 us */

	ret = set_anon_super(s, 0);  /* what is the second arg for? */
	if (ret != 0)
		goto bail;

	return ret;

bail:
	s->s_fs_info = 0;
	client->sb = 0;
	return ret;
}

/*
 * share superblock if same fs AND options
 */
static int ceph_compare_super(struct super_block *sb, void *data)
{
	struct ceph_client *new = data;
	struct ceph_mount_args *args = &new->mount_args;
	struct ceph_client *other = ceph_sb_to_client(sb);
	int i;
	dout(10, "ceph_compare_super %p\n", sb);

	/* either compare fsid, or specified mon_hostname */
	if (args->flags & CEPH_MOUNT_FSID) {
		if (!ceph_fsid_equal(&args->fsid, &other->fsid)) {
			dout(30, "fsid doesn't match\n");
			return 0;
		}
	} else {
		/* do we share (a) monitor? */
		for (i = 0; i < args->num_mon; i++)
			if (ceph_monmap_contains(other->monc.monmap,
						 &args->mon_addr[i]))
				break;
		if (i == args->num_mon) {
			dout(30, "mon ip not part of monmap\n");
			return 0;
		}
		dout(10, "mon ip matches existing sb %p\n", sb);
	}
	if (args->sb_flags != other->mount_args.sb_flags) {
		dout(30, "flags differ\n");
		return 0;
	}
	return 1;
}

static int ceph_get_sb(struct file_system_type *fs_type,
		       int flags, const char *dev_name, void *data,
		       struct vfsmount *mnt)
{
	struct super_block *sb;
	struct ceph_client *client;
	int err;
	int (*compare_super)(struct super_block *, void *) = ceph_compare_super;
	const char *path;

	dout(25, "ceph_get_sb\n");

	/* create client (which we may/may not use) */
	client = ceph_create_client();
	if (IS_ERR(client))
		return PTR_ERR(client);

	err = parse_mount_args(flags, data, dev_name,
			       &client->mount_args, &path);
	if (err < 0)
		goto out;

	if (client->mount_args.flags & CEPH_MOUNT_NOSHARE)
		compare_super = 0;

	/* superblock */
	sb = sget(fs_type, compare_super, ceph_set_super, client);
	if (IS_ERR(sb)) {
		err = PTR_ERR(sb);
		goto out;
	}
	if (ceph_client(sb) != client) {
		ceph_destroy_client(client);
		client = ceph_client(sb);
		dout(20, "get_sb got existing client %p\n", client);
	} else
		dout(20, "get_sb using new client %p\n", client);

	err = ceph_mount(client, mnt, path);
	if (err < 0)
		goto out_splat;
	dout(22, "root ino %llx.%llx\n", ceph_vinop(mnt->mnt_root->d_inode));
	return 0;

out_splat:
	up_write(&sb->s_umount);
	deactivate_super(sb);
	goto out_final;
out:
	ceph_destroy_client(client);
out_final:
	dout(25, "ceph_get_sb fail %d\n", err);
	return err;
}

static void ceph_kill_sb(struct super_block *s)
{
	struct ceph_client *client = ceph_sb_to_client(s);
	dout(1, "kill_sb %p\n", s);
	ceph_mdsc_pre_umount(&client->mdsc);
	kill_anon_super(s);    /* will call put_super after sb is r/o */
	ceph_destroy_client(client);
}





/************************************/

static struct file_system_type ceph_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ceph",
	.get_sb		= ceph_get_sb,
	.kill_sb	= ceph_kill_sb,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};

struct kobject *ceph_kobj;

static int __init init_ceph(void)
{
	int ret = 0;

	dout(1, "init_ceph\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	ret = -ENOMEM;
	ceph_kobj = kobject_create_and_add("ceph", fs_kobj);
	if (!ceph_kobj)
		goto out;
#endif

	ret = ceph_proc_init();
	if (ret < 0)
		goto out_kobj;

	ret = ceph_msgr_init();
	if (ret < 0)
		goto out_proc;

	ret = init_inodecache();
	if (ret)
		goto out_msgr;

	ret = register_filesystem(&ceph_fs_type);
	if (ret)
		goto out_icache;
	return 0;

out_icache:
	destroy_inodecache();	
out_msgr:
	ceph_msgr_exit();
out_proc:
	ceph_proc_cleanup();
out_kobj:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	kobject_put(ceph_kobj);
	ceph_kobj = 0;
out:
#endif
	return ret;
}

static void __exit exit_ceph(void)
{
	dout(1, "exit_ceph\n");
	unregister_filesystem(&ceph_fs_type);
	destroy_inodecache();
	ceph_msgr_exit();
	ceph_proc_cleanup();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
	kobject_put(ceph_kobj);
	ceph_kobj = 0;
#endif
}

module_init(init_ceph);
module_exit(exit_ceph);

MODULE_AUTHOR("Patience Warnick <patience@newdream.net>");
MODULE_AUTHOR("Sage Weil <sage@newdream.net>");
MODULE_DESCRIPTION("Ceph filesystem for Linux");
MODULE_LICENSE("GPL");
