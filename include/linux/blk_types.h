/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Block data types and constants.  Directly include this file only to
 * break include dependency loop.
 */
#ifndef __LINUX_BLK_TYPES_H
#define __LINUX_BLK_TYPES_H

#include <linux/kabi.h>
#include <linux/types.h>
#include <linux/bvec.h>
#include <linux/ktime.h>

struct bio_set;
struct bio;
struct bio_integrity_payload;
struct page;
struct block_device;
struct io_context;
struct cgroup_subsys_state;
typedef void (bio_end_io_t) (struct bio *);

/*
 * Block error status values.  See block/blk-core:blk_errors for the details.
 * Alpha cannot write a byte atomically, so we need to use 32-bit value.
 */
#if defined(CONFIG_ALPHA) && !defined(__alpha_bwx__)
typedef u32 __bitwise blk_status_t;
#else
typedef u8 __bitwise blk_status_t;
#endif
#define	BLK_STS_OK 0
#define BLK_STS_NOTSUPP		((__force blk_status_t)1)
#define BLK_STS_TIMEOUT		((__force blk_status_t)2)
#define BLK_STS_NOSPC		((__force blk_status_t)3)
#define BLK_STS_TRANSPORT	((__force blk_status_t)4)
#define BLK_STS_TARGET		((__force blk_status_t)5)
#define BLK_STS_NEXUS		((__force blk_status_t)6)
#define BLK_STS_MEDIUM		((__force blk_status_t)7)
#define BLK_STS_PROTECTION	((__force blk_status_t)8)
#define BLK_STS_RESOURCE	((__force blk_status_t)9)
#define BLK_STS_IOERR		((__force blk_status_t)10)

/* hack for device mapper, don't use elsewhere: */
#define BLK_STS_DM_REQUEUE    ((__force blk_status_t)11)

#define BLK_STS_AGAIN		((__force blk_status_t)12)

/*
 * BLK_STS_DEV_RESOURCE is returned from the driver to the block layer if
 * device related resources are unavailable, but the driver can guarantee
 * that the queue will be rerun in the future once resources become
 * available again. This is typically the case for device specific
 * resources that are consumed for IO. If the driver fails allocating these
 * resources, we know that inflight (or pending) IO will free these
 * resource upon completion.
 *
 * This is different from BLK_STS_RESOURCE in that it explicitly references
 * a device specific resource. For resources of wider scope, allocation
 * failure can happen without having pending IO. This means that we can't
 * rely on request completions freeing these resources, as IO may not be in
 * flight. Examples of that are kernel memory allocations, DMA mappings, or
 * any other system wide resources.
 */
#define BLK_STS_DEV_RESOURCE	((__force blk_status_t)13)

/**
 * blk_path_error - returns true if error may be path related
 * @error: status the request was completed with
 *
 * Description:
 *     This classifies block error status into non-retryable errors and ones
 *     that may be successful if retried on a failover path.
 *
 * Return:
 *     %false - retrying failover path will not help
 *     %true  - may succeed if retried
 */
static inline bool blk_path_error(blk_status_t error)
{
	switch (error) {
	case BLK_STS_NOTSUPP:
	case BLK_STS_NOSPC:
	case BLK_STS_TARGET:
	case BLK_STS_NEXUS:
	case BLK_STS_MEDIUM:
	case BLK_STS_PROTECTION:
		return false;
	}

	/* Anything else could be a path failure, so should be retried */
	return true;
}

/*
 * From most significant bit:
 * 1 bit: reserved for other usage, see below
 * 12 bits: original size of bio
 * 51 bits: issue time of bio
 */
#define BIO_ISSUE_RES_BITS      1
#define BIO_ISSUE_SIZE_BITS     12
#define BIO_ISSUE_RES_SHIFT     (64 - BIO_ISSUE_RES_BITS)
#define BIO_ISSUE_SIZE_SHIFT    (BIO_ISSUE_RES_SHIFT - BIO_ISSUE_SIZE_BITS)
#define BIO_ISSUE_TIME_MASK     ((1ULL << BIO_ISSUE_SIZE_SHIFT) - 1)
#define BIO_ISSUE_SIZE_MASK     \
	(((1ULL << BIO_ISSUE_SIZE_BITS) - 1) << BIO_ISSUE_SIZE_SHIFT)
#define BIO_ISSUE_RES_MASK      (~((1ULL << BIO_ISSUE_RES_SHIFT) - 1))

/* Reserved bit for blk-throtl */
#define BIO_ISSUE_THROTL_SKIP_LATENCY (1ULL << 63)

struct bio_issue {
	u64 value;
};

static inline u64 __bio_issue_time(u64 time)
{
	return time & BIO_ISSUE_TIME_MASK;
}

static inline u64 bio_issue_time(struct bio_issue *issue)
{
	return __bio_issue_time(issue->value);
}

