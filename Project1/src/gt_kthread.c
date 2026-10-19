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
#include <stdlib.h>

#include "gt_include.h"

#define KTHREAD_DEFAULT_SSIZE (256*1024)

/**********************************************************************/
/** DECLARATIONS **/
/**********************************************************************/

/**********************************************************************/
/* kthread context identification */
kthread_context_t *kthread_cpu_map[GT_MAX_KTHREADS];

/* kthread schedule information */
ksched_shared_info_t ksched_shared_info;
/**********************************************************************/
/* kthread */
extern int kthread_create(kthread_t *tid, int (*start_fun)(void *), void *arg);
static int kthread_handler(void *arg);
static void kthread_init(kthread_context_t *k_ctx);

/**********************************************************************/
/* kthread schedule */
static inline void ksched_info_init(ksched_shared_info_t *ksched_info);
static void ksched_priority(int );
static void ksched_cosched(int );
extern kthread_runqueue_t *ksched_find_target(uthread_struct_t *);

/**********************************************************************/
/* gtthread application (over kthreads and uthreads) */
static void gtthread_app_start(void *arg);

/**********************************************************************/
/* kthread creation */
int kthread_create(kthread_t *tid, int (*kthread_start_func)(void *), void *arg)
{
	int retval = 0;
	void **stack;
	int stacksize;

	stacksize = KTHREAD_DEFAULT_SSIZE;

	/* Create the new thread's stack */
	if(!(stack = (void **)MALLOC_SAFE(stacksize)))
	{
		perror("No memory !!");
		return -1;
	}
	stack = (void*)((char*)stack + stacksize);

	retval = clone(kthread_start_func, stack, CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | SIGCHLD, arg);

	if(retval > 0)
	{
		*tid = retval;
		return retval;
	}

	return -1;
}

static int kthread_handler(void *arg)
{
#define k_ctx ((kthread_context_t *)arg)
	kthread_init(k_ctx);
	k_ctx->kthread_app_func(NULL);
#undef k_ctx
	return 0;
}

static void kthread_init(kthread_context_t *k_ctx)
{
	cpu_set_t cpu_affinity_mask;

	/* initiate the mask, no CPU is selected*/
	CPU_ZERO(&cpu_affinity_mask);

	/* cpuid and kthread_app_func are set by the application 
	 * over kthread (eg. gtthread). */

	k_ctx->pid = syscall(SYS_getpid);
	k_ctx->tid = syscall(SYS_gettid);

	/* For priority co-scheduling */
	k_ctx->kthread_sched_timer = ksched_priority;
	k_ctx->kthread_sched_relay = ksched_cosched;

	/* XXX: kthread runqueue balancing (TBD) */
	k_ctx->kthread_runqueue_balance = NULL;

	/* Initialize kthread runqueue */

	kthread_init_runqueue(&(k_ctx->krunqueue));

	CPU_SET(k_ctx->cpuid, &cpu_affinity_mask);

	sched_setaffinity(k_ctx->tid, sizeof(cpu_set_t), &cpu_affinity_mask);

	sched_yield();

	/* Scheduled on target cpu */
	k_ctx->cpu_apic_id = kthread_apic_id();

	kthread_cpu_map[k_ctx->cpu_apic_id] = k_ctx;

	return;
}

/**********************************************************************/
/* kthread schedule */

/* Initialize the ksched_shared_info */
static inline void ksched_info_init(ksched_shared_info_t *ksched_info)
{
	gt_spinlock_init(&(ksched_info->ksched_lock));
	gt_spinlock_init(&(ksched_info->uthread_init_lock));
	gt_spinlock_init(&(ksched_info->__malloc_lock));
	gt_spinlock_init(&gt_io_lock);
	return;
}

extern kthread_runqueue_t *ksched_find_target(uthread_struct_t *u_obj)
{
	ksched_shared_info_t *ksched_info;
	unsigned int target_cpu, u_gid;

	ksched_info = &ksched_shared_info;
	u_gid = u_obj->uthread_gid;

	/* Read-modify-write of last_ugroup_kthread must be atomic wrt other creators */
	gt_spin_lock(&(ksched_info->ksched_lock));
	target_cpu = ksched_info->last_ugroup_kthread[u_gid];
	do
	{
		/* How dumb to assume there is atleast one cpu (haha) !! :-D */
		target_cpu = ((target_cpu + 1) % GT_MAX_CORES);
	} while(!kthread_cpu_map[target_cpu]);
	
	ksched_info->last_ugroup_kthread[u_gid] = target_cpu;
	gt_spin_unlock(&(ksched_info->ksched_lock));

	u_obj->cpu_id = kthread_cpu_map[target_cpu]->cpuid;
	u_obj->last_cpu_id = kthread_cpu_map[target_cpu]->cpuid;
	gt_debug(stderr, "PLACEMENT: tid:%d, target CPU is: %d\n", u_obj->uthread_tid, u_obj->cpu_id);

	return(&(kthread_cpu_map[target_cpu]->krunqueue));
}

static void ksched_cosched(int signal)
{
	/* This virtual processor (thread) was not
	 * picked by kernel for vtalrm signal.
	 * USR1 signal has been relayed to it. */

	uthread_schedule(gt_get_active_picker(), 0);
	return;
}

