/*
-*- linux-c -*-
   drbd_worker.c
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
#include <linux/version.h>

#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/drbd_config.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)) || defined(HAVE_MM_INLINE_H)
#include <linux/mm_inline.h> // for the page_count macro on RH/Fedora
#endif
#include <linux/slab.h>

#include <linux/drbd.h>
#include "drbd_int.h"

/* I choose to have all block layer end_io handlers defined here.

 * For all these callbacks, note the follwing:
 * The callbacks will be called in irq context by the IDE drivers,
 * and in Softirqs/Tasklets/BH context by the SCSI drivers.
 * Try to get the locking right :)
 *
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)

/* used for synchronous meta data and bitmap IO
 * submitted by FIXME (I'd say worker only, but currently this is not true...)
 */
void drbd_md_io_complete(struct buffer_head *bh, int uptodate)
{
	if (uptodate)
		set_bit(BH_Uptodate, &bh->b_state);

	complete((struct completion*)bh->b_private);
}

/* reads on behalf of the partner,
 * "submitted" by the receiver
 */
void enslaved_read_bi_end_io(drbd_bio_t *bh, int uptodate)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bh,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);

	mark_buffer_uptodate(bh, uptodate);
	clear_bit(BH_Lock, &bh->b_state);
	smp_mb__after_clear_bit();

	list_del(&e->w.list);
	if(list_empty(&mdev->read_ee)) wake_up(&mdev->ee_wait);
	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_chk_io_error(mdev,!uptodate);
	drbd_queue_work(mdev,&mdev->data.work,&e->w);
	dec_local(mdev);
}

/* writes on behalf of the partner, or resync writes,
 * "submitted" by the receiver.
 */
void drbd_dio_end_sec(struct buffer_head *bh, int uptodate)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bh,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);

	mark_buffer_uptodate(bh, uptodate);

	clear_bit(BH_Dirty, &bh->b_state);
	clear_bit(BH_Lock, &bh->b_state);
	smp_mb__after_clear_bit();

	list_del(&e->w.list);
	list_add_tail(&e->w.list,&mdev->done_ee);

	if (waitqueue_active(&mdev->ee_wait) &&
	    (list_empty(&mdev->active_ee) ||
	     list_empty(&mdev->sync_ee)))
		wake_up(&mdev->ee_wait);

	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_chk_io_error(mdev,!uptodate);
	wake_asender(mdev);
	dec_local(mdev);
}

/* writes on Primary comming from drbd_make_request
 */
void drbd_dio_end(struct buffer_head *bh, int uptodate)
{
	struct Drbd_Conf* mdev;
	drbd_request_t *req;
	sector_t rsector;

	mdev = bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	req = container_of(bh,struct drbd_request,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(req));

	drbd_chk_io_error(mdev,!uptodate);
        // req may get freed within drbd_end_req
	rsector = drbd_req_get_sector(req);
	drbd_end_req(req, RQ_DRBD_LOCAL, uptodate, rsector);
	drbd_al_complete_io(mdev,rsector);
	dec_local(mdev);
}

/* reads on Primary comming from drbd_make_request
 */
void drbd_read_bi_end_io(struct buffer_head *bh, int uptodate)
{
	struct Drbd_Conf* mdev;
	drbd_request_t *req;

	mdev = bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	req = container_of(bh,struct drbd_request,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(req));

	// no special case for READA here, in 2.4.X we submit them as READ.
	if (!uptodate) {
		// for the panic:
		drbd_chk_io_error(mdev,!uptodate); // handle panic and detach.
		if(mdev->on_io_error == PassOn) goto pass_on;
		// ok, if we survived this, retry:
		// FIXME sector ...
		if (DRBD_ratelimit(5*HZ,5))
			ERR("local read failed, retrying remotely\n");
		req->w.cb = w_read_retry_remote;
		drbd_queue_work(mdev,&mdev->data.work,&req->w);
	} else {
	pass_on:
		req->master_bio->b_end_io(req->master_bio,uptodate);
		dec_ap_bio(mdev);

		INVALIDATE_MAGIC(req);
		mempool_free(req,drbd_request_mempool);
	}
	dec_local(mdev);
}

