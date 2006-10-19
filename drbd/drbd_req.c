/*
-*- linux-c -*-
   drbd_req.c
   Kernel module for 2.4.x/2.6.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2006, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2006, Lars Ellenberg <lars.ellenberg@linbit.com>.
   Copyright (C) 2001-2006, LINBIT Information Technologies GmbH.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/slab.h>
#include <linux/drbd.h>
#include "drbd_int.h"

void _drbd_end_req(drbd_request_t *req, int nextstate, int er_flags)
{
	struct Drbd_Conf* mdev = drbd_req_get_mdev(req);
	sector_t rsector = req->sector;
	int uptodate;

	if(req->rq_status & nextstate) {
		ERR("request state error(%d)\n", req->rq_status);
	}

	if (nextstate == RQ_DRBD_SENT) dec_ap_pending(mdev);

	req->rq_status |= nextstate;
	req->rq_status &= er_flags | ~0x0001;
	if( (req->rq_status & RQ_DRBD_DONE) != RQ_DRBD_DONE )
		return;

	/* complete the master bio now. */

	/* if this sector was present in the current epoch, close it.
	 * FIXME compares reusable pointer addresses,
	 * possibly artificially reducing epoch size */
	if (req->barrier == mdev->newest_barrier)
		set_bit(ISSUE_BARRIER,&mdev->flags);

	uptodate = req->rq_status & 0x0001;
	if( !uptodate && mdev->on_io_error == Detach) {
		drbd_set_out_of_sync(mdev,rsector, req->size);
		// It should also be as out of sync on
		// the other side!  See w_io_error()

		drbd_bio_endio(req->master_bio,1);
		req->master_bio = NULL;
		dec_ap_bio(mdev);
		// The assumption is that we wrote it on the peer.

// FIXME proto A and diskless :)

		req->w.cb = w_io_error;
		_drbd_queue_work(&mdev->data.work,&req->w);

		goto out;

	}

	drbd_bio_endio(req->master_bio,uptodate);
	req->master_bio = NULL;
	dec_ap_bio(mdev);

	/* free the request,
	 * if it is not/no longer in the transfer log, because
	 *  it was local only (not connected), or
	 *  this is protocol C, or
	 *  the corresponding barrier ack has been received already, or
	 *  it has been cleared from the transfer log (after connection loss)
	 */
	if (!(req->rq_status & RQ_DRBD_IN_TL)) {
		INVALIDATE_MAGIC(req);
		mempool_free(req,drbd_request_mempool);
	}

 out:
	if (test_bit(ISSUE_BARRIER,&mdev->flags)) {
		if(list_empty(&mdev->barrier_work.list)) {
			_drbd_queue_work(&mdev->data.work,&mdev->barrier_work);
		}
	}
}

void drbd_end_req(drbd_request_t *req, int nextstate, int er_flags)
{
	struct Drbd_Conf* mdev = drbd_req_get_mdev(req);
	unsigned long flags=0;
	spin_lock_irqsave(&mdev->req_lock,flags);
	_drbd_end_req(req,nextstate,er_flags);
	spin_unlock_irqrestore(&mdev->req_lock,flags);
}


