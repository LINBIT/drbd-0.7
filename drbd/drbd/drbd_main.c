/*
-*- linux-c -*-
   drbd.c
   Kernel module for 2.2.x/2.4.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2001, Philipp Reisner <philipp.reisner@gmx.at>.
        main author.

   Copyright (C) 2000, Marcelo Tosatti <marcelo@conectiva.com.br>.
        Added code for Linux 2.3.x

   Copyright (C) 2000, F�bio Oliv� Leite <olive@conectiva.com.br>.
        Added sanity checks in IOCTL_SET_STATE.
		Added code to prevent zombie threads.

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


/*
  By introducing a "Shared" state beside "Primary" and "Secondary" for
  use with GFS at least the following items need to be done.
  *) transfer_log and epoch_set reside in the same memory now.
  *) writes on the receiver side must be done with a temporary
     buffer_head directly to the lower level device. 
     Otherwise we would get in an endless loop sending the same 
     block over all the time.
  *) All occurences of "Primary" or "Secondary" must be reviewed.
*/

#ifdef HAVE_AUTOCONF
#include <linux/autoconf.h>
#endif
#ifdef CONFIG_MODVERSIONS
#include <linux/modversions.h>
#endif

#include <asm/uaccess.h>
#include <asm/bitops.h> 
#include <net/sock.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/pkt_sched.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#include "drbd.h"
#include "drbd_int.h"
#include "mbds.h"

static int errno;

/* This maches BM_BLOCK_SIZE */

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
#include <linux/blkpg.h>
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,2,18)
#define init_MUTEX_LOCKED( A )    (*(A)=MUTEX_LOCKED)
#define init_MUTEX( A )           (*(A)=MUTEX)
#define init_waitqueue_head( A )  (*(A)=0)
typedef struct wait_queue*  wait_queue_head_t;
#endif
#define blkdev_dequeue_request(A) CURRENT=(A)->next
#endif

/*
 * GFP_DRBD is used for allocations inside drbd_do_request.
 *
 * 2.4 kernels will probably remove the __GFP_IO check in the VM code,
 * so lets use GFP_ATOMIC for allocations.  For 2.2, we abuse the GFP_BUFFER 
 * flag to avoid __GFP_IO, thus avoiding the use of the atomic queue and 
 *  avoiding the deadlock.
 *
 * - marcelo
 */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
#define GFP_DRBD GFP_ATOMIC
#else
#define GFP_DRBD GFP_BUFFER
#endif


/* #define ES_SIZE_STATS 50 */

int drbdd_init(struct Drbd_thread*);
int drbd_syncer(struct Drbd_thread*);
int drbd_asender(struct Drbd_thread*);
int drbd_md_compare(int minor,Drbd_Parameter_P* partner);
int drbd_md_syncq_ok(int minor,Drbd_Parameter_P* partner);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
/*static */ int drbd_proc_get_info(char *, char **, off_t, int, int *,
				   void *);
/*static */ void drbd_do_request(request_queue_t *);
#else
/*static */ int drbd_proc_get_info(char *, char **, off_t, int, int);
/*static */ void drbd_do_request(void);
#endif
/*static */ void drbd_dio_end(struct buffer_head *bh, int uptodate);

int drbd_init(void);
/*static */ int drbd_open(struct inode *inode, struct file *file);
/*static */ int drbd_close(struct inode *inode, struct file *file);
/*static */ int drbd_ioctl(struct inode *inode, struct file *file,
			   unsigned int cmd, unsigned long arg);
/*static */ int drbd_fsync(struct file *file, struct dentry *dentry);
void drbd_end_req(struct request *req, int nextstate,int uptodate);
void drbd_p_timeout(unsigned long arg);
struct mbds_operations bm_mops;

#ifdef DEVICE_REQUEST
#undef DEVICE_REQUEST
#endif
#define DEVICE_REQUEST drbd_do_request

MODULE_AUTHOR("Philipp Reisner <philipp@linuxfreak.com>");
MODULE_DESCRIPTION("drbd - Network block device");
MODULE_PARM(minor_count,"i");
MODULE_PARM_DESC(minor_count, "Maximum number of drbd devices (1-255)");

/*static */ int *drbd_blocksizes;
/*static */ int *drbd_sizes;
struct Drbd_Conf *drbd_conf;
int minor_count=2;


#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,40)
/*static */ struct block_device_operations drbd_ops = {
	open:		drbd_open,
	release:	drbd_close,
	ioctl:          drbd_ioctl
};
#else
/*static */ struct file_operations drbd_ops =
{
	read:           block_read,
	write:          block_write,
	ioctl:          drbd_ioctl,
	open:           drbd_open,
	release:        drbd_close,
	fsync:          block_fsync
};
#endif

#define min(a,b) ( (a) < (b) ? (a) : (b) )
#define max(a,b) ( (a) > (b) ? (a) : (b) )
#define ARRY_SIZE(A) (sizeof(A)/sizeof(A[0]))


int drbd_log2(int i)
{
	int bits = 0;
	int add_one=0; /* In case there is not a whole-numbered solution,
			  round up */
	while (i != 1) {
		bits++;
		if ( (i & 1) == 1) add_one=1;
		i >>= 1;
	}
	return bits+add_one;
}



/************************* The transfer log start */
#define TL_BARRIER    0
/* spinlock readme:
   tl_dependence() only needs a read-lock and is called from interrupt time.
   See Documentation/spinlocks.txt why this is valid.
*/

#if 0
void print_tl(struct Drbd_Conf *mdev)
{
	struct Tl_entry* p=mdev->tl_begin;

	printk(KERN_ERR "TransferLog (oldest entry first):\n");

	while( p != mdev->tl_end ) {
		if(p->req == TL_BARRIER)
			printk(KERN_ERR "BARRIER (%ld)\n",p->sector_nr);
		else
			printk(KERN_ERR "Sector %ld.\n",p->sector_nr);
		
		p++;
		if (p == mdev->transfer_log + mdev->conf.tl_size) 
			p = mdev->transfer_log;
	}
	
	printk(KERN_ERR "TransferLog (end)\n");
}
#endif