#else

/* used for synchronous meta data and bitmap IO
 * submitted by drbd_md_sync_page_io()
 */
int drbd_md_io_complete(struct bio *bio, unsigned int bytes_done, int error)
{
	if (bio->bi_size)
		return 1;

	complete((struct completion*)bio->bi_private);
	return 0;
}

/* reads on behalf of the partner,
 * "submitted" by the receiver
 */
int enslaved_read_bi_end_io(struct bio *bio, unsigned int bytes_done, int error)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bio->bi_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	/* we should be called via bio_endio, so this should never be the case
	 * but "everyone else does it", and so do we ;)		-lge
	 */
	ERR_IF (bio->bi_size)
		return 1;

	e = container_of(bio,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);
	list_del(&e->w.list);
	if(list_empty(&mdev->read_ee)) wake_up(&mdev->ee_wait);
	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_chk_io_error(mdev,error);
	drbd_queue_work(mdev,&mdev->data.work,&e->w);
	dec_local(mdev);
	return 0;
}

/* writes on behalf of the partner, or resync writes,
 * "submitted" by the receiver.
 */
int drbd_dio_end_sec(struct bio *bio, unsigned int bytes_done, int error)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bio->bi_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	// see above
	ERR_IF (bio->bi_size)
		return 1;

	e = container_of(bio,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);
	list_del(&e->w.list);
	list_add_tail(&e->w.list,&mdev->done_ee);

	if (waitqueue_active(&mdev->ee_wait) &&
	    (list_empty(&mdev->active_ee) ||
	     list_empty(&mdev->sync_ee)))
		wake_up(&mdev->ee_wait);

	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_chk_io_error(mdev,error);
	wake_asender(mdev);
	dec_local(mdev);
	return 0;
}

/* writes on Primary comming from drbd_make_request
 */
int drbd_dio_end(struct bio *bio, unsigned int bytes_done, int error)
{
	drbd_request_t *req=bio->bi_private;
	struct Drbd_Conf* mdev=req->mdev;
	sector_t rsector;

	// see above
	ERR_IF (bio->bi_size)
		return 1;

	drbd_chk_io_error(mdev,error);
        // req may get freed within drbd_end_req
	rsector = req->sector;
	drbd_end_req(req, RQ_DRBD_LOCAL, (error == 0));
	drbd_al_complete_io(mdev,rsector);
	dec_local(mdev);
	bio_put(bio);
	return 0;
}

/* reads on Primary comming from drbd_make_request
 */
int drbd_read_bi_end_io(struct bio *bio, unsigned int bytes_done, int error)
{
	drbd_request_t *req=bio->bi_private;
	struct Drbd_Conf* mdev=req->mdev;

	// see above
	ERR_IF (bio->bi_size)
		return 1;

	/* READAs may fail.
	 * upper layers need to be able to handle that themselves */
	if (bio_rw(bio) == READA) goto pass_on;
	if (error) {
		drbd_chk_io_error(mdev,error); // handle panic and detach.
		if(mdev->on_io_error == PassOn) goto pass_on;
		// ok, if we survived this, retry:
		// FIXME sector ...
		if (DRBD_ratelimit(5*HZ,5))
			ERR("local read failed, retrying remotely\n");
		req->w.cb = w_read_retry_remote;
		drbd_queue_work(mdev,&mdev->data.work,&req->w);
	} else {
	pass_on:
		bio_endio(req->master_bio,req->master_bio->bi_size,error);
		dec_ap_bio(mdev);

		INVALIDATE_MAGIC(req);
		mempool_free(req,drbd_request_mempool);
	}

	bio_put(bio);
	dec_local(mdev);
	return 0;
}
#endif

