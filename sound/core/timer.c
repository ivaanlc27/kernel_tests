/*
 *  Timers abstract layer
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sched/signal.h>
#include <sound/core.h>
#include <sound/timer.h>
#include <sound/control.h>
#include <sound/info.h>
#include <sound/minors.h>
#include <sound/initval.h>
#include <linux/kmod.h>

/* internal flags */
#define SNDRV_TIMER_IFLG_PAUSED		0x00010000

#if IS_ENABLED(CONFIG_SND_HRTIMER)
#define DEFAULT_TIMER_LIMIT 4
#else
#define DEFAULT_TIMER_LIMIT 1
#endif

static int timer_limit = DEFAULT_TIMER_LIMIT;
static int timer_tstamp_monotonic = 1;
MODULE_AUTHOR("Jaroslav Kysela <perex@perex.cz>, Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("ALSA timer interface");
MODULE_LICENSE("GPL");
module_param(timer_limit, int, 0444);
MODULE_PARM_DESC(timer_limit, "Maximum global timers in system.");
module_param(timer_tstamp_monotonic, int, 0444);
MODULE_PARM_DESC(timer_tstamp_monotonic, "Use posix monotonic clock source for timestamps (default).");

MODULE_ALIAS_CHARDEV(CONFIG_SND_MAJOR, SNDRV_MINOR_TIMER);
MODULE_ALIAS("devname:snd/timer");

struct snd_timer_user {
	struct snd_timer_instance *timeri;
	int tread;		/* enhanced read with timestamps and events */
	unsigned long ticks;
	unsigned long overrun;
	int qhead;
	int qtail;
	int qused;
	int queue_size;
	bool disconnected;
	struct snd_timer_read *queue;
	struct snd_timer_tread *tqueue;
	spinlock_t qlock;
	unsigned long last_resolution;
	unsigned int filter;
	struct timespec tstamp;		/* trigger tstamp */
	wait_queue_head_t qchange_sleep;
	struct fasync_struct *fasync;
	struct mutex ioctl_lock;
};

/* list of timers */
static LIST_HEAD(snd_timer_list);

/* list of slave instances */
static LIST_HEAD(snd_timer_slave_list);

/* lock for slave active lists */
static DEFINE_SPINLOCK(slave_active_lock);

#define MAX_SLAVE_INSTANCES	1000
static int num_slaves;

static DEFINE_MUTEX(register_mutex);

static int snd_timer_free(struct snd_timer *timer);
static int snd_timer_dev_free(struct snd_device *device);
static int snd_timer_dev_register(struct snd_device *device);
static int snd_timer_dev_disconnect(struct snd_device *device);

static void snd_timer_reschedule(struct snd_timer * timer, unsigned long ticks_left);

/*
 * create a timer instance with the given owner string.
 * when timer is not NULL, increments the module counter
 */
static struct snd_timer_instance *snd_timer_instance_new(char *owner,
							 struct snd_timer *timer)
{
	struct snd_timer_instance *timeri;
	timeri = kzalloc(sizeof(*timeri), GFP_KERNEL);
	if (timeri == NULL)
		return NULL;
	timeri->owner = kstrdup(owner, GFP_KERNEL);
	if (! timeri->owner) {
		kfree(timeri);
		return NULL;
	}
	INIT_LIST_HEAD(&timeri->open_list);
	INIT_LIST_HEAD(&timeri->active_list);
	INIT_LIST_HEAD(&timeri->ack_list);
	INIT_LIST_HEAD(&timeri->slave_list_head);
	INIT_LIST_HEAD(&timeri->slave_active_head);

	timeri->timer = timer;
	if (timer && !try_module_get(timer->module)) {
		kfree(timeri->owner);
		kfree(timeri);
		return NULL;
	}

	return timeri;
}

/*
 * find a timer instance from the given timer id
 */
static struct snd_timer *snd_timer_find(struct snd_timer_id *tid)
{
	struct snd_timer *timer = NULL;

	list_for_each_entry(timer, &snd_timer_list, device_list) {
		if (timer->tmr_class != tid->dev_class)
			continue;
		if ((timer->tmr_class == SNDRV_TIMER_CLASS_CARD ||
		     timer->tmr_class == SNDRV_TIMER_CLASS_PCM) &&
		    (timer->card == NULL ||
		     timer->card->number != tid->card))
			continue;
		if (timer->tmr_device != tid->device)
			continue;
		if (timer->tmr_subdevice != tid->subdevice)
			continue;
		return timer;
	}
	return NULL;
}

#ifdef CONFIG_MODULES

static void snd_timer_request(struct snd_timer_id *tid)
{
	switch (tid->dev_class) {
	case SNDRV_TIMER_CLASS_GLOBAL:
		if (tid->device < timer_limit)
			request_module("snd-timer-%i", tid->device);
		break;
	case SNDRV_TIMER_CLASS_CARD:
	case SNDRV_TIMER_CLASS_PCM:
		if (tid->card < snd_ecards_limit)
			request_module("snd-card-%i", tid->card);
		break;
	default:
		break;
	}
}

#endif

/*
 * look for a master instance matching with the slave id of the given slave.
 * when found, relink the open_link of the slave.
 *
 * call this with register_mutex down.
 */
static int snd_timer_check_slave(struct snd_timer_instance *slave)
{
	struct snd_timer *timer;
	struct snd_timer_instance *master;

	/* FIXME: it's really dumb to look up all entries.. */
	list_for_each_entry(timer, &snd_timer_list, device_list) {
		list_for_each_entry(master, &timer->open_list_head, open_list) {
			if (slave->slave_class == master->slave_class &&
			    slave->slave_id == master->slave_id) {
				if (master->timer->num_instances >=
				    master->timer->max_instances)
					return -EBUSY;
				list_move_tail(&slave->open_list,
					       &master->slave_list_head);
				master->timer->num_instances++;
				spin_lock_irq(&slave_active_lock);
				slave->master = master;
				slave->timer = master->timer;
				spin_unlock_irq(&slave_active_lock);
				return 0;
			}
		}
	}
	return 0;
}

/*
 * look for slave instances matching with the slave id of the given master.
 * when found, relink the open_link of slaves.
 *
 * call this with register_mutex down.
 */
static int snd_timer_check_master(struct snd_timer_instance *master)
{
	struct snd_timer_instance *slave, *tmp;

	/* check all pending slaves */
	list_for_each_entry_safe(slave, tmp, &snd_timer_slave_list, open_list) {
		if (slave->slave_class == master->slave_class &&
		    slave->slave_id == master->slave_id) {
			if (master->timer->num_instances >=
			    master->timer->max_instances)
				return -EBUSY;
			list_move_tail(&slave->open_list, &master->slave_list_head);
			master->timer->num_instances++;
			spin_lock_irq(&slave_active_lock);
			spin_lock(&master->timer->lock);
			slave->master = master;
			slave->timer = master->timer;
			if (slave->flags & SNDRV_TIMER_IFLG_RUNNING)
				list_add_tail(&slave->active_list,
					      &master->slave_active_head);
			spin_unlock(&master->timer->lock);
			spin_unlock_irq(&slave_active_lock);
		}
	}
	return 0;
}

static int snd_timer_close_locked(struct snd_timer_instance *timeri,
				  struct device **card_devp_to_put);

/*
 * open a timer instance
 * when opening a master, the slave id must be here given.
 */
int snd_timer_open(struct snd_timer_instance **ti,
		   char *owner, struct snd_timer_id *tid,
		   unsigned int slave_id)
{
	struct snd_timer *timer;
	struct snd_timer_instance *timeri = NULL;
	struct device *card_dev_to_put = NULL;
	int err;

	mutex_lock(&register_mutex);
	if (tid->dev_class == SNDRV_TIMER_CLASS_SLAVE) {
		/* open a slave instance */
		if (tid->dev_sclass <= SNDRV_TIMER_SCLASS_NONE ||
		    tid->dev_sclass > SNDRV_TIMER_SCLASS_OSS_SEQUENCER) {
			pr_debug("ALSA: timer: invalid slave class %i\n",
				 tid->dev_sclass);
			err = -EINVAL;
			goto unlock;
		}
		if (num_slaves >= MAX_SLAVE_INSTANCES) {
			err = -EBUSY;
			goto unlock;
		}
		timeri = snd_timer_instance_new(owner, NULL);
		if (!timeri) {
			err = -ENOMEM;
			goto unlock;
		}
		timeri->slave_class = tid->dev_sclass;
		timeri->slave_id = tid->device;
		timeri->flags |= SNDRV_TIMER_IFLG_SLAVE;
		list_add_tail(&timeri->open_list, &snd_timer_slave_list);
		num_slaves++;
		err = snd_timer_check_slave(timeri);
		if (err < 0) {
			snd_timer_close_locked(timeri, &card_dev_to_put);
			timeri = NULL;
		}
		goto unlock;
	}

	/* open a master instance */
	timer = snd_timer_find(tid);
#ifdef CONFIG_MODULES
	if (!timer) {
		mutex_unlock(&register_mutex);
		snd_timer_request(tid);
		mutex_lock(&register_mutex);
		timer = snd_timer_find(tid);
	}
#endif
	if (!timer) {
		err = -ENODEV;
		goto unlock;
	}
	if (!list_empty(&timer->open_list_head)) {
		struct snd_timer_instance *t =
			list_entry(timer->open_list_head.next,
				    struct snd_timer_instance, open_list);
		if (t->flags & SNDRV_TIMER_IFLG_EXCLUSIVE) {
			err = -EBUSY;
			goto unlock;
		}
	}
	if (timer->num_instances >= timer->max_instances) {
		err = -EBUSY;
		goto unlock;
	}
	timeri = snd_timer_instance_new(owner, timer);
	if (!timeri) {
		err = -ENOMEM;
		goto unlock;
	}
	/* take a card refcount for safe disconnection */
	if (timer->card)
		get_device(&timer->card->card_dev);
	timeri->slave_class = tid->dev_sclass;
	timeri->slave_id = slave_id;

	if (list_empty(&timer->open_list_head) && timer->hw.open) {
		err = timer->hw.open(timer);
		if (err) {
			kfree(timeri->owner);
			kfree(timeri);
			timeri = NULL;

			if (timer->card)
				card_dev_to_put = &timer->card->card_dev;
			module_put(timer->module);
			goto unlock;
		}
	}

	list_add_tail(&timeri->open_list, &timer->open_list_head);
	timer->num_instances++;
	err = snd_timer_check_master(timeri);
	if (err < 0) {
		snd_timer_close_locked(timeri, &card_dev_to_put);
		timeri = NULL;
	}

 unlock:
	mutex_unlock(&register_mutex);
	/* put_device() is called after unlock for avoiding deadlock */
	if (card_dev_to_put)
		put_device(card_dev_to_put);
	*ti = timeri;
	return err;
}
EXPORT_SYMBOL(snd_timer_open);

/*
 * close a timer instance
 * call this with register_mutex down.
 */
static int snd_timer_close_locked(struct snd_timer_instance *timeri,
				  struct device **card_devp_to_put)
{
	struct snd_timer *timer = NULL;
	struct snd_timer_instance *slave, *tmp;

	list_del(&timeri->open_list);
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		num_slaves--;

	/* force to stop the timer */
	snd_timer_stop(timeri);

	timer = timeri->timer;
	if (timer) {
		timer->num_instances--;
		/* wait, until the active callback is finished */
		spin_lock_irq(&timer->lock);
		while (timeri->flags & SNDRV_TIMER_IFLG_CALLBACK) {
			spin_unlock_irq(&timer->lock);
			udelay(10);
			spin_lock_irq(&timer->lock);
		}
		spin_unlock_irq(&timer->lock);

		/* remove slave links */
		spin_lock_irq(&slave_active_lock);
		spin_lock(&timer->lock);
		list_for_each_entry_safe(slave, tmp, &timeri->slave_list_head,
					 open_list) {
			list_move_tail(&slave->open_list, &snd_timer_slave_list);
			timer->num_instances--;
			slave->master = NULL;
			slave->timer = NULL;
			list_del_init(&slave->ack_list);
			list_del_init(&slave->active_list);
		}
		spin_unlock(&timer->lock);
		spin_unlock_irq(&slave_active_lock);

		/* slave doesn't need to release timer resources below */
		if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
			timer = NULL;
	}

	if (timeri->private_free)
		timeri->private_free(timeri);
	kfree(timeri->owner);
	kfree(timeri);

	if (timer) {
		if (list_empty(&timer->open_list_head) && timer->hw.close)
			timer->hw.close(timer);
		/* release a card refcount for safe disconnection */
		if (timer->card)
			*card_devp_to_put = &timer->card->card_dev;
		module_put(timer->module);
	}

	return 0;
}

/*
 * close a timer instance
 */
int snd_timer_close(struct snd_timer_instance *timeri)
{
	struct device *card_dev_to_put = NULL;
	int err;

	if (snd_BUG_ON(!timeri))
		return -ENXIO;

	mutex_lock(&register_mutex);
	err = snd_timer_close_locked(timeri, &card_dev_to_put);
	mutex_unlock(&register_mutex);
	/* put_device() is called after unlock for avoiding deadlock */
	if (card_dev_to_put)
		put_device(card_dev_to_put);
	return err;
}
EXPORT_SYMBOL(snd_timer_close);

static unsigned long snd_timer_hw_resolution(struct snd_timer *timer)
{
	if (timer->hw.c_resolution)
		return timer->hw.c_resolution(timer);
	else
		return timer->hw.resolution;
}

unsigned long snd_timer_resolution(struct snd_timer_instance *timeri)
{
	struct snd_timer * timer;
	unsigned long ret = 0;
	unsigned long flags;

	if (timeri == NULL)
		return 0;
	timer = timeri->timer;
	if (timer) {
		spin_lock_irqsave(&timer->lock, flags);
		ret = snd_timer_hw_resolution(timer);
		spin_unlock_irqrestore(&timer->lock, flags);
	}
	return ret;
}
EXPORT_SYMBOL(snd_timer_resolution);

static void snd_timer_notify1(struct snd_timer_instance *ti, int event)
{
	struct snd_timer *timer = ti->timer;
	unsigned long resolution = 0;
	struct snd_timer_instance *ts;
	struct timespec tstamp;

	if (timer_tstamp_monotonic)
		ktime_get_ts(&tstamp);
	else
		getnstimeofday(&tstamp);
	if (snd_BUG_ON(event < SNDRV_TIMER_EVENT_START ||
		       event > SNDRV_TIMER_EVENT_PAUSE))
		return;
	if (timer &&
	    (event == SNDRV_TIMER_EVENT_START ||
	     event == SNDRV_TIMER_EVENT_CONTINUE))
		resolution = snd_timer_hw_resolution(timer);
	if (ti->ccallback)
		ti->ccallback(ti, event, &tstamp, resolution);
	if (ti->flags & SNDRV_TIMER_IFLG_SLAVE)
		return;
	if (timer == NULL)
		return;
	if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
		return;
	list_for_each_entry(ts, &ti->slave_active_head, active_list)
		if (ts->ccallback)
			ts->ccallback(ts, event + 100, &tstamp, resolution);
}

/* start/continue a master timer */
static int snd_timer_start1(struct snd_timer_instance *timeri,
			    bool start, unsigned long ticks)
{
	struct snd_timer *timer;
	int result;
	unsigned long flags;

	timer = timeri->timer;
	if (!timer)
		return -EINVAL;

	spin_lock_irqsave(&timer->lock, flags);
	if (timer->card && timer->card->shutdown) {
		result = -ENODEV;
		goto unlock;
	}
	if (timeri->flags & (SNDRV_TIMER_IFLG_RUNNING |
			     SNDRV_TIMER_IFLG_START)) {
		result = -EBUSY;
		goto unlock;
	}

	/* check the actual time for the start tick;
	 * bail out as error if it's way too low (< 100us)
	 */
	if (start) {
		if ((u64)snd_timer_hw_resolution(timer) * ticks < 100000) {
			result = -EINVAL;
			goto unlock;
		}
	}

	if (start)
		timeri->ticks = timeri->cticks = ticks;
	else if (!timeri->cticks)
		timeri->cticks = 1;
	timeri->pticks = 0;

	list_move_tail(&timeri->active_list, &timer->active_list_head);
	if (timer->running) {
		if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
			goto __start_now;
		timer->flags |= SNDRV_TIMER_FLG_RESCHED;
		timeri->flags |= SNDRV_TIMER_IFLG_START;
		result = 1; /* delayed start */
	} else {
		if (start)
			timer->sticks = ticks;
		timer->hw.start(timer);
	      __start_now:
		timer->running++;
		timeri->flags |= SNDRV_TIMER_IFLG_RUNNING;
		result = 0;
	}
	snd_timer_notify1(timeri, start ? SNDRV_TIMER_EVENT_START :
			  SNDRV_TIMER_EVENT_CONTINUE);
 unlock:
	spin_unlock_irqrestore(&timer->lock, flags);
	return result;
}

/* start/continue a slave timer */
static int snd_timer_start_slave(struct snd_timer_instance *timeri,
				 bool start)
{
	unsigned long flags;

	spin_lock_irqsave(&slave_active_lock, flags);
	if (timeri->flags & SNDRV_TIMER_IFLG_RUNNING) {
		spin_unlock_irqrestore(&slave_active_lock, flags);
		return -EBUSY;
	}
	timeri->flags |= SNDRV_TIMER_IFLG_RUNNING;
	if (timeri->master && timeri->timer) {
		spin_lock(&timeri->timer->lock);
		list_add_tail(&timeri->active_list,
			      &timeri->master->slave_active_head);
		snd_timer_notify1(timeri, start ? SNDRV_TIMER_EVENT_START :
				  SNDRV_TIMER_EVENT_CONTINUE);
		spin_unlock(&timeri->timer->lock);
	}
	spin_unlock_irqrestore(&slave_active_lock, flags);
	return 1; /* delayed start */
}

/* stop/pause a master timer */
static int snd_timer_stop1(struct snd_timer_instance *timeri, bool stop)
{
	struct snd_timer *timer;
	int result = 0;
	unsigned long flags;

	timer = timeri->timer;
	if (!timer)
		return -EINVAL;
	spin_lock_irqsave(&timer->lock, flags);
	if (!(timeri->flags & (SNDRV_TIMER_IFLG_RUNNING |
			       SNDRV_TIMER_IFLG_START))) {
		result = -EBUSY;
		goto unlock;
	}
	list_del_init(&timeri->ack_list);
	list_del_init(&timeri->active_list);
	if (timer->card && timer->card->shutdown)
		goto unlock;
	if (stop) {
		timeri->cticks = timeri->ticks;
		timeri->pticks = 0;
	}
	if ((timeri->flags & SNDRV_TIMER_IFLG_RUNNING) &&
	    !(--timer->running)) {
		timer->hw.stop(timer);
		if (timer->flags & SNDRV_TIMER_FLG_RESCHED) {
			timer->flags &= ~SNDRV_TIMER_FLG_RESCHED;
			snd_timer_reschedule(timer, 0);
			if (timer->flags & SNDRV_TIMER_FLG_CHANGE) {
				timer->flags &= ~SNDRV_TIMER_FLG_CHANGE;
				timer->hw.start(timer);
			}
		}
	}
	timeri->flags &= ~(SNDRV_TIMER_IFLG_RUNNING | SNDRV_TIMER_IFLG_START);
	if (stop)
		timeri->flags &= ~SNDRV_TIMER_IFLG_PAUSED;
	else
		timeri->flags |= SNDRV_TIMER_IFLG_PAUSED;
	snd_timer_notify1(timeri, stop ? SNDRV_TIMER_EVENT_STOP :
			  SNDRV_TIMER_EVENT_PAUSE);
 unlock:
	spin_unlock_irqrestore(&timer->lock, flags);
	return result;
}

/* stop/pause a slave timer */
static int snd_timer_stop_slave(struct snd_timer_instance *timeri, bool stop)
{
	unsigned long flags;

	spin_lock_irqsave(&slave_active_lock, flags);
	if (!(timeri->flags & SNDRV_TIMER_IFLG_RUNNING)) {
		spin_unlock_irqrestore(&slave_active_lock, flags);
		return -EBUSY;
	}
	timeri->flags &= ~SNDRV_TIMER_IFLG_RUNNING;
	if (timeri->timer) {
		spin_lock(&timeri->timer->lock);
		list_del_init(&timeri->ack_list);
		list_del_init(&timeri->active_list);
		snd_timer_notify1(timeri, stop ? SNDRV_TIMER_EVENT_STOP :
				  SNDRV_TIMER_EVENT_PAUSE);
		spin_unlock(&timeri->timer->lock);
	}
	spin_unlock_irqrestore(&slave_active_lock, flags);
	return 0;
}

/*
 *  start the timer instance
 */
int snd_timer_start(struct snd_timer_instance *timeri, unsigned int ticks)
{
	if (timeri == NULL || ticks < 1)
		return -EINVAL;
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		return snd_timer_start_slave(timeri, true);
	else
		return snd_timer_start1(timeri, true, ticks);
}
EXPORT_SYMBOL(snd_timer_start);

/*
 * stop the timer instance.
 *
 * do not call this from the timer callback!
 */
int snd_timer_stop(struct snd_timer_instance *timeri)
{
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		return snd_timer_stop_slave(timeri, true);
	else
		return snd_timer_stop1(timeri, true);
}
EXPORT_SYMBOL(snd_timer_stop);

/*
 * start again..  the tick is kept.
 */
int snd_timer_continue(struct snd_timer_instance *timeri)
{
	/* timer can continue only after pause */
	if (!(timeri->flags & SNDRV_TIMER_IFLG_PAUSED))
		return -EINVAL;

	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		return snd_timer_start_slave(timeri, false);
	else
		return snd_timer_start1(timeri, false, 0);
}
EXPORT_SYMBOL(snd_timer_continue);

/*
 * pause.. remember the ticks left
 */
int snd_timer_pause(struct snd_timer_instance * timeri)
{
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		return snd_timer_stop_slave(timeri, false);
	else
		return snd_timer_stop1(timeri, false);
}
EXPORT_SYMBOL(snd_timer_pause);

/*
 * reschedule the timer
 *
 * start pending instances and check the scheduling ticks.
 * when the scheduling ticks is changed set CHANGE flag to reprogram the timer.
 */
static void snd_timer_reschedule(struct snd_timer * timer, unsigned long ticks_left)
{
	struct snd_timer_instance *ti;
	unsigned long ticks = ~0UL;

	list_for_each_entry(ti, &timer->active_list_head, active_list) {
		if (ti->flags & SNDRV_TIMER_IFLG_START) {
			ti->flags &= ~SNDRV_TIMER_IFLG_START;
			ti->flags |= SNDRV_TIMER_IFLG_RUNNING;
			timer->running++;
		}
		if (ti->flags & SNDRV_TIMER_IFLG_RUNNING) {
			if (ticks > ti->cticks)
				ticks = ti->cticks;
		}
	}
	if (ticks == ~0UL) {
		timer->flags &= ~SNDRV_TIMER_FLG_RESCHED;
		return;
	}
	if (ticks > timer->hw.ticks)
		ticks = timer->hw.ticks;
	if (ticks_left != ticks)
		timer->flags |= SNDRV_TIMER_FLG_CHANGE;
	timer->sticks = ticks;
}

/*
 * timer tasklet
 *
 */
static void snd_timer_tasklet(unsigned long arg)
{
	struct snd_timer *timer = (struct snd_timer *) arg;
	struct snd_timer_instance *ti;
	struct list_head *p;
	unsigned long resolution, ticks;
	unsigned long flags;

	if (timer->card && timer->card->shutdown)
		return;

	spin_lock_irqsave(&timer->lock, flags);
	/* now process all callbacks */
	while (!list_empty(&timer->sack_list_head)) {
		p = timer->sack_list_head.next;		/* get first item */
		ti = list_entry(p, struct snd_timer_instance, ack_list);

		/* remove from ack_list and make empty */
		list_del_init(p);

		ticks = ti->pticks;
		ti->pticks = 0;
		resolution = ti->resolution;

		ti->flags |= SNDRV_TIMER_IFLG_CALLBACK;
		spin_unlock(&timer->lock);
		if (ti->callback)
			ti->callback(ti, resolution, ticks);
		spin_lock(&timer->lock);
		ti->flags &= ~SNDRV_TIMER_IFLG_CALLBACK;
	}
	spin_unlock_irqrestore(&timer->lock, flags);
}

/*
 * timer interrupt
 *
 * ticks_left is usually equal to timer->sticks.
 *
 */
void snd_timer_interrupt(struct snd_timer * timer, unsigned long ticks_left)
{
	struct snd_timer_instance *ti, *ts, *tmp;
	unsigned long resolution, ticks;
	struct list_head *p, *ack_list_head;
	unsigned long flags;
	int use_tasklet = 0;

	if (timer == NULL)
		return;

	if (timer->card && timer->card->shutdown)
		return;

	spin_lock_irqsave(&timer->lock, flags);

	/* remember the current resolution */
	resolution = snd_timer_hw_resolution(timer);

	/* loop for all active instances
	 * Here we cannot use list_for_each_entry because the active_list of a
	 * processed instance is relinked to done_list_head before the callback
	 * is called.
	 */
	list_for_each_entry_safe(ti, tmp, &timer->active_list_head,
				 active_list) {
		if (!(ti->flags & SNDRV_TIMER_IFLG_RUNNING))
			continue;
		ti->pticks += ticks_left;
		ti->resolution = resolution;
		if (ti->cticks < ticks_left)
			ti->cticks = 0;
		else
			ti->cticks -= ticks_left;
		if (ti->cticks) /* not expired */
			continue;
		if (ti->flags & SNDRV_TIMER_IFLG_AUTO) {
			ti->cticks = ti->ticks;
		} else {
			ti->flags &= ~SNDRV_TIMER_IFLG_RUNNING;
			--timer->running;
			list_del_init(&ti->active_list);
		}
		if ((timer->hw.flags & SNDRV_TIMER_HW_TASKLET) ||
		    (ti->flags & SNDRV_TIMER_IFLG_FAST))
			ack_list_head = &timer->ack_list_head;
		else
			ack_list_head = &timer->sack_list_head;
		if (list_empty(&ti->ack_list))
			list_add_tail(&ti->ack_list, ack_list_head);
		list_for_each_entry(ts, &ti->slave_active_head, active_list) {
			ts->pticks = ti->pticks;
			ts->resolution = resolution;
			if (list_empty(&ts->ack_list))
				list_add_tail(&ts->ack_list, ack_list_head);
		}
	}
	if (timer->flags & SNDRV_TIMER_FLG_RESCHED)
		snd_timer_reschedule(timer, timer->sticks);
	if (timer->running) {
		if (timer->hw.flags & SNDRV_TIMER_HW_STOP) {
			timer->hw.stop(timer);
			timer->flags |= SNDRV_TIMER_FLG_CHANGE;
		}
		if (!(timer->hw.flags & SNDRV_TIMER_HW_AUTO) ||
		    (timer->flags & SNDRV_TIMER_FLG_CHANGE)) {
			/* restart timer */
			timer->flags &= ~SNDRV_TIMER_FLG_CHANGE;
			timer->hw.start(timer);
		}
	} else {
		timer->hw.stop(timer);
	}

	/* now process all fast callbacks */
	while (!list_empty(&timer->ack_list_head)) {
		p = timer->ack_list_head.next;		/* get first item */
		ti = list_entry(p, struct snd_timer_instance, ack_list);

		/* remove from ack_list and make empty */
		list_del_init(p);

		ticks = ti->pticks;
		ti->pticks = 0;

		ti->flags |= SNDRV_TIMER_IFLG_CALLBACK;
		spin_unlock(&timer->lock);
		if (ti->callback)
			ti->callback(ti, resolution, ticks);
		spin_lock(&timer->lock);
		ti->flags &= ~SNDRV_TIMER_IFLG_CALLBACK;
	}

	/* do we have any slow callbacks? */
	use_tasklet = !list_empty(&timer->sack_list_head);
	spin_unlock_irqrestore(&timer->lock, flags);

	if (use_tasklet)
		tasklet_schedule(&timer->task_queue);
}
EXPORT_SYMBOL(snd_timer_interrupt);

/*

 */

int snd_timer_new(struct snd_card *card, char *id, struct snd_timer_id *tid,
		  struct snd_timer **rtimer)
{
	struct snd_timer *timer;
	int err;
	static struct snd_device_ops ops = {
		.dev_free = snd_timer_dev_free,
		.dev_register = snd_timer_dev_register,
		.dev_disconnect = snd_timer_dev_disconnect,
	};

	if (snd_BUG_ON(!tid))
		return -EINVAL;
	if (tid->dev_class == SNDRV_TIMER_CLASS_CARD ||
	    tid->dev_class == SNDRV_TIMER_CLASS_PCM) {
		if (WARN_ON(!card))
			return -EINVAL;
	}
	if (rtimer)
		*rtimer = NULL;
	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer)
		return -ENOMEM;
	timer->tmr_class = tid->dev_class;
	timer->card = card;
	timer->tmr_device = tid->device;
	timer->tmr_subdevice = tid->subdevice;
	if (id)
		strlcpy(timer->id, id, sizeof(timer->id));
	timer->sticks = 1;
	INIT_LIST_HEAD(&timer->device_list);
	INIT_LIST_HEAD(&timer->open_list_head);
	INIT_LIST_HEAD(&timer->active_list_head);
	INIT_LIST_HEAD(&timer->ack_list_head);
	INIT_LIST_HEAD(&timer->sack_list_head);
	spin_lock_init(&timer->lock);
	tasklet_init(&timer->task_queue, snd_timer_tasklet,
		     (unsigned long)timer);
	timer->max_instances = 1000; /* default limit per timer */
	if (card != NULL) {
		timer->module = card->module;
		err = snd_device_new(card, SNDRV_DEV_TIMER, timer, &ops);
		if (err < 0) {
			snd_timer_free(timer);
			return err;
		}
	}
	if (rtimer)
		*rtimer = timer;
	return 0;
}
EXPORT_SYMBOL(snd_timer_new);

