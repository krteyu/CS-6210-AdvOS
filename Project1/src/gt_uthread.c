#define _GNU_SOURCE
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

#include "gt_include.h"
/**********************************************************************/
/** DECLARATIONS **/
/**********************************************************************/


/**********************************************************************/
/* uthread scheduling */
static void uthread_context_func(int);
static int uthread_init(uthread_struct_t *u_new);

/**********************************************************************/
/* uthread creation */
#define UTHREAD_DEFAULT_SSIZE (128 * 1024)

extern int uthread_create(uthread_t *u_tid, int (*u_func)(void *), void *u_arg, uthread_group_t u_gid, int u_credit, int u_size);
/**********************************************************************/
/** HELPERS **/
/**********************************************************************/
static inline long gt_elapsed_usec(struct timeval *start, struct timeval *end)
{
    long t_sec = end->tv_sec - start->tv_sec;
    long t_usec = end->tv_usec - start->tv_usec;

    if(t_usec < 0)
    {
        t_sec--;
        t_usec += 1000000;
    }

    return (t_sec * 1000000L) + t_usec;
}

static inline int gt_credit_cost(long elapsed_usec)
{
    int cost = (int)(elapsed_usec / USECS_PER_QUANTA);
    if(elapsed_usec > 0 && cost < 1)
        cost = 1;   /* Every CPU access should cost at least 1 credit */
    return cost;
}
/**********************************************************************/
/** DEFNITIONS **/
/**********************************************************************/

/**********************************************************************/
/* uthread scheduling */

extern void gt_yield(void)
{
    if(gt_yielding != GT_ENABLED)
        return;   /* -y not passed: no-op, thread keeps running uninterrupted */

    kthread_block_signal(SIGVTALRM);
    kthread_block_signal(SIGUSR1);

    uthread_schedule(gt_get_active_picker(), 1);

    return;
}
/* Assumes that the caller has disabled vtalrm and sigusr1 signals */
/* uthread_init will be using */
static int uthread_init(uthread_struct_t *u_new)
{
	stack_t oldstack;
	sigset_t set, oldset;
	struct sigaction act, oldact;

	gt_spin_lock(&(ksched_shared_info.uthread_init_lock));

	/* Register a signal(SIGUSR2) for alternate stack */
	act.sa_handler = uthread_context_func;
	act.sa_flags = (SA_ONSTACK | SA_RESTART);
	sigemptyset(&act.sa_mask); /* was uninitialized stack garbage */
	if(sigaction(SIGUSR2,&act,&oldact))
	{
		gt_debug(stderr, "uthread sigusr2 install failed !!");
		gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
		return -1;
	}

	/* Install alternate signal stack (for SIGUSR2) */
	if(sigaltstack(&(u_new->uthread_stack), &oldstack))
	{
		gt_debug(stderr, "uthread sigaltstack install failed.");
		gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
		return -1;
	}

	/* Unblock the signal(SIGUSR2) */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_UNBLOCK, &set, &oldset);


	/* SIGUSR2 handler expects kthread_runq->cur_uthread
	 * to point to the newly created thread. We will temporarily
	 * change cur_uthread, before entering the synchronous call
	 * to SIGUSR2. */

	/* kthread_runq is made to point to this new thread
	 * in the caller. Raise the signal(SIGUSR2) synchronously */
	syscall(__NR_tkill, kthread_cpu_map[kthread_apic_id()]->tid, SIGUSR2);

	/* Block the signal(SIGUSR2) */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	if(sigaction(SIGUSR2,&oldact,NULL))
	{
		gt_debug(stderr, "uthread sigusr2 revert failed !!");
		gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
		return -1;
	}

	/* Disable the stack for signal(SIGUSR2) handling */
	u_new->uthread_stack.ss_flags = SS_DISABLE;

	/* Restore the old stack/signal handling */
	if(sigaltstack(&oldstack, NULL))
	{
		gt_debug(stderr, "uthread sigaltstack revert failed.");
		gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
		return -1;
	}

	gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
	return 0;
}

