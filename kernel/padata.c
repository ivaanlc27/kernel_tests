// SPDX-License-Identifier: GPL-2.0
/*
 * padata.c - generic interface to process data streams in parallel
 *
 * See Documentation/padata.txt for an api documentation.
 *
 * Copyright (C) 2008, 2009 secunet Security Networks AG
 * Copyright (C) 2008, 2009 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/export.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/padata.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/rcupdate.h>
#include <linux/module.h>

#define MAX_OBJ_NUM 1000

static void padata_free_pd(struct parallel_data *pd);

static int padata_index_to_cpu(struct parallel_data *pd, int cpu_index)
{
	int cpu, target_cpu;

	target_cpu = cpumask_first(pd->cpumask.pcpu);
	for (cpu = 0; cpu < cpu_index; cpu++)
		target_cpu = cpumask_next(target_cpu, pd->cpumask.pcpu);

	return target_cpu;
}

static int padata_cpu_hash(struct parallel_data *pd)
{
	unsigned int seq_nr;
	int cpu_index;

	/*
	 * Hash the sequence numbers to the cpus by taking
	 * seq_nr mod. number of cpus in use.
	 */

	seq_nr = atomic_inc_return(&pd->seq_nr);
	cpu_index = seq_nr % cpumask_weight(pd->cpumask.pcpu);

	return padata_index_to_cpu(pd, cpu_index);
}

static void padata_parallel_worker(struct work_struct *parallel_work)
{
	struct padata_parallel_queue *pqueue;
	LIST_HEAD(local_list);

	local_bh_disable();
	pqueue = container_of(parallel_work,
			      struct padata_parallel_queue, work);

	spin_lock(&pqueue->parallel.lock);
	list_replace_init(&pqueue->parallel.list, &local_list);
	spin_unlock(&pqueue->parallel.lock);

	while (!list_empty(&local_list)) {
		struct padata_priv *padata;

		padata = list_entry(local_list.next,
				    struct padata_priv, list);

		list_del_init(&padata->list);

		padata->parallel(padata);
	}

	local_bh_enable();
}

/**
 * padata_do_parallel - padata parallelization function
 *
 * @pinst: padata instance
 * @padata: object to be parallelized
 * @cb_cpu: pointer to the CPU that the serialization callback function should
 *          run on.  If it's not in the serial cpumask of @pinst
 *          (i.e. cpumask.cbcpu), this function selects a fallback CPU and if
 *          none found, returns -EINVAL.
 *
 * The parallelization callback function will run with BHs off.
 * Note: Every object which is parallelized by padata_do_parallel
 * must be seen by padata_do_serial.
 */
int padata_do_parallel(struct padata_instance *pinst,
		       struct padata_priv *padata, int *cb_cpu)
{
	int i, cpu, cpu_index, target_cpu, err;
	struct padata_parallel_queue *queue;
	struct parallel_data *pd;

	rcu_read_lock_bh();

	pd = rcu_dereference_bh(pinst->pd);

	err = -EINVAL;
	if (!(pinst->flags & PADATA_INIT) || pinst->flags & PADATA_INVALID)
		goto out;

	if (!cpumask_test_cpu(*cb_cpu, pd->cpumask.cbcpu)) {
		if (!cpumask_weight(pd->cpumask.cbcpu))
			goto out;

		/* Select an alternate fallback CPU and notify the caller. */
		cpu_index = *cb_cpu % cpumask_weight(pd->cpumask.cbcpu);

		cpu = cpumask_first(pd->cpumask.cbcpu);
		for (i = 0; i < cpu_index; i++)
			cpu = cpumask_next(cpu, pd->cpumask.cbcpu);

		*cb_cpu = cpu;
	}

	err = -EBUSY;
	if ((pinst->flags & PADATA_RESET))
		goto out;

	if (atomic_read(&pd->refcnt) >= MAX_OBJ_NUM)
		goto out;

	err = 0;
	atomic_inc(&pd->refcnt);
	padata->pd = pd;
	padata->cb_cpu = *cb_cpu;