inline void tl_add(struct Drbd_Conf *mdev, struct request * new_item)
{
	unsigned long flags;

	write_lock_irqsave(&mdev->tl_lock,flags);


	  /*printk(KERN_ERR DEVICE_NAME "%d: tl_add(%ld)\n",
	    (int)(mdev-drbd_conf),new_item->sector);*/

	mdev->tl_end->req = new_item;
	mdev->tl_end->sector_nr = new_item->sector;

	mdev->tl_end++;

	if (mdev->tl_end == mdev->transfer_log + mdev->conf.tl_size)
		mdev->tl_end = mdev->transfer_log;

	if (mdev->tl_end == mdev->tl_begin)
		printk(KERN_CRIT DEVICE_NAME "%d: transferlog too small!! \n",
		 (int)(mdev-drbd_conf));

	/* DEBUG CODE */
	/* look if this block is alrady in this epoch */
#if 0
	/* print_tl(mdev); */


	{
		struct Tl_entry* p=mdev->tl_end;
		while( p != mdev->tl_begin ) {

			if(p->req == TL_BARRIER) break;
			if(p->sector_nr == new_item->sector) {
				printk(KERN_CRIT DEVICE_NAME 
				       "%d: Sector %ld is already in !!\n",
				       (int)(mdev-drbd_conf),
				       new_item->sector);
			}

			p--;
			if (p == mdev->transfer_log) 
				p = mdev->transfer_log + mdev->conf.tl_size;
		}
	}
#endif


	write_unlock_irqrestore(&mdev->tl_lock,flags);
}

inline unsigned int tl_add_barrier(struct Drbd_Conf *mdev)
{
        static unsigned int br_cnt=0;
	unsigned long flags;

	write_lock_irqsave(&mdev->tl_lock,flags);

	/*printk(KERN_DEBUG DEVICE_NAME "%d: tl_add(TL_BARRIER)\n",
	  (int)(mdev-drbd_conf));*/

	br_cnt++;
	if(br_cnt == 0) br_cnt = 1;

	mdev->tl_end->req = TL_BARRIER;
	mdev->tl_end->sector_nr = br_cnt;

	mdev->tl_end++;

	if (mdev->tl_end == mdev->transfer_log + mdev->conf.tl_size)
		mdev->tl_end = mdev->transfer_log;

	if (mdev->tl_end == mdev->tl_begin)
		printk(KERN_CRIT DEVICE_NAME "%d: transferlog too small!!\n",
		       (int)(mdev-drbd_conf));

	write_unlock_irqrestore(&mdev->tl_lock,flags);

	return br_cnt;
}

void tl_release(struct Drbd_Conf *mdev,unsigned int barrier_nr,
		       unsigned int set_size)
{
        int epoch_size=0; 
	unsigned long flags;
	write_lock_irqsave(&mdev->tl_lock,flags);

	/* printk(KERN_DEBUG DEVICE_NAME ": tl_release(%u)\n",barrier_nr); */

	if (mdev->tl_begin->req == TL_BARRIER) epoch_size--;

	do {
		mdev->tl_begin++;

		if (mdev->tl_begin == mdev->transfer_log + mdev->conf.tl_size)
			mdev->tl_begin = mdev->transfer_log;

		if (mdev->tl_begin == mdev->tl_end)
			printk(KERN_ERR DEVICE_NAME "%d: tl messed up!\n",
			       (int)(mdev-drbd_conf));
		epoch_size++;
	} while (mdev->tl_begin->req != TL_BARRIER);

	if(mdev->tl_begin->sector_nr != barrier_nr) /* CHK */
		printk(KERN_ERR DEVICE_NAME "%d: invalid barrier number!!"
		       "found=%u, reported=%u\n",(int)(mdev-drbd_conf),
		       (unsigned int)mdev->tl_begin->sector_nr,barrier_nr);

	if(epoch_size != set_size) /* CHK */
		printk(KERN_ERR DEVICE_NAME "%d: Epoch set size wrong!!"
		       "found=%d reported=%d \n",(int)(mdev-drbd_conf),
		       epoch_size,set_size);

	write_unlock_irqrestore(&mdev->tl_lock,flags);

#ifdef ES_SIZE_STATS
	mdev->essss[set_size]++;
#endif  

}

inline int tl_dependence(struct Drbd_Conf *mdev, unsigned long sect_nr)
{
	struct Tl_entry* p;
	int r;

	read_lock(&mdev->tl_lock);

	p = mdev->tl_end;
	while( TRUE ) {
		if ( p==mdev->tl_begin ) {r=FALSE; break;}
	        if ( p==mdev->transfer_log) {
			p = p + mdev->conf.tl_size;
			if ( p==mdev->tl_begin ) {r=FALSE; break;}
		}
		p--;
		if ( p->req==TL_BARRIER) {r=FALSE; break;}
		if ( p->sector_nr == sect_nr) {r=TRUE; break;}
	}

	read_unlock(&mdev->tl_lock);
	return r;
}

void tl_clear(struct Drbd_Conf *mdev)
{
	struct Tl_entry* p = mdev->tl_begin;
	kdev_t dev = MKDEV(MAJOR_NR,mdev-drbd_conf);
	int end_them = mdev->conf.wire_protocol == DRBD_PROT_B || 
                       mdev->conf.wire_protocol == DRBD_PROT_C;
	unsigned long flags;
	write_lock_irqsave(&mdev->tl_lock,flags);

	while(p != mdev->tl_end) {
	        if(p->req != TL_BARRIER) {
	                mdev->mops->set_block_status(mdev->mbds_id,
				     p->sector_nr >> (mdev->blk_size_b-9),
				     mdev->blk_size_b, SS_OUT_OF_SYNC);
			if(end_them && 
			   p->req->rq_status != RQ_INACTIVE &&
			   p->req->rq_dev == dev &&
			   p->req->sector == p->sector_nr ) {
		                drbd_end_req(p->req,RQ_DRBD_SENT,1);
				mdev->pending_cnt--;
#if 0 /*Debug ... */
				printk(KERN_CRIT DEVICE_NAME 
				       "%d: end_req(Sector %ld)\n",
				       (int)(mdev-drbd_conf),
				       p->sector_nr);
#endif
			}
#if 0 /* dEBGug */
			else {
				printk(KERN_CRIT DEVICE_NAME 
				       "%d: not_ending(Sector %ld)\n",
				       (int)(mdev-drbd_conf),
				       p->sector_nr);
			}
#endif 

		}
		p++;
		if (p == mdev->transfer_log + mdev->conf.tl_size)
		        p = mdev->transfer_log;	    
	}
	tl_init(mdev);
	write_unlock_irqrestore(&mdev->tl_lock,flags);
}     

int drbd_thread_setup(void* arg)
{
	struct Drbd_thread *thi = (struct Drbd_thread *) arg;
	int retval;

	lock_kernel();
	exit_mm(current);	/* give up UL-memory context */
	exit_files(current);	/* give up open filedescriptors */

	current->session = 1;
	current->pgrp = 1;

	if (!thi->pid) sleep_on(&thi->wait);

	retval = thi->function(thi);

	thi->pid = 0;
	wake_up(&thi->wait);
	set_bit(COLLECT_ZOMBIES,&drbd_conf[thi->minor].flags);

	return retval;
}