static inline sector_t bio_issue_size(struct bio_issue *issue)
{
	return ((issue->value & BIO_ISSUE_SIZE_MASK) >> BIO_ISSUE_SIZE_SHIFT);
}

static inline void bio_issue_init(struct bio_issue *issue,
				       sector_t size)
{
	size &= (1ULL << BIO_ISSUE_SIZE_BITS) - 1;
	issue->value = ((issue->value & BIO_ISSUE_RES_MASK) |
			(ktime_get_ns() & BIO_ISSUE_TIME_MASK) |
			((u64)size << BIO_ISSUE_SIZE_SHIFT));
}

/*
 * main unit of I/O for the block layer and lower layers (ie drivers and
 * stacking drivers)
 */
struct bio {
	struct bio		*bi_next;	/* request queue link */
	struct gendisk		*bi_disk;
	unsigned int		bi_opf;		/* bottom bits req flags,
						 * top bits REQ_OP. Use
						 * accessors.
						 */
	unsigned short		bi_flags;	/* status, etc and bvec pool number */
	unsigned short		bi_ioprio;
	unsigned short		bi_write_hint;
	blk_status_t		bi_status;
	u8			bi_partno;

	/* Number of segments in this BIO after
	 * physical address coalescing is performed.
	 */
	unsigned int		bi_phys_segments;

	/*
	 * To keep track of the max segment size, we account for the
	 * sizes of the first and last mergeable segments in this bio.
	 */
	unsigned int		bi_seg_front_size;
	unsigned int		bi_seg_back_size;

	struct bvec_iter	bi_iter;

	atomic_t		__bi_remaining;
	bio_end_io_t		*bi_end_io;

	void			*bi_private;
#ifdef CONFIG_BLK_CGROUP
	/*
	 * Optional ioc and css associated with this bio.  Put on bio
	 * release.  Read comment on top of bio_associate_current().
	 */
	struct io_context	*bi_ioc;
	struct cgroup_subsys_state *bi_css;
	struct blkcg_gq		*bi_blkg;
	struct bio_issue	bi_issue;
#endif
	union {
#if defined(CONFIG_BLK_DEV_INTEGRITY)
		struct bio_integrity_payload *bi_integrity; /* data integrity */
#endif
	};

	unsigned short		bi_vcnt;	/* how many bio_vec's */

	/*
	 * Everything starting with bi_max_vecs will be preserved by bio_reset()
	 */

	unsigned short		bi_max_vecs;	/* max bvl_vecs we can hold */

	atomic_t		__bi_cnt;	/* pin count */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */

	struct bio_set		*bi_pool;

#if defined(CONFIG_BLK_BIO_ALLOC_TIME) && !defined(__GENKSYMS__)
	u64			bi_alloc_time_ns;
#else
	KABI_RESERVE(1)
#endif
#if defined(CONFIG_BLK_BIO_ALLOC_TASK) && !defined(__GENKSYMS__)
	struct pid		*pid;
#else
	KABI_RESERVE(2)
#endif
#if defined(CONFIG_BLK_IO_HIERARCHY_STATS) && !defined(__GENKSYMS__)
	struct bio_hierarchy_data *hdata;
#else
	KABI_RESERVE(3)
#endif

	/*
	 * We can inline a number of vecs at the end of the bio, to avoid
	 * double allocations for a small number of bio_vecs. This member
	 * MUST obviously be kept at the very end of the bio.
	 */
	struct bio_vec		bi_inline_vecs[0];
};

#define BIO_RESET_BYTES		offsetof(struct bio, bi_max_vecs)

/*
 * bio flags
 */
#ifdef CONFIG_BLK_IO_HIERARCHY_STATS
#define BIO_HIERARCHY_ACCT 0	/*
				 * This bio has already been subjected to
				 * blk-io-hierarchy, don't do it again.
				 */
#endif
#define BIO_SEG_VALID	1	/* bi_phys_segments valid */
#define BIO_CLONED	2	/* doesn't own data */
#define BIO_BOUNCED	3	/* bio is a bounce bio */
#define BIO_USER_MAPPED 4	/* contains user pages */
#define BIO_NULL_MAPPED 5	/* contains invalid user pages */
#define BIO_QUIET	6	/* Make BIO Quiet */
#define BIO_CHAIN	7	/* chained bio, ->bi_remaining in effect */
#define BIO_REFFED	8	/* bio has elevated ->bi_cnt */
#define BIO_THROTTLED	9	/* This bio has already been subjected to
				 * throttling rules. Don't do it again. */
#define BIO_TRACE_COMPLETION 10	/* bio_endio() should trace the final completion
				 * of this bio. */
#define BIO_QUEUE_ENTERED 11	/* can use blk_queue_enter_live() */
#define BIO_TRACKED 12		/* set if bio goes through the rq_qos path */