static int snd_timer_free(struct snd_timer *timer)
{
	if (!timer)
		return 0;

	mutex_lock(&register_mutex);
	if (! list_empty(&timer->open_list_head)) {
		struct list_head *p, *n;
		struct snd_timer_instance *ti;
		pr_warn("ALSA: timer %p is busy?\n", timer);
		list_for_each_safe(p, n, &timer->open_list_head) {
			list_del_init(p);
			ti = list_entry(p, struct snd_timer_instance, open_list);
			ti->timer = NULL;
		}
	}
	list_del(&timer->device_list);
	mutex_unlock(&register_mutex);

	if (timer->private_free)
		timer->private_free(timer);
	kfree(timer);
	return 0;
}

static int snd_timer_dev_free(struct snd_device *device)
{
	struct snd_timer *timer = device->device_data;
	return snd_timer_free(timer);
}

static int snd_timer_dev_register(struct snd_device *dev)
{
	struct snd_timer *timer = dev->device_data;
	struct snd_timer *timer1;

	if (snd_BUG_ON(!timer || !timer->hw.start || !timer->hw.stop))
		return -ENXIO;
	if (!(timer->hw.flags & SNDRV_TIMER_HW_SLAVE) &&
	    !timer->hw.resolution && timer->hw.c_resolution == NULL)
	    	return -EINVAL;

	mutex_lock(&register_mutex);
	list_for_each_entry(timer1, &snd_timer_list, device_list) {
		if (timer1->tmr_class > timer->tmr_class)
			break;
		if (timer1->tmr_class < timer->tmr_class)
			continue;
		if (timer1->card && timer->card) {
			if (timer1->card->number > timer->card->number)
				break;
			if (timer1->card->number < timer->card->number)
				continue;
		}
		if (timer1->tmr_device > timer->tmr_device)
			break;
		if (timer1->tmr_device < timer->tmr_device)
			continue;
		if (timer1->tmr_subdevice > timer->tmr_subdevice)
			break;
		if (timer1->tmr_subdevice < timer->tmr_subdevice)
			continue;
		/* conflicts.. */
		mutex_unlock(&register_mutex);
		return -EBUSY;
	}
	list_add_tail(&timer->device_list, &timer1->device_list);
	mutex_unlock(&register_mutex);
	return 0;
}