void drbd_thread_init(int minor, struct Drbd_thread *thi,
		      int (*func) (struct Drbd_thread *))
{
	thi->pid = 0;
	init_waitqueue_head(&thi->wait);
	thi->function = func;
	thi->minor = minor;
}

void drbd_thread_start(struct Drbd_thread *thi)
{
	int pid;

	if (thi->pid == 0) {
		thi->t_state = Running;

		pid = kernel_thread(drbd_thread_setup, (void *) thi, CLONE_FS);

		if (pid < 0) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: Couldn't start thread (%d)\n", thi->minor,
			       pid);
			return;
		}
		/* printk(KERN_DEBUG DEVICE_NAME ": pid = %d\n", pid); */
		thi->pid = pid;
		wake_up(&thi->wait);
	}
}


void _drbd_thread_stop(struct Drbd_thread *thi, int restart,int wait)
{
        int err;
	if (!thi->pid) return;

	if (restart)
		thi->t_state = Restarting;
	else
		thi->t_state = Exiting;

	err = kill_proc_info(SIGTERM, NULL, thi->pid);

	if (err == 0) {
		if(wait) {
			sleep_on(&thi->wait);	/* wait until the thread
						   has closed the socket */

			/*
			  This would be the *nice* solution, but it crashed
			  my machine...
			  
			  struct task_struct *p;
			  read_lock(&tasklist_lock);
			  p = find_task_by_pid(drbd_conf[minor].rpid);
			  p->p_pptr = current;
			  errno = send_sig_info(SIGTERM, NULL, p);
			  read_unlock(&tasklist_lock);
			  interruptible_sleep_on(&current->wait_chldexit);
			*/

			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 10);
		}
	} else {
		printk(KERN_ERR DEVICE_NAME "%d: could not send signal\n",
		       thi->minor);
	}
}

int drbd_send_cmd(int minor,Drbd_Packet_Cmd cmd)
{
	int err;
	Drbd_Packet head;

	down(&drbd_conf[minor].send_mutex);
	err = drbd_send(&drbd_conf[minor], cmd,&head,sizeof(head),0,0);
	up(&drbd_conf[minor].send_mutex);

	return err;  
}

int drbd_send_param(int minor)
{
	Drbd_Parameter_Packet param;
	int err,i;
	kdev_t ll_dev = drbd_conf[minor].lo_device;

	if (blk_size[MAJOR(ll_dev)]) {
		param.h.size =
		    cpu_to_be64(blk_size[MAJOR(ll_dev)][MINOR(ll_dev)]);
	} else
		printk(KERN_ERR DEVICE_NAME
		       "%d: LL device has no size ?!?\n\n",minor);

	param.h.blksize = cpu_to_be32(1 << drbd_conf[minor].blk_size_b);
	param.h.state = cpu_to_be32(drbd_conf[minor].state);
	param.h.protocol = cpu_to_be32(drbd_conf[minor].conf.wire_protocol);
	param.h.version = cpu_to_be32(MOD_VERSION);

	for(i=0;i<=PrimaryInd;i++) 
		param.h.gen_cnt[i]=cpu_to_be32(drbd_conf[minor].gen_cnt[i]);

	down(&drbd_conf[minor].send_mutex);
	err = drbd_send(&drbd_conf[minor], ReportParams, (Drbd_Packet*)&param, 
			sizeof(param),0,0);
	up(&drbd_conf[minor].send_mutex);
	
	if(err < sizeof(Drbd_Parameter_Packet))
		printk(KERN_ERR DEVICE_NAME
		       "%d: Sending of parameter block failed!!\n",minor);  

	return err;
}

int drbd_send_cstate(struct Drbd_Conf *mdev)
{
	Drbd_CState_Packet head;
	int ret;
	
	head.h.cstate = cpu_to_be32(mdev->cstate);

	down(&mdev->send_mutex);
	ret=drbd_send(mdev,CStateChanged,(Drbd_Packet*)&head,sizeof(head),0,0);
	up(&mdev->send_mutex);
	return ret;

}

int _drbd_send_barrier(struct Drbd_Conf *mdev)
{
	int r;
        Drbd_Barrier_Packet head;

	/* tl_add_barrier() must be called with the send_mutex aquired */
	head.h.barrier=tl_add_barrier(mdev); 

	/* printk(KERN_DEBUG DEVICE_NAME": issuing a barrier\n"); */
       
	r=drbd_send(mdev,Barrier,(Drbd_Packet*)&head,sizeof(head),0,0);

	if( r == sizeof(head) ) inc_pending((int)(mdev-drbd_conf));

	return r;
}

int drbd_send_b_ack(struct Drbd_Conf *mdev, u32 barrier_nr,u32 set_size)
{
        Drbd_BarrierAck_Packet head;
	int ret;
       
        head.h.barrier = barrier_nr;
	head.h.set_size = cpu_to_be32(set_size);
	down(&mdev->send_mutex);
	ret=drbd_send(mdev,BarrierAck,(Drbd_Packet*)&head,sizeof(head),0,0);
	up(&mdev->send_mutex);
	return ret;
}


int drbd_send_ack(struct Drbd_Conf *mdev, int cmd, unsigned long block_nr,
		  u64 block_id)
{
        Drbd_BlockAck_Packet head;
	int ret;

	head.h.block_nr = cpu_to_be64(block_nr);
        head.h.block_id = block_id;
	down(&mdev->send_mutex);
	ret=drbd_send(mdev,cmd,(Drbd_Packet*)&head,sizeof(head),0,0);
	up(&mdev->send_mutex);
	return ret;
}

int drbd_send_data(struct Drbd_Conf *mdev, void* data, size_t data_size,
		   unsigned long block_nr, u64 block_id)
{
        Drbd_Data_Packet head;
	int ret;

	head.h.block_nr = cpu_to_be64(block_nr);
	head.h.block_id = block_id;

	down(&mdev->send_mutex);
	
	if(test_and_clear_bit(ISSUE_BARRIER,&mdev->flags)) {
	        _drbd_send_barrier(mdev);
	}
	
	ret=drbd_send(mdev,Data,(Drbd_Packet*)&head,sizeof(head),data,
		      data_size);

	if(block_id != ID_SYNCER) {
		if( ret == data_size + sizeof(head)) {
			/* This must be within the semaphore */
			tl_add(mdev,(struct request*)(unsigned long)block_id);
			if(mdev->conf.wire_protocol != DRBD_PROT_A)
				inc_pending((int)(mdev-drbd_conf));
		} else {
			mdev->mops->set_block_status(mdev->mbds_id,
			      block_nr,mdev->blk_size_b,SS_OUT_OF_SYNC);
			ret=0;
		}
	}

	up(&mdev->send_mutex);

	return ret;
}

