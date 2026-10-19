#ifndef __GT_UTHREAD_H
#define __GT_UTHREAD_H

/* User-level thread implementation (using alternate signal stacks) */

typedef unsigned int uthread_t;
typedef unsigned int uthread_group_t;

/* uthread states */
#define UTHREAD_INIT 0x01
#define UTHREAD_RUNNABLE 0x02
#define UTHREAD_RUNNING 0x04
#define UTHREAD_CANCELLED 0x08
#define UTHREAD_DONE 0x10

/* uthread struct : has all the uthread context info */
typedef struct uthread_struct
{

	int uthread_state; /* UTHREAD_INIT, UTHREAD_RUNNABLE, UTHREAD_RUNNING, UTHREAD_CANCELLED, UTHREAD_DONE */
	int uthread_priority; /* uthread running priority */
	int cpu_id; /* cpu it is currently executing on */
	int last_cpu_id; /* last cpu it was executing on */

	uthread_t uthread_tid; /* thread id */
	uthread_group_t uthread_gid; /* thread group id  */
	int (*uthread_func)(void*);
	void *uthread_arg;

	int uthread_credit;		/*Available credit balance*/
	int uthread_credit_group;	/*Initial credit value {25, 50, 75, 100}; Will be used to refill uthread_credit*/
	struct timeval uthread_last_start;	/*Timestamp of last start on CPU to calculate elapsed CPU time for credit reduction*/
	struct timeval uthread_last_stop;	/*Timestamp of last preemption on CPU to calculate wait time*/
	unsigned int total_exec_time;
	unsigned int total_wait_time;


	int uthread_workload_size;	/*Used to store size of workload being done for logging purposes*/

	void *exit_status; /* exit status */
	int reserved1;
	int reserved2;
	int reserved3;

	sigjmp_buf uthread_env; /* 156 bytes : save user-level thread context*/
	stack_t uthread_stack; /* 12 bytes : user-level thread stack */
	TAILQ_ENTRY(uthread_struct) uthread_runq;
	FILE *flog;
} uthread_struct_t;

struct __kthread_runqueue;
extern void gt_yield(void);
extern void uthread_schedule(uthread_struct_t * (*kthread_best_sched_uthread)(struct __kthread_runqueue *), int yield);
#endif