static int snd_timer_dev_disconnect(struct snd_device *device)
{
	struct snd_timer *timer = device->device_data;
	struct snd_timer_instance *ti;

	mutex_lock(&register_mutex);
	list_del_init(&timer->device_list);
	/* wake up pending sleepers */
	list_for_each_entry(ti, &timer->open_list_head, open_list) {
		if (ti->disconnect)
			ti->disconnect(ti);
	}
	mutex_unlock(&register_mutex);
	return 0;
}

void snd_timer_notify(struct snd_timer *timer, int event, struct timespec *tstamp)
{
	unsigned long flags;
	unsigned long resolution = 0;
	struct snd_timer_instance *ti, *ts;

	if (timer->card && timer->card->shutdown)
		return;
	if (! (timer->hw.flags & SNDRV_TIMER_HW_SLAVE))
		return;
	if (snd_BUG_ON(event < SNDRV_TIMER_EVENT_MSTART ||
		       event > SNDRV_TIMER_EVENT_MRESUME))
		return;
	spin_lock_irqsave(&timer->lock, flags);
	if (event == SNDRV_TIMER_EVENT_MSTART ||
	    event == SNDRV_TIMER_EVENT_MCONTINUE ||
	    event == SNDRV_TIMER_EVENT_MRESUME)
		resolution = snd_timer_hw_resolution(timer);
	list_for_each_entry(ti, &timer->active_list_head, active_list) {
		if (ti->ccallback)
			ti->ccallback(ti, event, tstamp, resolution);
		list_for_each_entry(ts, &ti->slave_active_head, active_list)
			if (ts->ccallback)
				ts->ccallback(ts, event, tstamp, resolution);
	}
	spin_unlock_irqrestore(&timer->lock, flags);
}
EXPORT_SYMBOL(snd_timer_notify);