	target_cpu = padata_cpu_hash(pd);
	padata->cpu = target_cpu;
	queue = per_cpu_ptr(pd->pqueue, target_cpu);

	spin_lock(&queue->parallel.lock);
	list_add_tail(&padata->list, &queue->parallel.list);
	spin_unlock(&queue->parallel.lock);

	queue_work_on(target_cpu, pinst->wq, &queue->work);

out:
	rcu_read_unlock_bh();

	return err;
}
EXPORT_SYMBOL(padata_do_parallel);

/*
 * padata_get_next - Get the next object that needs serialization.
 *
 * Return values are:
 *
 * A pointer to the control struct of the next object that needs
 * serialization, if present in one of the percpu reorder queues.
 *
 * -EINPROGRESS, if the next object that needs serialization will
 *  be parallel processed by another cpu and is not yet present in
 *  the cpu's reorder queue.
 *
 * -ENODATA, if this cpu has to do the parallel processing for
 *  the next object.
 */
static struct padata_priv *padata_get_next(struct parallel_data *pd)
{
	struct padata_parallel_queue *next_queue;
	struct padata_priv *padata;
	struct padata_list *reorder;
	int cpu = pd->cpu;

	next_queue = per_cpu_ptr(pd->pqueue, cpu);
	reorder = &next_queue->reorder;

	spin_lock(&reorder->lock);
	if (!list_empty(&reorder->list)) {
		padata = list_entry(reorder->list.next,
				    struct padata_priv, list);

		list_del_init(&padata->list);
		atomic_dec(&pd->reorder_objects);

		pd->cpu = cpumask_next_wrap(cpu, pd->cpumask.pcpu, -1,
					    false);

		spin_unlock(&reorder->lock);
		goto out;
	}
	spin_unlock(&reorder->lock);

	if (__this_cpu_read(pd->pqueue->cpu_index) == next_queue->cpu_index) {
		padata = ERR_PTR(-ENODATA);
		goto out;
	}

	padata = ERR_PTR(-EINPROGRESS);
out:
	return padata;
}

static void padata_reorder(struct parallel_data *pd)
{
	int cb_cpu;
	struct padata_priv *padata;
	struct padata_serial_queue *squeue;
	struct padata_instance *pinst = pd->pinst;
	struct padata_parallel_queue *next_queue;

	/*
	 * We need to ensure that only one cpu can work on dequeueing of
	 * the reorder queue the time. Calculating in which percpu reorder
	 * queue the next object will arrive takes some time. A spinlock
	 * would be highly contended. Also it is not clear in which order
	 * the objects arrive to the reorder queues. So a cpu could wait to
	 * get the lock just to notice that there is nothing to do at the
	 * moment. Therefore we use a trylock and let the holder of the lock
	 * care for all the objects enqueued during the holdtime of the lock.
	 */
	if (!spin_trylock_bh(&pd->lock))
		return;

	while (1) {
		padata = padata_get_next(pd);

		/*
		 * If the next object that needs serialization is parallel
		 * processed by another cpu and is still on it's way to the
		 * cpu's reorder queue, nothing to do for now.
		 */
		if (PTR_ERR(padata) == -EINPROGRESS)
			break;

		/*
		 * This cpu has to do the parallel processing of the next
		 * object. It's waiting in the cpu's parallelization queue,
		 * so exit immediately.
		 */
		if (PTR_ERR(padata) == -ENODATA) {
			spin_unlock_bh(&pd->lock);
			return;
		}

		cb_cpu = padata->cb_cpu;
		squeue = per_cpu_ptr(pd->squeue, cb_cpu);

		spin_lock(&squeue->serial.lock);
		list_add_tail(&padata->list, &squeue->serial.list);
		spin_unlock(&squeue->serial.lock);

		queue_work_on(cb_cpu, pinst->wq, &squeue->work);
	}

	spin_unlock_bh(&pd->lock);

	/*
	 * The next object that needs serialization might have arrived to
	 * the reorder queues in the meantime.
	 *
	 * Ensure reorder queue is read after pd->lock is dropped so we see
	 * new objects from another task in padata_do_serial.  Pairs with
	 * smp_mb__after_atomic in padata_do_serial.
	 */
	smp_mb();

	next_queue = per_cpu_ptr(pd->pqueue, pd->cpu);
	if (!list_empty(&next_queue->reorder.list))
		queue_work(pinst->wq, &pd->reorder_work);
}

