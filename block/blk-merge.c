// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to segment and merge handling
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>

#include <trace/events/block.h>

#include "blk.h"

static struct bio *blk_bio_discard_split(struct request_queue *q,
					 struct bio *bio,
					 struct bio_set *bs,
					 unsigned *nsegs)
{
	unsigned int max_discard_sectors, granularity;
	int alignment;
	sector_t tmp;
	unsigned split_sectors;

	*nsegs = 1;

	/* Zero-sector (unknown) and one-sector granularities are the same.  */
	granularity = max(q->limits.discard_granularity >> 9, 1U);

	max_discard_sectors = min(q->limits.max_discard_sectors,
			bio_allowed_max_sectors(q));
	max_discard_sectors -= max_discard_sectors % granularity;

	if (unlikely(!max_discard_sectors)) {
		/* XXX: warn */
		return NULL;
	}

	if (bio_sectors(bio) <= max_discard_sectors)
		return NULL;

	split_sectors = max_discard_sectors;

	/*
	 * If the next starting sector would be misaligned, stop the discard at
	 * the previous aligned sector.
	 */
	alignment = (q->limits.discard_alignment >> 9) % granularity;

	tmp = bio->bi_iter.bi_sector + split_sectors - alignment;
	tmp = sector_div(tmp, granularity);

	if (split_sectors > tmp)
		split_sectors -= tmp;

	return bio_split(bio, split_sectors, GFP_NOIO, bs);
}

static struct bio *blk_bio_write_zeroes_split(struct request_queue *q,
		struct bio *bio, struct bio_set *bs, unsigned *nsegs)
{
	*nsegs = 1;

	if (!q->limits.max_write_zeroes_sectors)
		return NULL;

	if (bio_sectors(bio) <= q->limits.max_write_zeroes_sectors)
		return NULL;

	return bio_split(bio, q->limits.max_write_zeroes_sectors, GFP_NOIO, bs);
}

static struct bio *blk_bio_write_same_split(struct request_queue *q,
					    struct bio *bio,
					    struct bio_set *bs,
					    unsigned *nsegs)
{
	*nsegs = 1;

	if (!q->limits.max_write_same_sectors)
		return NULL;

	if (bio_sectors(bio) <= q->limits.max_write_same_sectors)
		return NULL;

	return bio_split(bio, q->limits.max_write_same_sectors, GFP_NOIO, bs);
}

static inline unsigned get_max_io_size(struct request_queue *q,
				       struct bio *bio)
{
	unsigned sectors = blk_max_size_offset(q, bio->bi_iter.bi_sector);
	unsigned mask = queue_logical_block_size(q) - 1;

	/* aligned to logical block size */
	sectors &= ~(mask >> 9);

	return sectors;
}

