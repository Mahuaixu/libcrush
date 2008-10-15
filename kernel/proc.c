#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/ctype.h>
#include <asm/uaccess.h>

#include "ceph_debug.h"

int ceph_debug_proc = -1;
#define DOUT_MASK DOUT_MASK_PROC
#define DOUT_VAR ceph_debug_proc
#define DOUT_PREFIX "proc: "

#include "ceph_fs.h"
#include "super.h"


static int ceph_debug_level_read(char *page, char **start, off_t off,
		       int count, int *eof, void *data)
{
	int len;
	int *debug = data;

	len = sprintf(page, "%d\n", *debug);

	if ((len < 0) || (len <= off)) {
		*start = page;
		*eof = 1;
		return 0;
	}

	len -= off;

	*start = page + off;

	if (len > count)
		len = count;
	else
		*eof = 1;

	return len;
}

static int ceph_debug_mask_read(char *page, char **start, off_t off,
		       int count, int *eof, void *data)
{
	int len;
	int *debug = data;

	len = sprintf(page, "0x%x\n", *debug);

	if ((len < 0) || (len <= off)) {
		*start = page;
		*eof = 1;
		return 0;
	}

	len -= off;

	*start = page + off;

	if (len > count)
		len = count;
	else
		*eof = 1;

	return len;
}

static int ceph_debug_level_write(struct file *file, const char __user *buffer,
				unsigned long count, void *data)
{
#define PROC_STR_LEN	16
	char level_str[PROC_STR_LEN];
	int new_dl;
	int *debug = data;

	if ((count < 1) || (count > sizeof(level_str)-1))
		return -EINVAL;

	level_str[PROC_STR_LEN-1] = 0;

	if (copy_from_user(level_str, buffer, count))
		return -EFAULT;

	level_str[count] = 0;

	new_dl = simple_strtol(level_str, NULL, 0);

	*debug = new_dl;

	return count;
}

static int ceph_debug_mask_write(struct file *file, const char __user *buffer,
				unsigned long count, void *data)
{
	char  *mask_str, *tok;
	int new_dl;
	int *debug = data;

#define MAX_BUF	512
	if ((count < 1) || (count > MAX_BUF))
		return -EINVAL;

	mask_str = kmalloc(count+1, GFP_KERNEL);

	if (copy_from_user(mask_str, buffer, count))
		return -EFAULT;

	mask_str[count] = 0;

	do {
		tok = strsep(&mask_str, " \t\r\n");

		if (isdigit(*tok)) {
			new_dl = simple_strtol(tok, NULL, 0);
			*debug = new_dl;
		} else {
			int remove=0;
			int mask;

			if (*tok == '-') {
				remove=1;
				tok++;
			} else if (*tok =='+')
				tok++;

			mask = ceph_get_debug_mask(tok);

			if (remove)
				*debug &= ~mask;
			else
				*debug |= mask;

		}
	} while (mask_str);


	kfree(mask_str);

	return count;
}

static struct proc_dir_entry *proc_fs_ceph;

int ceph_proc_init(void)
{
	struct proc_dir_entry *pde;

	proc_fs_ceph = proc_mkdir("fs/ceph", NULL);
	if (!proc_fs_ceph)
		return -ENOMEM;

	proc_fs_ceph->owner = THIS_MODULE;
	pde = create_proc_read_entry("debug", 0,
				     proc_fs_ceph, ceph_debug_level_read,
				     &ceph_debug);
	if (pde)
		pde->write_proc = ceph_debug_level_write;
	pde = create_proc_read_entry("debug_msgr", 0,
				     proc_fs_ceph, ceph_debug_level_read,
				     &ceph_debug_msgr);
	if (pde)
		pde->write_proc = ceph_debug_level_write;
	pde = create_proc_read_entry("debug_console", 0,
				     proc_fs_ceph, ceph_debug_level_read,
				     &ceph_debug_console);
	if (pde)
		pde->write_proc = ceph_debug_level_write;

	pde = create_proc_read_entry("debug_mask", 0,
				     proc_fs_ceph, ceph_debug_mask_read,
				     &ceph_debug_mask);
	if (pde)
		pde->write_proc = ceph_debug_mask_write;

	return 0;
}

void ceph_proc_cleanup(void)
{
	remove_proc_entry("debug", proc_fs_ceph);
	remove_proc_entry("debug_msgr", proc_fs_ceph);
	remove_proc_entry("debug_console", proc_fs_ceph);
	remove_proc_entry("fs/ceph", NULL);
}