static void invoke_padata_reorder(struct work_struct *work)
{
	struct parallel_data *pd;

	local_bh_disable();
	pd = container_of(work, struct parallel_data, reorder_work);
	padata_reorder(pd);
	local_bh_enable();
}

static void padata_serial_worker(struct work_struct *serial_work)
{
	struct padata_serial_queue *squeue;
	struct parallel_data *pd;
	LIST_HEAD(local_list);
	int cnt;

	local_bh_disable();
	squeue = container_of(serial_work, struct padata_serial_queue, work);
	pd = squeue->pd;

	spin_lock(&squeue->serial.lock);
	list_replace_init(&squeue->serial.list, &local_list);
	spin_unlock(&squeue->serial.lock);

	cnt = 0;

	while (!list_empty(&local_list)) {
		struct padata_priv *padata;

		padata = list_entry(local_list.next,
				    struct padata_priv, list);

		list_del_init(&padata->list);

		padata->serial(padata);
		cnt++;
	}
	local_bh_enable();

	if (atomic_sub_and_test(cnt, &pd->refcnt))
		padata_free_pd(pd);
}

/**
 * padata_do_serial - padata serialization function
 *
 * @padata: object to be serialized.
 *
 * padata_do_serial must be called for every parallelized object.
 * The serialization callback function will run with BHs off.
 */
void padata_do_serial(struct padata_priv *padata)
{
	struct parallel_data *pd = padata->pd;
	struct padata_parallel_queue *pqueue = per_cpu_ptr(pd->pqueue,
							   padata->cpu);

	spin_lock(&pqueue->reorder.lock);
	list_add_tail(&padata->list, &pqueue->reorder.list);
	atomic_inc(&pd->reorder_objects);
	spin_unlock(&pqueue->reorder.lock);

	/*
	 * Ensure the addition to the reorder list is ordered correctly
	 * with the trylock of pd->lock in padata_reorder.  Pairs with smp_mb
	 * in padata_reorder.
	 */
	smp_mb__after_atomic();

	padata_reorder(pd);
}
EXPORT_SYMBOL(padata_do_serial);

static int padata_setup_cpumasks(struct parallel_data *pd,
				 const struct cpumask *pcpumask,
				 const struct cpumask *cbcpumask)
{
	if (!alloc_cpumask_var(&pd->cpumask.pcpu, GFP_KERNEL))
		return -ENOMEM;

	cpumask_and(pd->cpumask.pcpu, pcpumask, cpu_online_mask);
	if (!alloc_cpumask_var(&pd->cpumask.cbcpu, GFP_KERNEL)) {
		free_cpumask_var(pd->cpumask.pcpu);
		return -ENOMEM;
	}

	cpumask_and(pd->cpumask.cbcpu, cbcpumask, cpu_online_mask);
	return 0;
}

static void __padata_list_init(struct padata_list *pd_list)
{
	INIT_LIST_HEAD(&pd_list->list);
	spin_lock_init(&pd_list->lock);
}

/* Initialize all percpu queues used by serial workers */
static void padata_init_squeues(struct parallel_data *pd)
{
	int cpu;
	struct padata_serial_queue *squeue;

	for_each_cpu(cpu, pd->cpumask.cbcpu) {
		squeue = per_cpu_ptr(pd->squeue, cpu);
		squeue->pd = pd;
		__padata_list_init(&squeue->serial);
		INIT_WORK(&squeue->work, padata_serial_worker);
	}
}