static struct bio *blk_bio_segment_split(struct request_queue *q,
					 struct bio *bio,
					 struct bio_set *bs,
					 unsigned *segs)
{
	struct bio_vec bv, bvprv, *bvprvp = NULL;
	struct bvec_iter iter;
	unsigned seg_size = 0, nsegs = 0, sectors = 0;
	unsigned front_seg_size = bio->bi_seg_front_size;
	bool do_split = true;
	struct bio *new = NULL;
	const unsigned max_sectors = get_max_io_size(q, bio);

	bio_for_each_segment(bv, bio, iter) {
		/*
		 * If the queue doesn't support SG gaps and adding this
		 * offset would create a gap, disallow it.
		 */
		if (bvprvp && bvec_gap_to_prev(q, bvprvp, bv.bv_offset))
			goto split;

		if (sectors + (bv.bv_len >> 9) > max_sectors) {
			/*
			 * Consider this a new segment if we're splitting in
			 * the middle of this vector.
			 */
			if (nsegs < queue_max_segments(q) &&
			    sectors < max_sectors) {
				nsegs++;
				sectors = max_sectors;
			}
			goto split;
		}

		if (bvprvp && blk_queue_cluster(q)) {
			if (seg_size + bv.bv_len > queue_max_segment_size(q))
				goto new_segment;
			if (!BIOVEC_PHYS_MERGEABLE(bvprvp, &bv))
				goto new_segment;
			if (!BIOVEC_SEG_BOUNDARY(q, bvprvp, &bv))
				goto new_segment;

			seg_size += bv.bv_len;
			bvprv = bv;
			bvprvp = &bvprv;
			sectors += bv.bv_len >> 9;

			continue;
		}
new_segment:
		if (nsegs == queue_max_segments(q))
			goto split;

		if (nsegs == 1 && seg_size > front_seg_size)
			front_seg_size = seg_size;

		nsegs++;
		bvprv = bv;
		bvprvp = &bvprv;
		seg_size = bv.bv_len;
		sectors += bv.bv_len >> 9;

	}

	do_split = false;
split:
	*segs = nsegs;

	if (do_split) {
		new = bio_split(bio, sectors, GFP_NOIO, bs);
		if (new)
			bio = new;
	}

	if (nsegs == 1 && seg_size > front_seg_size)
		front_seg_size = seg_size;
	bio->bi_seg_front_size = front_seg_size;
	if (seg_size > bio->bi_seg_back_size)
		bio->bi_seg_back_size = seg_size;

	return do_split ? new : NULL;
}