/* See BVEC_POOL_OFFSET below before adding new flags */

/*
 * We support 6 different bvec pools, the last one is magic in that it
 * is backed by a mempool.
 */
#define BVEC_POOL_NR		6
#define BVEC_POOL_MAX		(BVEC_POOL_NR - 1)

/*
 * Top 3 bits of bio flags indicate the pool the bvecs came from.  We add
 * 1 to the actual index so that 0 indicates that there are no bvecs to be
 * freed.
 */
#define BVEC_POOL_BITS		(3)
#define BVEC_POOL_OFFSET	(16 - BVEC_POOL_BITS)
#define BVEC_POOL_IDX(bio)	((bio)->bi_flags >> BVEC_POOL_OFFSET)
#if (1<< BVEC_POOL_BITS) < (BVEC_POOL_NR+1)
# error "BVEC_POOL_BITS is too small"
#endif

/*
 * Flags starting here get preserved by bio_reset() - this includes
 * only BVEC_POOL_IDX()
 */
#define BIO_RESET_BITS	BVEC_POOL_OFFSET

typedef __u32 __bitwise blk_mq_req_flags_t;

/*
 * Operations and flags common to the bio and request structures.
 * We use 8 bits for encoding the operation, and the remaining 24 for flags.
 *
 * The least significant bit of the operation number indicates the data
 * transfer direction:
 *
 *   - if the least significant bit is set transfers are TO the device
 *   - if the least significant bit is not set transfers are FROM the device
 *
 * If a operation does not transfer data the least significant bit has no
 * meaning.
 */
#define REQ_OP_BITS	8
#define REQ_OP_MASK	((1 << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS	24

enum req_opf {
	/* read sectors from the device */
	REQ_OP_READ		= 0,
	/* write sectors to the device */
	REQ_OP_WRITE		= 1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH		= 2,
	/* discard sectors */
	REQ_OP_DISCARD		= 3,
	/* get zone information */
	REQ_OP_ZONE_REPORT	= 4,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE	= 5,
	/* seset a zone write pointer */
	REQ_OP_ZONE_RESET	= 6,
	/* write the same sector many times */
	REQ_OP_WRITE_SAME	= 7,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES	= 9,

	/* SCSI passthrough using struct scsi_request */
	REQ_OP_SCSI_IN		= 32,
	REQ_OP_SCSI_OUT		= 33,
	/* Driver private requests */
	REQ_OP_DRV_IN		= 34,
	REQ_OP_DRV_OUT		= 35,

	REQ_OP_LAST,
};

enum req_flag_bits {
	__REQ_FAILFAST_DEV =	/* no driver retries of device errors */
		REQ_OP_BITS,
	__REQ_FAILFAST_TRANSPORT, /* no driver retries of transport errors */
	__REQ_FAILFAST_DRIVER,	/* no driver retries of driver errors */
	__REQ_SYNC,		/* request is sync (sync write or read) */
	__REQ_META,		/* metadata io request */
	__REQ_PRIO,		/* boost priority in cfq */
	__REQ_NOMERGE,		/* don't touch this for merging */
	__REQ_IDLE,		/* anticipate more IO after this one */
	__REQ_INTEGRITY,	/* I/O includes block integrity payload */
	__REQ_FUA,		/* forced unit access */
	__REQ_PREFLUSH,		/* request for cache flush */
	__REQ_RAHEAD,		/* read ahead, can fail anytime */
	__REQ_BACKGROUND,	/* background IO */
	__REQ_NOWAIT,           /* Don't wait if request will block */

	/* command specific flags for REQ_OP_WRITE_ZEROES: */
	__REQ_NOUNMAP,		/* do not free blocks when zeroing */

	/* for driver use */
	__REQ_DRV,
	__REQ_SWAP,		/* swapping request. */
#ifdef CONFIG_BLK_IO_HIERARCHY_STATS
	_REQ_HAS_DATA,		/* io contain data. */
#endif
	__REQ_NR_BITS,		/* stops here */
};

#define REQ_FAILFAST_DEV	(1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT	(1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER	(1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC		(1ULL << __REQ_SYNC)
#define REQ_META		(1ULL << __REQ_META)
#define REQ_PRIO		(1ULL << __REQ_PRIO)
#define REQ_NOMERGE		(1ULL << __REQ_NOMERGE)
#define REQ_IDLE		(1ULL << __REQ_IDLE)
#define REQ_INTEGRITY		(1ULL << __REQ_INTEGRITY)
#define REQ_FUA			(1ULL << __REQ_FUA)
#define REQ_PREFLUSH		(1ULL << __REQ_PREFLUSH)
#define REQ_RAHEAD		(1ULL << __REQ_RAHEAD)
#define REQ_BACKGROUND		(1ULL << __REQ_BACKGROUND)
#define REQ_NOWAIT		(1ULL << __REQ_NOWAIT)

