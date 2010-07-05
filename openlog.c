#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#define LOGSIZE			(32*1024)
#define PRINT_BUF_SIZE		1024
#define FNAME			"/tmp/trace"
#define SEC			10

static char *logbuf = NULL;
static unsigned long in = 0;
static unsigned long out = 0;
static struct task_struct *bgt = NULL;

static struct file *jdo_filp_open(int dfd, const char *pathname,
		int open_flag, int mode, int acc_mode)
{
	static char print_buf[PRINT_BUF_SIZE];
	unsigned long l, len = LOGSIZE - in + out;	

	len = snprintf(print_buf, PRINT_BUF_SIZE, "%s: open(\"%s\", %d, %d) = %ld\n",
			current->comm, pathname, open_flag, mode, 0L);
	len = min(len, LOGSIZE - in + out);
	l = min(len, LOGSIZE - (in & (LOGSIZE - 1)));
	memcpy(logbuf + (in & (LOGSIZE - 1)), print_buf, l);
	memcpy(logbuf, print_buf + l, len - l);
	in += len;
	if (in - out > (LOGSIZE >> 2)) {
		if (bgt)
			wake_up_process(bgt);
	}
	jprobe_return();
	return 0;
}

static int log_bg_thread(void *info)
{
	struct file *fp;
	mm_segment_t fs;
	unsigned long l, len;

	printk("background thread log_bg_thread started, PID %d", current->pid);

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ*SEC);
	fp = filp_open(FNAME, O_RDWR|O_CREAT, 0600);
	if (IS_ERR(fp)) {
		printk("open file %s failed.\n", FNAME);
		goto out;
	}

	fs = get_fs();
	set_fs(KERNEL_DS);

	for (;;) {
		if (kthread_should_stop())
			break;
		set_current_state(TASK_INTERRUPTIBLE);
		if (in - out < (LOGSIZE >> 2)) {
			schedule();
			continue;
		} else
			__set_current_state(TASK_RUNNING);
		
		len = in - out;
		l = min(len, LOGSIZE - (out & (LOGSIZE - 1)));
		vfs_write(fp, logbuf + (out & (LOGSIZE - 1)), l, &fp->f_pos);
		vfs_write(fp, logbuf, len - l, &fp->f_pos);
		printk("write\n");
		
		out += len;

		cond_resched();
	}
	
	len = in - out;
	l = min(len, LOGSIZE - (out & (LOGSIZE - 1)));
	vfs_write(fp, logbuf + (out & (LOGSIZE - 1)), l, &fp->f_pos);
	vfs_write(fp, logbuf, len - l, &fp->f_pos);
	printk("write\n");

	filp_close(fp, NULL);
	set_fs(fs);
out:
	bgt = NULL;
	printk("background thread log_bg_thread stops\n");
	return 0;
}

static struct jprobe my_jprobe = {
	.entry			= jdo_filp_open,
	.kp = {
		.symbol_name	= "do_filp_open",
	},
};

static int __init openlog_init(void)
{
	int ret;

	logbuf = kzalloc(LOGSIZE, GFP_KERNEL);
	if (!logbuf)
		return -ENOMEM;

	ret = register_jprobe(&my_jprobe);
	if (ret < 0) {
		printk(KERN_INFO "register_jprobe failed, returned %d\n", ret);
		goto out_jprobe;
	}

	bgt = kthread_run(log_bg_thread, NULL, "log_bg_thread");
	if (IS_ERR(bgt)) {
		ret = PTR_ERR(bgt);
		bgt = NULL;
		printk("cannot spawn log_bg_thread, error %d\n", ret);
		goto out_bgt;
	}

	printk("success\n");
	return 0;
out_bgt:
	unregister_jprobe(&my_jprobe);
out_jprobe:
	kfree(logbuf);
	return ret;
}

static void __exit openlog_exit(void)
{
	unregister_jprobe(&my_jprobe);
	
	if (bgt)
		kthread_stop(bgt);
	kfree(logbuf);
}

module_init(openlog_init);
module_exit(openlog_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("xhacker");