/*
 * exported functions for global timers
 */
int snd_timer_global_new(char *id, int device, struct snd_timer **rtimer)
{
	struct snd_timer_id tid;

	tid.dev_class = SNDRV_TIMER_CLASS_GLOBAL;
	tid.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	tid.card = -1;
	tid.device = device;
	tid.subdevice = 0;
	return snd_timer_new(NULL, id, &tid, rtimer);
}
EXPORT_SYMBOL(snd_timer_global_new);

int snd_timer_global_free(struct snd_timer *timer)
{
	return snd_timer_free(timer);
}
EXPORT_SYMBOL(snd_timer_global_free);

int snd_timer_global_register(struct snd_timer *timer)
{
	struct snd_device dev;

	memset(&dev, 0, sizeof(dev));
	dev.device_data = timer;
	return snd_timer_dev_register(&dev);
}
EXPORT_SYMBOL(snd_timer_global_register);

/*
 *  System timer
 */

struct snd_timer_system_private {
	struct timer_list tlist;
	struct snd_timer *snd_timer;
	unsigned long last_expires;
	unsigned long last_jiffies;
	unsigned long correction;
};

static void snd_timer_s_function(struct timer_list *t)
{
	struct snd_timer_system_private *priv = from_timer(priv, t,
								tlist);
	struct snd_timer *timer = priv->snd_timer;
	unsigned long jiff = jiffies;
	if (time_after(jiff, priv->last_expires))
		priv->correction += (long)jiff - (long)priv->last_expires;
	snd_timer_interrupt(timer, (long)jiff - (long)priv->last_jiffies);
}