void drbd_timeout(unsigned long arg)
{
	struct task_struct *p = (struct task_struct *) arg;

	printk(KERN_ERR DEVICE_NAME" : timeout detected! (pid=%d)\n",p->pid);

	send_sig_info(DRBD_SIG, NULL, p);

}

void drbd_a_timeout(unsigned long arg)
{
	struct Drbd_thread* thi = (struct Drbd_thread* ) arg;

	printk(KERN_ERR DEVICE_NAME "%d: ack timeout detected (pc=%d)!\n",
	       thi->minor,
	       drbd_conf[thi->minor].pending_cnt);

	if(drbd_conf[thi->minor].cstate >= Connected) {
		set_cstate(&drbd_conf[thi->minor],Timeout);
		drbd_thread_restart_nowait(thi);
	}
}

int drbd_send(struct Drbd_Conf *mdev, Drbd_Packet_Cmd cmd, 
	      Drbd_Packet* header, size_t header_size, 
	      void* data, size_t data_size)
{
	mm_segment_t oldfs;
	struct msghdr msg;
	struct iovec iov[2];

	int err;

	if (!mdev->sock) return -1000;
	if (mdev->cstate < WFReportParams) return -1001;

	header->magic  =  cpu_to_be32(DRBD_MAGIC);
	header->command = cpu_to_be16(cmd);
	header->length  = cpu_to_be16(data_size);

	mdev->sock->sk->allocation = GFP_DRBD;

	iov[0].iov_base = header;
	iov[0].iov_len = header_size;
	iov[1].iov_base = data;
	iov[1].iov_len = data_size;

	msg.msg_iov = iov;
	msg.msg_iovlen = data_size > 0 ? 2 : 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_flags = MSG_NOSIGNAL;

	/* repeat: */

	if (mdev->conf.timeout) {
		init_timer(&mdev->s_timeout);
		mdev->s_timeout.data = (unsigned long) current;
		mdev->s_timeout.expires =
		    jiffies + mdev->conf.timeout * HZ / 10;
		add_timer(&mdev->s_timeout);
	}
	lock_kernel();
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	err = sock_sendmsg(mdev->sock, &msg, header_size+data_size);
	set_fs(oldfs);
	unlock_kernel();

	if (mdev->conf.timeout) {
		unsigned long flags;
		del_timer(&mdev->s_timeout);
		spin_lock_irqsave(&current->sigmask_lock,flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
		if (sigismember(&current->signal, DRBD_SIG)) {
			sigdelset(&current->signal, DRBD_SIG);
#else
		if (sigismember(&current->pending.signal, DRBD_SIG)) {
			sigdelset(&current->pending.signal, DRBD_SIG);
#endif
			recalc_sigpending(current);
			spin_unlock_irqrestore(&current->sigmask_lock,flags);

			/*
			if (mdev->sock->flags & SO_NOSPACE) {
				printk(KERN_ERR DEVICE_NAME
				       "%d: no mem for sock!\n",
				       (int)(mdev-drbd_conf));
				goto repeat;
			}
			*/

			printk(KERN_ERR DEVICE_NAME
			       "%d: send timed out!! (pid=%d)\n",
			       (int)(mdev-drbd_conf),current->pid);

			set_cstate(mdev,Timeout);

			drbd_thread_restart_nowait(&mdev->receiver);

			return -1002;
		} else spin_unlock_irqrestore(&current->sigmask_lock,flags);
	}
	if (err != header_size+data_size) {
		printk(KERN_ERR DEVICE_NAME "%d: sock_sendmsg returned %d\n",
		       (int)(mdev-drbd_conf),err);
	}
	if (err < 0) {
		set_cstate(mdev,BrokenPipe);
		drbd_thread_restart_nowait(&mdev->receiver);	  
		return -1003;
	}

	return err;
}

void drbd_setup_sock(struct Drbd_Conf *mdev)
{
	/* to prevent oom deadlock... */
	/* The default allocation priority was GFP_KERNEL */
	mdev->sock->sk->allocation = GFP_DRBD;

	/*
	  We could also use TC_PRIO_CONTROL / TC_PRIO_BESTEFFORT
	*/
	switch(mdev->state) {
	case Primary: 
		mdev->sock->sk->priority=TC_PRIO_BULK;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		mdev->sock->sk->tp_pinfo.af_tcp.nonagle=0;
#else
		mdev->sock->sk->nonagle=0;
#endif
		mdev->sock->sk->sndbuf = 2*65535; 
		/* This boosts the performance of the syncer to 6M/s max */

		break;
	case Secondary:
		mdev->sock->sk->priority=TC_PRIO_INTERACTIVE;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		mdev->sock->sk->tp_pinfo.af_tcp.nonagle=1;
#else
		mdev->sock->sk->nonagle=1;
#endif
		mdev->sock->sk->sndbuf = 2*32767;
		/* Small buffer -> small response time */

		break;
	case Unknown:
	}
}

/*static */ int drbd_open(struct inode *inode, struct file *file)
{
	int minor;

	minor = MINOR(inode->i_rdev);
	if(minor >= minor_count) return -ENODEV;

	if (file->f_mode & FMODE_WRITE) {
		if( drbd_conf[minor].state == Secondary) {
			return -EROFS;
		}
		set_bit(WRITER_PRESENT, &drbd_conf[minor].flags);
	}

	drbd_conf[minor].open_cnt++;

	MOD_INC_USE_COUNT;

	return 0;
}

/*static */ int drbd_close(struct inode *inode, struct file *file)
{
	/* do not use *file (May be NULL, in case of a unmount :-) */
	int minor;

	minor = MINOR(inode->i_rdev);
	if(minor >= minor_count) return -ENODEV;

	/*
	printk(KERN_ERR DEVICE_NAME ": close(inode=%p,file=%p)"
	       "current=%p,minor=%d,wc=%d\n", inode, file, current, minor,
	       inode->i_writecount);
	*/

	if (--drbd_conf[minor].open_cnt == 0) {
		clear_bit(WRITER_PRESENT, &drbd_conf[minor].flags);
	}

	MOD_DEC_USE_COUNT;

	return 0;
}


void drbd_end_req(struct request *req, int nextstate, int uptodate)
{
	int wake_asender=0;
	unsigned long flags=0;
	struct Drbd_Conf* mdev = &drbd_conf[MINOR(req->rq_dev)];

	if (req->cmd == READ)
		goto end_it_unlocked;

	/* This was a hard one! Can you see the race?
	   (It hit me about once out of 20000 blocks) 

	   switch(status) {
	   ..: status = ...;
	   }
	*/

	spin_lock_irqsave(&mdev->req_lock,flags);

	switch (req->rq_status & 0xfffe) {
	case RQ_DRBD_SEC_WRITE:
	        wake_asender=1;
		goto end_it;
	case RQ_DRBD_NOTHING:
		req->rq_status = nextstate | (uptodate ? 1 : 0);
		break;
	case RQ_DRBD_SENT:
		if (nextstate == RQ_DRBD_WRITTEN)
			goto end_it;
		printk(KERN_ERR DEVICE_NAME "%d: request state error(A)\n",
		       (int)(mdev-drbd_conf));
		break;
	case RQ_DRBD_WRITTEN:
		if (nextstate == RQ_DRBD_SENT)
			goto end_it;
		printk(KERN_ERR DEVICE_NAME "%d: request state error(B)\n",
		       (int)(mdev-drbd_conf));
		break;
	default:
		printk(KERN_ERR DEVICE_NAME "%d: request state error(%X)\n",
		       (int)(mdev-drbd_conf),req->rq_status);
	}

	spin_unlock_irqrestore(&mdev->req_lock,flags);

	return;

/* We only report uptodate == TRUE if both operations (WRITE && SEND)
   reported uptodate == TRUE 
 */

	end_it:
	spin_unlock_irqrestore(&mdev->req_lock,flags);

	end_it_unlocked:

	if(mdev->state == Primary && mdev->cstate >= Connected) {
	  /* If we are unconnected we may not call tl_dependece, since
	     then this call could be from tl_clear(). => spinlock deadlock!
	  */
	        if(tl_dependence(mdev,req->sector)) {
	                set_bit(ISSUE_BARRIER,&mdev->flags);
			wake_asender=1;
		}
	}

	if(!end_that_request_first(req, uptodate & req->rq_status,DEVICE_NAME))
	        end_that_request_last(req);


	if( mdev->do_panic && !(uptodate & req->rq_status) ) {
		panic(DEVICE_NAME": The lower-level device had an error.\n");
	}

	/* NICE: It would be nice if we could AND this condition.
	   But we must also wake the asender if we are receiving 
	   syncer blocks! */
	if(wake_asender /*&& mdev->conf.wire_protocol == DRBD_PROT_C*/ ) {
	        wake_up_interruptible(&mdev->asender_wait);
	}
}

void drbd_dio_end(struct buffer_head *bh, int uptodate)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        struct request *req = bh->b_dev_id;
#else
	struct request *req = bh->b_private;
#endif
	// READs are sorted out in drbd_end_req().
	drbd_end_req(req, RQ_DRBD_WRITTEN, uptodate);
	
	kfree(bh);
}

/*
  We should _nerver_ sleep with the io_request_lock aquired. (See ll_rw_block)
  Up to now I have considered these ways out:
  * 1) unlock the io_request_lock for the time of the send 
         Not possible, because I do not have the flags for the unlock.
           -> Forget the flags, look at the loop block device!!
  * 2) postpone the send to some point in time when the request lock
       is not hold. 
         Maybe using the tq_scheduler task queue, or an dedicated
         execution context (kernel thread).

         I am not sure if tq_schedule is a good idea, because we
         could send some process to sleep, which would not sleep
	 otherwise.
	   -> tq_schedule is a bad idea, sometimes sock_sendmsg
	      behaves *bad* ( return value does not indicate
	      an error, but ... )

  Non atomic things, that need to be done are:
  sock_sendmsg(), kmalloc(,GFP_KERNEL) and ll_rw_block().
*/

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
/*static */ void drbd_do_request(request_queue_t * q)
#else
/*static */ void drbd_do_request()
#endif
{
	int minor = 0;
	struct request *req;
	int sending;

	minor = MINOR(CURRENT->rq_dev);

	if (blksize_size[MAJOR_NR][minor] !=
	    (1 << drbd_conf[minor].blk_size_b)) {
		/* If someone called set_blocksize() from fs/buffer.c ... */
		int new_blksize;

		spin_unlock_irq(&io_request_lock);

		new_blksize = blksize_size[MAJOR_NR][minor];
		set_blocksize(drbd_conf[minor].lo_device, new_blksize);
		drbd_conf[minor].blk_size_b = drbd_log2(new_blksize);

		printk(KERN_INFO DEVICE_NAME "%d: blksize=%d B\n",
		       minor,new_blksize);

		spin_lock_irq(&io_request_lock);
	}
	while (TRUE) {
		INIT_REQUEST;
		req=CURRENT;
		blkdev_dequeue_request(req);
		
#if 0
		{
			static const char *strs[2] = 
			{
				"READ",
				"WRITE"
			};
			
			/* if(req->cmd == WRITE) */
			printk(KERN_ERR DEVICE_NAME "%d: do_request(cmd=%s,"
			       "sec=%ld,nr_sec=%ld,cnr_sec=%ld)\n",
			       minor,
			       strs[req->cmd == READ ? 0 : 1],req->sector,
			       req->nr_sectors,
			       req->current_nr_sectors);
		}
#endif

		spin_unlock_irq(&io_request_lock);

		sending = 0;

		if (req->cmd == WRITE && drbd_conf[minor].state == Primary) {
			if ( drbd_conf[minor].cstate >= Connected
			     && req->sector >= drbd_conf[minor].synced_to) {
				sending = 1;
			}
		}

		/* Do disk - IO */
		{
			struct buffer_head *bh;
			int size_kb=1<<(drbd_conf[minor].blk_size_b-10);
		
			bh = kmalloc(sizeof(struct buffer_head), GFP_DRBD);
			if (!bh) {
				printk(KERN_ERR DEVICE_NAME
				       "%d: could not kmalloc()\n",minor);
				return;
			}

			memset(bh, 0, sizeof(*bh));
			bh->b_blocknr=req->bh->b_blocknr;
			bh->b_size=req->bh->b_size;
			bh->b_data=req->bh->b_data;
			bh->b_list = BUF_LOCKED;
			bh->b_end_io = drbd_dio_end;
			bh->b_dev = drbd_conf[minor].lo_device;
			bh->b_rdev = drbd_conf[minor].lo_device;
			bh->b_rsector = req->bh->b_rsector;
			bh->b_end_io = drbd_dio_end;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
			bh->b_count=0;
			bh->b_this_page=0;
			bh->b_dev_id = req;
			bh->b_state = (1 << BH_Req) | (1 << BH_Dirty);
#else
			bh->b_page=req->bh->b_page; /* missing in 2.2.x part*/
			atomic_set(&bh->b_count, 0);
			bh->b_private = req;
			bh->b_state = (1 << BH_Req) | (1 << BH_Dirty)
			  | ( 1 << BH_Mapped) | (1 << BH_Lock);
#endif
			
#ifdef BH_JWrite
			if (test_bit(BH_JWrite, &req->bh->b_state))
				set_bit(BH_JWrite, &bh->b_state);
#endif			

			
			if(req->cmd == WRITE) 
				drbd_conf[minor].writ_cnt+=size_kb;
			else drbd_conf[minor].read_cnt+=size_kb;

			if (sending)
				req->rq_status = RQ_DRBD_NOTHING;
			else if (req->cmd == WRITE) {
			        if(drbd_conf[minor].state == Secondary)
				  req->rq_status = RQ_DRBD_SEC_WRITE | 0x0001;
				else {
				  req->rq_status = RQ_DRBD_SENT | 0x0001;
				  drbd_conf[minor].mops->
				    set_block_status(drbd_conf[minor].mbds_id,
			               req->sector >> 
					  (drbd_conf[minor].blk_size_b-9),
				       drbd_conf[minor].blk_size_b, 
				       SS_OUT_OF_SYNC);
				}
			}
			else
				req->rq_status = RQ_DRBD_READ | 0x0001;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
			ll_rw_block(req->cmd, 1, &bh);
#else
			generic_make_request(req->cmd,bh);
#endif
		}

		/* Send it out to the network */
		if (sending) {
			int bnr;
			int send_ok;
			bnr = req->sector >> (drbd_conf[minor].blk_size_b - 9);
     		        send_ok=drbd_send_data(&drbd_conf[minor], req->buffer,
					   req->current_nr_sectors << 9,
					   bnr,(unsigned long)req);

			if(send_ok) {
			        drbd_conf[minor].send_cnt+=
					req->current_nr_sectors<<1;
			}

			if( drbd_conf[minor].conf.wire_protocol==DRBD_PROT_A ||
			    (!send_ok) ) {
				/* If sending failed, we can not expect
				   an ack packet. */
			         drbd_end_req(req, RQ_DRBD_SENT, 1);
			}
				
		}
		spin_lock_irq(&io_request_lock);
	}
}

int __init drbd_init(void)
{

	int i;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	drbd_proc = create_proc_read_entry("drbd", 0, &proc_root,
					   drbd_proc_get_info, NULL);
	if (!drbd_proc)
#else
	if (proc_register(&proc_root, &drbd_proc_dir))
#endif
	{
		printk(KERN_ERR DEVICE_NAME": unable to register proc file\n");
		return -EIO;
	}

	if (register_blkdev(MAJOR_NR, DEVICE_NAME, &drbd_ops)) {

		printk(KERN_ERR DEVICE_NAME ": Unable to get major %d\n",
		       MAJOR_NR);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		if (drbd_proc)
			remove_proc_entry("drbd", &proc_root);
#else
		proc_unregister(&proc_root, drbd_proc_dir.low_ino);
#endif

		return -EBUSY;
	}


	drbd_blocksizes = kmalloc(sizeof(int)*minor_count,GFP_KERNEL);
	drbd_sizes = kmalloc(sizeof(int)*minor_count,GFP_KERNEL);
	drbd_conf = kmalloc(sizeof(struct Drbd_Conf)*minor_count,GFP_KERNEL);

	/* Initialize size arrays. */

	for (i = 0; i < minor_count; i++) {
		drbd_blocksizes[i] = INITIAL_BLOCK_SIZE;
		drbd_conf[i].blk_size_b = drbd_log2(INITIAL_BLOCK_SIZE);
		drbd_sizes[i] = 0;
		set_device_ro(MKDEV(MAJOR_NR, i), FALSE /*TRUE */ );
		drbd_conf[i].do_panic = 0;
		drbd_conf[i].sock = 0;
		drbd_conf[i].lo_file = 0;
		drbd_conf[i].lo_device = 0;
		drbd_conf[i].state = Secondary;
		init_waitqueue_head(&drbd_conf[i].state_wait);
		drbd_conf[i].o_state = Unknown;
		drbd_conf[i].cstate = Unconfigured;
		drbd_conf[i].send_cnt = 0;
		drbd_conf[i].recv_cnt = 0;
		drbd_conf[i].writ_cnt = 0;
		drbd_conf[i].read_cnt = 0;
		drbd_conf[i].pending_cnt = 0;
		drbd_conf[i].unacked_cnt = 0;
		drbd_conf[i].transfer_log = 0;
		drbd_conf[i].mops = &bm_mops;
		drbd_conf[i].mbds_id = 0;
		drbd_conf[i].flags=0;
		tl_init(&drbd_conf[i]);
		drbd_conf[i].epoch_size=0;
		drbd_conf[i].a_timeout.function = drbd_a_timeout;
		drbd_conf[i].a_timeout.data = (unsigned long) 
			&drbd_conf[i].receiver;
		init_timer(&drbd_conf[i].a_timeout);
		drbd_conf[i].p_timeout.function = drbd_p_timeout;
		drbd_conf[i].p_timeout.data = (unsigned long) &drbd_conf[i];
		init_timer(&drbd_conf[i].p_timeout);
		drbd_conf[i].s_timeout.function = drbd_timeout;
		init_timer(&drbd_conf[i].s_timeout);
		drbd_conf[i].synced_to=0;
		init_MUTEX(&drbd_conf[i].send_mutex);
		drbd_thread_init(i, &drbd_conf[i].receiver, drbdd_init);
		drbd_thread_init(i, &drbd_conf[i].syncer, drbd_syncer);
		drbd_thread_init(i, &drbd_conf[i].asender, drbd_asender);
		drbd_conf[i].tl_lock = RW_LOCK_UNLOCKED;
		drbd_conf[i].es_lock = SPIN_LOCK_UNLOCKED;
		drbd_conf[i].req_lock = SPIN_LOCK_UNLOCKED;
		drbd_conf[i].sl_lock = SPIN_LOCK_UNLOCKED;
		init_waitqueue_head(&drbd_conf[i].asender_wait);
		init_waitqueue_head(&drbd_conf[i].cstate_wait);
		drbd_conf[i].open_cnt = 0;
		{
			int j;
			for(j=0;j<SYNC_LOG_S;j++) drbd_conf[i].sync_log[j]=0;
			for(j=0;j<=PrimaryInd;j++) drbd_conf[i].gen_cnt[j]=0;
			for(j=0;j<=PrimaryInd;j++) 
				drbd_conf[i].bit_map_gen[j]=0;
#ifdef ES_SIZE_STATS
			for(j=0;j<ES_SIZE_STATS;j++) drbd_conf[i].essss[j]=0;
#endif  
		}
	}
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), DEVICE_REQUEST);
#else
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
#endif
	blksize_size[MAJOR_NR] = drbd_blocksizes;
	blk_size[MAJOR_NR] = drbd_sizes;	/* Size in Kb */

	return 0;
}

