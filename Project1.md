# Implementation of MLFQ + Stride Scheduler
## 1. Introduction

In our previous xv6 system, the default scheduling was Round Robin (RR) which works by alternating every process for a specific time slice to increase the efficiency of response time of each and every processes that are currently running.

In this project, our goal is to implement a better scheduler which not only giving processes better response time, but also better average turnaround time. This can be done by implementing MLFQ scheduler as our default scheduler and on top of that, combined with stride scheduler so that whenever a process requesting a portion of CPU time, the OS will give that specific amount of CPU share, no more and no less, while preserving at least 20% of the CPU time for processes in MLFQ scheduler.

## 2. Implementation
For this project, there are a few specific tasks that has to be done before successfully changing the old xv6 scheduler: 
* Updating ``proc.h``, defining constants and updating ``struct proc``.
* Rewrite ``scheduler()`` function in ``proc.c`` for handling MLFQ and Stride.
* Adding few important system calls.

### **Updating ``proc.h``**

In this file, we need to define few important constants such as time allotment and time quanta for every level in MLFQ, MLFQ and Stride structure, MLFQ levels and finally updating the old ``struct proc``.

```c
//constants for new scheduler
#define     TOP_QUANTA    1
#define     MID_QUANTA    2
#define     LOW_QUANTA    4
#define     TOP_ALLOT    5
#define     MID_ALLOT    10
#define     PR_BOOST_TICK    100
#define     TOT_TICKET       10000

//schedule mode and mlfq levels
enum sched_mode { MLFQ_SCHED = 0, STRIDE_SCHED};
enum mlfq_level {TOP = 0, MID, LOW};

//structure for procs in MLFQ
struct proc_mlfq {
    int tick;                  //current proc ticks
    enum mlfq_level lev;       //current proc level in MLFQ
    int priority;              //proc priority (same priority will use RR)
};

//structure for whole MLFQ (used during boosting)
struct proc_mlfq_queue {         
    int totaltick;             //total ticks from start of MLFQ initialization (useful during boosting)
    int top_count;             //total procs in top level
    int mid_count;             //total procs in mid level
    int low_count;             //total procs in low level
};

//structure for procs in Stride
struct proc_stride {
    int pass;                  //pass value
    int stride;                //stride value
    int share;                 //current CPU share (%)
};
```

After that, we update the old ``struct proc`` so that all the processes will have the properties needed for MLFQ and Stride scheduler.

```c
// Per-process state
struct proc {
  ....
  char name[16];               // Process name (debugging)
  
  enum sched_mode mode;        //scheduler mode of this proc
  struct proc_stride strd;     //will be used if in stride scheduler
  struct proc_mlfq mlfq;       //will be used if in MLFQ scheduler
};
```

### **Updating ``proc.c``**

#### 1. Local variables

```c
struct proc_mlfq_queue mlfq_queue;     //the whole struct of MLFQ including all levels
int requested_share_total = 0;         //initial total CPU share requested by processes
int mlfq_share = 100;                  //initial CPU share hold by MLFQ
```

#### 2. ``allocproc()`` : This is where the initialization during OS booting occur. We need to update it so that MLFQ will be the default scheduler of our OS.

```c
found:
  ...
  memset(&p->mlfq, 0, sizeof(p->mlfq));    //MLFQ processes are dynamically allocated
  memset(&p->strd, 0, sizeof(p->strd));    //Stride processes are dynamically allocated
  p->mode = MLFQ_SCHED;                    //set default scheduler mode to MLFQ
```

#### 3. ``userinit()`` : Also for OS initialization. We will define our ``mlfq_queue`` here.

```c
void
userinit(void)
{
  ....
  memset(&mlfq_queue, 0, sizeof(mlfq_queue) );  //MLFQ queue defined here
}
```

#### 4. ``scheduler()`` : Finally, for our main task here, we will rewrite the old RR scheduler to handle processes in MLFQ and Stride scheduler.