/* Initialize all percpu queues used by parallel workers */
static void padata_init_pqueues(struct parallel_data *pd)
{
	int cpu_index, cpu;
	struct padata_parallel_queue *pqueue;

	cpu_index = 0;
	for_each_possible_cpu(cpu) {
		pqueue = per_cpu_ptr(pd->pqueue, cpu);

		if (!cpumask_test_cpu(cpu, pd->cpumask.pcpu)) {
			pqueue->cpu_index = -1;
			continue;
		}

		pqueue->cpu_index = cpu_index;
		cpu_index++;

		__padata_list_init(&pqueue->reorder);
		__padata_list_init(&pqueue->parallel);
		INIT_WORK(&pqueue->work, padata_parallel_worker);
		atomic_set(&pqueue->num_obj, 0);
	}
}

/* Allocate and initialize the internal cpumask dependend resources. */
static struct parallel_data *padata_alloc_pd(struct padata_instance *pinst,
					     const struct cpumask *pcpumask,
					     const struct cpumask *cbcpumask)
{
	struct parallel_data *pd;

	pd = kzalloc(sizeof(struct parallel_data), GFP_KERNEL);
	if (!pd)
		goto err;

	pd->pqueue = alloc_percpu(struct padata_parallel_queue);
	if (!pd->pqueue)
		goto err_free_pd;

	pd->squeue = alloc_percpu(struct padata_serial_queue);
	if (!pd->squeue)
		goto err_free_pqueue;
	if (padata_setup_cpumasks(pd, pcpumask, cbcpumask) < 0)
		goto err_free_squeue;

	padata_init_pqueues(pd);
	padata_init_squeues(pd);
	atomic_set(&pd->seq_nr, -1);
	atomic_set(&pd->reorder_objects, 0);
	atomic_set(&pd->refcnt, 1);
	pd->pinst = pinst;
	spin_lock_init(&pd->lock);
	pd->cpu = cpumask_first(pd->cpumask.pcpu);
	INIT_WORK(&pd->reorder_work, invoke_padata_reorder);

	return pd;

err_free_squeue:
	free_percpu(pd->squeue);
err_free_pqueue:
	free_percpu(pd->pqueue);
err_free_pd:
	kfree(pd);
err:
	return NULL;
}

static void padata_free_pd(struct parallel_data *pd)
{
	free_cpumask_var(pd->cpumask.pcpu);
	free_cpumask_var(pd->cpumask.cbcpu);
	free_percpu(pd->pqueue);
	free_percpu(pd->squeue);
	kfree(pd);
}

static void __padata_start(struct padata_instance *pinst)
{
	pinst->flags |= PADATA_INIT;
}

static void __padata_stop(struct padata_instance *pinst)
{
	if (!(pinst->flags & PADATA_INIT))
		return;

	pinst->flags &= ~PADATA_INIT;

	synchronize_rcu();
}

/* Replace the internal control structure with a new one. */
static void padata_replace(struct padata_instance *pinst,
			   struct parallel_data *pd_new)
{
	struct parallel_data *pd_old = pinst->pd;
	int notification_mask = 0;

	pinst->flags |= PADATA_RESET;

	rcu_assign_pointer(pinst->pd, pd_new);

	synchronize_rcu();

	if (!cpumask_equal(pd_old->cpumask.pcpu, pd_new->cpumask.pcpu))
		notification_mask |= PADATA_CPU_PARALLEL;
	if (!cpumask_equal(pd_old->cpumask.cbcpu, pd_new->cpumask.cbcpu))
		notification_mask |= PADATA_CPU_SERIAL;

	if (atomic_dec_and_test(&pd_old->refcnt))
		padata_free_pd(pd_old);

	if (notification_mask)
		blocking_notifier_call_chain(&pinst->cpumask_change_notifier,
					     notification_mask,
					     &pd_new->cpumask);

	pinst->flags &= ~PADATA_RESET;
}

/**
 * padata_register_cpumask_notifier - Registers a notifier that will be called
 *                             if either pcpu or cbcpu or both cpumasks change.
 *
 * @pinst: A poineter to padata instance
 * @nblock: A pointer to notifier block.
 */