int w_io_error(drbd_dev* mdev, struct drbd_work* w,int cancel)
{
	drbd_request_t *req = (drbd_request_t*)w;
	int ok;

	/* FIXME send a "set_out_of_sync" packet to the peer
	 * in the PassOn case...
	 * in the Detach (or Panic) case, we (try to) send
	 * a "we are diskless" param packet anyways, and the peer
	 * will then set the FullSync bit in the meta data ...
	 */
	D_ASSERT(mdev->on_io_error != PassOn);

	INVALIDATE_MAGIC(req);
	mempool_free(req,drbd_request_mempool);

	if(unlikely(cancel)) return 1;

	ok = drbd_io_error(mdev);
	if(unlikely(!ok)) ERR("Sending in w_io_error() failed\n");
	return ok;
}

int w_read_retry_remote(drbd_dev* mdev, struct drbd_work* w,int cancel)
{
	drbd_request_t *req = (drbd_request_t*)w;
	int ok;

	smp_rmb();
	if ( cancel ||
	     mdev->cstate < Connected ||
	     !test_bit(PARTNER_CONSISTENT,&mdev->flags) ) {
		drbd_panic("WE ARE LOST. Local IO failure, no peer.\n");

		// does not make much sense, but anyways...
		drbd_bio_endio(req->master_bio,0);
		dec_ap_bio(mdev);
		mempool_free(req,drbd_request_mempool);
		return 1;
	}

	// FIXME: what if partner was SyncTarget, and is out of sync for
	// this area ?? ... should be handled in the receiver.

	ok = drbd_io_error(mdev);
	if(unlikely(!ok)) ERR("Sending in w_read_retry_remote() failed\n");
	
	inc_ap_pending(mdev);
	ok = drbd_read_remote(mdev,req);
	if(unlikely(!ok)) {
		ERR("drbd_read_remote() failed\n");
		/* dec_ap_pending and bio_io_error are done in
		 * drbd_fail_pending_reads
		 */
	}
	return ok;
}

int w_resync_inactive(drbd_dev *mdev, struct drbd_work *w, int cancel)
{
	ERR_IF(cancel) return 1;
	ERR("resync inactive, but callback triggered??\n");
	return 0;
}

/* FIXME
 * not used any longer, they now use e_end_resync_block.
 * maybe remove again?
 */
int w_is_resync_read(drbd_dev *mdev, struct drbd_work *w, int unused)
{
	ERR("%s: Typecheck only, should never be called!\n", __FUNCTION__ );
	return 0;
}

/* in case we need it. currently unused,
 * since should be assigned to "w_read_retry_remote"
 */
int w_is_app_read(drbd_dev *mdev, struct drbd_work *w, int unused)
{
	ERR("%s: Typecheck only, should never be called!\n", __FUNCTION__ );
	return 0;
}

void resync_timer_fn(unsigned long data)
{
	unsigned long flags;
	drbd_dev* mdev = (drbd_dev*) data;

	spin_lock_irqsave(&mdev->req_lock,flags);

	if(likely(!test_and_clear_bit(STOP_SYNC_TIMER,&mdev->flags))) {
		mdev->resync_work.cb = w_make_resync_request;
	} else {
		mdev->resync_work.cb = w_resume_next_sg;
	}

	if(list_empty(&mdev->resync_work.list)) {
		_drbd_queue_work(&mdev->data.work,&mdev->resync_work);
	} else INFO("Avoided requeue of resync_work\n");

	spin_unlock_irqrestore(&mdev->req_lock,flags);
}

#define SLEEP_TIME (HZ/10)

