/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_DST_OPS_H
#define _NET_DST_OPS_H
#include <linux/kabi.h>
#include <linux/types.h>
#include <linux/percpu_counter.h>
#include <linux/cache.h>

struct dst_entry;
struct kmem_cachep;
struct net_device;
struct sk_buff;
struct sock;
struct net;

struct dst_ops {
	unsigned short		family;
	unsigned int		gc_thresh;

	int			(*gc)(struct dst_ops *ops);
	struct dst_entry *	(*check)(struct dst_entry *, __u32 cookie);
	unsigned int		(*default_advmss)(const struct dst_entry *);
	unsigned int		(*mtu)(const struct dst_entry *);
	u32 *			(*cow_metrics)(struct dst_entry *, unsigned long);
	void			(*destroy)(struct dst_entry *);
	void			(*ifdown)(struct dst_entry *,
					  struct net_device *dev, int how);
#ifdef __GENKSYMS__
	struct dst_entry *	(*negative_advice)(struct dst_entry *);
#else
	void			(*negative_advice)(struct sock *sk, struct dst_entry *);
#endif
	void			(*link_failure)(struct sk_buff *);
	void			(*update_pmtu)(struct dst_entry *dst, struct sock *sk,
					       struct sk_buff *skb, u32 mtu,
					       bool confirm_neigh);
	void			(*redirect)(struct dst_entry *dst, struct sock *sk,
					    struct sk_buff *skb);
	int			(*local_out)(struct net *net, struct sock *sk, struct sk_buff *skb);
	struct neighbour *	(*neigh_lookup)(const struct dst_entry *dst,
						struct sk_buff *skb,
						const void *daddr);
	void			(*confirm_neigh)(const struct dst_entry *dst,
						 const void *daddr);

	struct kmem_cache	*kmem_cachep;

	struct percpu_counter	pcpuc_entries ____cacheline_aligned_in_smp;

	KABI_RESERVE(1)
	KABI_RESERVE(2)
	KABI_RESERVE(3)
	KABI_RESERVE(4)
	KABI_RESERVE(5)
	KABI_RESERVE(6)
	KABI_RESERVE(7)
	KABI_RESERVE(8)
};

static inline int dst_entries_get_fast(struct dst_ops *dst)
{
	return percpu_counter_read_positive(&dst->pcpuc_entries);
}

static inline int dst_entries_get_slow(struct dst_ops *dst)
{
	return percpu_counter_sum_positive(&dst->pcpuc_entries);
}

#define DST_PERCPU_COUNTER_BATCH 32
static inline void dst_entries_add(struct dst_ops *dst, int val)
{
	percpu_counter_add_batch(&dst->pcpuc_entries, val,
				 DST_PERCPU_COUNTER_BATCH);
}

static inline int dst_entries_init(struct dst_ops *dst)
{
	return percpu_counter_init(&dst->pcpuc_entries, 0, GFP_KERNEL);
}

static inline void dst_entries_destroy(struct dst_ops *dst)
{
	percpu_counter_destroy(&dst->pcpuc_entries);
}

#endif