static void ksched_priority(int signo)
{
	kthread_context_t *cur_k_ctx, *tmp_k_ctx;
	int inx;

	cur_k_ctx = kthread_cpu_map[kthread_apic_id()];

	/* Relay the signal to all other virtual processors(kthreads) */
	for(inx=0; inx<GT_MAX_KTHREADS; inx++)
	{
		/* XXX: We can avoid the last check (tmp to cur) by
		 * temporarily marking cur as DONE. But chuck it !! */
		if((tmp_k_ctx = kthread_cpu_map[inx]) && (tmp_k_ctx != cur_k_ctx))
		{
			if(tmp_k_ctx->kthread_flags & KTHREAD_DONE)
				continue;
			/* tkill : send signal to specific threads */
			syscall(__NR_tkill, tmp_k_ctx->tid, SIGUSR1);
		}
	}

	uthread_schedule(gt_get_active_picker(), 0);
	return;
}

/**********************************************************************/

/* gtthread_app_start (kthread_app_func for gtthreads).
 * All application cleanup must be done at the end of this function. */
extern unsigned int gtthread_app_running;

static void gtthread_app_start(void *arg)
{
	kthread_context_t *k_ctx;

	k_ctx = kthread_cpu_map[kthread_apic_id()];
	assert((k_ctx->cpu_apic_id == kthread_apic_id()));

	while(!(k_ctx->kthread_flags & KTHREAD_DONE))
	{
		__asm__ __volatile__ ("pause\n");
		if(sigsetjmp(k_ctx->kthread_env, 0))
		{
			/* siglongjmp to this point is done when there
			 * are no more uthreads to schedule.*/
			/* XXX: gtthread app cleanup has to be done. */
			continue;
		}
		/* kthread_env is valid now : safe to take scheduling signals. */
		kthread_unblock_signal(SIGVTALRM);
		kthread_unblock_signal(SIGUSR1);
		uthread_schedule(gt_get_active_picker(), 0);
	}

	return;
}


extern void gtthread_app_init()
{
	kthread_context_t *k_ctx, *k_ctx_main;
	kthread_t k_tid;
	unsigned int num_cpus, inx;


	/* Initialize shared schedule information */
	ksched_info_init(&ksched_shared_info);

	/* kthread (virtual processor) on the first logical processor */
	k_ctx_main = (kthread_context_t *)MALLOCZ_SAFE(sizeof(kthread_context_t));
	k_ctx_main->cpuid = 0;
	k_ctx_main->kthread_app_func = &gtthread_app_start;
	kthread_init(k_ctx_main);

	/* Install handlers before arming the timer (default SIGVTALRM kills the process).
	 * Then keep scheduling signals blocked - kthreads (incl. main) unblock them only
	 * once their dispatch loop has a valid kthread_env; cloned kthreads inherit the mask. */
	kthread_install_sighandler(SIGVTALRM, k_ctx_main->kthread_sched_timer);
	kthread_install_sighandler(SIGUSR1, k_ctx_main->kthread_sched_relay);
	kthread_block_signal(SIGVTALRM);
	kthread_block_signal(SIGUSR1);
	kthread_init_vtalrm_timeslice();

	/* Num of logical processors (cpus/cores) */
	num_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
	if(num_cpus > GT_MAX_KTHREADS) /* kthread_cpu_map only has GT_MAX_KTHREADS slots */
		num_cpus = GT_MAX_KTHREADS;
	/* kthreads (virtual processors) on all other logical processors */
	for(inx=1; inx<num_cpus; inx++)
	{
		k_ctx = (kthread_context_t *)MALLOCZ_SAFE(sizeof(kthread_context_t));
		k_ctx->cpuid = inx;
		k_ctx->kthread_app_func = &gtthread_app_start;
		/* kthread_init called inside kthread_handler */
		if(kthread_create(&k_tid, kthread_handler, (void *)k_ctx) < 0)
		{
			gt_debug(stderr, "kthread creation failed (errno:%d)\n", errno );
			exit(0);
		}
		gt_spin_lock(&gt_io_lock);
		printf( "kthread(%d) created !!\n", inx);
		gt_spin_unlock(&gt_io_lock);
	}

	{
		/* yield till other kthreads initialize */
		int init_done;
yield_again:
		sched_yield();
		init_done = 0;
		for(inx=0; inx<GT_MAX_KTHREADS; inx++)
		{
			/* XXX: We can avoid the last check (tmp to cur) by
			 * temporarily marking cur as DONE. But chuck it !! */
			if(kthread_cpu_map[inx])
				init_done++;
		}
		assert(init_done <= num_cpus);
		if(init_done < num_cpus)
			goto yield_again;
	}

	return;
}

extern void gtthread_app_exit()
{
	/* gtthread_app_exit called by only main thread. */
	/* For main thread, trigger start again. */
	kthread_context_t *k_ctx;

	k_ctx = kthread_cpu_map[kthread_apic_id()];
	k_ctx->kthread_flags &= ~KTHREAD_DONE;

	while(!(k_ctx->kthread_flags & KTHREAD_DONE))
	{
		__asm__ __volatile__ ("pause\n");
		if(sigsetjmp(k_ctx->kthread_env, 0))
		{
			/* siglongjmp to this point is done when there
			 * are no more uthreads to schedule.*/
			/* XXX: gtthread app cleanup has to be done. */
			continue;
		}
		/* kthread_env is valid now : safe to take scheduling signals. */
		kthread_unblock_signal(SIGVTALRM);
		kthread_unblock_signal(SIGUSR1);
		uthread_schedule(gt_get_active_picker(), 0);
	}

	kthread_block_signal(SIGVTALRM);
	kthread_block_signal(SIGUSR1);

	/* volatile : other kthreads update the counter */
	while(*((volatile unsigned int *)&(ksched_shared_info.kthread_cur_uthreads)))
	{
		/* Main thread has to wait for other kthreads */
		__asm__ __volatile__ ("pause\n");
	}
	return;	
}