static int snd_timer_s_start(struct snd_timer * timer)
{
	struct snd_timer_system_private *priv;
	unsigned long njiff;

	priv = (struct snd_timer_system_private *) timer->private_data;
	njiff = (priv->last_jiffies = jiffies);
	if (priv->correction > timer->sticks - 1) {
		priv->correction -= timer->sticks - 1;
		njiff++;
	} else {
		njiff += timer->sticks - priv->correction;
		priv->correction = 0;
	}
	priv->last_expires = njiff;
	mod_timer(&priv->tlist, njiff);
	return 0;
}

static int snd_timer_s_stop(struct snd_timer * timer)
{
	struct snd_timer_system_private *priv;
	unsigned long jiff;

	priv = (struct snd_timer_system_private *) timer->private_data;
	del_timer(&priv->tlist);
	jiff = jiffies;
	if (time_before(jiff, priv->last_expires))
		timer->sticks = priv->last_expires - jiff;
	else
		timer->sticks = 1;
	priv->correction = 0;
	return 0;
}

static int snd_timer_s_close(struct snd_timer *timer)
{
	struct snd_timer_system_private *priv;

	priv = (struct snd_timer_system_private *)timer->private_data;
	del_timer_sync(&priv->tlist);
	return 0;
}

static struct snd_timer_hardware snd_timer_system =
{
	.flags =	SNDRV_TIMER_HW_FIRST | SNDRV_TIMER_HW_TASKLET,
	.resolution =	1000000000L / HZ,
	.ticks =	10000000L,
	.close =	snd_timer_s_close,
	.start =	snd_timer_s_start,
	.stop =		snd_timer_s_stop
};

static void snd_timer_free_system(struct snd_timer *timer)
{
	kfree(timer->private_data);
}

static int snd_timer_register_system(void)
{
	struct snd_timer *timer;
	struct snd_timer_system_private *priv;
	int err;

	err = snd_timer_global_new("system", SNDRV_TIMER_GLOBAL_SYSTEM, &timer);
	if (err < 0)
		return err;
	strcpy(timer->name, "system timer");
	timer->hw = snd_timer_system;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		snd_timer_free(timer);
		return -ENOMEM;
	}
	priv->snd_timer = timer;
	timer_setup(&priv->tlist, snd_timer_s_function, 0);
	timer->private_data = priv;
	timer->private_free = snd_timer_free_system;
	return snd_timer_global_register(timer);
}

#ifdef CONFIG_SND_PROC_FS
/*
 *  Info interface
 */

static void snd_timer_proc_read(struct snd_info_entry *entry,
				struct snd_info_buffer *buffer)
{
	struct snd_timer *timer;
	struct snd_timer_instance *ti;

	mutex_lock(&register_mutex);
	list_for_each_entry(timer, &snd_timer_list, device_list) {
		if (timer->card && timer->card->shutdown)
			continue;
		switch (timer->tmr_class) {
		case SNDRV_TIMER_CLASS_GLOBAL:
			snd_iprintf(buffer, "G%i: ", timer->tmr_device);
			break;
		case SNDRV_TIMER_CLASS_CARD:
			snd_iprintf(buffer, "C%i-%i: ",
				    timer->card->number, timer->tmr_device);
			break;
		case SNDRV_TIMER_CLASS_PCM:
			snd_iprintf(buffer, "P%i-%i-%i: ", timer->card->number,
				    timer->tmr_device, timer->tmr_subdevice);
			break;
		default:
			snd_iprintf(buffer, "?%i-%i-%i-%i: ", timer->tmr_class,
				    timer->card ? timer->card->number : -1,
				    timer->tmr_device, timer->tmr_subdevice);
		}
		snd_iprintf(buffer, "%s :", timer->name);
		if (timer->hw.resolution)
			snd_iprintf(buffer, " %lu.%03luus (%lu ticks)",
				    timer->hw.resolution / 1000,
				    timer->hw.resolution % 1000,
				    timer->hw.ticks);
		if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
			snd_iprintf(buffer, " SLAVE");
		snd_iprintf(buffer, "\n");
		list_for_each_entry(ti, &timer->open_list_head, open_list)
			snd_iprintf(buffer, "  Client %s : %s\n",
				    ti->owner ? ti->owner : "unknown",
				    ti->flags & (SNDRV_TIMER_IFLG_START |
						 SNDRV_TIMER_IFLG_RUNNING)
				    ? "running" : "stopped");
	}
	mutex_unlock(&register_mutex);
}

static struct snd_info_entry *snd_timer_proc_entry;