#define REQ_NOUNMAP		(1ULL << __REQ_NOUNMAP)

#define REQ_DRV			(1ULL << __REQ_DRV)
#define REQ_SWAP		(1ULL << __REQ_SWAP)
#ifdef CONFIG_BLK_IO_HIERARCHY_STATS
#define REQ_HAS_DATA		(1UL << _REQ_HAS_DATA)
#endif

#define REQ_FAILFAST_MASK \
	(REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER)

#define REQ_NOMERGE_FLAGS \
	(REQ_NOMERGE | REQ_PREFLUSH | REQ_FUA)

enum stat_group {
	STAT_READ,
	STAT_WRITE,
	STAT_DISCARD,

	NR_STAT_GROUPS,
	STAT_FLUSH = NR_STAT_GROUPS,
	NR_NEW_STAT_GROUPS,
};

enum stage_group {
#ifdef CONFIG_BLK_DEV_THROTTLING
	STAGE_THROTTLE,
#endif
#ifdef CONFIG_BLK_WBT
	STAGE_WBT,
#endif
	STAGE_GETTAG,
	NR_BIO_STAGE_GROUPS,
	STAGE_PLUG = NR_BIO_STAGE_GROUPS,
#if IS_ENABLED(CONFIG_MQ_IOSCHED_DEADLINE)
	STAGE_DEADLINE,
#endif
#if IS_ENABLED(CONFIG_IOSCHED_BFQ)
	STAGE_BFQ,
#endif
#if IS_ENABLED(CONFIG_MQ_IOSCHED_KYBER)
	STAGE_KYBER,
#endif
	STAGE_HCTX,
	STAGE_REQUEUE,
	STAGE_RQ_DRIVER,
	NR_RQ_STAGE_GROUPS,
	STAGE_BIO = NR_RQ_STAGE_GROUPS,
	NR_STAGE_GROUPS,
};

#define bio_op(bio) \
	((bio)->bi_opf & REQ_OP_MASK)
#define req_op(req) \
	((req)->cmd_flags & REQ_OP_MASK)

/* obsolete, don't use in new code */
static inline void bio_set_op_attrs(struct bio *bio, unsigned op,
		unsigned op_flags)
{
	bio->bi_opf = op | op_flags;
}

static inline bool op_is_write(unsigned int op)
{
	return (op & 1);
}

/*
 * Check if the bio or request is one that needs special treatment in the
 * flush state machine.
 */
static inline bool op_is_flush(unsigned int op)
{
	return op & (REQ_FUA | REQ_PREFLUSH);
}

/*
 * Reads are always treated as synchronous, as are requests with the FUA or
 * PREFLUSH flag.  Other operations may be marked as synchronous using the
 * REQ_SYNC flag.
 */
static inline bool op_is_sync(unsigned int op)
{
	return (op & REQ_OP_MASK) == REQ_OP_READ ||
		(op & (REQ_SYNC | REQ_FUA | REQ_PREFLUSH));
}

static inline bool op_is_discard(unsigned int op)
{
	return (op & REQ_OP_MASK) == REQ_OP_DISCARD;
}

static inline int op_stat_group(unsigned int op)
{
	if (op_is_discard(op))
		return STAT_DISCARD;
	return op_is_write(op);
}

typedef unsigned int blk_qc_t;
#define BLK_QC_T_NONE		-1U
#define BLK_QC_T_SHIFT		16
#define BLK_QC_T_INTERNAL	(1U << 31)

static inline bool blk_qc_t_valid(blk_qc_t cookie)
{
	return cookie != BLK_QC_T_NONE;
}

static inline blk_qc_t blk_tag_to_qc_t(unsigned int tag, unsigned int queue_num,
				       bool internal)
{
	blk_qc_t ret = tag | (queue_num << BLK_QC_T_SHIFT);

	if (internal)
		ret |= BLK_QC_T_INTERNAL;

	return ret;
}

static inline unsigned int blk_qc_t_to_queue_num(blk_qc_t cookie)
{
	return (cookie & ~BLK_QC_T_INTERNAL) >> BLK_QC_T_SHIFT;
}

static inline unsigned int blk_qc_t_to_tag(blk_qc_t cookie)
{
	return cookie & ((1u << BLK_QC_T_SHIFT) - 1);
}

static inline bool blk_qc_t_is_internal(blk_qc_t cookie)
{
	return (cookie & BLK_QC_T_INTERNAL) != 0;
}

struct blk_rq_stat {
	u64 mean;
	u64 min;
	u64 max;
	u32 nr_samples;
	u64 batch;
};

#endif /* __LINUX_BLK_TYPES_H */