int drbd_read_remote(drbd_dev *mdev, drbd_request_t *req)
{
	int rv;
	drbd_bio_t *bio = req->master_bio;

	req->w.cb = w_is_app_read;
	spin_lock(&mdev->pr_lock);
	list_add(&req->w.list,&mdev->app_reads);
	spin_unlock(&mdev->pr_lock);
	set_bit(UNPLUG_REMOTE,&mdev->flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
	rv=drbd_send_drequest(mdev, DataRequest, bio->b_rsector, bio->b_size,
			      (unsigned long)req);
#else
	rv=drbd_send_drequest(mdev, DataRequest, bio->bi_sector, bio->bi_size,
			      (unsigned long)req);
#endif
	return rv;
}


/* we may do a local read if:
 * - we are consistent (of course),
 * - or we are generally inconsistent,
 *   BUT we are still/already IN SYNC for this area.
 *   since size may be up to PAGE_SIZE, but BM_BLOCK_SIZE may be smaller
 *   than PAGE_SIZE, we may need to check several bits.
 */
STATIC int drbd_may_do_local_read(drbd_dev *mdev, sector_t sector, int size)
{
	unsigned long sbnr,ebnr,bnr;
	sector_t esector, nr_sectors;

	if (drbd_md_test_flag(mdev,MDF_Consistent)) return 1;

	nr_sectors = drbd_get_capacity(mdev->this_bdev);
	esector = sector + (size>>9) -1;

	D_ASSERT(sector  < nr_sectors);
	D_ASSERT(esector < nr_sectors);

	sbnr = BM_SECT_TO_BIT(sector);
	ebnr = BM_SECT_TO_BIT(esector);

	for (bnr = sbnr; bnr <= ebnr; bnr++) {
		if (drbd_bm_test_bit(mdev,bnr)) return 0;
	}
	return 1;
}

STATIC int
drbd_make_request_common(drbd_dev *mdev, int rw, int size,
			 sector_t sector, drbd_bio_t *bio)
{
	drbd_request_t *req;
	int local, remote;
	unsigned long flags;

	ONLY_IN_26(
	/* Currently our BARRIER code is disabled. */
	if(unlikely(bio_barrier(bio))) {
		bio_endio(bio, bio->bi_size, -EOPNOTSUPP);
		return 0;
	}
	)
	if (unlikely(drbd_did_panic == DRBD_MAGIC)) {
		drbd_bio_IO_error(bio);
		return 0;
	}

	if( rw == WRITE && test_bit(IO_FROZEN, &mdev->flags)) {
		bio->REQ_NEXT = NULL;

		spin_lock_irqsave(&mdev->req_lock,flags);
		if(mdev->last_frozen_bio == NULL) {
			mdev->first_frozen_bio = bio;
			mdev->last_frozen_bio = bio;
		} else {
			mdev->last_frozen_bio->REQ_NEXT = bio;
			mdev->last_frozen_bio = bio;
		}
		spin_unlock_irqrestore(&mdev->req_lock,flags);
		return 0;
	}
	/*
	 * If someone tries to mount on Secondary, and this is a 2.4 kernel,
	 * it would lead to a readonly mounted, but not cache-coherent,
	 * therefore dangerous, filesystem.
	 * On 2.6 this is prevented by bd_claiming the device.
	 * It is not that easy in 2.4.
	 *
	 * Because people continue to report they mount readonly, it does not
	 * do what they expect, and their logs fill with messages and stuff.
	 *
	 * Since it just won't work, we just fail IO here.
	 * [ ... until we implement some shared mode, and our users confirm by
	 * configuration, that they handle cache coherency themselves ... ]
	 */
	if (mdev->state != Primary &&
		( !disable_bd_claim || rw == WRITE ) ) {
		if (DRBD_ratelimit(5*HZ,5)) {
			ERR("Not in Primary state, no %s requests allowed\n",
					disable_bd_claim ? "WRITE" : "IO");
		}
		drbd_bio_IO_error(bio);
		return 0;
	}

	/*
	 * Paranoia: we might have been primary, but sync target, or
	 * even diskless, then lost the connection.
	 * This should have been handled (panic? suspend?) somehwere
	 * else. But maybe it was not, so check again here.
	 * Caution: as long as we do not have a read/write lock on mdev,
	 * to serialize state changes, this is racy, since we may lose
	 * the connection *after* we test for the cstate.
	 */
	if ( (    test_bit(DISKLESS,&mdev->flags)
	      || !drbd_md_test_flag(mdev,MDF_Consistent)
	     ) && mdev->cstate < Connected )
	{
		ERR("Sorry, I have no access to good data anymore.\n");
/*
	FIXME suspend, loop waiting on cstate wait? panic?
*/
		drbd_bio_IO_error(bio);
		return 0;
	}

	/* allocate outside of all locks
	 */
	req = mempool_alloc(drbd_request_mempool, GFP_NOIO);
	if (!req) {
		/* only pass the error to the upper layers.
		 * if user cannot handle io errors, thats not our business.
		 */
		ERR("could not kmalloc() req\n");
		drbd_bio_IO_error(bio);
		return 0;
	}
	SET_MAGIC(req);
	req->master_bio = bio;

	// XXX maybe merge both variants into one
	if (rw == WRITE) drbd_req_prepare_write(mdev,req);
	else             drbd_req_prepare_read(mdev,req);

	/* XXX req->w.cb = something; drbd_queue_work() ....
	 * Not yet.
	 */

	// down_read(mdev->device_lock);

	wait_event( mdev->cstate_wait,
		    (volatile int)mdev->cstate < WFBitMapS ||
		    (volatile int) mdev->cstate > WFBitMapT);

	local = inc_local(mdev);
	NOT_IN_26( if (rw == READA) rw=READ );
	if (rw == READ || rw == READA) {
		if (local) {
			if (!drbd_may_do_local_read(mdev,sector,size)) {
				/* whe could kick the syncer to
				 * sync this extent asap, wait for
				 * it, then continue locally.
				 * Or just issue the request remotely.
				 */
				/* FIXME
				 * I think we have a RACE here. We request
				 * something from the peer, then later some
				 * write starts ...  and finished *before*
				 * the answer to the read comes in, because
				 * the ACK for the WRITE goes over
				 * meta-socket ...
				 * Maybe we need to properly lock reads
				 * against the syncer, too. But if we have
				 * some user issuing writes on an area that
				 * he has pending reads on, _he_ is really
				 * broke anyways, and would get "undefined
				 * results" on _any_ io stack, even just the
				 * local io stack.
				 */
				local = 0;
				dec_local(mdev);
			}
		}
		remote = !local && test_bit(PARTNER_CONSISTENT, &mdev->flags);
	} else {
		remote = 1;
	}

	/* If we have a disk, but a READA request is mapped to remote,
	 * we are Primary, Inconsistent, SyncTarget.
	 * Just fail that READA request right here.
	 *
	 * THINK: maybe fail all READA when not local?
	 *        or make this configurable...
	 *        if network is slow, READA won't do any good.
	 */
	if (rw == READA && !test_bit(DISKLESS,&mdev->flags) && !local) {
		drbd_bio_IO_error(bio);
		return 0;
	}

	if (rw == WRITE && local)
		drbd_al_begin_io(mdev, sector);

	remote = remote && (mdev->cstate >= Connected)
			&& !test_bit(PARTNER_DISKLESS,&mdev->flags);

	if (!(local || remote)) {
		ERR("IO ERROR: neither local nor remote disk\n");
		// FIXME PANIC ??
		drbd_bio_IO_error(bio);
		return 0;
	}

	/* do this first, so I do not need to call drbd_end_req,
	 * but can set the rq_status directly.
	 */
	if (!local)
		req->rq_status |= RQ_DRBD_LOCAL;
	if (!remote)
		req->rq_status |= RQ_DRBD_SENT;

	/* we need to plug ALWAYS since we possibly need to kick lo_dev */
	drbd_plug_device(mdev);

	inc_ap_bio(mdev);
	if (remote) {
		/* either WRITE and Connected,
		 * or READ, and no local disk,
		 * or READ, but not in sync.
		 */
		inc_ap_pending(mdev);
		if (rw == WRITE) {
			if (!drbd_send_dblock(mdev,req)) {
				if (mdev->cstate >= Connected)
					set_cstate(mdev,NetworkFailure);
				drbd_thread_restart_nowait(&mdev->receiver);
			}
		} else {
			// this node is diskless ...
			drbd_read_remote(mdev,req);
		}
	}

	if (local) {
		if (rw == WRITE) {
			if (!remote) drbd_set_out_of_sync(mdev,sector,size);
		} else {
			D_ASSERT(!remote);
		}
		/* FIXME
		 * Should we add even local reads to some list, so
		 * they can be grabbed and freed somewhen?
		 *
		 * They already have a reference count (sort of...)
		 * on mdev via inc_local()
		 */
		if(rw == WRITE) mdev->writ_cnt += size>>9;
		else            mdev->read_cnt += size>>9;

		// in 2.4.X, READA are submitted as READ.
		drbd_generic_make_request(rw,drbd_req_private_bio(req));
	}

	// up_read(mdev->device_lock);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
int drbd_make_request_24(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct Drbd_Conf* mdev = drbd_conf + MINOR(bh->b_rdev);
	if (MINOR(bh->b_rdev) >= minor_count || mdev->cstate < StandAlone) {
		buffer_IO_error(bh);
		return 0;
	}

	return drbd_make_request_common(mdev,rw,bh->b_size,bh->b_rsector,bh);
}
#else
int drbd_make_request_26(request_queue_t *q, struct bio *bio)
{
	unsigned int s_enr,e_enr;
	struct Drbd_Conf* mdev = (drbd_dev*) q->queuedata;
	if (mdev->cstate < StandAlone) {
		drbd_bio_IO_error(bio);
		return 0;
	}

	/*
	 * what we "blindly" assume:
	 */
	D_ASSERT(bio->bi_size > 0);
	D_ASSERT( (bio->bi_size & 0x1ff) == 0);
	D_ASSERT(bio->bi_size <= PAGE_SIZE);
	D_ASSERT(bio->bi_vcnt == 1);
	D_ASSERT(bio->bi_idx == 0);

	s_enr = bio->bi_sector >> (AL_EXTENT_SIZE_B-9);
	e_enr = (bio->bi_sector+(bio->bi_size>>9)-1) >> (AL_EXTENT_SIZE_B-9);
	D_ASSERT(e_enr >= s_enr);

	if(unlikely(s_enr != e_enr)) {
		/* This bio crosses an AL_EXTENT boundary, so we have to
		 * split it. [So far, only XFS is known to do this...]
		 */
		struct bio_pair *bp;
		bp = bio_split(bio, bio_split_pool, 
			       (e_enr<<(AL_EXTENT_SIZE_B-9)) - bio->bi_sector);
		drbd_make_request_26(q,&bp->bio1);
		drbd_make_request_26(q,&bp->bio2);
		bio_pair_release(bp);
		return 0;
	}

	return drbd_make_request_common(mdev,bio_rw(bio),bio->bi_size,
					bio->bi_sector,bio);
}
#endif

void drbd_thaw_frozen_reqs(drbd_dev *mdev)
{
	drbd_bio_t *bio;
	int reqs=0;

	spin_lock_irq(&mdev->req_lock);
	bio = mdev->first_frozen_bio;
	mdev->first_frozen_bio = NULL;
	mdev->last_frozen_bio = NULL;
	spin_unlock_irq(&mdev->req_lock);
	
	while(bio) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
		drbd_make_request_common(mdev,WRITE,bio->b_size,bio->b_rsector,bio);
#else
		drbd_make_request_common(mdev,WRITE,bio->bi_size,bio->bi_sector,bio);
#endif
		bio = bio->REQ_NEXT;
		reqs++;
	}
	WARN("Continued %d requests (which where issued after IO-freeze).\n",
	     reqs);
}