static void __init snd_timer_proc_init(void)
{
	struct snd_info_entry *entry;

	entry = snd_info_create_module_entry(THIS_MODULE, "timers", NULL);
	if (entry != NULL) {
		entry->c.text.read = snd_timer_proc_read;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	snd_timer_proc_entry = entry;
}

static void __exit snd_timer_proc_done(void)
{
	snd_info_free_entry(snd_timer_proc_entry);
}
#else /* !CONFIG_SND_PROC_FS */
#define snd_timer_proc_init()
#define snd_timer_proc_done()
#endif

/*
 *  USER SPACE interface
 */

static void snd_timer_user_interrupt(struct snd_timer_instance *timeri,
				     unsigned long resolution,
				     unsigned long ticks)
{
	struct snd_timer_user *tu = timeri->callback_data;
	struct snd_timer_read *r;
	int prev;

	spin_lock(&tu->qlock);
	if (tu->qused > 0) {
		prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
		r = &tu->queue[prev];
		if (r->resolution == resolution) {
			r->ticks += ticks;
			goto __wake;
		}
	}
	if (tu->qused >= tu->queue_size) {
		tu->overrun++;
	} else {
		r = &tu->queue[tu->qtail++];
		tu->qtail %= tu->queue_size;
		r->resolution = resolution;
		r->ticks = ticks;
		tu->qused++;
	}
      __wake:
	spin_unlock(&tu->qlock);
	kill_fasync(&tu->fasync, SIGIO, POLL_IN);
	wake_up(&tu->qchange_sleep);
}

static void snd_timer_user_append_to_tqueue(struct snd_timer_user *tu,
					    struct snd_timer_tread *tread)
{
	if (tu->qused >= tu->queue_size) {
		tu->overrun++;
	} else {
		memcpy(&tu->tqueue[tu->qtail++], tread, sizeof(*tread));
		tu->qtail %= tu->queue_size;
		tu->qused++;
	}
}

static void snd_timer_user_ccallback(struct snd_timer_instance *timeri,
				     int event,
				     struct timespec *tstamp,
				     unsigned long resolution)
{
	struct snd_timer_user *tu = timeri->callback_data;
	struct snd_timer_tread r1;
	unsigned long flags;

	if (event >= SNDRV_TIMER_EVENT_START &&
	    event <= SNDRV_TIMER_EVENT_PAUSE)
		tu->tstamp = *tstamp;
	if ((tu->filter & (1 << event)) == 0 || !tu->tread)
		return;
	memset(&r1, 0, sizeof(r1));
	r1.event = event;
	r1.tstamp = *tstamp;
	r1.val = resolution;
	spin_lock_irqsave(&tu->qlock, flags);
	snd_timer_user_append_to_tqueue(tu, &r1);
	spin_unlock_irqrestore(&tu->qlock, flags);
	kill_fasync(&tu->fasync, SIGIO, POLL_IN);
	wake_up(&tu->qchange_sleep);
}

static void snd_timer_user_disconnect(struct snd_timer_instance *timeri)
{
	struct snd_timer_user *tu = timeri->callback_data;

	tu->disconnected = true;
	wake_up(&tu->qchange_sleep);
}

static void snd_timer_user_tinterrupt(struct snd_timer_instance *timeri,
				      unsigned long resolution,
				      unsigned long ticks)
{
	struct snd_timer_user *tu = timeri->callback_data;
	struct snd_timer_tread *r, r1;
	struct timespec tstamp;
	int prev, append = 0;

	memset(&r1, 0, sizeof(r1));
	memset(&tstamp, 0, sizeof(tstamp));
	spin_lock(&tu->qlock);
	if ((tu->filter & ((1 << SNDRV_TIMER_EVENT_RESOLUTION) |
			   (1 << SNDRV_TIMER_EVENT_TICK))) == 0) {
		spin_unlock(&tu->qlock);
		return;
	}
	if (tu->last_resolution != resolution || ticks > 0) {
		if (timer_tstamp_monotonic)
			ktime_get_ts(&tstamp);
		else
			getnstimeofday(&tstamp);
	}
	if ((tu->filter & (1 << SNDRV_TIMER_EVENT_RESOLUTION)) &&
	    tu->last_resolution != resolution) {
		r1.event = SNDRV_TIMER_EVENT_RESOLUTION;
		r1.tstamp = tstamp;
		r1.val = resolution;
		snd_timer_user_append_to_tqueue(tu, &r1);
		tu->last_resolution = resolution;
		append++;
	}
	if ((tu->filter & (1 << SNDRV_TIMER_EVENT_TICK)) == 0)
		goto __wake;
	if (ticks == 0)
		goto __wake;
	if (tu->qused > 0) {
		prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
		r = &tu->tqueue[prev];
		if (r->event == SNDRV_TIMER_EVENT_TICK) {
			r->tstamp = tstamp;
			r->val += ticks;
			append++;
			goto __wake;
		}
	}
	r1.event = SNDRV_TIMER_EVENT_TICK;
	r1.tstamp = tstamp;
	r1.val = ticks;
	snd_timer_user_append_to_tqueue(tu, &r1);
	append++;
      __wake:
	spin_unlock(&tu->qlock);
	if (append == 0)
		return;
	kill_fasync(&tu->fasync, SIGIO, POLL_IN);
	wake_up(&tu->qchange_sleep);
}

static int realloc_user_queue(struct snd_timer_user *tu, int size)
{
	struct snd_timer_read *queue = NULL;
	struct snd_timer_tread *tqueue = NULL;

	if (tu->tread) {
		tqueue = kcalloc(size, sizeof(*tqueue), GFP_KERNEL);
		if (!tqueue)
			return -ENOMEM;
	} else {
		queue = kcalloc(size, sizeof(*queue), GFP_KERNEL);
		if (!queue)
			return -ENOMEM;
	}

	spin_lock_irq(&tu->qlock);
	kfree(tu->queue);
	kfree(tu->tqueue);
	tu->queue_size = size;
	tu->queue = queue;
	tu->tqueue = tqueue;
	tu->qhead = tu->qtail = tu->qused = 0;
	spin_unlock_irq(&tu->qlock);

	return 0;
}

static int snd_timer_user_open(struct inode *inode, struct file *file)
{
	struct snd_timer_user *tu;
	int err;

	err = nonseekable_open(inode, file);
	if (err < 0)
		return err;

	tu = kzalloc(sizeof(*tu), GFP_KERNEL);
	if (tu == NULL)
		return -ENOMEM;
	spin_lock_init(&tu->qlock);
	init_waitqueue_head(&tu->qchange_sleep);
	mutex_init(&tu->ioctl_lock);
	tu->ticks = 1;
	if (realloc_user_queue(tu, 128) < 0) {
		kfree(tu);
		return -ENOMEM;
	}
	file->private_data = tu;
	return 0;
}

static int snd_timer_user_release(struct inode *inode, struct file *file)
{
	struct snd_timer_user *tu;

	if (file->private_data) {
		tu = file->private_data;
		file->private_data = NULL;
		mutex_lock(&tu->ioctl_lock);
		if (tu->timeri)
			snd_timer_close(tu->timeri);
		mutex_unlock(&tu->ioctl_lock);
		kfree(tu->queue);
		kfree(tu->tqueue);
		kfree(tu);
	}
	return 0;
}

static void snd_timer_user_zero_id(struct snd_timer_id *id)
{
	id->dev_class = SNDRV_TIMER_CLASS_NONE;
	id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	id->card = -1;
	id->device = -1;
	id->subdevice = -1;
}

static void snd_timer_user_copy_id(struct snd_timer_id *id, struct snd_timer *timer)
{
	id->dev_class = timer->tmr_class;
	id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	id->card = timer->card ? timer->card->number : -1;
	id->device = timer->tmr_device;
	id->subdevice = timer->tmr_subdevice;
}

static int snd_timer_user_next_device(struct snd_timer_id __user *_tid)
{
	struct snd_timer_id id;
	struct snd_timer *timer;
	struct list_head *p;

	if (copy_from_user(&id, _tid, sizeof(id)))
		return -EFAULT;
	mutex_lock(&register_mutex);
	if (id.dev_class < 0) {		/* first item */
		if (list_empty(&snd_timer_list))
			snd_timer_user_zero_id(&id);
		else {
			timer = list_entry(snd_timer_list.next,
					   struct snd_timer, device_list);
			snd_timer_user_copy_id(&id, timer);
		}
	} else {
		switch (id.dev_class) {
		case SNDRV_TIMER_CLASS_GLOBAL:
			id.device = id.device < 0 ? 0 : id.device + 1;
			list_for_each(p, &snd_timer_list) {
				timer = list_entry(p, struct snd_timer, device_list);
				if (timer->tmr_class > SNDRV_TIMER_CLASS_GLOBAL) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_device >= id.device) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
			}
			if (p == &snd_timer_list)
				snd_timer_user_zero_id(&id);
			break;
		case SNDRV_TIMER_CLASS_CARD:
		case SNDRV_TIMER_CLASS_PCM:
			if (id.card < 0) {
				id.card = 0;
			} else {
				if (id.device < 0) {
					id.device = 0;
				} else {
					if (id.subdevice < 0)
						id.subdevice = 0;
					else if (id.subdevice < INT_MAX)
						id.subdevice++;
				}
			}
			list_for_each(p, &snd_timer_list) {
				timer = list_entry(p, struct snd_timer, device_list);
				if (timer->tmr_class > id.dev_class) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_class < id.dev_class)
					continue;
				if (timer->card->number > id.card) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->card->number < id.card)
					continue;
				if (timer->tmr_device > id.device) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_device < id.device)
					continue;
				if (timer->tmr_subdevice > id.subdevice) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_subdevice < id.subdevice)
					continue;
				snd_timer_user_copy_id(&id, timer);
				break;
			}
			if (p == &snd_timer_list)
				snd_timer_user_zero_id(&id);
			break;
		default:
			snd_timer_user_zero_id(&id);
		}
	}
	mutex_unlock(&register_mutex);
	if (copy_to_user(_tid, &id, sizeof(*_tid)))
		return -EFAULT;
	return 0;
}

static int snd_timer_user_ginfo(struct file *file,
				struct snd_timer_ginfo __user *_ginfo)
{
	struct snd_timer_ginfo *ginfo;
	struct snd_timer_id tid;
	struct snd_timer *t;
	struct list_head *p;
	int err = 0;

	ginfo = memdup_user(_ginfo, sizeof(*ginfo));
	if (IS_ERR(ginfo))
		return PTR_ERR(ginfo);

	tid = ginfo->tid;
	memset(ginfo, 0, sizeof(*ginfo));
	ginfo->tid = tid;
	mutex_lock(&register_mutex);
	t = snd_timer_find(&tid);
	if (t != NULL) {
		ginfo->card = t->card ? t->card->number : -1;
		if (t->hw.flags & SNDRV_TIMER_HW_SLAVE)
			ginfo->flags |= SNDRV_TIMER_FLG_SLAVE;
		strlcpy(ginfo->id, t->id, sizeof(ginfo->id));
		strlcpy(ginfo->name, t->name, sizeof(ginfo->name));
		ginfo->resolution = t->hw.resolution;
		if (t->hw.resolution_min > 0) {
			ginfo->resolution_min = t->hw.resolution_min;
			ginfo->resolution_max = t->hw.resolution_max;
		}
		list_for_each(p, &t->open_list_head) {
			ginfo->clients++;
		}
	} else {
		err = -ENODEV;
	}
	mutex_unlock(&register_mutex);
	if (err >= 0 && copy_to_user(_ginfo, ginfo, sizeof(*ginfo)))
		err = -EFAULT;
	kfree(ginfo);
	return err;
}

static int timer_set_gparams(struct snd_timer_gparams *gparams)
{
	struct snd_timer *t;
	int err;

	mutex_lock(&register_mutex);
	t = snd_timer_find(&gparams->tid);
	if (!t) {
		err = -ENODEV;
		goto _error;
	}
	if (!list_empty(&t->open_list_head)) {
		err = -EBUSY;
		goto _error;
	}
	if (!t->hw.set_period) {
		err = -ENOSYS;
		goto _error;
	}
	err = t->hw.set_period(t, gparams->period_num, gparams->period_den);
_error:
	mutex_unlock(&register_mutex);
	return err;
}

static int snd_timer_user_gparams(struct file *file,
				  struct snd_timer_gparams __user *_gparams)
{
	struct snd_timer_gparams gparams;