int __init init_module()
{
	printk(KERN_INFO DEVICE_NAME ": module initialised. Version: %d\n",
	       MOD_VERSION);

	return drbd_init();

}

void cleanup_module()
{
	int i;

	for (i = 0; i < minor_count; i++) {
		drbd_set_state(i,Secondary);
		fsync_dev(MKDEV(MAJOR_NR, i));
		drbd_thread_stop(&drbd_conf[i].syncer);
		drbd_thread_stop(&drbd_conf[i].receiver);
		drbd_thread_stop(&drbd_conf[i].asender);
		drbd_free_resources(i);
		if (drbd_conf[i].transfer_log)
			kfree(drbd_conf[i].transfer_log);		    
		if (drbd_conf[i].mbds_id)
			drbd_conf[i].mops->cleanup(drbd_conf[i].mbds_id);
	}

	if (unregister_blkdev(MAJOR_NR, DEVICE_NAME) != 0)
		printk(KERN_ERR DEVICE_NAME": unregister of device failed\n");


#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
#else
	blk_dev[MAJOR_NR].request_fn = NULL;
#endif

	blksize_size[MAJOR_NR] = NULL;
	blk_size[MAJOR_NR] = NULL;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	if (drbd_proc)
		remove_proc_entry("drbd", &proc_root);
#else
	proc_unregister(&proc_root, drbd_proc_dir.low_ino);
#endif

	kfree(drbd_blocksizes);
	kfree(drbd_sizes);
	kfree(drbd_conf);
}



