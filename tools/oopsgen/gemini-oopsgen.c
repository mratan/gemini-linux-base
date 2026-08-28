// SPDX-License-Identifier: GPL-2.0
/*
 * gemini-oopsgen — take a kernel oops on purpose, so #72's can be read.
 *
 * WHY THIS EXISTS
 *
 * Every occurrence of #72 truncates its own report. Two independent channels
 * stop at the same place — netconsole, and the ramoops console zone, which is
 * a memcpy into DRAM with no transport to blame — and the point at which they
 * stop MOVES between occurrences:
 *
 *   #98  ... FSC = 0x05: level 1 translation fault   <- 6 lines
 *   #99  ...   ESR = 0x0000000096000004              <- 3 lines
 *
 * Those lines are inside mem_abort_decode(), which is a run of pr_alert()s
 * printing decoded bits of the ESR. There is nothing in there to fault on, and
 * a deterministic code-path fault would stop in the same place every time. So
 * the report is not being cut short by the bug — the machine is stopping while
 * PRINTING it, at whatever line it happens to have reached.
 *
 * That is testable, and this is what tests it: an oops on demand, in user
 * process context, so die() kills the writing process and the machine lives
 * (kernel.panic_on_oops is 0 here). Run it with the framebuffer console bound
 * and again with it unbound, and compare how far the report gets. /proc/consoles
 * on this board reads
 *
 *     ttyS0  netcon0  ramoops-1  tty0
 *
 * and tty0 is fbcon — last in the list, so a write that never returns there
 * stops every console for every line after it, which is exactly the shape of
 * what we see. If unbinding fbcon lets the report finish, #72 becomes readable
 * for the first time and the "sharpest lead" WARNING at __queue_work is very
 * likely part of the printing path rather than part of the bug.
 *
 * DELIBERATELY not a module parameter for the address: the default is the exact
 * value the real fault reads, so the synthetic report is comparable line for
 * line with the captured ones.
 *
 *     insmod gemini-oopsgen.ko
 *     echo 1 > /sys/kernel/debug/gemini_oopsgen/read_wild          # process
 *     echo 1 > /sys/kernel/debug/gemini_oopsgen/read_wild_wq       # kworker
 *     echo 1 > /sys/kernel/debug/gemini_oopsgen/read_wild_softirq  # ksoftirqd
 *
 * ONE SHOT PER BOOT, and the process-context trigger is one shot per boot in a
 * way worth knowing about: the task dies inside read_wild_write() without ever
 * returning through full_proxy_write(), so the module reference that took is
 * never dropped and rmmod blocks in D state for ever. That is inherent to
 * oopsing inside a file operation and is not worth engineering around; build a
 * second copy under another name if you need another go without rebooting.
 *
 * The softirq trigger panics ("Fatal exception in interrupt") by design, which
 * with kernel.panic = 0 means the machine sits until the hardware watchdog
 * resets it. Run it last.
 *
 * WHAT IT FOUND (build #100, 2026-08-28)
 *
 * All three print the report IN FULL -- Mem abort info, Data abort info,
 * "address between user and kernel address ranges", Internal error, the module
 * list, all thirty registers, the call trace, "end trace" -- at the same
 * address, with the same ESR (0x96000004), through the same four consoles in
 * the same order, with fbcon bound. So the console path, fbcon, netconsole,
 * the ramoops zone and the context of the fault are all exonerated: this
 * kernel is perfectly capable of printing this oops.
 *
 * Which means #72's truncation is not a property of the report. Something
 * stops the machine within a millisecond or two of the real fault, and finding
 * out what is the next question rather than this one.
 */

#define pr_fmt(fmt) "gemini-oopsgen: " fmt

#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

/*
 * The address the real fault reads, top 32 bits and all. Constant across 18
 * occurrences and several kernel builds, which is what makes it worth
 * reproducing exactly rather than using a round number.
 */
static unsigned long wild_addr = 0x000182027e8d5109UL;
module_param(wild_addr, ulong, 0644);
MODULE_PARM_DESC(wild_addr, "address to dereference (default: #72's own)");

static struct dentry *oopsgen_dir;

static noinline unsigned long oopsgen_touch(const char *where)
{
	volatile unsigned long *p = (volatile unsigned long *)wild_addr;

	pr_alert("about to read %px from %s — this is deliberate\n",
		 (void *)p, where);
	return *p;
}

static void oopsgen_work_fn(struct work_struct *w)
{
	oopsgen_touch("a workqueue");
}
static DECLARE_WORK(oopsgen_work, oopsgen_work_fn);

static void oopsgen_tasklet_fn(struct tasklet_struct *t)
{
	oopsgen_touch("a tasklet (softirq)");
}
static DECLARE_TASKLET(oopsgen_tasklet, oopsgen_tasklet_fn);

static ssize_t read_wild_write(struct file *f, const char __user *buf,
			       size_t len, loff_t *off)
{
	pr_alert("read back %lx, which should not have happened\n",
		 oopsgen_touch("process context"));
	return len;
}

/*
 * The same fault from a workqueue and from a softirq.
 *
 * Context is the one thing the synthetic report and the real one do not share.
 * From process context the report prints COMPLETELY on this board -- measured,
 * build #100: Mem abort info, Data abort info, "address between user and kernel
 * address ranges", Internal error, the module list, all thirty registers, the
 * call trace and "end trace", with fbcon bound and the same four consoles in
 * the same order. So the printing path is not what truncates #72, and neither
 * is the transport; both were reasonable suspects and both are now out.
 *
 * What is left is where the fault happens. #72's own trigger is always the same
 * shape -- sync, or drop_caches, walking every cached object -- so these two
 * put the identical dereference somewhere with no user process behind it.
 */
static ssize_t wild_wq_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	schedule_work(&oopsgen_work);
	return len;
}

static ssize_t wild_softirq_write(struct file *f, const char __user *buf,
				  size_t len, loff_t *off)
{
	tasklet_schedule(&oopsgen_tasklet);
	return len;
}

static const struct file_operations read_wild_fops = {
	.owner	= THIS_MODULE,
	.write	= read_wild_write,
	.llseek	= noop_llseek,
};

static const struct file_operations wild_wq_fops = {
	.owner	= THIS_MODULE,
	.write	= wild_wq_write,
	.llseek	= noop_llseek,
};

static const struct file_operations wild_softirq_fops = {
	.owner	= THIS_MODULE,
	.write	= wild_softirq_write,
	.llseek	= noop_llseek,
};

static int __init oopsgen_init(void)
{
	oopsgen_dir = debugfs_create_dir("gemini_oopsgen", NULL);
	debugfs_create_file("read_wild", 0200, oopsgen_dir, NULL,
			    &read_wild_fops);
	debugfs_create_file("read_wild_wq", 0200, oopsgen_dir, NULL,
			    &wild_wq_fops);
	debugfs_create_file("read_wild_softirq", 0200, oopsgen_dir, NULL,
			    &wild_softirq_fops);
	pr_info("armed: echo 1 > /sys/kernel/debug/gemini_oopsgen/read_wild (addr %px)\n",
		(void *)wild_addr);
	return 0;
}

static void __exit oopsgen_exit(void)
{
	debugfs_remove_recursive(oopsgen_dir);
}

module_init(oopsgen_init);
module_exit(oopsgen_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Deliberate kernel oops, to find out why #72's reports truncate");
