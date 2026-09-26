#ifndef __GT_PQ_H
#define __GT_PQ_H

#define MAX_UTHREAD_PRIORITY 32
#define MAX_UTHREAD_GROUPS 32
#define DEFAULT_UTHREAD_PRIORITY 16

#define USECS_PER_QUANTA 1000

#define GT_SCHED_O1 0
#define GT_SCHED_CRED 1

#define GT_DISABLED 0
#define GT_ENABLED 1

extern int gt_scheduler;		//Determines which scheduler to use; set only once in main() of gt_matrix.c
extern int gt_load_balance;		//Determines if load balancing is enabled; set only once in main() of gt_matrix.c
extern int gt_yielding;		//Determines if yielding is enabled; set only once in main() of gt_matrix.c
extern gt_spinlock_t gt_io_lock;

TAILQ_HEAD(uthread_head, uthread_struct);
typedef struct uthread_head uthread_head_t;

typedef struct prio_struct
{
	unsigned int group_mask; /* mask(i) : groups with atleast one thread */
	unsigned int reserved[3];

	uthread_head_t group[MAX_UTHREAD_GROUPS]; /* array(i) : uthreads from uthread_group 'i' */
} prio_struct_t;


typedef struct __runqueue
{
	unsigned int uthread_mask; /* mask : prio levels with atleast one uthread */
	unsigned int uthread_tot; /* cnt : Tot num of uthreads in the runq (all priorities) */

	unsigned int uthread_prio_tot[MAX_UTHREAD_PRIORITY]; /* array(i) : Tot num of uthreads at priority 'i' */

	unsigned int uthread_group_mask[MAX_UTHREAD_GROUPS]; /* array(i) : prio levels with atleast one uthread from uthread_group 'i' */
	unsigned int uthread_group_tot[MAX_UTHREAD_GROUPS]; /* array(i) : Tot num of uthreads in uthread_group 'i' */

	prio_struct_t prio_array[MAX_UTHREAD_PRIORITY];
} runqueue_t;

/* XXX: Move it to gt_sched.h later */


/* NOTE: kthread active/expires/zombie use the same link(uthread_runq) in uthread_struct.
 * This is perfectly fine since active/expires/zombie are mutually exclusive. */

/* NOTE: Each kthread simulates a virtual processor.
 * Since we are running only one uthread on a kthread
 * at any given time, we (typically) do not need a runqlock 
 * when executing uthreads. But, runqlock is necessary to allow 
 * a uthread_create from any kthread to post a uthread to 
 * any kthread's runq.
 * There is not much performance penalty (only one additional check), 
 * since we are using spinlocks. */

/* NOTE: We can have locks at finer granularity than kthread_runqlock.
 * For instance, a lock for each queue. But, for the current design,
 * it will be an overkill. So, we go with one *GIANT* lock for the
 * entire kthread runqueue */
typedef struct __kthread_runqueue
{
	runqueue_t *active_runq;
	runqueue_t *expires_runq;
	gt_spinlock_t kthread_runqlock;

	uthread_struct_t *cur_uthread;	/* current running uthread (not in active/expires) */
	unsigned int reserved0;
	uthread_head_t zombie_uthreads;

	/*credit_runq and credit_runq_tot are only for use in the credit scheduler mode*/
	uthread_head_t credit_runq;	//Flat list of uthreads for credit scheduler mode
	unsigned int credit_runq_tot;	//Count of uthreads in credit_runq for O(1) lookup

	runqueue_t runqueues[2];
} kthread_runqueue_t;

/* only lock protected versions are exported */
extern void init_runqueue(runqueue_t *runq);
extern void add_to_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem);
extern void rem_from_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem);
extern void switch_runqueue(runqueue_t *from_runq, gt_spinlock_t *from_runqlock, 
		runqueue_t *to_runq, gt_spinlock_t *to_runqlock, uthread_struct_t *u_elem);

/* Insert into the current active runq, resolving the pointer under the lock */
extern void add_to_active_runqueue(kthread_runqueue_t *kthread_runq, uthread_struct_t *u_elem);

/* Insert into the credit runqueue, resolving the pointer under the lock*/
extern void add_to_credit_runqueue(kthread_runqueue_t *kthread_runq, uthread_struct_t *u_elem);

/* kthread runqueue */
extern void kthread_init_runqueue(kthread_runqueue_t *kthread_runq);

/* Find the highest priority uthread (O(1) scheduler)
 * Called by kthread handling VTALRM. */
extern uthread_struct_t *sched_find_best_uthread(kthread_runqueue_t *kthread_runq);

/* Find the uthread with highest remaining credit (credit scheduler). */
extern uthread_struct_t *sched_find_best_credit_uthread(kthread_runqueue_t *kthread_runq);

/*Wrapper that returns sched_find_best_uthread (for O(1) scheduler) or sched_find_best_credit_uthread (for credit scheduler)*/
extern uthread_struct_t *(*gt_get_active_picker(void))(kthread_runqueue_t *);

extern uthread_struct_t *try_load_balance_credit(kthread_runqueue_t *my_runq); 

extern void gt_print_credit_queue(kthread_runqueue_t *, const char *);

extern void atomic_fprintf(FILE *stream, const char *fmt, ...);
/* Example - picking the highest-priority uthread of one group via its mask
 * (do it under kthread_runqlock, like sched_find_best_uthread does):
 *
 *	unsigned int mask = runq->uthread_group_mask[u_gid];
 *	if(mask)
 *	{
 *		unsigned int uprio = LOWEST_BIT_SET(mask);
 *		uthread_struct_t *u_obj =
 *			TAILQ_FIRST(&(runq->prio_array[uprio].group[u_gid]));
 *		... remove u_obj from runq and dispatch it ...
 *	}
 */


#endif