void drbd_free_ll_dev(int minor)
{
	if (drbd_conf[minor].lo_file) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		blkdev_put(drbd_conf[minor].lo_file->f_dentry->d_inode->i_bdev,
			   BDEV_FILE);
#else
		blkdev_release(drbd_conf[minor].lo_file->f_dentry->
			       d_inode);
#endif
		fput(drbd_conf[minor].lo_file);
		drbd_conf[minor].lo_file = 0;
		drbd_conf[minor].lo_device = 0;
	}
}

void drbd_free_sock(int minor)
{
	if (drbd_conf[minor].sock) {
		sock_release(drbd_conf[minor].sock);
		drbd_conf[minor].sock = 0;
	}
}


void drbd_free_resources(int minor)
{
	drbd_free_sock(minor);
	drbd_free_ll_dev(minor);
}

/*********************************/

/*** The bitmap stuff. ***/
/*
  We need to store one bit for a block. 
  Example: 1GB disk @ 4096 byte blocks ==> we need 32 KB bitmap.
  Bit 0 ==> Primary and secondary nodes are in sync.
  Bit 1 ==> secondary node's block must be updated. (')
*/

#include <asm/types.h>
#include <linux/vmalloc.h>

#define BM_BLOCK_SIZE_B  12  
#define BM_BLOCK_SIZE    (1<<12)