int w_make_resync_request(drbd_dev* mdev, struct drbd_work* w,int cancel)
{
	unsigned long bit;
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	int number,i,size;

	PARANOIA_BUG_ON(w != &mdev->resync_work);

	if(unlikely(cancel)) return 1;

	if(unlikely(mdev->cstate < Connected)) {
		ERR("Confused in w_make_resync_request()! cstate < Connected");
		return 0;
	}

	if (mdev->cstate != SyncTarget) {
		ERR("%s in w_make_resync_request\n", cstate_to_name(mdev->cstate));
	}

        number = SLEEP_TIME*mdev->sync_conf.rate / ((BM_BLOCK_SIZE/1024)*HZ);

	if (atomic_read(&mdev->rs_pending_cnt)>number) {
		goto requeue;
	}
	number -= atomic_read(&mdev->rs_pending_cnt);

	for(i=0;i<number;i++) {

	next_sector:
		size = BM_BLOCK_SIZE;
		bit  = drbd_bm_find_next(mdev);

		if (bit == -1UL) {
			/* FIXME either test_and_set some bit,
			 * or make this the _only_ place that is allowed
			 * to assign w_resync_inactive! */
			mdev->resync_work.cb = w_resync_inactive;
			return 1;
		}

		sector = BM_BIT_TO_SECT(bit);

		if(!drbd_rs_begin_io(mdev,sector)) {
			// we have been interrupted, probably connection lost!
			D_ASSERT(signal_pending(current));
			return 0;
		}

		if(unlikely( drbd_bm_test_bit(mdev,bit) == 0 )) {
		      //INFO("Block got synced while in drbd_rs_begin_io()\n");
			drbd_rs_complete_io(mdev,sector);
			goto next_sector;
		}

		if (sector + (size>>9) > capacity) size = (capacity-sector)<<9;
		inc_rs_pending(mdev);
		if(!drbd_send_drequest(mdev,RSDataRequest,
				       sector,size,ID_SYNCER)) {
			ERR("drbd_send_drequest() failed, aborting...");
			dec_rs_pending(mdev);
			return 0; // FAILED. worker will abort!
		}
	}

	if(drbd_bm_rs_done(mdev)) {
		/* last syncer _request_ was sent,
		 * but the RSDataReply not yet received.  sync will end (and
		 * next sync group will resume), as soon as we receive the last
		 * resync data block, and the last bit is cleared.
		 * until then resync "work" is "inactive" ...
		 */
		mdev->resync_work.cb = w_resync_inactive;
		return 1;
	}

 requeue:
	mod_timer(&mdev->resync_timer, jiffies + SLEEP_TIME);
	return 1;
}

int drbd_resync_finished(drbd_dev* mdev)
{
	unsigned long db,dt,dbdt;

	dt = (jiffies - mdev->rs_start - mdev->rs_paused) / HZ;
	if (dt <= 0) dt=1;
	db = mdev->rs_total;
	dbdt = Bit2KB(db/dt);
	mdev->rs_paused /= HZ;
	INFO("Resync done (total %lu sec; paused %lu sec; %lu K/sec)\n",
	     dt + mdev->rs_paused, mdev->rs_paused, dbdt);

	if (mdev->cstate == SyncTarget || mdev->cstate == PausedSyncT) {
		drbd_md_set_flag(mdev,MDF_Consistent);
		ERR_IF(drbd_md_test_flag(mdev,MDF_FullSync))
			drbd_md_clear_flag(mdev,MDF_FullSync);
		drbd_md_write(mdev);
	} else if (mdev->cstate == SyncSource || mdev->cstate == PausedSyncS) {
		set_bit(PARTNER_CONSISTENT, &mdev->flags);
	} else {
		ERR("unexpected cstate (%s) in drbd_resync_finished\n",
		    cstate_to_name(mdev->cstate));
	}

	// assert that all bit-map parts are cleared.
	D_ASSERT(list_empty(&mdev->resync->lru));
	D_ASSERT(drbd_bm_total_weight(mdev) == 0);
	mdev->rs_total  = 0;
	mdev->rs_paused = 0;

	set_cstate(mdev,Connected);

	return 1;
}

int w_e_end_data_req(drbd_dev *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	int ok;

	if(unlikely(cancel)) {
		spin_lock_irq(&mdev->ee_lock);
		drbd_put_ee(mdev,e);
		spin_unlock_irq(&mdev->ee_lock);
		dec_unacked(mdev);
		return 1;
	}

	if(likely(drbd_bio_uptodate(&e->private_bio))) {
		ok=drbd_send_block(mdev, DataReply, e);
	} else {
		ok=drbd_send_ack(mdev,NegDReply,e);
		if (DRBD_ratelimit(5*HZ,5))
			ERR("Sending NegDReply. I guess it gets messy.\n");
		drbd_io_error(mdev);
	}

	dec_unacked(mdev);

	spin_lock_irq(&mdev->ee_lock);
	if( page_count(drbd_bio_get_page(&e->private_bio)) > 1 ) {
		/* This might happen if sendpage() has not finished */
		list_add_tail(&e->w.list,&mdev->net_ee);
	} else {
		drbd_put_ee(mdev,e);
	}
	spin_unlock_irq(&mdev->ee_lock);

	if(unlikely(!ok)) ERR("drbd_send_block() failed\n");
	return ok;
}