	if (copy_from_user(&gparams, _gparams, sizeof(gparams)))
		return -EFAULT;
	return timer_set_gparams(&gparams);
}

static int snd_timer_user_gstatus(struct file *file,
				  struct snd_timer_gstatus __user *_gstatus)
{
	struct snd_timer_gstatus gstatus;
	struct snd_timer_id tid;
	struct snd_timer *t;
	int err = 0;

	if (copy_from_user(&gstatus, _gstatus, sizeof(gstatus)))
		return -EFAULT;
	tid = gstatus.tid;
	memset(&gstatus, 0, sizeof(gstatus));
	gstatus.tid = tid;
	mutex_lock(&register_mutex);
	t = snd_timer_find(&tid);
	if (t != NULL) {
		spin_lock_irq(&t->lock);
		gstatus.resolution = snd_timer_hw_resolution(t);
		if (t->hw.precise_resolution) {
			t->hw.precise_resolution(t, &gstatus.resolution_num,
						 &gstatus.resolution_den);
		} else {
			gstatus.resolution_num = gstatus.resolution;
			gstatus.resolution_den = 1000000000uL;
		}
		spin_unlock_irq(&t->lock);
	} else {
		err = -ENODEV;
	}
	mutex_unlock(&register_mutex);
	if (err >= 0 && copy_to_user(_gstatus, &gstatus, sizeof(gstatus)))
		err = -EFAULT;
	return err;
}

static int snd_timer_user_tselect(struct file *file,
				  struct snd_timer_select __user *_tselect)
{
	struct snd_timer_user *tu;
	struct snd_timer_select tselect;
	char str[32];
	int err = 0;

	tu = file->private_data;
	if (tu->timeri) {
		snd_timer_close(tu->timeri);
		tu->timeri = NULL;
	}
	if (copy_from_user(&tselect, _tselect, sizeof(tselect))) {
		err = -EFAULT;
		goto __err;
	}
	sprintf(str, "application %i", current->pid);
	if (tselect.id.dev_class != SNDRV_TIMER_CLASS_SLAVE)
		tselect.id.dev_sclass = SNDRV_TIMER_SCLASS_APPLICATION;
	err = snd_timer_open(&tu->timeri, str, &tselect.id, current->pid);
	if (err < 0)
		goto __err;

	tu->timeri->flags |= SNDRV_TIMER_IFLG_FAST;
	tu->timeri->callback = tu->tread
			? snd_timer_user_tinterrupt : snd_timer_user_interrupt;
	tu->timeri->ccallback = snd_timer_user_ccallback;
	tu->timeri->callback_data = (void *)tu;
	tu->timeri->disconnect = snd_timer_user_disconnect;

      __err:
	return err;
}

static int snd_timer_user_info(struct file *file,
			       struct snd_timer_info __user *_info)
{
	struct snd_timer_user *tu;
	struct snd_timer_info *info;
	struct snd_timer *t;
	int err = 0;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	t = tu->timeri->timer;
	if (!t)
		return -EBADFD;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (! info)
		return -ENOMEM;
	info->card = t->card ? t->card->number : -1;
	if (t->hw.flags & SNDRV_TIMER_HW_SLAVE)
		info->flags |= SNDRV_TIMER_FLG_SLAVE;
	strlcpy(info->id, t->id, sizeof(info->id));
	strlcpy(info->name, t->name, sizeof(info->name));
	info->resolution = t->hw.resolution;
	if (copy_to_user(_info, info, sizeof(*_info)))
		err = -EFAULT;
	kfree(info);
	return err;
}

static int snd_timer_user_params(struct file *file,
				 struct snd_timer_params __user *_params)
{
	struct snd_timer_user *tu;
	struct snd_timer_params params;
	struct snd_timer *t;
	int err;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	t = tu->timeri->timer;
	if (!t)
		return -EBADFD;
	if (copy_from_user(&params, _params, sizeof(params)))
		return -EFAULT;
	if (!(t->hw.flags & SNDRV_TIMER_HW_SLAVE)) {
		u64 resolution;

		if (params.ticks < 1) {
			err = -EINVAL;
			goto _end;
		}

		/* Don't allow resolution less than 1ms */
		resolution = snd_timer_resolution(tu->timeri);
		resolution *= params.ticks;
		if (resolution < 1000000) {
			err = -EINVAL;
			goto _end;
		}
	}
	if (params.queue_size > 0 &&
	    (params.queue_size < 32 || params.queue_size > 1024)) {
		err = -EINVAL;
		goto _end;
	}
	if (params.filter & ~((1<<SNDRV_TIMER_EVENT_RESOLUTION)|
			      (1<<SNDRV_TIMER_EVENT_TICK)|
			      (1<<SNDRV_TIMER_EVENT_START)|
			      (1<<SNDRV_TIMER_EVENT_STOP)|
			      (1<<SNDRV_TIMER_EVENT_CONTINUE)|
			      (1<<SNDRV_TIMER_EVENT_PAUSE)|
			      (1<<SNDRV_TIMER_EVENT_SUSPEND)|
			      (1<<SNDRV_TIMER_EVENT_RESUME)|
			      (1<<SNDRV_TIMER_EVENT_MSTART)|
			      (1<<SNDRV_TIMER_EVENT_MSTOP)|
			      (1<<SNDRV_TIMER_EVENT_MCONTINUE)|
			      (1<<SNDRV_TIMER_EVENT_MPAUSE)|
			      (1<<SNDRV_TIMER_EVENT_MSUSPEND)|
			      (1<<SNDRV_TIMER_EVENT_MRESUME))) {
		err = -EINVAL;
		goto _end;
	}
	snd_timer_stop(tu->timeri);
	spin_lock_irq(&t->lock);
	tu->timeri->flags &= ~(SNDRV_TIMER_IFLG_AUTO|
			       SNDRV_TIMER_IFLG_EXCLUSIVE|
			       SNDRV_TIMER_IFLG_EARLY_EVENT);
	if (params.flags & SNDRV_TIMER_PSFLG_AUTO)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_AUTO;
	if (params.flags & SNDRV_TIMER_PSFLG_EXCLUSIVE)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_EXCLUSIVE;
	if (params.flags & SNDRV_TIMER_PSFLG_EARLY_EVENT)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_EARLY_EVENT;
	spin_unlock_irq(&t->lock);
	if (params.queue_size > 0 &&
	    (unsigned int)tu->queue_size != params.queue_size) {
		err = realloc_user_queue(tu, params.queue_size);
		if (err < 0)
			goto _end;
	}
	spin_lock_irq(&tu->qlock);
	tu->qhead = tu->qtail = tu->qused = 0;
	if (tu->timeri->flags & SNDRV_TIMER_IFLG_EARLY_EVENT) {
		if (tu->tread) {
			struct snd_timer_tread tread;
			memset(&tread, 0, sizeof(tread));
			tread.event = SNDRV_TIMER_EVENT_EARLY;
			tread.tstamp.tv_sec = 0;
			tread.tstamp.tv_nsec = 0;
			tread.val = 0;
			snd_timer_user_append_to_tqueue(tu, &tread);
		} else {
			struct snd_timer_read *r = &tu->queue[0];
			r->resolution = 0;
			r->ticks = 0;
			tu->qused++;
			tu->qtail++;
		}
	}
	tu->filter = params.filter;
	tu->ticks = params.ticks;
	spin_unlock_irq(&tu->qlock);
	err = 0;
 _end:
	if (copy_to_user(_params, &params, sizeof(params)))
		return -EFAULT;
	return err;
}

static int snd_timer_user_status(struct file *file,
				 struct snd_timer_status __user *_status)
{
	struct snd_timer_user *tu;
	struct snd_timer_status status;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	memset(&status, 0, sizeof(status));
	status.tstamp = tu->tstamp;
	status.resolution = snd_timer_resolution(tu->timeri);
	status.lost = tu->timeri->lost;
	status.overrun = tu->overrun;
	spin_lock_irq(&tu->qlock);
	status.queue = tu->qused;
	spin_unlock_irq(&tu->qlock);
	if (copy_to_user(_status, &status, sizeof(status)))
		return -EFAULT;
	return 0;
}

static int snd_timer_user_start(struct file *file)
{
	int err;
	struct snd_timer_user *tu;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	snd_timer_stop(tu->timeri);
	tu->timeri->lost = 0;
	tu->last_resolution = 0;
	return (err = snd_timer_start(tu->timeri, tu->ticks)) < 0 ? err : 0;
}

static int snd_timer_user_stop(struct file *file)
{
	int err;
	struct snd_timer_user *tu;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	return (err = snd_timer_stop(tu->timeri)) < 0 ? err : 0;
}