int padata_register_cpumask_notifier(struct padata_instance *pinst,
				     struct notifier_block *nblock)
{
	return blocking_notifier_chain_register(&pinst->cpumask_change_notifier,
						nblock);
}
EXPORT_SYMBOL(padata_register_cpumask_notifier);

/**
 * padata_unregister_cpumask_notifier - Unregisters cpumask notifier
 *        registered earlier  using padata_register_cpumask_notifier
 *
 * @pinst: A pointer to data instance.
 * @nlock: A pointer to notifier block.
 */
int padata_unregister_cpumask_notifier(struct padata_instance *pinst,
				       struct notifier_block *nblock)
{
	return blocking_notifier_chain_unregister(
		&pinst->cpumask_change_notifier,
		nblock);
}
EXPORT_SYMBOL(padata_unregister_cpumask_notifier);


/* If cpumask contains no active cpu, we mark the instance as invalid. */
static bool padata_validate_cpumask(struct padata_instance *pinst,
				    const struct cpumask *cpumask)
{
	if (!cpumask_intersects(cpumask, cpu_online_mask)) {
		pinst->flags |= PADATA_INVALID;
		return false;
	}

	pinst->flags &= ~PADATA_INVALID;
	return true;
}

static int __padata_set_cpumasks(struct padata_instance *pinst,
				 cpumask_var_t pcpumask,
				 cpumask_var_t cbcpumask)
{
	int valid;
	struct parallel_data *pd;

	valid = padata_validate_cpumask(pinst, pcpumask);
	if (!valid) {
		__padata_stop(pinst);
		goto out_replace;
	}

	valid = padata_validate_cpumask(pinst, cbcpumask);
	if (!valid)
		__padata_stop(pinst);

out_replace:
	pd = padata_alloc_pd(pinst, pcpumask, cbcpumask);
	if (!pd)
		return -ENOMEM;

	cpumask_copy(pinst->cpumask.pcpu, pcpumask);
	cpumask_copy(pinst->cpumask.cbcpu, cbcpumask);

	padata_replace(pinst, pd);

	if (valid)
		__padata_start(pinst);

	return 0;
}

/**
 * padata_set_cpumask: Sets specified by @cpumask_type cpumask to the value
 *                     equivalent to @cpumask.
 *
 * @pinst: padata instance
 * @cpumask_type: PADATA_CPU_SERIAL or PADATA_CPU_PARALLEL corresponding
 *                to parallel and serial cpumasks respectively.
 * @cpumask: the cpumask to use
 */
int padata_set_cpumask(struct padata_instance *pinst, int cpumask_type,
		       cpumask_var_t cpumask)
{
	struct cpumask *serial_mask, *parallel_mask;
	int err = -EINVAL;

	get_online_cpus();
	mutex_lock(&pinst->lock);

	switch (cpumask_type) {
	case PADATA_CPU_PARALLEL:
		serial_mask = pinst->cpumask.cbcpu;
		parallel_mask = cpumask;
		break;
	case PADATA_CPU_SERIAL:
		parallel_mask = pinst->cpumask.pcpu;
		serial_mask = cpumask;
		break;
	default:
		 goto out;
	}

	err =  __padata_set_cpumasks(pinst, parallel_mask, serial_mask);

out:
	mutex_unlock(&pinst->lock);
	put_online_cpus();

	return err;
}
EXPORT_SYMBOL(padata_set_cpumask);

/**
 * padata_start - start the parallel processing
 *
 * @pinst: padata instance to start
 */
int padata_start(struct padata_instance *pinst)
{
	int err = 0;

	mutex_lock(&pinst->lock);

	if (pinst->flags & PADATA_INVALID)
		err = -EINVAL;

	 __padata_start(pinst);

	mutex_unlock(&pinst->lock);

	return err;
}
EXPORT_SYMBOL(padata_start);

/**
 * padata_stop - stop the parallel processing
 *
 * @pinst: padata instance to stop
 */