extern void uthread_schedule(uthread_struct_t * (*kthread_best_sched_uthread)(kthread_runqueue_t *), int yield)
{
	kthread_context_t *k_ctx;
	kthread_runqueue_t *kthread_runq;
	uthread_struct_t *u_obj;

	/* Scheduler is not reentrant : a signal here would deadlock on the runq
	 * spinlock or corrupt saved contexts. Unblocked again at each resume point. */
	kthread_block_signal(SIGVTALRM);
	kthread_block_signal(SIGUSR1);

	k_ctx = kthread_cpu_map[kthread_apic_id()];
	kthread_runq = &(k_ctx->krunqueue);

	if((u_obj = kthread_runq->cur_uthread))
	{
		/*Go through the runq and schedule the next thread to run */
		kthread_runq->cur_uthread = NULL;

		if(u_obj->uthread_state & (UTHREAD_DONE | UTHREAD_CANCELLED))
		{
			
			struct timeval now;
			gettimeofday(&now, NULL);
			long final_quantum = gt_elapsed_usec(&(u_obj->uthread_last_start), &now);
			u_obj->total_exec_time += final_quantum;
			if(gt_scheduler == GT_SCHED_CRED){
				int credit_cost = gt_credit_cost(final_quantum);
				u_obj->uthread_credit -= credit_cost;
				//gt_print_credit_queue(kthread_runq, "periodic");
				gt_debug(stderr, "\ntid=%d credit now=%d total_exec_time = %d usecs total_wait_time = %d usecs(final quantum charged, thread done)",
						u_obj->uthread_tid, u_obj->uthread_credit, u_obj->total_exec_time, u_obj->total_wait_time);
				gt_debug_logs(u_obj->flog, "\ntid=%d credit now=%d total_exec_time = %d usecs total_wait_time = %d usecs(final quantum charged, thread done)",
						u_obj->uthread_tid, u_obj->uthread_credit, u_obj->total_exec_time, u_obj->total_wait_time);
				if(u_obj->flog) fclose(u_obj->flog);
			}
			else{
				gt_debug(stderr, "\ntid=%d total_exec_time = %d usecs total_wait_time = %d usecs(thread done)",
						u_obj->uthread_tid, u_obj->total_exec_time, u_obj->total_wait_time);
				gt_debug_logs(u_obj->flog, "\ntid=%d total_exec_time = %d usecs total_wait_time = %d usecs(thread done)",
						u_obj->uthread_tid, u_obj->total_exec_time, u_obj->total_wait_time);
				if(u_obj->flog) fclose(u_obj->flog);
			}
			
			/* XXX: Inserting uthread into zombie queue is causing improper
			 * cleanup/exit of uthread (core dump) */
			uthread_head_t * kthread_zhead = &(kthread_runq->zombie_uthreads);
			gt_spin_lock(&(kthread_runq->kthread_runqlock));
			kthread_runq->kthread_runqlock.holder = 0x01;
			TAILQ_INSERT_TAIL(kthread_zhead, u_obj, uthread_runq);
			gt_spin_unlock(&(kthread_runq->kthread_runqlock));

			{
				ksched_shared_info_t *ksched_info = &ksched_shared_info;	
				gt_spin_lock(&ksched_info->ksched_lock);
				ksched_info->kthread_cur_uthreads--;
				gt_spin_unlock(&ksched_info->ksched_lock);
			}
		}
		else
		{	
			int before = u_obj->uthread_credit;
			struct timeval now;
			gettimeofday(&now, NULL);
			unsigned int time_run = gt_elapsed_usec(&(u_obj->uthread_last_start), &now);
			u_obj->total_exec_time += time_run;

			if(gt_scheduler == GT_SCHED_CRED){
				int credit_cost = gt_credit_cost(time_run);
				u_obj->uthread_credit -= credit_cost;
				if(yield){
					gt_spin_lock(&gt_io_lock);
					printf("GT_YIELD|k=%u|u=%u|g=%u|before=%d|used=%u|charged=%d|after=%d\n",
                			k_ctx->cpuid, u_obj->uthread_tid, u_obj->uthread_gid,
                   			before, time_run, credit_cost, u_obj->uthread_credit);
					gt_spin_unlock(&gt_io_lock);
					gt_debug(stderr, "\ntid=%d credit now=%d (deducted based on last_start after yielding)", u_obj->uthread_tid, u_obj->uthread_credit);
					gt_debug_logs(u_obj->flog, "\ntid=%d credit now=%d (deducted based on last_start after yielding)", u_obj->uthread_tid, u_obj->uthread_credit);
				}
				else{
					//gt_print_credit_queue(kthread_runq, "periodic");
					gt_debug(stderr, "\ntid=%d credit now=%d (deducted based on last_start)", u_obj->uthread_tid, u_obj->uthread_credit);
					gt_debug_logs(u_obj->flog, "\ntid=%d credit now=%d (deducted based on last_start)", u_obj->uthread_tid, u_obj->uthread_credit);
				}
			}
			else{
				if(yield){
					gt_debug(stderr, "\ntid=%d yielding", u_obj->uthread_tid);
					gt_debug_logs(u_obj->flog, "\ntid=%d yielding", u_obj->uthread_tid);
				}
				else{
					gt_debug(stderr, "\ntid=%d preempted", u_obj->uthread_tid);
					gt_debug_logs(u_obj->flog, "\ntid=%d preempted", u_obj->uthread_tid);
				}
			}
			u_obj->uthread_state = UTHREAD_RUNNABLE;
			/* Save the context BEFORE it becomes visible in the runqueue,
			 * else it can be picked and resumed with a stale env. */
			if(sigsetjmp(u_obj->uthread_env, 0))
			{
				/* Resumed : re-enable scheduling signals.*/
				kthread_unblock_signal(SIGVTALRM);
				kthread_unblock_signal(SIGUSR1);
				return;
			}
			gettimeofday(&(u_obj->uthread_last_stop), NULL);
			if(gt_scheduler == GT_SCHED_CRED){
				add_to_credit_runqueue(kthread_runq, u_obj);
				if(yield)
					gt_print_credit_queue(kthread_runq, "yield");
			}
			else
				add_to_runqueue(kthread_runq->expires_runq, &(kthread_runq->kthread_runqlock), u_obj);
		}
	}

	/* kthread_best_sched_uthread acquires kthread_runqlock. Dont lock it up when calling the function. */
	if(!(u_obj = kthread_best_sched_uthread(kthread_runq)))
	{
		/* Done executing all uthreads. Return to main */
		/* XXX: We can actually get rid of KTHREAD_DONE flag */
		if(ksched_shared_info.kthread_tot_uthreads && !ksched_shared_info.kthread_cur_uthreads)
		{
			gt_debug(stderr, "\nQuitting kthread (%d)\n", k_ctx->cpuid);
			k_ctx->kthread_flags |= KTHREAD_DONE;
		}

		siglongjmp(k_ctx->kthread_env, 1);
		return;
	}

	kthread_runq->cur_uthread = u_obj;

	if((u_obj->uthread_state == UTHREAD_INIT) && (uthread_init(u_obj)))
	{
		gt_debug(stderr, "\nuthread_init failed on kthread(%d)\n", k_ctx->cpuid);
		exit(0);
	}

	u_obj->uthread_state = UTHREAD_RUNNING;
	gettimeofday(&(u_obj->uthread_last_start), NULL);

	long wait_time = gt_elapsed_usec(&(u_obj->uthread_last_stop), &(u_obj->uthread_last_start));
	u_obj->total_wait_time += wait_time;
	gt_debug(stderr, "\ntid=%d dispatched, wait_time = %ld us", u_obj->uthread_tid, wait_time);
	gt_debug_logs(u_obj->flog, "\ntid=%d dispatched, wait_time = %ld us", u_obj->uthread_tid, wait_time);

	/* Handlers are process-wide (installed once in gtthread_app_init). Signals
	 * stay blocked across the jump; the resume point unblocks them. */
	siglongjmp(u_obj->uthread_env, 1);

	return;
}


