#include <stdio.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "gt_include.h"

#define MAX_SIZE 256
#define ROWS MAX_SIZE
#define COLS ROWS

#define NUM_CPUS 2
#define NUM_GROUPS NUM_CPUS

#define NUM_THREADS 128
//#define SMOKE_TEST

#ifdef SMOKE_TEST
	#undef NUM_THREADS
	#define NUM_THREADS 8
#endif


/* A[SIZE][SIZE] X B[SIZE][SIZE] = C[SIZE][SIZE]
 * Let T(g, t) be thread 't' in group 'g'. 
 * T(g, t) is responsible for multiplication : 
 * A(rows)[(t-1)*SIZE -> (t*SIZE - 1)] X B(cols)[(g-1)*SIZE -> (g*SIZE - 1)] */

typedef struct matrix
{
	int m[MAX_SIZE][MAX_SIZE];

	int rows;
	int cols;
	unsigned int reserved[2];
} matrix_t;

int credit_groups[] = {25, 50, 75, 100};
int matrix_sizes[] = {32, 64, 128, 256};


typedef struct __uthread_arg
{
	matrix_t *_A, *_B, *_C;
	unsigned int reserved0;

	unsigned int tid;
	unsigned int gid;
	int credit;	/*Initial credits given to uthread {25,50,75,100}*/
	int size;	/*Size of matrix to be multiplied by the thread*/

}uthread_arg_t;

struct timeval tv1;

static void generate_matrix(matrix_t *mat, int size, int val)
{

	int i,j;
	mat->rows = size;
	mat->cols = size;
	for(i = 0; i < mat->rows;i++)
		for( j = 0; j < mat->cols; j++ )
		{
			mat->m[i][j] = val;
		}
	return;
}

extern int uthread_create(uthread_t *, int (*)(void *), void *, uthread_group_t, int, int);

extern void gt_yield(void);


static int uthread_mulmat(void *p)
{
	int i, j, k;
	int start_row, end_row;
	int start_col, end_col;
	unsigned int cpuid;
	struct timeval tv2;

#define ptr ((uthread_arg_t *)p)

	i=0; j= 0; k=0;

	start_row = 0;
	end_row = ptr->size;

	start_col = 0;
	end_col = ptr->size;

	for(i = start_row; i < end_row; i++)
		for(j = start_col; j < end_col; j++){
			for(k = 0; k < ptr->size; k++){
				/*Choosing an arbitrary point for some threads to yield*/
				if(ptr->tid%16 == 0 && ((i == 5 && j == 5 && k == 5) || (i == 45 && j == 45 && k == 45)))
					gt_yield();
				ptr->_C->m[i][j] += ptr->_A->m[i][k] * ptr->_B->m[k][j];
			}
		}
			

	gettimeofday(&tv2,NULL); /* tv2 was never set in the GT_THREADS branch */
	{
		/* Normalize : %lu on a negative usec diff printed garbage */
		long t_sec = tv2.tv_sec - tv1.tv_sec;
		long t_usec = tv2.tv_usec - tv1.tv_usec;
		if(t_usec < 0)
		{
			t_sec--;
			t_usec += 1000000;
		}
	}

#undef ptr
	return 0;
}

matrix_t A[NUM_THREADS], B[NUM_THREADS], C[NUM_THREADS];

static void init_matrices()
{
	for(int i=0;i<NUM_THREADS;i++){
		int size = matrix_sizes[(i / 8) % 4];
		generate_matrix(&A[i], size, 1);
		generate_matrix(&B[i], size, 1);
		generate_matrix(&C[i], size, 0);
	}
	return;
}


int parse_args(int argc, char* argv[])
{
	int inx;

	for(inx=0; inx<argc; inx++)
	{
		if(argv[inx][0]=='-')
		{
			if(!strcmp(&argv[inx][1], "lb"))
			{
				gt_load_balance = GT_ENABLED;
			}
			else if(!strcmp(&argv[inx][1], "s"))
			{
				if(inx+1<argc){
					inx++;
					if(!strcmp(&argv[inx][0], "0"))
					{
						gt_scheduler = GT_SCHED_O1;
					}
					else if(!strcmp(&argv[inx][0], "1"))
					{
						gt_scheduler = GT_SCHED_CRED;
					}
					else{
						printf("Error in arguments...exiting\n");
						return -1;
					}
				}
				else{
					printf("Error in arguments...exiting\n");
					return -1;
				}	
			}
			else if(!strcmp(&argv[inx][1], "y")){
				gt_yielding = GT_ENABLED;
			}
			else{
				printf("Error in arguments...exiting\n");
				return -1;
			}
		}
	}

	return 0;
}