```c
void
scheduler(void)
{
  struct proc *p;
  struct proc *tproc;
  struct cpu *c = mycpu();

  int tpri;  
  enum mlfq_level tlev = LOW;
  
  c->proc = 0;
  tproc = 0;
  for(;;) {
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
    tpri = 999999999;
    tlev = LOW;  
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if((p->state != RUNNABLE) || (p->mode != MLFQ_SCHED))
        continue;
        
      if(p->mlfq.lev <= tlev) {
        if(p->mlfq.priority <= tpri) {
          tlev = p->mlfq.lev;
          tpri = p->mlfq.priority;
          tproc = p;
        }
      }
    }
        
    /* switch process */
    p = tproc;
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;
      
    swtch(&c->scheduler, p->context);
    switchkvm();
      
    int m = get_count(MLFQ_SCHED);

//--------------------MLFQ CHECKING STARTS HERE--------------------------------      
    if(p->mode == MLFQ_SCHED) {
      p->mlfq.tick++;
      mlfq_queue.totaltick++;
        
      //CHECKING QUANTA AND ALLOT TIME FOR TOP 
        if(p->mlfq.lev == TOP) {
		    if(p->mlfq.tick >= TOP_ALLOT) {
		      p->mlfq.lev = MID;
		      p->mlfq.tick = 0;
		      mlfq_queue.top_count--;
		      mlfq_queue.mid_count++;

      	}else if(p->mlfq.tick % TOP_QUANTA == 0) {
        	p->mlfq.priority++;
      	}
      	
      //CHECKING QUANTA AND ALLOT TIME FOR MID 	
    	}else if(p->mlfq.lev == MID) {

		    if(p->mlfq.tick >= MID_ALLOT) {
		      p->mlfq.lev = LOW;
		      p->mlfq.tick = 0;
		      mlfq_queue.mid_count--;
		      mlfq_queue.low_count++;
		    }else if(p->mlfq.tick % MID_QUANTA == 0) {
		      p->mlfq.priority++;
		    }
		    
		  //CHECKING QUANTA TIME FOR LOW 
        } else if(p->mlfq.lev == LOW) {

		    if(p->mlfq.tick % LOW_QUANTA == 0) {
		      p->mlfq.priority++;
		    }
		    
    } else {
      panic("p->mlfq.lev == UNKNOWN\n");
    }

		//PRIORITY BOOSTING EVERY 100 TICKS
    if(mlfq_queue.totaltick >= PR_BOOST_TICK) {
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if((p->state != RUNNABLE) || (p->mode != MLFQ_SCHED))
          continue;
        p->mlfq.tick = 0;
        p->mlfq.priority = 0;
        p->mlfq.lev = TOP; //all procs in mlfq get top level
      }
      mlfq_queue.totaltick = 0; //reset runtime for mlfq queue
      mlfq_queue.low_count = 0;
      mlfq_queue.mid_count = 0;
      mlfq_queue.top_count = m;
    } 
    
    
//-------------------------------STRIDE CHECKING STARTS HERE-----------------------------------------------
    }else if (p->mode == STRIDE_SCHED){
      
		  int min = get_min_pass(); 

			//find next proc with minimum pass value
		  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		  	if(p->state != RUNNABLE || p->mode == MLFQ_SCHED)
		  	  continue;
		 		if(min >= p->strd.pass) {
		   	 	min = p->strd.pass;
		  		  tproc = p;
		  	}
		  }
		  	
		  //switch to that proc
		  p = tproc;
		  c->proc = p;
		  switchuvm(p);
		  p->state = RUNNING;
		   	 
		  swtch(&c->scheduler, p->context);
		  switchkvm();
		  
		  //update current proc pass value
		  p->strd.pass += p->strd.stride;
      
      
    }else{
      panic("p->mode == UNKNOWN\n");
    }
 
    c->proc = 0;
    
    release(&ptable.lock);
  }
}
``` 

### **Helper functions**

#### 1. ``get_count()`` : This function will return the current number of processes in scheduling mode passed in argument.
```c
int
get_count(enum sched_mode mode)
{
  int count = 0;
  struct proc *p = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state != RUNNABLE)
      continue;
      
    if(p->mode == mode)
      count++;
  }
  return count;
}
```

#### 2. ``get_min_pass()`` : This function will return the minimum pass value of processes in Stride scheduler

```c
int
get_min_pass(void)
{
  int min = 99999999;
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE || p->mode != STRIDE_SCHED)
      continue;
     
    if(min > p->strd.pass)
      min = p->strd.pass;
  }

  if(!min){             //min not available (when there's 0 processes in Stride scheduler)
  	return 0;
  }
  return min;
  	 
}
```

## 3. System Calls

Now that we are done with updating ``proc.h`` and ``proc.c`` files, the only left thing to do is to implement a few necessary system calls that was specified in the project specifications. Since we have already added ``yield()`` function as system call from previous lab, there are 2 more system calls to be defined. All system call functions are defined in ``proc.c`` file. 