int w_e_end_rsdata_req(drbd_dev *mdev, struct drbd_work *w, int cancel)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	int ok;

	if(unlikely(cancel)) {
		spin_lock_irq(&mdev->ee_lock);
		drbd_put_ee(mdev,e);
		spin_unlock_irq(&mdev->ee_lock);
		dec_unacked(mdev);
		return 1;
	}

	drbd_rs_complete_io(mdev,drbd_ee_get_sector(e));

	if(likely(drbd_bio_uptodate(&e->private_bio))) {
		if (likely( !test_bit(PARTNER_DISKLESS,&mdev->flags) )) {
			inc_rs_pending(mdev);
			ok=drbd_send_block(mdev, RSDataReply, e);
		} else {
			if (DRBD_ratelimit(5*HZ,5))
				ERR("Not sending RSDataReply, partner DISKLESS!\n");
			ok=1;
		}
	} else {
		ok=drbd_send_ack(mdev,NegRSDReply,e);
		if (DRBD_ratelimit(5*HZ,5))
			ERR("Sending NegDReply. I guess it gets messy.\n");
		drbd_io_error(mdev);
	}

	dec_unacked(mdev);

	spin_lock_irq(&mdev->ee_lock);
	if( page_count(drbd_bio_get_page(&e->private_bio)) > 1 ) {
		/* This might happen if sendpage() has not finished */
		list_add_tail(&e->w.list,&mdev->net_ee);
	} else {
		drbd_put_ee(mdev,e);
	}
	spin_unlock_irq(&mdev->ee_lock);

	if(unlikely(!ok)) ERR("drbd_send_block() failed\n");
	return ok;
}

int w_try_send_barrier(drbd_dev *mdev, struct drbd_work *w, int cancel)
{
	int ok=1;

	if(unlikely(cancel)) return ok;

	down(&mdev->data.mutex);
	if(test_and_clear_bit(ISSUE_BARRIER,&mdev->flags)) {
		ok = _drbd_send_barrier(mdev);
	}
	up(&mdev->data.mutex);

	return ok;
}

int w_send_write_hint(drbd_dev *mdev, struct drbd_work *w, int cancel)
{
	if (cancel) return 1;
	NOT_IN_26(clear_bit(UNPLUG_QUEUED,&mdev->flags));
	return drbd_send_short_cmd(mdev,UnplugRemote);
}

STATIC void drbd_global_lock(void)
{
	int i;

	local_irq_disable();
	for (i=0; i < minor_count; i++) {
		spin_lock(&drbd_conf[i].req_lock);
	}
}

STATIC void drbd_global_unlock(void)
{
	int i;

	for (i=0; i < minor_count; i++) {
		spin_unlock(&drbd_conf[i].req_lock);
	}
	local_irq_enable();
}

STATIC void _drbd_rs_resume(drbd_dev *mdev)
{
	Drbd_CState ns;

	ns = mdev->cstate - (PausedSyncS - SyncSource);
	D_ASSERT(ns == SyncSource || ns == SyncTarget);

	INFO("Syncer continues.\n");
	mdev->rs_paused += (long)jiffies-(long)mdev->rs_mark_time;
	_set_cstate(mdev,ns);

	if(mdev->cstate == SyncTarget) {
		int stop_syncer_timer_was_set =
			test_and_clear_bit(STOP_SYNC_TIMER,&mdev->flags);
		ERR_IF(stop_syncer_timer_was_set) {
			unsigned long rs_left = drbd_bm_total_weight(mdev);
			if (rs_left == 0) {
				INFO("rs_left==0 in _drbd_rs_resume\n");
			} else {
				ERR("STOP_SYNC_TIMER was set in "
				    "_drbd_rs_resume, but rs_left still %lu\n",
				    rs_left);
			}
		}
		mod_timer(&mdev->resync_timer,jiffies);
	}
}


