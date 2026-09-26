#include <stdio.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

#include "gt_include.h"

/**********************************************************************/
/* Default Scheduler is O(1), shouldn't matter as arg parser will throw an error without a selected scheduler*/
int gt_scheduler = GT_SCHED_O1;
int gt_yielding = GT_DISABLED;
int gt_load_balance = GT_DISABLED;
gt_spinlock_t gt_io_lock;

/**********************************************************************/
/* runqueue operations */
static void __add_to_runqueue(runqueue_t *runq, uthread_struct_t *u_elm);
static void __rem_from_runqueue(runqueue_t *runq, uthread_struct_t *u_elm);


/**********************************************************************/
/* runqueue operations */
static inline void __add_to_runqueue(runqueue_t *runq, uthread_struct_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->uthread_priority;
	ugroup = u_elem->uthread_gid;

	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_INSERT_TAIL(uhead, u_elem, uthread_runq);

	/* Update information */
	if(!IS_BIT_SET(runq->prio_array[uprio].group_mask, ugroup))
		SET_BIT(runq->prio_array[uprio].group_mask, ugroup);

	runq->uthread_tot++;

	runq->uthread_prio_tot[uprio]++;
	if(!IS_BIT_SET(runq->uthread_mask, uprio))
		SET_BIT(runq->uthread_mask, uprio);

	runq->uthread_group_tot[ugroup]++;
	if(!IS_BIT_SET(runq->uthread_group_mask[ugroup], uprio))
		SET_BIT(runq->uthread_group_mask[ugroup], uprio);

	return;
}

static inline void __rem_from_runqueue(runqueue_t *runq, uthread_struct_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->uthread_priority;
	ugroup = u_elem->uthread_gid;

	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_REMOVE(uhead, u_elem, uthread_runq);

	/* Update information */
	if(TAILQ_EMPTY(uhead))
	{
		RESET_BIT(runq->prio_array[uprio].group_mask, ugroup);
		/* The (group, prio) bucket is empty : clear its bit now.
		 * Clearing only when the whole group empties leaves stale bits. */
		RESET_BIT(runq->uthread_group_mask[ugroup], uprio);
	}

	runq->uthread_tot--;

	if(!(--(runq->uthread_prio_tot[uprio])))
		RESET_BIT(runq->uthread_mask, uprio);

	if(!(--(runq->uthread_group_tot[ugroup])))
		assert(TAILQ_EMPTY(uhead));

	return;
}


/**********************************************************************/
/* Exported runqueue operations */
extern void init_runqueue(runqueue_t *runq)
{
	uthread_head_t *uhead;
	int i, j;
	/* Everything else is global, so already initialized to 0(correct init value) */
	for(i=0; i<MAX_UTHREAD_PRIORITY; i++)
	{
		for(j=0; j<MAX_UTHREAD_GROUPS; j++)
		{
			uhead = &((runq)->prio_array[i].group[j]);
			TAILQ_INIT(uhead);
		}
	}
	return;
}

extern void add_to_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem)
{
	gt_spin_lock(runq_lock);
	runq_lock->holder = 0x02;
	__add_to_runqueue(runq, u_elem);
	gt_spin_unlock(runq_lock);
	return;
}

extern void rem_from_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem)
{
	gt_spin_lock(runq_lock);
	runq_lock->holder = 0x03;
	__rem_from_runqueue(runq, u_elem);
	gt_spin_unlock(runq_lock);
	return;
}

extern void switch_runqueue(runqueue_t *from_runq, gt_spinlock_t *from_runqlock,
		runqueue_t *to_runq, gt_spinlock_t *to_runqlock, uthread_struct_t *u_elem)
{
	rem_from_runqueue(from_runq, from_runqlock, u_elem);
	add_to_runqueue(to_runq, to_runqlock, u_elem);
	return;
}

/* active_runq/expires_runq are swapped by the owner kthread (under the lock),
 * so the active pointer must also be read under the lock. */
extern void add_to_active_runqueue(kthread_runqueue_t *kthread_runq, uthread_struct_t *u_elem)
{
	gt_spin_lock(&(kthread_runq->kthread_runqlock));
	kthread_runq->kthread_runqlock.holder = 0x02;
	__add_to_runqueue(kthread_runq->active_runq, u_elem);
	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
	return;
}

extern void add_to_credit_runqueue(kthread_runqueue_t *kthread_runq, uthread_struct_t *u_elem){
	gt_spin_lock(&(kthread_runq->kthread_runqlock));
	kthread_runq->kthread_runqlock.holder = 0x05;
	TAILQ_INSERT_TAIL(&(kthread_runq->credit_runq), u_elem, uthread_runq);
	kthread_runq->credit_runq_tot++;
	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
}

/**********************************************************************/