### ``set_cpu_share()`` 

When a process calls this function, that process will receive CPU share accordingly to the argument passed (in %) and will be added in Stride scheduler. Exception handling required if total requested CPU exceeds 80%.

```c
int 
set_cpu_share(int share)
{
   
    struct proc *p = myproc();
    int n, m;

    acquire(&ptable.lock);
    
    if(requested_share_total < 0 || share < 0) {
        panic("share_sum < 0 || share < 0");
        return -1;
    }
   
    requested_share_total += share;
    mlfq_share -= share;
    p->mode = STRIDE_SCHED;
    p->strd.share = share;
   
    //exception handling when total requested share >80
    if(requested_share_total > 80) {
        //roll back
        requested_share_total -= share;
        mlfq_share += share;
        p->mode = MLFQ_SCHED;

        n = get_count(MLFQ_SCHED);
        m = get_count(STRIDE_SCHED);
        
        if(n > 0) {
            p->strd.share = (80 - requested_share_total) / m;  //if there's mlfq procs, leave 20% CPU untouched for it 
        } else if(n == 0) {
            p->strd.share = (100 - requested_share_total) / m; //if none, use all remaining CPU
        } else {
            panic("# of MLFQ running process < 0\n");
            return -1;
        }
       
				       
        p->strd.stride = mlfq_share * (p->strd.share/100);    
        p->strd.pass = get_min_pass();   
        
        release(&ptable.lock);
        return 1;
    }
		
    p->strd.stride = mlfq_share * (share/100);
    p->strd.pass = get_min_pass(); //new process in stride get pass value = current min pass 
    
    release(&ptable.lock);
    return 0;  
}
```

### ``getlev()`` 

A simple system call that returns the curretn MLFQ level of the caller.

```c
int
getlev(void)
{	
	return myproc()->mlfq.lev;
}
```

Finally, to register both functions as system calls, we will update the necessary files:

* Makefile
* defs.h
* syscall.c
* syscall.h
* sysproc.c  (*Wrapper function defined here*)
* user.h
* usys.S 

Wrapper functions for ``getlev()`` and ``set_cpu_share`` :

```c
int
sys_getlev(void)
{
	return getlev();
}

int
sys_set_cpu_share(void)
{
	  int share;
	  if(argint(0, &share) < 0)
        return -1;
    return set_cpu_share(share);
```

## 4. Testing Scheduler and Result

To test our new scheduler, we are given a test code ``test_scheduler.c`` which will use both MLFQ and Stride scheduler and prints out the ``cnt value`` which have been accumulated during the whole workload lifetime. 

![image](uploads/e2f640c2f4c3e9e1c6a7652fba860d96/image.png)

### a) Expected Result

If we want our scheduler to work properly, we need to make sure these things to be true and printed by the program:

* STRIDE(5) : Prints ``cnt`` that is 5% of total ``cnt``
* STRIDE(15) : Prints ``cnt`` that is 15% of total ``cnt``
* MLFQ LEV : Prints ``cnt`` that is at least 20% of total ``cnt``
* MLFQ : Prints ``cnt`` that is remaining of the total ``cnt``

### b) Actual Result

To get the accurate result, I ran the ``test_scheduler`` program 10 times and calculate the mean of the result for each scheduler mode. The result is as below:

![image](uploads/214e42224cc4d58675f37fbbf22e86e3/image.png)

![image](uploads/c97d00704a6aac7f3964ba1ce4803c9f/image.png)

### c) Comparing Expected Result and Actual Result

With all these data, we can calculate the mean percentage for every scheduler mode.

* STRIDE(5) mean percentage : (21190.4/407912.4) * 100 = **5.19%**
* STRIDE(15) mean percentage : (63437.4/407912.4) * 100 = **15.55%**
* MLFQ LEV mean percentage : (110690.3/407912.4) * 100 = **27.14%**
* MLFQ mean percentage : (212573.0/407912.4) * 100 = **52.11%**

|             | Expected |   Actual
| ----------- | ----------- | ----------|
| STRIDE(5)      | 5%       |   5.19%     |
| STRIDE (15)   | 15%        |   15.55%   |
| MLFQ LEV     |   >= 20%    |    27.14%   |
| MLFQ        |  >= 40%      |    52.11%  |

## 4. Conclusion

Our actual result are very similar to our expected result with margin less than 1%. In conclusion, we can say that our MLFQ + Stride scheduler algorithm worked perfectly as planned!
