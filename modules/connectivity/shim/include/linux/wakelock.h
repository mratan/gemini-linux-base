/* Shim: Android wakelock wrapper over modern wakeup sources.
 *
 * The vendor 3.18 include/linux/wakelock.h embedded a struct wakeup_source
 * and used wakeup_source_init()/wakeup_source_trash(), both removed in v5.4
 * (transition completed by v5.19). This shim keeps the wake_lock_* call
 * sites in the vendored code unchanged by holding a registered wakeup
 * source pointer instead — the same approach frank-w used for the BPI-R2
 * port (BPI-Router-Linux 922ecdd3bb "Drop usage of wakelocks Android API,
 * use wake sources instead"). See API-CHURN-LOG.md entry W1.
 */
#ifndef _LINUX_WAKELOCK_H
#define _LINUX_WAKELOCK_H

#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>

enum {
	WAKE_LOCK_SUSPEND, /* Prevent suspend */
	WAKE_LOCK_TYPE_COUNT
};

struct wake_lock {
	struct wakeup_source *ws;
};

static inline void wake_lock_init(struct wake_lock *lock, int type,
				  const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}

static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}

static inline void wake_lock(struct wake_lock *lock)
{
	__pm_stay_awake(lock->ws);
}

static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}

static inline void wake_unlock(struct wake_lock *lock)
{
	__pm_relax(lock->ws);
}

static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

#endif