void padata_stop(struct padata_instance *pinst)
{
	mutex_lock(&pinst->lock);
	__padata_stop(pinst);
	mutex_unlock(&pinst->lock);
}
EXPORT_SYMBOL(padata_stop);

#ifdef CONFIG_HOTPLUG_CPU

static int __padata_add_cpu(struct padata_instance *pinst, int cpu)
{
	struct parallel_data *pd;

	if (cpumask_test_cpu(cpu, cpu_online_mask)) {
		pd = padata_alloc_pd(pinst, pinst->cpumask.pcpu,
				     pinst->cpumask.cbcpu);
		if (!pd)
			return -ENOMEM;

		padata_replace(pinst, pd);

		if (padata_validate_cpumask(pinst, pinst->cpumask.pcpu) &&
		    padata_validate_cpumask(pinst, pinst->cpumask.cbcpu))
			__padata_start(pinst);
	}

	return 0;
}

static int __padata_remove_cpu(struct padata_instance *pinst, int cpu)
{
	struct parallel_data *pd = NULL;

	if (cpumask_test_cpu(cpu, cpu_online_mask)) {

		if (!padata_validate_cpumask(pinst, pinst->cpumask.pcpu) ||
		    !padata_validate_cpumask(pinst, pinst->cpumask.cbcpu))
			__padata_stop(pinst);

		pd = padata_alloc_pd(pinst, pinst->cpumask.pcpu,
				     pinst->cpumask.cbcpu);
		if (!pd)
			return -ENOMEM;

		padata_replace(pinst, pd);

		cpumask_clear_cpu(cpu, pd->cpumask.cbcpu);
		cpumask_clear_cpu(cpu, pd->cpumask.pcpu);
	}

	return 0;
}

 /**
 * padata_remove_cpu - remove a cpu from the one or both(serial and parallel)
 *                     padata cpumasks.
 *
 * @pinst: padata instance
 * @cpu: cpu to remove
 * @mask: bitmask specifying from which cpumask @cpu should be removed
 *        The @mask may be any combination of the following flags:
 *          PADATA_CPU_SERIAL   - serial cpumask
 *          PADATA_CPU_PARALLEL - parallel cpumask
 */
int padata_remove_cpu(struct padata_instance *pinst, int cpu, int mask)
{
	int err;

	if (!(mask & (PADATA_CPU_SERIAL | PADATA_CPU_PARALLEL)))
		return -EINVAL;

	mutex_lock(&pinst->lock);

	get_online_cpus();
	if (mask & PADATA_CPU_SERIAL)
		cpumask_clear_cpu(cpu, pinst->cpumask.cbcpu);
	if (mask & PADATA_CPU_PARALLEL)
		cpumask_clear_cpu(cpu, pinst->cpumask.pcpu);

	err = __padata_remove_cpu(pinst, cpu);
	put_online_cpus();

	mutex_unlock(&pinst->lock);

	return err;
}
EXPORT_SYMBOL(padata_remove_cpu);

static inline int pinst_has_cpu(struct padata_instance *pinst, int cpu)
{
	return cpumask_test_cpu(cpu, pinst->cpumask.pcpu) ||
		cpumask_test_cpu(cpu, pinst->cpumask.cbcpu);
}

static int padata_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct padata_instance *pinst;
	int ret;

	pinst = hlist_entry_safe(node, struct padata_instance, node);
	if (!pinst_has_cpu(pinst, cpu))
		return 0;

	mutex_lock(&pinst->lock);
	ret = __padata_add_cpu(pinst, cpu);
	mutex_unlock(&pinst->lock);
	return ret;
}

static int padata_cpu_prep_down(unsigned int cpu, struct hlist_node *node)
{
	struct padata_instance *pinst;
	int ret;

	pinst = hlist_entry_safe(node, struct padata_instance, node);
	if (!pinst_has_cpu(pinst, cpu))
		return 0;

	mutex_lock(&pinst->lock);
	ret = __padata_remove_cpu(pinst, cpu);
	mutex_unlock(&pinst->lock);
	return ret;
}