void blk_queue_split(struct request_queue *q, struct bio **bio)
{
	struct bio *split, *res;
	unsigned nsegs;

	switch (bio_op(*bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		split = blk_bio_discard_split(q, *bio, &q->bio_split, &nsegs);
		break;
	case REQ_OP_WRITE_ZEROES:
		split = blk_bio_write_zeroes_split(q, *bio, &q->bio_split, &nsegs);
		break;
	case REQ_OP_WRITE_SAME:
		split = blk_bio_write_same_split(q, *bio, &q->bio_split, &nsegs);
		break;
	default:
		split = blk_bio_segment_split(q, *bio, &q->bio_split, &nsegs);
		break;
	}

	/* physical segments can be figured out during splitting */
	res = split ? split : *bio;
	res->bi_phys_segments = nsegs;
	bio_set_flag(res, BIO_SEG_VALID);

	if (split) {
		/* there isn't chance to merge the splitted bio */
		split->bi_opf |= REQ_NOMERGE;

		/*
		 * Since we're recursing into make_request here, ensure
		 * that we mark this bio as already having entered the queue.
		 * If not, and the queue is going away, we can get stuck
		 * forever on waiting for the queue reference to drop. But
		 * that will never happen, as we're already holding a
		 * reference to it.
		 */
		bio_set_flag(*bio, BIO_QUEUE_ENTERED);

		bio_chain(split, *bio);
		trace_block_split(q, split, (*bio)->bi_iter.bi_sector);
		generic_make_request(*bio);
		*bio = split;
	}
}
EXPORT_SYMBOL(blk_queue_split);

static unsigned int __blk_recalc_rq_segments(struct request_queue *q,
					     struct bio *bio,
					     bool no_sg_merge)
{
	struct bio_vec bv, bvprv = { NULL };
	int cluster, prev = 0;
	unsigned int nr_phys_segs = 0;
	unsigned int seg_size;
	struct bio *fbio, *bbio;
	struct bvec_iter iter;

	if (!bio)
		return 0;

	switch (bio_op(bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		if (queue_max_discard_segments(q) > 1) {
			for_each_bio(bio)
				nr_phys_segs++;
			return nr_phys_segs;
		}
		return 1;
	case REQ_OP_WRITE_ZEROES:
		return 0;
	case REQ_OP_WRITE_SAME:
		return 1;
	}

	fbio = bio;
	cluster = blk_queue_cluster(q);
	seg_size = 0;
	for_each_bio(bio) {
		bio_for_each_segment(bv, bio, iter) {
			/*
			 * If SG merging is disabled, each bio vector is
			 * a segment
			 */
			if (no_sg_merge)
				goto new_segment;

			if (prev && cluster) {
				if (seg_size + bv.bv_len
				    > queue_max_segment_size(q))
					goto new_segment;
				if (!BIOVEC_PHYS_MERGEABLE(&bvprv, &bv))
					goto new_segment;
				if (!BIOVEC_SEG_BOUNDARY(q, &bvprv, &bv))
					goto new_segment;

				seg_size += bv.bv_len;
				bvprv = bv;
				continue;
			}
new_segment:
			if (nr_phys_segs == 1 && seg_size >
			    fbio->bi_seg_front_size)
				fbio->bi_seg_front_size = seg_size;

			nr_phys_segs++;
			bvprv = bv;
			prev = 1;
			seg_size = bv.bv_len;
		}
		bbio = bio;
	}

	if (nr_phys_segs == 1 && seg_size > fbio->bi_seg_front_size)
		fbio->bi_seg_front_size = seg_size;
	if (seg_size > bbio->bi_seg_back_size)
		bbio->bi_seg_back_size = seg_size;

	return nr_phys_segs;
}

void blk_recalc_rq_segments(struct request *rq)
{
	bool no_sg_merge = !!test_bit(QUEUE_FLAG_NO_SG_MERGE,
			&rq->q->queue_flags);

	rq->nr_phys_segments = __blk_recalc_rq_segments(rq->q, rq->bio,
			no_sg_merge);
}

void blk_recount_segments(struct request_queue *q, struct bio *bio)
{
	unsigned short seg_cnt = bio_segments(bio);

	if (test_bit(QUEUE_FLAG_NO_SG_MERGE, &q->queue_flags) &&
			(seg_cnt < queue_max_segments(q)))
		bio->bi_phys_segments = seg_cnt;
	else {
		struct bio *nxt = bio->bi_next;

		bio->bi_next = NULL;
		bio->bi_phys_segments = __blk_recalc_rq_segments(q, bio, false);
		bio->bi_next = nxt;
	}

	bio_set_flag(bio, BIO_SEG_VALID);
}
EXPORT_SYMBOL(blk_recount_segments);

static int blk_phys_contig_segment(struct request_queue *q, struct bio *bio,
				   struct bio *nxt)
{
	struct bio_vec end_bv = { NULL }, nxt_bv;

	if (!blk_queue_cluster(q))
		return 0;

	if (bio->bi_seg_back_size + nxt->bi_seg_front_size >
	    queue_max_segment_size(q))
		return 0;

	if (!bio_has_data(bio))
		return 1;

	bio_get_last_bvec(bio, &end_bv);
	bio_get_first_bvec(nxt, &nxt_bv);

	if (!BIOVEC_PHYS_MERGEABLE(&end_bv, &nxt_bv))
		return 0;

	/*
	 * bio and nxt are contiguous in memory; check if the queue allows
	 * these two to be merged into one
	 */
	if (BIOVEC_SEG_BOUNDARY(q, &end_bv, &nxt_bv))
		return 1;

	return 0;
}

static inline void
__blk_segment_map_sg(struct request_queue *q, struct bio_vec *bvec,
		     struct scatterlist *sglist, struct bio_vec *bvprv,
		     struct scatterlist **sg, int *nsegs, int *cluster)
{

	int nbytes = bvec->bv_len;

	if (*sg && *cluster) {
		if ((*sg)->length + nbytes > queue_max_segment_size(q))
			goto new_segment;

		if (!BIOVEC_PHYS_MERGEABLE(bvprv, bvec))
			goto new_segment;
		if (!BIOVEC_SEG_BOUNDARY(q, bvprv, bvec))
			goto new_segment;

		(*sg)->length += nbytes;
	} else {
new_segment:
		if (!*sg)
			*sg = sglist;
		else {
			/*
			 * If the driver previously mapped a shorter
			 * list, we could see a termination bit
			 * prematurely unless it fully inits the sg
			 * table on each mapping. We KNOW that there
			 * must be more entries here or the driver
			 * would be buggy, so force clear the
			 * termination bit to avoid doing a full
			 * sg_init_table() in drivers for each command.
			 */
			sg_unmark_end(*sg);
			*sg = sg_next(*sg);
		}

		sg_set_page(*sg, bvec->bv_page, nbytes, bvec->bv_offset);
		(*nsegs)++;
	}
	*bvprv = *bvec;
}

static inline int __blk_bvec_map_sg(struct request_queue *q, struct bio_vec bv,
		struct scatterlist *sglist, struct scatterlist **sg)
{
	*sg = sglist;
	sg_set_page(*sg, bv.bv_page, bv.bv_len, bv.bv_offset);
	return 1;
}

static int __blk_bios_map_sg(struct request_queue *q, struct bio *bio,
			     struct scatterlist *sglist,
			     struct scatterlist **sg)
{
	struct bio_vec bvec, bvprv = { NULL };
	struct bvec_iter iter;
	int cluster = blk_queue_cluster(q), nsegs = 0;

	for_each_bio(bio)
		bio_for_each_segment(bvec, bio, iter)
			__blk_segment_map_sg(q, &bvec, sglist, &bvprv, sg,
					     &nsegs, &cluster);

	return nsegs;
}

/*
 * map a request to scatterlist, return number of sg entries setup. Caller
 * must make sure sg can hold rq->nr_phys_segments entries
 */
int blk_rq_map_sg(struct request_queue *q, struct request *rq,
		  struct scatterlist *sglist)
{
	struct scatterlist *sg = NULL;
	int nsegs = 0;

	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD)
		nsegs = __blk_bvec_map_sg(q, rq->special_vec, sglist, &sg);
	else if (rq->bio && bio_op(rq->bio) == REQ_OP_WRITE_SAME)
		nsegs = __blk_bvec_map_sg(q, bio_iovec(rq->bio), sglist, &sg);
	else if (rq->bio)
		nsegs = __blk_bios_map_sg(q, rq->bio, sglist, &sg);

	if (unlikely(rq->rq_flags & RQF_COPY_USER) &&
	    (blk_rq_bytes(rq) & q->dma_pad_mask)) {
		unsigned int pad_len =
			(q->dma_pad_mask & ~blk_rq_bytes(rq)) + 1;

		sg->length += pad_len;
		rq->extra_len += pad_len;
	}

	if (q->dma_drain_size && q->dma_drain_needed(rq)) {
		if (op_is_write(req_op(rq)))
			memset(q->dma_drain_buffer, 0, q->dma_drain_size);

		sg_unmark_end(sg);
		sg = sg_next(sg);
		sg_set_page(sg, virt_to_page(q->dma_drain_buffer),
			    q->dma_drain_size,
			    ((unsigned long)q->dma_drain_buffer) &
			    (PAGE_SIZE - 1));
		nsegs++;
		rq->extra_len += q->dma_drain_size;
	}

	if (sg)
		sg_mark_end(sg);

	/*
	 * Something must have been wrong if the figured number of
	 * segment is bigger than number of req's physical segments
	 */
	WARN_ON(nsegs > blk_rq_nr_phys_segments(rq));

	return nsegs;
}
EXPORT_SYMBOL(blk_rq_map_sg);

static inline unsigned int blk_rq_get_max_segments(struct request *rq)
{
	if (req_op(rq) == REQ_OP_DISCARD)
		return queue_max_discard_segments(rq->q);
	return queue_max_segments(rq->q);
}

static inline int ll_new_hw_segment(struct request_queue *q,
				    struct request *req,
				    struct bio *bio)
{
	int nr_phys_segs = bio_phys_segments(q, bio);

	if (req->nr_phys_segments + nr_phys_segs > blk_rq_get_max_segments(req))
		goto no_merge;

	if (blk_integrity_merge_bio(q, req, bio) == false)
		goto no_merge;

	/*
	 * This will form the start of a new hw segment.  Bump both
	 * counters.
	 */
	req->nr_phys_segments += nr_phys_segs;
	return 1;

no_merge:
	req_set_nomerge(q, req);
	return 0;
}

int ll_back_merge_fn(struct request_queue *q, struct request *req,
		     struct bio *bio)
{
	if (req_gap_back_merge(req, bio))
		return 0;
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_back_merge(req, bio))
		return 0;
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req))) {
		req_set_nomerge(q, req);
		return 0;
	}
	if (!bio_flagged(req->biotail, BIO_SEG_VALID))
		blk_recount_segments(q, req->biotail);
	if (!bio_flagged(bio, BIO_SEG_VALID))
		blk_recount_segments(q, bio);

	return ll_new_hw_segment(q, req, bio);
}