extern void kthread_init_runqueue(kthread_runqueue_t *kthread_runq)
{
	kthread_runq->active_runq = &(kthread_runq->runqueues[0]);
	kthread_runq->expires_runq = &(kthread_runq->runqueues[1]);

	TAILQ_INIT(&(kthread_runq->credit_runq));	//Initialize runqueue for credit scheduler mode
	kthread_runq->credit_runq_tot = 0;

	gt_spinlock_init(&(kthread_runq->kthread_runqlock));
	init_runqueue(kthread_runq->active_runq);
	init_runqueue(kthread_runq->expires_runq);

	TAILQ_INIT(&(kthread_runq->zombie_uthreads));
	return;
}

/* Debug helper (currently unused) : the load-balancing deliverable
 * requires printing queue status - hook it up from there. */
static void print_runq_stats(runqueue_t *runq, char *runq_str)
{
	int inx;
	gt_spin_lock(&gt_io_lock);
	printf("******************************************************\n");
	printf("Run queue(%s) state : \n", runq_str);
	printf("******************************************************\n");
	printf("uthreads details - (tot:%d , mask:%x)\n", runq->uthread_tot, runq->uthread_mask);
	printf("******************************************************\n");
	printf("uthread priority details : \n");
	for(inx=0; inx<MAX_UTHREAD_PRIORITY; inx++)
		printf("uthread priority (%d) - (tot:%d)\n", inx, runq->uthread_prio_tot[inx]);
	printf("******************************************************\n");
	printf("uthread group details : \n");
	for(inx=0; inx<MAX_UTHREAD_GROUPS; inx++)
		printf("uthread group (%d) - (tot:%d , mask:%x)\n", inx, runq->uthread_group_tot[inx], runq->uthread_group_mask[inx]);
	printf("******************************************************\n");
	gt_spin_unlock(&gt_io_lock);
	return;
}

extern void gt_print_credit_queue(kthread_runqueue_t *kthread_runq, const char *event)
{
    uthread_struct_t *u_obj;
    char buf[4096] = "";
	unsigned int cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;
    int pos = 0, first = 1;

    TAILQ_FOREACH(u_obj, &(kthread_runq->credit_runq), uthread_runq)
    {
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%su%u:g%u:%d/%d",
                         first ? "" : ",", u_obj->uthread_tid, u_obj->uthread_gid,
                         u_obj->uthread_credit, u_obj->uthread_credit_group);
        first = 0;
    }
	gt_spin_lock(&gt_io_lock);
    printf("GT_CQ|k=%u|event=%s|active=%s|expired=-\n", cpuid, event, first ? "-" : buf);
	gt_spin_unlock(&gt_io_lock);
}

/*Helper function to guard all I/O operations*/
extern void atomic_fprintf(FILE *stream, const char *fmt, ...)
{
    if(!stream) return;
    va_list args;
    gt_spin_lock(&gt_io_lock);
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);
    gt_spin_unlock(&gt_io_lock);
}

extern uthread_struct_t *(*gt_get_active_picker(void))(kthread_runqueue_t *){
	return (gt_scheduler== GT_SCHED_O1 ? &sched_find_best_uthread : &sched_find_best_credit_uthread);
}

extern uthread_struct_t *sched_find_best_uthread(kthread_runqueue_t *kthread_runq)
{
	/* [1] Tries to find the highest priority RUNNABLE uthread in active-runq.
	 * [2] Found - Jump to [FOUND]
	 * [3] Switches runqueues (active/expires)
	 * [4] Repeat [1] through [2]
	 * [NOT FOUND] Return NULL(no more jobs)
	 * [FOUND] Remove uthread from pq and return it. */

	runqueue_t *runq;
	prio_struct_t *prioq;
	uthread_head_t *u_head;
	uthread_struct_t *u_obj;
	unsigned int uprio, ugroup;

	gt_spin_lock(&(kthread_runq->kthread_runqlock));

	runq = kthread_runq->active_runq;

	kthread_runq->kthread_runqlock.holder = 0x04;
	if(!(runq->uthread_mask))
	{ /* No jobs in active. switch runqueue */
		assert(!runq->uthread_tot);
		kthread_runq->active_runq = kthread_runq->expires_runq;
		kthread_runq->expires_runq = runq;

		/* pick from the swapped-in queue (was re-reading the emptied one) */
		runq = kthread_runq->active_runq;
		if(!runq->uthread_mask)
		{
			assert(!runq->uthread_tot);
			gt_spin_unlock(&(kthread_runq->kthread_runqlock));
			return NULL;
		}
	}

	/* Find the highest priority bucket */
	uprio = LOWEST_BIT_SET(runq->uthread_mask);
	prioq = &(runq->prio_array[uprio]);

	assert(prioq->group_mask);
	ugroup = LOWEST_BIT_SET(prioq->group_mask);

	u_head = &(prioq->group[ugroup]);
	u_obj = TAILQ_FIRST(u_head);
	__rem_from_runqueue(runq, u_obj);

	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
	return(u_obj);
}