static enum cpuhp_state hp_online;
#endif

static void __padata_free(struct padata_instance *pinst)
{
#ifdef CONFIG_HOTPLUG_CPU
	cpuhp_state_remove_instance_nocalls(hp_online, &pinst->node);
#endif

	padata_stop(pinst);
	padata_free_pd(pinst->pd);
	free_cpumask_var(pinst->cpumask.pcpu);
	free_cpumask_var(pinst->cpumask.cbcpu);
	kfree(pinst);
}

#define kobj2pinst(_kobj)					\
	container_of(_kobj, struct padata_instance, kobj)
#define attr2pentry(_attr)					\
	container_of(_attr, struct padata_sysfs_entry, attr)

static void padata_sysfs_release(struct kobject *kobj)
{
	struct padata_instance *pinst = kobj2pinst(kobj);
	__padata_free(pinst);
}

struct padata_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct padata_instance *, struct attribute *, char *);
	ssize_t (*store)(struct padata_instance *, struct attribute *,
			 const char *, size_t);
};

static ssize_t show_cpumask(struct padata_instance *pinst,
			    struct attribute *attr,  char *buf)
{
	struct cpumask *cpumask;
	ssize_t len;

	mutex_lock(&pinst->lock);
	if (!strcmp(attr->name, "serial_cpumask"))
		cpumask = pinst->cpumask.cbcpu;
	else
		cpumask = pinst->cpumask.pcpu;

	len = snprintf(buf, PAGE_SIZE, "%*pb\n",
		       nr_cpu_ids, cpumask_bits(cpumask));
	mutex_unlock(&pinst->lock);
	return len < PAGE_SIZE ? len : -EINVAL;
}