int ll_front_merge_fn(struct request_queue *q, struct request *req,
		      struct bio *bio)
{

	if (req_gap_front_merge(req, bio))
		return 0;
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_front_merge(req, bio))
		return 0;
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, bio->bi_iter.bi_sector)) {
		req_set_nomerge(q, req);
		return 0;
	}
	if (!bio_flagged(bio, BIO_SEG_VALID))
		blk_recount_segments(q, bio);
	if (!bio_flagged(req->bio, BIO_SEG_VALID))
		blk_recount_segments(q, req->bio);

	return ll_new_hw_segment(q, req, bio);
}

/*
 * blk-mq uses req->special to carry normal driver per-request payload, it
 * does not indicate a prepared command that we cannot merge with.
 */
static bool req_no_special_merge(struct request *req)
{
	struct request_queue *q = req->q;

	return !q->mq_ops && req->special;
}

static bool req_attempt_discard_merge(struct request_queue *q, struct request *req,
		struct request *next)
{
	unsigned short segments = blk_rq_nr_discard_segments(req);

	if (segments >= queue_max_discard_segments(q))
		goto no_merge;
	if (blk_rq_sectors(req) + bio_sectors(next->bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		goto no_merge;

	req->nr_phys_segments = segments + blk_rq_nr_discard_segments(next);
	return true;
no_merge:
	req_set_nomerge(q, req);
	return false;
}

static int ll_merge_requests_fn(struct request_queue *q, struct request *req,
				struct request *next)
{
	int total_phys_segments;
	unsigned int seg_size =
		req->biotail->bi_seg_back_size + next->bio->bi_seg_front_size;

	/*
	 * First check if the either of the requests are re-queued
	 * requests.  Can't merge them if they are.
	 */
	if (req_no_special_merge(req) || req_no_special_merge(next))
		return 0;

	if (req_gap_back_merge(req, next->bio))
		return 0;

	/*
	 * Will it become too large?
	 */
	if ((blk_rq_sectors(req) + blk_rq_sectors(next)) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		return 0;

	total_phys_segments = req->nr_phys_segments + next->nr_phys_segments;
	if (blk_phys_contig_segment(q, req->biotail, next->bio)) {
		if (req->nr_phys_segments == 1)
			req->bio->bi_seg_front_size = seg_size;
		if (next->nr_phys_segments == 1)
			next->biotail->bi_seg_back_size = seg_size;
		total_phys_segments--;
	}

	if (total_phys_segments > blk_rq_get_max_segments(req))
		return 0;

	if (blk_integrity_merge_rq(q, req, next) == false)
		return 0;

	/* Merge is OK... */
	req->nr_phys_segments = total_phys_segments;
	return 1;
}

/**
 * blk_rq_set_mixed_merge - mark a request as mixed merge
 * @rq: request to mark as mixed merge
 *
 * Description:
 *     @rq is about to be mixed merged.  Make sure the attributes
 *     which can be mixed are set in each bio and mark @rq as mixed
 *     merged.
 */
void blk_rq_set_mixed_merge(struct request *rq)
{
	unsigned int ff = rq->cmd_flags & REQ_FAILFAST_MASK;
	struct bio *bio;

	if (rq->rq_flags & RQF_MIXED_MERGE)
		return;

	/*
	 * @rq will no longer represent mixable attributes for all the
	 * contained bios.  It will just track those of the first one.
	 * Distributes the attributs to each bio.
	 */
	for (bio = rq->bio; bio; bio = bio->bi_next) {
		WARN_ON_ONCE((bio->bi_opf & REQ_FAILFAST_MASK) &&
			     (bio->bi_opf & REQ_FAILFAST_MASK) != ff);
		bio->bi_opf |= ff;
	}
	rq->rq_flags |= RQF_MIXED_MERGE;
}

static void blk_account_io_merge(struct request *req)
{
	if (blk_do_io_stat(req)) {
		struct hd_struct *part;
		int cpu;

		cpu = part_stat_lock();
		part = req->part;

		if (precise_iostat)
			part_round_stats(req->q, cpu, part);
		part_dec_in_flight(req->q, part, rq_data_dir(req));

		hd_struct_put(part);
		part_stat_unlock();
	}
}
/*
 * Two cases of handling DISCARD merge:
 * If max_discard_segments > 1, the driver takes every bio
 * as a range and send them to controller together. The ranges
 * needn't to be contiguous.
 * Otherwise, the bios/requests will be handled as same as
 * others which should be contiguous.
 */
static inline bool blk_discard_mergable(struct request *req)
{
	if (req_op(req) == REQ_OP_DISCARD &&
	    queue_max_discard_segments(req->q) > 1)
		return true;
	return false;
}

enum elv_merge blk_try_req_merge(struct request *req, struct request *next)
{
	if (blk_discard_mergable(req))
		return ELEVATOR_DISCARD_MERGE;
	else if (blk_rq_pos(req) + blk_rq_sectors(req) == blk_rq_pos(next))
		return ELEVATOR_BACK_MERGE;

	return ELEVATOR_NO_MERGE;
}

/*
 * For non-mq, this has to be called with the request spinlock acquired.
 * For mq with scheduling, the appropriate queue wide lock should be held.
 */
static struct request *attempt_merge(struct request_queue *q,
				     struct request *req, struct request *next)
{
	if (!q->mq_ops)
		lockdep_assert_held(q->queue_lock);

	if (!rq_mergeable(req) || !rq_mergeable(next))
		return NULL;

	if (req_op(req) != req_op(next))
		return NULL;

	if (rq_data_dir(req) != rq_data_dir(next)
	    || req->rq_disk != next->rq_disk
	    || req_no_special_merge(next))
		return NULL;

	if (req_op(req) == REQ_OP_WRITE_SAME &&
	    !blk_write_same_mergeable(req->bio, next->bio))
		return NULL;

	/*
	 * Don't allow merge of different write hints, or for a hint with
	 * non-hint IO.
	 */
	if (req->write_hint != next->write_hint)
		return NULL;

	if (req->ioprio != next->ioprio)
		return NULL;

	/*
	 * If we are allowed to merge, then append bio list
	 * from next to rq and release next. merge_requests_fn
	 * will have updated segment counts, update sector
	 * counts here. Handle DISCARDs separately, as they
	 * have separate settings.
	 */

	switch (blk_try_req_merge(req, next)) {
	case ELEVATOR_DISCARD_MERGE:
		if (!req_attempt_discard_merge(q, req, next))
			return NULL;
		break;
	case ELEVATOR_BACK_MERGE:
		if (!ll_merge_requests_fn(q, req, next))
			return NULL;
		break;
	default:
		return NULL;
	}

	/*
	 * If failfast settings disagree or any of the two is already
	 * a mixed merge, mark both as mixed before proceeding.  This
	 * makes sure that all involved bios have mixable attributes
	 * set properly.
	 */
	if (((req->rq_flags | next->rq_flags) & RQF_MIXED_MERGE) ||
	    (req->cmd_flags & REQ_FAILFAST_MASK) !=
	    (next->cmd_flags & REQ_FAILFAST_MASK)) {
		blk_rq_set_mixed_merge(req);
		blk_rq_set_mixed_merge(next);
	}

	/*
	 * At this point we have either done a back merge or front merge. We
	 * need the smaller start_time_ns of the merged requests to be the
	 * current request for accounting purposes.
	 */
	if (next->start_time_ns < req->start_time_ns)
		req->start_time_ns = next->start_time_ns;

	req->biotail->bi_next = next->bio;
	req->biotail = next->biotail;

	req->__data_len += blk_rq_bytes(next);
	blk_rq_update_bi_alloc_time(req, NULL, next);

	if (!blk_discard_mergable(req))
		elv_merge_requests(q, req, next);

	/*
	 * 'next' is going away, so update stats accordingly
	 */
	blk_account_io_merge(next);

	if (blk_rq_cpu_valid(next))
		req->cpu = next->cpu;

	/*
	 * ownership of bio passed from next to req, return 'next' for
	 * the caller to free
	 */
	next->bio = NULL;
	return next;
}

struct request *attempt_back_merge(struct request_queue *q, struct request *rq)
{
	struct request *next = elv_latter_request(q, rq);

	if (next)
		return attempt_merge(q, rq, next);

	return NULL;
}

struct request *attempt_front_merge(struct request_queue *q, struct request *rq)
{
	struct request *prev = elv_former_request(q, rq);

	if (prev)
		return attempt_merge(q, prev, rq);

	return NULL;
}

/*
 * Try to merge 'next' into 'rq'. Return true if the merge happened, false
 * otherwise. The caller is responsible for freeing 'next' if the merge
 * happened.
 */
bool blk_attempt_req_merge(struct request_queue *q, struct request *rq,
			  struct request *next)
{
	struct elevator_queue *e = q->elevator;

	if (!e->uses_mq && e->type->ops.sq.elevator_allow_rq_merge_fn)
		if (!e->type->ops.sq.elevator_allow_rq_merge_fn(q, rq, next))
			return 0;

	return attempt_merge(q, rq, next);
}

bool blk_rq_merge_ok(struct request *rq, struct bio *bio)
{
	if (!rq_mergeable(rq) || !bio_mergeable(bio))
		return false;

	if (req_op(rq) != bio_op(bio))
		return false;

	/* different data direction or already started, don't merge */
	if (bio_data_dir(bio) != rq_data_dir(rq))
		return false;

	/* must be same device and not a special request */
	if (rq->rq_disk != bio->bi_disk || req_no_special_merge(rq))
		return false;

	/* only merge integrity protected bio into ditto rq */
	if (blk_integrity_merge_bio(rq->q, rq, bio) == false)
		return false;

	/* must be using the same buffer */
	if (req_op(rq) == REQ_OP_WRITE_SAME &&
	    !blk_write_same_mergeable(rq->bio, bio))
		return false;

	/*
	 * Don't allow merge of different write hints, or for a hint with
	 * non-hint IO.
	 */
	if (rq->write_hint != bio->bi_write_hint)
		return false;

	if (rq->ioprio != bio_prio(bio))
		return false;

	return true;
}

enum elv_merge blk_try_merge(struct request *rq, struct bio *bio)
{
	if (blk_discard_mergable(rq))
		return ELEVATOR_DISCARD_MERGE;
	else if (blk_rq_pos(rq) + blk_rq_sectors(rq) == bio->bi_iter.bi_sector)
		return ELEVATOR_BACK_MERGE;
	else if (blk_rq_pos(rq) - bio_sectors(bio) == bio->bi_iter.bi_sector)
		return ELEVATOR_FRONT_MERGE;
	return ELEVATOR_NO_MERGE;
}
