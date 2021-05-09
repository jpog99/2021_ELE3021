#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initemp_proc;


struct proc_mlfq_queue mlfq_queue;
int requested_share_total = 0;
int mlfq_share = 100;

int lwp_count = 0;

struct st{
	int use;
};

struct {
	struct st st[NPROC*50];
} stack_table;

int nextpid = 1;
int nextlwpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  
  memset(&p->mlfq, 0, sizeof(p->mlfq));
  memset(&p->strd, 0, sizeof(p->strd));
  p->mode = MLFQ_SCHED;
	
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initemp_proc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
  
  memset(&mlfq_queue, 0, sizeof(mlfq_queue) );
  
  for(int i=0 ; i<NPROC*50 ; i++){
  	stack_table.st[i].use = 0;
  }
  
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initemp_proc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initemp_proc;
      if(p->state == ZOMBIE)
        wakeup1(initemp_proc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

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

int
getlev(void)
{	
	return myproc()->mlfq.lev;
}

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

  if(!min){
  	return 0;
  }
  
  return min;
  	 
}

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

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
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

//get the stack base of the process
uint 
find_sbase(struct proc* p) 
{    
  //find stack base that is unused
  for(uint i=0; i<NPROC*50; i++) {
    if(stack_table.st[i].use != 0){
      continue;
    }
      
    //if found, set to used
    stack_table.st[i].use = 1;
    //by the requesting proc
    p->stack_idx = i;
	  //and return the stack base
    return i;
  }
  panic("stack full.");
}

//create LWP.
//Similar to fork() system call, but instead of copying the resources
//of manager process, the child (LWP) will share those resources.
//Need to find a new stack position for the new LWP
//which is different from any other proccess or LWP.
int 
thread_create(thread_t* thread, void* (*start_routine)(void*), void* arg) 
{
    
  struct proc *np; //new LWP
  struct proc *curproc = myproc(); //caller proc

	//alloc proc for LWP
  if((np = allocproc()) == 0) {
      return -1;
  }
  lwp_count++;
    
  
  acquire(&ptable.lock);
  
  //if LWP created by normal process
  if(curproc->lwpid == 0){ 
      np->lwpmid = curproc->pid; //the caller process will become the manager of the LWP
      np->parent = curproc;  

	//if LWP created by other LWP
  }else if(curproc->lwpid > 0) { 
      np->lwpmid = curproc->lwpmid; //the new LWP will have the same manager proc as the caller LWP
      np->parent = curproc->parent; //and same parent proc
      
  //else invalid caller,return non zero
  } else {
      cprintf("invalid caller process.\n");
      return -1;
  }
    
	//assign the LWP ID and resources (shares the same resc as manager proc)
  *thread = np->lwpid;
  np->lwpid = nextlwpid++;
  np->pgdir = curproc->pgdir; //share page dir
  *np->tf = *curproc->tf; 	  //share trap frame
  np->cwd = idup(curproc->cwd);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name)); //share name (debug)

  //set LWP stack 
  //2 page for stack and guard
  np->stack_base = curproc->sz + (uint)(2*PGSIZE*(find_sbase(np)+1));

  if((np->sz = allocuvm(np->pgdir, np->stack_base, np->stack_base + 2*PGSIZE)) == 0) {
      np->state = UNUSED;
      return -1;
  } 
    
  //set instruction pointer to start_routine function
  np->tf->eip = (uint)start_routine;
  
  //save the arg pointer into stack
  np->tf->esp = np->sz - 4;
  *((uint*)(np->tf->esp)) = (uint)arg;
  
  //and set the stack pointer
  np->tf->esp = np->sz - 8;
  *((uint*)(np->tf->esp)) = 0xffffffff;
    
  //same procedure as fork()
  for(int i=0; i<NOFILE; i++)
      if(curproc->ofile[i])
          np->ofile[i] = filedup(curproc->ofile[i]);

  np->state = RUNNABLE;
  release(&ptable.lock);
  
  return 0;
}

//thread_exit to change the caller LWP to zombie. 
//Very similar to exit() function with little modification
void
thread_exit(void* retval)
{
  struct proc *curproc = myproc();
  //struct proc *p;
  int fd;

  if(curproc == initemp_proc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);
  
  //set retval pointer to LWP retval
  curproc->retval = retval;

  // Manager proc might be sleeping in wait().
  wakeup1(curproc->parent);

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  lwp_count--;
  sched();
  panic("zombie exit");
}

//Called only by the LWP manager proc
//to return the resources to the
//joining LWP into manager's LWP list.
//Similar to wait() function.
int 
thread_join(thread_t thread, void** retval)
{
	struct proc *p;
  struct proc *curproc = myproc();
  
  //this function can only be called by the manager proc
  if(curproc->lwpid > 0) {
  	cprintf("Not a manager proc.\n");
  	return -1;
  }
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for to-be-joined thread
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      //if(p->parent != curproc)
        //continue;
      //havekids = 1;
      if(p->lwpid != thread)
        continue;  
      if(p->lwpmid != curproc->pid)
        continue;
        
      if(p->state == ZOMBIE){
        // Found one.
        *retval = p->retval; //set retval
        //pid = p->pid;
        kfree(p->kstack); //free the LWP stack
        p->kstack = 0;  
        /*instead of freevm, we only dealloc vm of the LWP since it is a shared resource*/
        //freevm(p->pgdir);
        deallocuvm(p->pgdir, p->stack_base+2*PGSIZE, p->stack_base); 
        stack_table.st[p->stack_idx].use = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->lwpid = 0;
        p->lwpmid = 0;
        p->stack_base = 0;
        p->stack_idx = 0;
        p->retval = 0;
        
        release(&ptable.lock);
        return 0;
      }
    }

    // No point joining if we don't have any manager proc
    if(curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for LWP to finish.  
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }

}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void){	
	return myproc()->parent->pid;
}