/* For uthreads, we obtain a seperate stack by registering an alternate
 * stack for SIGUSR2 signal. Once the context is saved, we turn this 
 * into a regular stack for uthread (by using SS_DISABLE). */
static void uthread_context_func(int signo)
{
	uthread_struct_t *cur_uthread;
	kthread_runqueue_t *kthread_runq;

	kthread_runq = &(kthread_cpu_map[kthread_apic_id()]->krunqueue);

	//printf("..... uthread_context_func .....\n");
	/* kthread->cur_uthread points to newly created uthread */
	if(!sigsetjmp(kthread_runq->cur_uthread->uthread_env,0))
	{
		/* In UTHREAD_INIT : saves the context and returns.
		 * Otherwise, continues execution. */
		/* DONT USE any locks here !! */
		assert(kthread_runq->cur_uthread->uthread_state == UTHREAD_INIT);
		kthread_runq->cur_uthread->uthread_state = UTHREAD_RUNNABLE;
		return;
	}

	/* UTHREAD_RUNNING : siglongjmp was executed. */
	cur_uthread = kthread_runq->cur_uthread;
	assert(cur_uthread->uthread_state == UTHREAD_RUNNING);
	/* The scheduler jumped here with scheduling signals blocked : re-enable. */
	kthread_unblock_signal(SIGVTALRM);
	kthread_unblock_signal(SIGUSR1);
	/* Execute the uthread task */
	cur_uthread->uthread_func(cur_uthread->uthread_arg);
	cur_uthread->uthread_state = UTHREAD_DONE;

	uthread_schedule(gt_get_active_picker(), 0);
	return;
}