STATIC void _drbd_rs_pause(drbd_dev *mdev)
{
	Drbd_CState ns;

	D_ASSERT(mdev->cstate == SyncSource || mdev->cstate == SyncTarget);
	ns = mdev->cstate + (PausedSyncS - SyncSource);

	if(mdev->cstate == SyncTarget) set_bit(STOP_SYNC_TIMER,&mdev->flags);

	mdev->rs_mark_time = jiffies;
	// mdev->rs_mark_left = drbd_bm_total_weight(mdev); // I don't care...
	_set_cstate(mdev,ns);
	INFO("Syncer waits for sync group.\n");
}

STATIC int _drbd_pause_higher_sg(drbd_dev *mdev)
{
	drbd_dev *odev;
	int i,rv=0;

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		if ( odev->sync_conf.group > mdev->sync_conf.group
		     && ( odev->cstate == SyncSource || 
			  odev->cstate == SyncTarget ) ) {
			_drbd_rs_pause(odev);
			rv = 1;
		}
	}

	return rv;
}

STATIC int _drbd_lower_sg_running(drbd_dev *mdev)
{
	drbd_dev *odev;
	int i,rv=0;

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		if ( odev->sync_conf.group < mdev->sync_conf.group
		     && ( odev->cstate == SyncSource || 
			  odev->cstate == SyncTarget ) ) {
			rv = 1;
		}
	}

	return rv;
}

STATIC int _drbd_resume_lower_sg(drbd_dev *mdev)
{
	drbd_dev *odev;
	int i,rv=0;

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		if ( odev->sync_conf.group < mdev->sync_conf.group
		     && ( odev->cstate == PausedSyncS || 
			  odev->cstate == PausedSyncT ) ) {
			_drbd_rs_resume(odev);
			rv = 1;
		}
	}

	return rv;
}

int w_resume_next_sg(drbd_dev* mdev, struct drbd_work* w, int unused)
{
	drbd_dev *odev;
	int i,ng=10000;
	/* FIXME need to document that 10000 somewhere!
	 * maybe better: first iterate to find the max sync group number */

	PARANOIA_BUG_ON(w != &mdev->resync_work);

	drbd_global_lock();

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		/* currently we should never reach this while being
		 * mdev->cstate == Sync*, but just incase: skip ourselves */
		if (odev == mdev) continue;
		if ( odev->sync_conf.group <= mdev->sync_conf.group
		     && ( odev->cstate == SyncSource || 
			  odev->cstate == SyncTarget ) ) {
			goto out; // Sync on an other device in this group
			          // or a lower group still runs.
		}
	}

	for (i=0; i < minor_count; i++) { // find next sync group
		odev = drbd_conf + i;
		if (odev == mdev) continue;
		if ( odev->sync_conf.group > mdev->sync_conf.group
		     && odev->sync_conf.group < ng && 
		     (odev->cstate==PausedSyncS || odev->cstate==PausedSyncT)){
		  ng = odev->sync_conf.group;
		}
	}

	for (i=0; i < minor_count; i++) { // resume all devices in next group
		odev = drbd_conf + i;
		if (odev == mdev) continue;
		if ( odev->sync_conf.group == ng &&
		     (odev->cstate==PausedSyncS || odev->cstate==PausedSyncT)){
			_drbd_rs_resume(odev);
		}
	}

 out:
	w->cb = w_resync_inactive;
	drbd_global_unlock();

	return 1;
}

void drbd_alter_sg(drbd_dev *mdev, int ng)
{
	int c = 0, p = 0;
	int d = (ng - mdev->sync_conf.group);

	drbd_global_lock();
	mdev->sync_conf.group = ng;

	if( ( mdev->cstate == PausedSyncS || 
	      mdev->cstate == PausedSyncT ) && ( d < 0 ) ) {
		if(_drbd_pause_higher_sg(mdev)) c=1;
		else if(!_drbd_lower_sg_running(mdev)) c=1;
		if(c) _drbd_rs_resume(mdev);
	}

	if( ( mdev->cstate == SyncSource || 
	      mdev->cstate == SyncTarget ) && ( d > 0 ) ) {
		if(_drbd_resume_lower_sg(mdev)) p=1;
		else if(_drbd_lower_sg_running(mdev)) p=1;
		if(p) _drbd_rs_pause(mdev);
	}
	drbd_global_unlock();
}