#define BM_IN_SYNC       0
#define BM_OUT_OF_SYNC   1

#if BITS_PER_LONG == 32
#define LN2_BPL 5
#elif BITS_PER_LONG == 64
#define LN2_BPL 6
#else
#error "LN2 of BITS_PER_LONG unknown!"
#endif

struct BitMap {
	kdev_t dev;
	unsigned long size;
	unsigned long* bm;
	unsigned long sb_bitnr;
	unsigned long sb_mask;
	unsigned long gb_bitnr;
	unsigned long gb_snr;
	spinlock_t bm_lock;
};

void* bm_init(kdev_t dev)
{
        struct BitMap* sbm;
	unsigned long size;

	size = blk_size[MAJOR(dev)][MINOR(dev)]>>(BM_BLOCK_SIZE_B-7);
	/* 7 = 10 - 3 ; 10 => blk_size is KB ; 3 -> 2^3=8 Bits per Byte */

	if(size == 0) return 0;

	sbm = vmalloc(size + sizeof(struct BitMap));

	sbm->dev = dev;
	sbm->size = size;
	sbm->bm = (unsigned long*)((char*)sbm + sizeof(struct BitMap));
	sbm->sb_bitnr=0;
	sbm->sb_mask=0;
	sbm->gb_bitnr=0;
	sbm->gb_snr=0;
	sbm->bm_lock = SPIN_LOCK_UNLOCKED;

	memset(sbm->bm,0,size);

	printk(KERN_INFO DEVICE_NAME " : vmallocing %ld B for bitmap."
	       " @%p\n",size,sbm->bm);
  
	return sbm;
}     

void bm_cleanup(void* bm_id)
{
        vfree(bm_id);
}

/* THINK:
   What happens when the block_size (ln2_block_size) changes between
   calls 
*/

void bm_set_bit(void* bm_id,unsigned long blocknr,int ln2_block_size, int bit)
{
        struct BitMap* sbm = (struct BitMap*) bm_id;
        unsigned long* bm = sbm->bm;
	unsigned long bitnr;
	int cb = (BM_BLOCK_SIZE_B-ln2_block_size);

	//if(bit) printk("Block %ld out of sync\n",blocknr);
	//else    printk("Block %ld now in sync\n",blocknr);
		

	bitnr = blocknr >> cb;

 	spin_lock(&sbm->bm_lock);

	if(!bit && cb) {
		if(sbm->sb_bitnr == bitnr) {
		        sbm->sb_mask |= 1L << (blocknr & ((1L<<cb)-1));
			if(sbm->sb_mask != (1L<<(1<<cb))-1) goto out;
		} else {
	                sbm->sb_bitnr = bitnr;
			sbm->sb_mask = 1L << (blocknr & ((1L<<cb)-1));
			goto out;
		}
	}

	if(bitnr>>LN2_BPL >= sbm->size) {
		printk(KERN_ERR DEVICE_NAME" : BitMap too small!\n");	  
		goto out;
	}

	bm[bitnr>>LN2_BPL] = bit ?
	  bm[bitnr>>LN2_BPL] |  ( 1L << (bitnr & ((1L<<LN2_BPL)-1)) ) :
	  bm[bitnr>>LN2_BPL] & ~( 1L << (bitnr & ((1L<<LN2_BPL)-1)) );

 out:
	spin_unlock(&sbm->bm_lock);
}