extern uthread_struct_t *sched_find_best_credit_uthread(kthread_runqueue_t *kthread_runq){
	/* [1] Tries to find the highest credit (>0 credits) RUNNABLE uthread in credit-runq.
	 * [2] Found - Jump to [FOUND]
	 * [3] All credits <=0? Replenish all credits
	 * [4] Repeat [1] through [2]
	 * [NOT FOUND] Return NULL(no more jobs)
	 * [FOUND] Remove uthread from queue and return it. */

	uthread_struct_t *u_obj, *best_obj;

	gt_spin_lock(&(kthread_runq->kthread_runqlock));
	kthread_runq->kthread_runqlock.holder = 0x05;
	if(!(kthread_runq->credit_runq_tot)){
		/*NO MORE JOBS*/
		gt_spin_unlock(&(kthread_runq->kthread_runqlock));
		if(gt_load_balance==GT_ENABLED){
			uthread_struct_t *migrant = try_load_balance_credit(kthread_runq);	//Try and get a migrated job over from the other core
			if(migrant)
				return migrant;
		}
		return NULL;
	}

	best_obj=NULL;
	TAILQ_FOREACH(u_obj, &(kthread_runq->credit_runq), uthread_runq){
		if(u_obj->uthread_credit <=0){
			/*IGNORE THREADS WITH NEGATIVE CREDITS*/
			continue;
		}
		else if(!best_obj || u_obj->uthread_credit > best_obj->uthread_credit){
			/*SELECT STRICTLY GREATEST CREDIT UTHREAD*/
			best_obj = u_obj;
		}
	}

	if(!best_obj){
		/*REFILL CREDITS FOR ALL THREADS*/
		gt_debug(stderr, "Replenishing credits\n");
		TAILQ_FOREACH(u_obj, &(kthread_runq->credit_runq), uthread_runq){
			u_obj->uthread_credit = u_obj->uthread_credit_group;
			gt_debug(stderr, "tid=%d credit now=%d (credits replenished)\n",
						u_obj->uthread_tid, u_obj->uthread_credit);
			gt_debug_logs(u_obj->flog, "\ntid=%d credit now=%d (credits replenished)",
						u_obj->uthread_tid, u_obj->uthread_credit);
			if(!best_obj || u_obj->uthread_credit > best_obj->uthread_credit){
				/*SELECT STRICTLY GREATEST CREDIT UTHREAD*/
				best_obj = u_obj;
			}
		}
		gt_print_credit_queue(kthread_runq, "refill");
	}
	
	TAILQ_REMOVE(&(kthread_runq->credit_runq), best_obj, uthread_runq);
	kthread_runq->credit_runq_tot--;

	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
	return (best_obj);
}

extern uthread_struct_t *try_load_balance_credit(kthread_runqueue_t *my_runq){
	/*Iterate through all the other kthread runqueues and pick the last job in a busy queue(This can be changed later)*/	
	kthread_context_t *victim_ctx;
	kthread_runqueue_t *victim_runq;
	uthread_struct_t *migrant;
	for(int inx = 0; inx < GT_MAX_KTHREADS; inx++){
		victim_ctx = kthread_cpu_map[inx];
		if(!victim_ctx)
			continue;

		victim_runq = &(victim_ctx->krunqueue);

		if(victim_runq == my_runq)
			continue;	//Cannot steal from itself
		
		gt_spin_lock(&(victim_runq->kthread_runqlock));
		if(victim_runq->credit_runq_tot <= 1){
			gt_spin_unlock(&(victim_runq->kthread_runqlock));
			continue;	//Dont steal if it will empty another runqueue
		}

		/*Policy is to pick the oldest uthread in a valid busy queue*/
		migrant = TAILQ_LAST(&(victim_runq->credit_runq), uthread_head);
		TAILQ_REMOVE(&(victim_runq->credit_runq), migrant, uthread_runq);
		victim_runq->credit_runq_tot--;

		gt_spin_unlock(&(victim_runq->kthread_runqlock));

		gt_debug(stderr, "\nLOAD BALANCE: migrating tid=%u from cpu=%u to cpu=%u\n",
                migrant->uthread_tid, victim_ctx->cpuid,
                kthread_cpu_map[kthread_apic_id()]->cpuid);
		gt_debug_logs(migrant->flog, "\nLOAD BALANCE: migrating tid=%u from cpu=%u to cpu=%u\n",
                migrant->uthread_tid, victim_ctx->cpuid,
                kthread_cpu_map[kthread_apic_id()]->cpuid);

		unsigned int my_cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;

		gt_spin_lock(&gt_io_lock);
		printf("GT_LB|k=%u|from=%u|n=1|migrated=u%u:g%u\n",
			my_cpuid, victim_ctx->cpuid, migrant->uthread_tid, migrant->uthread_gid);
		gt_spin_unlock(&gt_io_lock);
		
		/*Add to current runqueue*/
		add_to_credit_runqueue(my_runq, migrant);
		gt_print_credit_queue(my_runq, "lb");
		return sched_find_best_credit_uthread(my_runq); 
	}
	return NULL;
}