void drbd_start_resync(drbd_dev *mdev, Drbd_CState side)
{
	if(side == SyncTarget) {
		drbd_md_clear_flag(mdev,MDF_Consistent);
		drbd_bm_reset_find(mdev);
	} else if (side == SyncSource) {
		clear_bit(PARTNER_CONSISTENT, &mdev->flags);
		/* If we are SyncSource we must be consistent.
		 * FIXME this should be an assertion only,
		 * otherwise it masks a logic bug somewhere else...
		 */
		ERR_IF (!drbd_md_test_flag(mdev,MDF_Consistent)) {
			// FIXME this is actually a BUG()!
			drbd_md_set_flag(mdev,MDF_Consistent);
		}
	} else {
		ERR("Usage error in drbd_start_resync! (side == %s)\n",
		     cstate_to_name(side));
		return;
	}
	drbd_md_write(mdev);

	set_cstate(mdev,side);
	mdev->rs_total     =
	mdev->rs_mark_left = drbd_bm_total_weight(mdev);
	mdev->rs_paused    = 0;
	mdev->rs_start     =
	mdev->rs_mark_time = jiffies;

	INFO("Resync started as %s (need to sync %lu KB [%lu bits set]).\n",
	     cstate_to_name(side),
	     (unsigned long) mdev->rs_total << (BM_BLOCK_SIZE_B-10),
	     (unsigned long) mdev->rs_total);

	// FIXME: this was a PARANOIA_BUG_ON, but it triggered! ??
	if (mdev->resync_work.cb != w_resync_inactive) {
		if (mdev->resync_work.cb == w_make_resync_request)
			ERR("resync_work.cb == w_make_resync_request, should be w_resync_inactive\n");
		else if (mdev->resync_work.cb == w_resume_next_sg)
			ERR("resync_work.cb == w_resume_next_sg, should be w_resync_inactive\n");
		else
			ERR("resync_work.cb == %p ???, should be w_resync_inactive\n",
					mdev->resync_work.cb);
	}

	if ( mdev->rs_total == 0 ) {
		drbd_resync_finished(mdev);
		return;
	}

	drbd_global_lock();
	if (mdev->cstate == SyncTarget || mdev->cstate == SyncSource) {
		_drbd_pause_higher_sg(mdev);
		if(_drbd_lower_sg_running(mdev)) {
			_drbd_rs_pause(mdev);
		}
	} /* else:
	   * thread of other mdev already paused us,
	   * or something very strange happend to our cstate!
	   * I really hate it that we can't have a consistent view of cstate.
	   */
	drbd_global_unlock();

	/* THINK: for consistency, should this be before the drbd_global_unlock() ? */
	{
		int stop_syncer_timer_was_set =
			test_and_clear_bit(STOP_SYNC_TIMER,&mdev->flags);
		if (mdev->cstate == SyncTarget) {
			D_ASSERT(!stop_syncer_timer_was_set);
			mod_timer(&mdev->resync_timer,jiffies);
		} else if (mdev->cstate == PausedSyncT) {
			D_ASSERT(stop_syncer_timer_was_set);
		}
	}
}