/**********************************************************************/
/* uthread creation */

extern kthread_runqueue_t *ksched_find_target(uthread_struct_t *);

extern int uthread_create(uthread_t *u_tid, int (*u_func)(void *), void *u_arg, uthread_group_t u_gid, int u_credit, int u_size)
{
	kthread_runqueue_t *kthread_runq;
	uthread_struct_t *u_new;
	sigset_t set, oldset;

	/* Locks taken below must not be interrupted by the scheduler.
	 * Save/restore (not block/unblock) : main keeps them blocked till app_exit. */
	sigemptyset(&set);
	sigaddset(&set, SIGVTALRM);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, &oldset);

	/* create a new uthread structure and fill it */
	if(!(u_new = (uthread_struct_t *)MALLOCZ_SAFE(sizeof(uthread_struct_t))))
	{
		gt_debug(stderr, "uthread mem alloc failure !!");
		exit(0);
	}

	u_new->uthread_state = UTHREAD_INIT;
	u_new->uthread_priority = DEFAULT_UTHREAD_PRIORITY;
	u_new->uthread_gid = u_gid;
	u_new->uthread_func = u_func;
	u_new->uthread_arg = u_arg;
	u_new->uthread_credit = u_credit;
	u_new->uthread_credit_group = u_credit;
	u_new->uthread_workload_size = u_size;
	u_new->total_exec_time = 0;
	u_new->total_wait_time = 0;

	/* Allocate new stack for uthread */
	u_new->uthread_stack.ss_flags = 0; /* Stack enabled for signal handling */
	if(!(u_new->uthread_stack.ss_sp = (void *)MALLOC_SAFE(UTHREAD_DEFAULT_SSIZE)))
	{
		gt_debug(stderr, "uthread stack mem alloc failure !!");
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		return -1;
	}
	u_new->uthread_stack.ss_size = UTHREAD_DEFAULT_SSIZE;


	{
		ksched_shared_info_t *ksched_info = &ksched_shared_info;

		gt_spin_lock(&ksched_info->ksched_lock);
		u_new->uthread_tid = ksched_info->kthread_tot_uthreads++;
		ksched_info->kthread_cur_uthreads++;
		gt_spin_unlock(&ksched_info->ksched_lock);
	}
	/* XXX: ksched_find_target should be a function pointer */
	kthread_runq = ksched_find_target(u_new);

	*u_tid = u_new->uthread_tid;

	char *logname;
	if(0>asprintf(&logname, "./logs/m%dc%dtid%u.txt", u_size, u_credit, *u_tid)) return -1;
	u_new->flog = fopen(logname,"w");
	if(!u_new->flog)
	{
		gt_debug(stderr, "Failed to open log file %s\n", logname);
	}
	free(logname);
	if(gt_scheduler==GT_SCHED_CRED)
		gt_debug_logs(u_new->flog, "tid=%d credit_group=%d size=%d", u_new->uthread_tid, u_new->uthread_credit_group, u_new->uthread_workload_size);
	else
		gt_debug_logs(u_new->flog, "tid=%d size=%d", u_new->uthread_tid, u_new->uthread_workload_size);
	gettimeofday(&(u_new->uthread_last_stop),NULL);

	if(gt_scheduler == GT_SCHED_CRED){
		add_to_credit_runqueue(kthread_runq, u_new);
	}
	else{
		/* Queue the uthread for target-cpu. Let target-cpu take care of initialization.
		 * The active_runq pointer must be resolved under the lock (owner swaps it). */
		add_to_active_runqueue(kthread_runq, u_new);
	}
	/* WARNING : DONOT USE u_new WITHOUT A LOCK, ONCE IT IS ENQUEUED. */

	/* Restore the caller's signal mask */
	sigprocmask(SIG_SETMASK, &oldset, NULL);

	return 0;
}