inline int bm_get_bn(unsigned long word,int nr)
{
	if(nr == BITS_PER_LONG-1) return -1;
	word >>= ++nr;
	while (! (word & 1)) {
                word >>= 1;
		if (++nr == BITS_PER_LONG) return -1;
	}
	return nr;
}

unsigned long bm_get_blocknr(void* bm_id,int ln2_block_size)
{
        struct BitMap* sbm = (struct BitMap*) bm_id;
        unsigned long* bm = sbm->bm;
	unsigned long wnr;
	unsigned long nw = sbm->size/sizeof(unsigned long);
	unsigned long rv;
	int cb = (BM_BLOCK_SIZE_B-ln2_block_size);

 	spin_lock(&sbm->bm_lock);

	if(sbm->gb_snr >= (1L<<cb)) {	  
		for(wnr=sbm->gb_bitnr>>LN2_BPL;wnr<nw;wnr++) {
	                if (bm[wnr]) {
				int bnr;
				if (wnr == sbm->gb_bitnr>>LN2_BPL)
					bnr = sbm->gb_bitnr & ((1<<LN2_BPL)-1);
				else bnr = -1;
				bnr = bm_get_bn(bm[wnr],bnr);
				if (bnr == -1) continue; 
			        sbm->gb_bitnr = (wnr<<LN2_BPL) + bnr;
				sbm->gb_snr = 0;
				goto out;
			}
		}
		rv = MBDS_DONE;
		goto r_out;
	}
 out:
	rv = (sbm->gb_bitnr<<cb) + sbm->gb_snr++;
 r_out:
 	spin_unlock(&sbm->bm_lock);	
	return rv;
}

void bm_reset(void* bm_id,int ln2_block_size)
{
	struct BitMap* sbm = (struct BitMap*) bm_id;

 	spin_lock(&sbm->bm_lock);

	sbm->gb_bitnr=0;
	if (sbm->bm[0] & 1) sbm->gb_snr=0;
	else sbm->gb_snr = 1L<<(BM_BLOCK_SIZE_B-ln2_block_size);

 	spin_unlock(&sbm->bm_lock);	
}

struct mbds_operations bm_mops = {
	bm_init,
	bm_cleanup,
	bm_reset,
	bm_set_bit,
	bm_get_blocknr
};


/* meta data management */

void drbd_md_write(int minor)
{
	u32 buffer[6];
	mm_segment_t oldfs;
	struct inode* inode;
	struct file* fp;
	char fname[25];
	int i;		

	drbd_conf[minor].gen_cnt[PrimaryInd]=(drbd_conf[minor].state==Primary);
	
	for(i=0;i<=PrimaryInd;i++) 
		buffer[i]=cpu_to_be32(drbd_conf[minor].gen_cnt[i]);
	buffer[MagicNr]=cpu_to_be32(DRBD_MAGIC);
	
	sprintf(fname,DRBD_MD_FILES,minor);
	fp=filp_open(fname,O_WRONLY|O_CREAT|O_TRUNC,00600);
	if(IS_ERR(fp)) goto err;
        oldfs = get_fs();
        set_fs(get_ds());
	inode = fp->f_dentry->d_inode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        down(&inode->i_sem);
#endif
	i=fp->f_op->write(fp,(const char*)&buffer,sizeof(buffer),&fp->f_pos);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	up(&inode->i_sem);
#endif
	set_fs(oldfs);
	filp_close(fp,NULL);
	if (i==sizeof(buffer)) return;
 err:
	printk(KERN_ERR DEVICE_NAME 
	       "%d: Error writing state file\n\"%s\"\n",minor,fname);
	return;
}

void drbd_md_read(int minor)
{
	u32 buffer[6];
	mm_segment_t oldfs;
	struct inode* inode;
	struct file* fp;
	char fname[25];
	int i;		

	sprintf(fname,DRBD_MD_FILES,minor);
	fp=filp_open(fname,O_RDONLY,0);
	if(IS_ERR(fp)) goto err;
        oldfs = get_fs();
        set_fs(get_ds());
	inode = fp->f_dentry->d_inode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        down(&inode->i_sem); 
#endif
	i=fp->f_op->read(fp,(char*)&buffer,sizeof(buffer),&fp->f_pos);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	up(&inode->i_sem);
#endif
	set_fs(oldfs);
	filp_close(fp,NULL);

	if(i != sizeof(buffer)) goto err;
	if(be32_to_cpu(buffer[MagicNr]) != DRBD_MAGIC) goto err;
	for(i=0;i<=PrimaryInd;i++) 
		drbd_conf[minor].gen_cnt[i]=be32_to_cpu(buffer[i]);

	return;
 err:
	printk(KERN_ERR DEVICE_NAME 
	       "%d: Error reading state file\n\"%s\"\n",minor,fname);
	for(i=0;i<PrimaryInd;i++) drbd_conf[minor].gen_cnt[i]=1;
	drbd_conf[minor].gen_cnt[PrimaryInd]=
		(drbd_conf[minor].state==Primary);
	drbd_md_write(minor);
	return;
}


/* Returns  1 if I have the good bits,
            0 if both are nice
	   -1 if the partner has the good bits.
*/
int drbd_md_compare(int minor,Drbd_Parameter_P* partner)
{
	int i;
	u32 me,other;
	
	for(i=0;i<=PrimaryInd;i++) {
		me=drbd_conf[minor].gen_cnt[i];
		other=be32_to_cpu(partner->gen_cnt[i]);
		if( me > other ) return 1;
		if( me < other ) return -1;
	}
	return 0;
}

/* Returns  1 if SyncingQuick is sufficient
            0 if SyncAll is needed.
*/
int drbd_md_syncq_ok(int minor,Drbd_Parameter_P* partner)
{
	int i;
	u32 me,other;

	if(be32_to_cpu(partner->gen_cnt[PrimaryInd])==1) return 0;
	for(i=HumanCnt;i<=ArbitraryCnt;i++) {
		me=drbd_conf[minor].bit_map_gen[i];
		other=be32_to_cpu(partner->gen_cnt[i]);
		if( me != other ) return 0;
	}
	return 1;
}

void drbd_md_inc(int minor, enum MetaDataIndex order)
{
	drbd_conf[minor].gen_cnt[order]++;
}