static ssize_t store_cpumask(struct padata_instance *pinst,
			     struct attribute *attr,
			     const char *buf, size_t count)
{
	cpumask_var_t new_cpumask;
	ssize_t ret;
	int mask_type;

	if (!alloc_cpumask_var(&new_cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = bitmap_parse(buf, count, cpumask_bits(new_cpumask),
			   nr_cpumask_bits);
	if (ret < 0)
		goto out;

	mask_type = !strcmp(attr->name, "serial_cpumask") ?
		PADATA_CPU_SERIAL : PADATA_CPU_PARALLEL;
	ret = padata_set_cpumask(pinst, mask_type, new_cpumask);
	if (!ret)
		ret = count;

out:
	free_cpumask_var(new_cpumask);
	return ret;
}

#define PADATA_ATTR_RW(_name, _show_name, _store_name)		\
	static struct padata_sysfs_entry _name##_attr =		\
		__ATTR(_name, 0644, _show_name, _store_name)
#define PADATA_ATTR_RO(_name, _show_name)		\
	static struct padata_sysfs_entry _name##_attr = \
		__ATTR(_name, 0400, _show_name, NULL)

PADATA_ATTR_RW(serial_cpumask, show_cpumask, store_cpumask);
PADATA_ATTR_RW(parallel_cpumask, show_cpumask, store_cpumask);

/*
 * Padata sysfs provides the following objects:
 * serial_cpumask   [RW] - cpumask for serial workers
 * parallel_cpumask [RW] - cpumask for parallel workers
 */
static struct attribute *padata_default_attrs[] = {
	&serial_cpumask_attr.attr,
	&parallel_cpumask_attr.attr,
	NULL,
};

static ssize_t padata_sysfs_show(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	struct padata_instance *pinst;
	struct padata_sysfs_entry *pentry;
	ssize_t ret = -EIO;

	pinst = kobj2pinst(kobj);
	pentry = attr2pentry(attr);
	if (pentry->show)
		ret = pentry->show(pinst, attr, buf);

	return ret;
}

static ssize_t padata_sysfs_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct padata_instance *pinst;
	struct padata_sysfs_entry *pentry;
	ssize_t ret = -EIO;

	pinst = kobj2pinst(kobj);
	pentry = attr2pentry(attr);
	if (pentry->show)
		ret = pentry->store(pinst, attr, buf, count);

	return ret;
}

static const struct sysfs_ops padata_sysfs_ops = {
	.show = padata_sysfs_show,
	.store = padata_sysfs_store,
};

static struct kobj_type padata_attr_type = {
	.sysfs_ops = &padata_sysfs_ops,
	.default_attrs = padata_default_attrs,
	.release = padata_sysfs_release,
};

/**
 * padata_alloc - allocate and initialize a padata instance and specify
 *                cpumasks for serial and parallel workers.
 *
 * @wq: workqueue to use for the allocated padata instance
 * @pcpumask: cpumask that will be used for padata parallelization
 * @cbcpumask: cpumask that will be used for padata serialization
 *
 * Must be called from a cpus_read_lock() protected region
 */
static struct padata_instance *padata_alloc(struct workqueue_struct *wq,
					    const struct cpumask *pcpumask,
					    const struct cpumask *cbcpumask)
{
	struct padata_instance *pinst;
	struct parallel_data *pd = NULL;

	pinst = kzalloc(sizeof(struct padata_instance), GFP_KERNEL);
	if (!pinst)
		goto err;

	if (!alloc_cpumask_var(&pinst->cpumask.pcpu, GFP_KERNEL))
		goto err_free_inst;
	if (!alloc_cpumask_var(&pinst->cpumask.cbcpu, GFP_KERNEL)) {
		free_cpumask_var(pinst->cpumask.pcpu);
		goto err_free_inst;
	}
	if (!padata_validate_cpumask(pinst, pcpumask) ||
	    !padata_validate_cpumask(pinst, cbcpumask))
		goto err_free_masks;

	pd = padata_alloc_pd(pinst, pcpumask, cbcpumask);
	if (!pd)
		goto err_free_masks;

	rcu_assign_pointer(pinst->pd, pd);

	pinst->wq = wq;

	cpumask_copy(pinst->cpumask.pcpu, pcpumask);
	cpumask_copy(pinst->cpumask.cbcpu, cbcpumask);

	pinst->flags = 0;

	BLOCKING_INIT_NOTIFIER_HEAD(&pinst->cpumask_change_notifier);
	kobject_init(&pinst->kobj, &padata_attr_type);
	mutex_init(&pinst->lock);

#ifdef CONFIG_HOTPLUG_CPU
	cpuhp_state_add_instance_nocalls_cpuslocked(hp_online, &pinst->node);
#endif
	return pinst;

err_free_masks:
	free_cpumask_var(pinst->cpumask.pcpu);
	free_cpumask_var(pinst->cpumask.cbcpu);
err_free_inst:
	kfree(pinst);
err:
	return NULL;
}

/**
 * padata_alloc_possible - Allocate and initialize padata instance.
 *                         Use the cpu_possible_mask for serial and
 *                         parallel workers.
 *
 * @wq: workqueue to use for the allocated padata instance
 *
 * Must be called from a cpus_read_lock() protected region
 */
struct padata_instance *padata_alloc_possible(struct workqueue_struct *wq)
{
	lockdep_assert_cpus_held();
	return padata_alloc(wq, cpu_possible_mask, cpu_possible_mask);
}
EXPORT_SYMBOL(padata_alloc_possible);

/**
 * padata_free - free a padata instance
 *
 * @padata_inst: padata instance to free
 */
void padata_free(struct padata_instance *pinst)
{
	kobject_put(&pinst->kobj);
}
EXPORT_SYMBOL(padata_free);

#ifdef CONFIG_HOTPLUG_CPU

static __init int padata_driver_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "padata:online",
				      padata_cpu_online,
				      padata_cpu_prep_down);
	if (ret < 0)
		return ret;
	hp_online = ret;
	return 0;
}
module_init(padata_driver_init);

static __exit void padata_driver_exit(void)
{
	cpuhp_remove_multi_state(hp_online);
}
module_exit(padata_driver_exit);
#endif