int drbd_worker(struct Drbd_thread *thi)
{
	drbd_dev *mdev = thi->mdev;
	struct drbd_work *w = 0;
	LIST_HEAD(work_list);
	int intr,i;

	sprintf(current->comm, "drbd%d_worker", (int)(mdev-drbd_conf));

	while (get_t_state(thi) == Running) {
		intr = down_interruptible(&mdev->data.work.s);

		if (unlikely(drbd_did_panic == DRBD_MAGIC)) {
			drbd_suicide();
		}

		if (intr) {
			D_ASSERT(intr == -EINTR);
			drbd_flush_signals(current);
			ERR_IF (get_t_state(thi) == Running)
				continue;
			break;
		}

		if (get_t_state(thi) != Running) break;
		/* With this break, we have done an down() but not consumed
		   the entry from the list. The cleanup code takes care of
		   this...   */

		w = 0;
		spin_lock_irq(&mdev->req_lock);
		ERR_IF(list_empty(&mdev->data.work.q)) {
			/* something terribly wrong in our logic.
			 * we were able to down() the semaphore,
			 * but the list is empty... doh.
			 *
			 * what is the best thing to do now?
			 * try again from scratch, restarting the receiver,
			 * asender, whatnot? could break even more ugly,
			 * e.g. when we are primary, but no good local data.
			 *
			 * I'll try to get away just starting over this loop.
			 */
			spin_unlock_irq(&mdev->req_lock);
			continue;
		}
		w = list_entry(mdev->data.work.q.next,struct drbd_work,list);
		list_del_init(&w->list);
		spin_unlock_irq(&mdev->req_lock);

		if(!w->cb(mdev,w, mdev->cstate < Connected )) {
			//WARN("worker: a callback failed! \n");
			if (mdev->cstate >= Connected)
				set_cstate(mdev,NetworkFailure);
			drbd_thread_restart_nowait(&mdev->receiver);
		}
	}

	drbd_wait_ee(mdev,&mdev->read_ee);

	/* When we terminate a resync process, either because it finished
	 * sucessfully, or because (like in this case here) we lost
	 * communications, we need to "w_resume_next_sg".
	 * We cannot use del_timer_sync from within _set_cstate, and since the
	 * resync timer may still be scheduled and would then trigger anyways,
	 * we set the STOP_SYNC_TIMER bit, and schedule the timer for immediate
	 * execution from within _set_cstate().
	 * The timer should then clear that bit and queue w_resume_next_sg.
	 *
	 * This is fine for the normal "resync finished" case.
	 *
	 * In this case (worker thread beeing stopped), there is a race:
	 * we cannot be sure that the timer already triggered.
	 *
	 * So we del_timer_sync here, and check that "STOP_SYNC_TIMER" bit.
	 * if it is still set, we queue w_resume_next_sg anyways,
	 * just to be sure.
	 */

	del_timer_sync(&mdev->resync_timer);
	/* possible paranoia check: the STOP_SYNC_TIMER bit should be set
	 * if and only if del_timer_sync returns true ... */

	spin_lock_irq(&mdev->req_lock);
	if (test_and_clear_bit(STOP_SYNC_TIMER,&mdev->flags)) {
		mdev->resync_work.cb = w_resume_next_sg;
		if (list_empty(&mdev->resync_work.list))
			_drbd_queue_work(&mdev->data.work,&mdev->resync_work);
		// else: already queued
	} else {
		/* timer already consumed that bit, or it was never set */
		if (list_empty(&mdev->resync_work.list)) {
			/* not queued, should be inactive */
			ERR_IF (mdev->resync_work.cb != w_resync_inactive)
				mdev->resync_work.cb = w_resync_inactive;
		} else {
			/* still queued; should be w_resume_next_sg */
			ERR_IF (mdev->resync_work.cb != w_resume_next_sg)
				mdev->resync_work.cb = w_resume_next_sg;
		}
	}

	i = 0;
  again:
	list_splice_init(&mdev->data.work.q,&work_list);
	spin_unlock_irq(&mdev->req_lock);

	while(!list_empty(&work_list)) {
		w = list_entry(work_list.next, struct drbd_work,list);
		list_del_init(&w->list);
		w->cb(mdev,w,1);
		i++; /* dead debugging code */
	}

	spin_lock_irq(&mdev->req_lock);
	ERR_IF(!list_empty(&mdev->data.work.q))
		goto again;
	sema_init(&mdev->data.work.s,0);
	/* DANGEROUS race: if someone did queue his work within the spinlock,
	 * but up() ed outside the spinlock, we could get an up() on the
	 * semaphore without corresponding list entry.
	 * So don't do that.
	 */
	spin_unlock_irq(&mdev->req_lock);

	INFO("worker terminated\n");

	return 0;
}
