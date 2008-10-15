#ifndef _FS_CEPH_DEBUG_H
#define _FS_CEPH_DEBUG_H

#include <linux/string.h>

extern int ceph_debug;
extern int ceph_debug_mask;
extern int ceph_debug_console;

/* different debug level for the different modules */
extern int ceph_debug_addr;
extern int ceph_debug_caps;
extern int ceph_debug_dir;
extern int ceph_debug_export;
extern int ceph_debug_file;
extern int ceph_debug_inode;
extern int ceph_debug_ioctl;
extern int ceph_debug_mdsc;
extern int ceph_debug_mdsmap;
extern int ceph_debug_msgr;
extern int ceph_debug_mon;
extern int ceph_debug_osdc;
extern int ceph_debug_osdmap;
extern int ceph_debug_snap;
extern int ceph_debug_super;
extern int ceph_debug_protocol;
extern int ceph_debug_proc;

#define DOUT_MASK_ADDR		0x00000001
#define DOUT_MASK_CAPS		0x00000002
#define DOUT_MASK_DIR		0x00000004
#define DOUT_MASK_EXPORT	0x00000008
#define DOUT_MASK_FILE		0x00000010
#define DOUT_MASK_INODE		0x00000020
#define DOUT_MASK_IOCTL		0x00000040
#define DOUT_MASK_MDSC		0x00000080
#define DOUT_MASK_MDSMAP	0x00000100
#define DOUT_MASK_MSGR		0x00000200
#define DOUT_MASK_MON		0x00000400
#define DOUT_MASK_OSDC		0x00000800
#define DOUT_MASK_OSDMAP	0x00001000
#define DOUT_MASK_SNAP		0x00002000
#define DOUT_MASK_SUPER		0x00004000
#define DOUT_MASK_PROTOCOL	0x00008000
#define DOUT_MASK_PROC		0x00010000

#define DOUT_UNMASKABLE	0x80000000

struct _debug_mask_name {
	int mask;
	char *name;
};

static struct _debug_mask_name _debug_mask_names[] = {
		{DOUT_MASK_ADDR, "addr"},
		{DOUT_MASK_CAPS, "caps"},
		{DOUT_MASK_DIR, "dir"},
		{DOUT_MASK_EXPORT, "export"},
		{DOUT_MASK_FILE, "file"},
		{DOUT_MASK_INODE, "inode"},
		{DOUT_MASK_IOCTL, "ioctl"},
		{DOUT_MASK_MDSC, "mdsc"},
		{DOUT_MASK_MDSMAP, "mdsmap"},
		{DOUT_MASK_MSGR, "msgr"},	
		{DOUT_MASK_MON, "mon"},
		{DOUT_MASK_OSDC, "osdc"},
		{DOUT_MASK_OSDMAP, "osdmap"},
		{DOUT_MASK_SNAP, "snap"},
		{DOUT_MASK_SUPER, "super"},
		{DOUT_MASK_PROTOCOL, "protocol"},
		{DOUT_MASK_PROC, "proc"},
		{0, NULL}	
};

static inline int ceph_get_debug_mask(char *name)
{
	int i=0;

	while (_debug_mask_names[i].name) {
		if (strcmp(_debug_mask_names[i].name, name) == 0)
			return _debug_mask_names[i].mask;
		i++;
	}

	return 0;
}

#endif