static int snd_timer_user_continue(struct file *file)
{
	int err;
	struct snd_timer_user *tu;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	/* start timer instead of continue if it's not used before */
	if (!(tu->timeri->flags & SNDRV_TIMER_IFLG_PAUSED))
		return snd_timer_user_start(file);
	tu->timeri->lost = 0;
	return (err = snd_timer_continue(tu->timeri)) < 0 ? err : 0;
}

static int snd_timer_user_pause(struct file *file)
{
	int err;
	struct snd_timer_user *tu;

	tu = file->private_data;
	if (!tu->timeri)
		return -EBADFD;
	return (err = snd_timer_pause(tu->timeri)) < 0 ? err : 0;
}

enum {
	SNDRV_TIMER_IOCTL_START_OLD = _IO('T', 0x20),
	SNDRV_TIMER_IOCTL_STOP_OLD = _IO('T', 0x21),
	SNDRV_TIMER_IOCTL_CONTINUE_OLD = _IO('T', 0x22),
	SNDRV_TIMER_IOCTL_PAUSE_OLD = _IO('T', 0x23),
};

static long __snd_timer_user_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct snd_timer_user *tu;
	void __user *argp = (void __user *)arg;
	int __user *p = argp;

	tu = file->private_data;
	switch (cmd) {
	case SNDRV_TIMER_IOCTL_PVERSION:
		return put_user(SNDRV_TIMER_VERSION, p) ? -EFAULT : 0;
	case SNDRV_TIMER_IOCTL_NEXT_DEVICE:
		return snd_timer_user_next_device(argp);
	case SNDRV_TIMER_IOCTL_TREAD:
	{
		int xarg, old_tread;

		if (tu->timeri)	/* too late */
			return -EBUSY;
		if (get_user(xarg, p))
			return -EFAULT;
		old_tread = tu->tread;
		tu->tread = xarg ? 1 : 0;
		if (tu->tread != old_tread &&
		    realloc_user_queue(tu, tu->queue_size) < 0) {
			tu->tread = old_tread;
			return -ENOMEM;
		}
		return 0;
	}
	case SNDRV_TIMER_IOCTL_GINFO:
		return snd_timer_user_ginfo(file, argp);
	case SNDRV_TIMER_IOCTL_GPARAMS:
		return snd_timer_user_gparams(file, argp);
	case SNDRV_TIMER_IOCTL_GSTATUS:
		return snd_timer_user_gstatus(file, argp);
	case SNDRV_TIMER_IOCTL_SELECT:
		return snd_timer_user_tselect(file, argp);
	case SNDRV_TIMER_IOCTL_INFO:
		return snd_timer_user_info(file, argp);
	case SNDRV_TIMER_IOCTL_PARAMS:
		return snd_timer_user_params(file, argp);
	case SNDRV_TIMER_IOCTL_STATUS:
		return snd_timer_user_status(file, argp);
	case SNDRV_TIMER_IOCTL_START:
	case SNDRV_TIMER_IOCTL_START_OLD:
		return snd_timer_user_start(file);
	case SNDRV_TIMER_IOCTL_STOP:
	case SNDRV_TIMER_IOCTL_STOP_OLD:
		return snd_timer_user_stop(file);
	case SNDRV_TIMER_IOCTL_CONTINUE:
	case SNDRV_TIMER_IOCTL_CONTINUE_OLD:
		return snd_timer_user_continue(file);
	case SNDRV_TIMER_IOCTL_PAUSE:
	case SNDRV_TIMER_IOCTL_PAUSE_OLD:
		return snd_timer_user_pause(file);
	}
	return -ENOTTY;
}

static long snd_timer_user_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct snd_timer_user *tu = file->private_data;
	long ret;

	mutex_lock(&tu->ioctl_lock);
	ret = __snd_timer_user_ioctl(file, cmd, arg);
	mutex_unlock(&tu->ioctl_lock);
	return ret;
}

static int snd_timer_user_fasync(int fd, struct file * file, int on)
{
	struct snd_timer_user *tu;

	tu = file->private_data;
	return fasync_helper(fd, file, on, &tu->fasync);
}

static ssize_t snd_timer_user_read(struct file *file, char __user *buffer,
				   size_t count, loff_t *offset)
{
	struct snd_timer_user *tu;
	long result = 0, unit;
	int qhead;
	int err = 0;

	tu = file->private_data;
	unit = tu->tread ? sizeof(struct snd_timer_tread) : sizeof(struct snd_timer_read);
	mutex_lock(&tu->ioctl_lock);
	spin_lock_irq(&tu->qlock);
	while ((long)count - result >= unit) {
		while (!tu->qused) {
			wait_queue_entry_t wait;

			if ((file->f_flags & O_NONBLOCK) != 0 || result > 0) {
				err = -EAGAIN;
				goto _error;
			}

			set_current_state(TASK_INTERRUPTIBLE);
			init_waitqueue_entry(&wait, current);
			add_wait_queue(&tu->qchange_sleep, &wait);

			spin_unlock_irq(&tu->qlock);
			mutex_unlock(&tu->ioctl_lock);
			schedule();
			mutex_lock(&tu->ioctl_lock);
			spin_lock_irq(&tu->qlock);

			remove_wait_queue(&tu->qchange_sleep, &wait);

			if (tu->disconnected) {
				err = -ENODEV;
				goto _error;
			}
			if (signal_pending(current)) {
				err = -ERESTARTSYS;
				goto _error;
			}
		}

		qhead = tu->qhead++;
		tu->qhead %= tu->queue_size;
		tu->qused--;
		spin_unlock_irq(&tu->qlock);

		if (tu->tread) {
			if (copy_to_user(buffer, &tu->tqueue[qhead],
					 sizeof(struct snd_timer_tread)))
				err = -EFAULT;
		} else {
			if (copy_to_user(buffer, &tu->queue[qhead],
					 sizeof(struct snd_timer_read)))
				err = -EFAULT;
		}

		spin_lock_irq(&tu->qlock);
		if (err < 0)
			goto _error;
		result += unit;
		buffer += unit;
	}
 _error:
	spin_unlock_irq(&tu->qlock);
	mutex_unlock(&tu->ioctl_lock);
	return result > 0 ? result : err;
}

static __poll_t snd_timer_user_poll(struct file *file, poll_table * wait)
{
        __poll_t mask;
        struct snd_timer_user *tu;

        tu = file->private_data;

        poll_wait(file, &tu->qchange_sleep, wait);

	mask = 0;
	spin_lock_irq(&tu->qlock);
	if (tu->qused)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (tu->disconnected)
		mask |= EPOLLERR;
	spin_unlock_irq(&tu->qlock);

	return mask;
}

#ifdef CONFIG_COMPAT
#include "timer_compat.c"
#else
#define snd_timer_user_ioctl_compat	NULL
#endif

static const struct file_operations snd_timer_f_ops =
{
	.owner =	THIS_MODULE,
	.read =		snd_timer_user_read,
	.open =		snd_timer_user_open,
	.release =	snd_timer_user_release,
	.llseek =	no_llseek,
	.poll =		snd_timer_user_poll,
	.unlocked_ioctl =	snd_timer_user_ioctl,
	.compat_ioctl =	snd_timer_user_ioctl_compat,
	.fasync = 	snd_timer_user_fasync,
};

/* unregister the system timer */
static void snd_timer_free_all(void)
{
	struct snd_timer *timer, *n;

	list_for_each_entry_safe(timer, n, &snd_timer_list, device_list)
		snd_timer_free(timer);
}

static struct device timer_dev;

/*
 *  ENTRY functions
 */

static int __init alsa_timer_init(void)
{
	int err;

	snd_device_initialize(&timer_dev, NULL);
	dev_set_name(&timer_dev, "timer");

#ifdef SNDRV_OSS_INFO_DEV_TIMERS
	snd_oss_info_register(SNDRV_OSS_INFO_DEV_TIMERS, SNDRV_CARDS - 1,
			      "system timer");
#endif

	err = snd_timer_register_system();
	if (err < 0) {
		pr_err("ALSA: unable to register system timer (%i)\n", err);
		goto put_timer;
	}

	err = snd_register_device(SNDRV_DEVICE_TYPE_TIMER, NULL, 0,
				  &snd_timer_f_ops, NULL, &timer_dev);
	if (err < 0) {
		pr_err("ALSA: unable to register timer device (%i)\n", err);
		snd_timer_free_all();
		goto put_timer;
	}

	snd_timer_proc_init();
	return 0;

put_timer:
	put_device(&timer_dev);
	return err;
}

static void __exit alsa_timer_exit(void)
{
	snd_unregister_device(&timer_dev);
	snd_timer_free_all();
	put_device(&timer_dev);
	snd_timer_proc_done();
#ifdef SNDRV_OSS_INFO_DEV_TIMERS
	snd_oss_info_unregister(SNDRV_OSS_INFO_DEV_TIMERS, SNDRV_CARDS - 1);
#endif
}

module_init(alsa_timer_init)
module_exit(alsa_timer_exit)