uthread_arg_t uargs[NUM_THREADS];
uthread_t utids[NUM_THREADS];

int main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	uthread_arg_t *uarg;
	int inx_c, inx_s, inx_u;

	if(parse_args(argc, argv)!=0){
		return -1;
	}
	printf("GT_ARGS|sched=%s|lb=%d|yield=%d\n",
       gt_scheduler == GT_SCHED_CRED ? "credit" : "prio",
       gt_load_balance, gt_yielding);

	int num_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if(num_cpus > GT_MAX_KTHREADS)
        num_cpus = GT_MAX_KTHREADS;
	gtthread_app_init();

	init_matrices();

	gettimeofday(&tv1,NULL);
	for(inx_c = 0; inx_c < 4; inx_c++){
		for(inx_s = 0; inx_s < 4; inx_s++){
			for(inx_u = 0; inx_u < 8; inx_u++){
				int tid = inx_c * 32  + inx_s * 8 + inx_u;
				uarg = &uargs[tid];
				uarg->_A = &A[tid];
				uarg->_B = &B[tid];
				uarg->_C = &C[tid];
				uarg->tid = tid;
				uarg->gid = (tid % num_cpus);
				uarg->credit = credit_groups[inx_c];
				uarg->size = matrix_sizes[inx_s];
				uthread_create(&utids[tid], uthread_mulmat, uarg, uarg->gid, uarg->credit, uarg->size);
			}
		}
	}
	gtthread_app_exit();

	{
		FILE *detail = fopen("Detailed_output.csv", "w");
		fprintf(detail, "group_name,thread_number,cpu_time(us),wait_time(us),exec_time(us)\n");

		long sum_cpu[4][4] = {{0}}, sum_wait[4][4] = {{0}}, sum_exec[4][4] = {{0}};
		int cnt[4][4] = {{0}};

		for(int inx = 0; inx < GT_MAX_KTHREADS; inx++)
		{
			kthread_context_t *k_ctx = kthread_cpu_map[inx];
			if(!k_ctx) continue;

			uthread_struct_t *u;
			TAILQ_FOREACH(u, &(k_ctx->krunqueue.zombie_uthreads), uthread_runq)
			{
				int c_idx = -1, s_idx = -1;
				for(int i=0;i<4;i++) if(credit_groups[i]==u->uthread_credit_group) c_idx=i;
				for(int i=0;i<4;i++) if(matrix_sizes[i]==u->uthread_workload_size) s_idx=i;
				if(c_idx<0 || s_idx<0) continue;

				int tnum = cnt[c_idx][s_idx]++;
				long cpu_t = u->total_exec_time;
				long wait_t = u->total_wait_time;
				long exec_t = cpu_t + wait_t;

				fprintf(detail, "c_%d_m_%d,%d,%ld,%ld,%ld\n",
						u->uthread_credit_group, u->uthread_workload_size, tnum, cpu_t, wait_t, exec_t);

				sum_cpu[c_idx][s_idx] += cpu_t;
				sum_wait[c_idx][s_idx] += wait_t;
				sum_exec[c_idx][s_idx] += exec_t;
			}
		}
		fclose(detail);

		FILE *cout = fopen("Cummulative_output.csv", "w");
		fprintf(cout, "group_name,mean_cpu_time,mean_wait_time,mean_exec_time\n");
		for(int c=0;c<4;c++)
			for(int s=0;s<4;s++)
				fprintf(cout, "c_%d_m_%d,%.2f,%.2f,%.2f\n",
						credit_groups[c], matrix_sizes[s],
						sum_cpu[c][s]/8.0, sum_wait[c][s]/8.0, sum_exec[c][s]/8.0);
		fclose(cout);
	}
	printf("GT_DONE|threads=%d|sched=%s|lb=%d|yield=%d\n", NUM_THREADS, gt_scheduler == GT_SCHED_CRED ? "credit" : "prio", gt_load_balance, gt_yielding);
	return(0);
}
