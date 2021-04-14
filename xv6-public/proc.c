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

//defining structs and vars for MLFQ and Stride scheduler
struct mlfq mlfq_queue; 
int mlfq_tot_cpushare= 100; 
int stride_tot_cpushare = 0; 
int num_proc = 0; 

int nextpid = 1;
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
  
  //Initialize the val to 0 in memory
  memset(&p->mlfq, 0, sizeof(p->mlfq));
  memset(&p->stride, 0, sizeof(p->stride));  
  p->mode = 0; //mlfq as default
	
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
  
  memset(&mlfq_queue, 0, sizeof(mlfq_queue));
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
  
  if(curproc->mode == STRIDE_S) {
    mlfq_tot_cpushare += curproc->stride.share; //mlfq get back the share from terminated proc
    stride_tot_cpushare -= curproc->stride.share; //total share hold by stride_s decreased 
  }

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
get_proc_count(enum schedule_mode mode)
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
	return myproc()->mlfq.level;
}

int 
get_min_pass(void)
{
	  int min = 99999999;
    struct proc *p;
    
    acquire(&ptable.lock);

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
      
      if(p->mode == STRIDE_S)
        if(min > p->stride.pass)
          min = p->stride.pass;
    }
    
    release(&ptable.lock);

		int mlfq_count = get_proc_count(MLFQ_S);
    if(mlfq_count > 0)
      if(min > mlfq_queue.pass)
        min = mlfq_queue.pass;
    return min; 
}

int 
update_stride(void)
{	
  int mlfq_ps = get_proc_count(MLFQ_S);
	int stride_ps = get_proc_count(STRIDE_S);
  
  struct proc *p = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE)
      continue;
      
    switch(p->mode) {
      case MLFQ_S:
        if(mlfq_ps <= 0) {
          p->stride.share = 0; 
          p->stride.stride = 0; 
          mlfq_queue.share = 0; 
          break;
        } else {
          p->stride.share = mlfq_queue.share / mlfq_ps;
          p->stride.stride = 10001 / p->stride.share;
          break;
        }
        
      case STRIDE_S:
        p->stride.share = stride_tot_cpushare / stride_ps;
        p->stride.stride = 10001 / p->stride.share;
        break;
    }
  }
  return 0;
}

int
init_pass(void)
{
	struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state != RUNNABLE || p->mode == STRIDE_S)
      continue;
    p->stride.pass = 0;
  }
  
  mlfq_queue.pass = 0;
  return 0;
}

int 
set_cpu_share(int share)
{
  struct proc *p = myproc();
  int mlfq_proc_count , stride_proc_count;
  acquire(&ptable.lock);
    
  if(stride_tot_cpushare < 0 || share < 0) {
      panic("share_sum < 0 || share < 0");
      return -1;
  }
   
  stride_tot_cpushare += share;
  mlfq_tot_cpushare -= share;
  
  p->mode = STRIDE_S; //change proc scheduler to stride
  p->stride.share = share;
   
  if(stride_tot_cpushare > 80) {
      stride_tot_cpushare -= share;
      mlfq_tot_cpushare += share;
      p->mode = MLFQ_S;
      
      mlfq_proc_count = get_proc_count(MLFQ_S);
      stride_proc_count = get_proc_count(STRIDE_S);
        
      if(mlfq_proc_count > 0) {
          p->stride.share = (80 - stride_tot_cpushare) / stride_proc_count;    
      } else if(mlfq_proc_count == 0) {
          p->stride.share = (100 - stride_tot_cpushare) / stride_proc_count;
      } else {
          panic("# of MLFQ running process < 0\n");
          return -1;
      }
       
      /* update stride for every process*/
      update_stride();
      /* initialize all pass */
      init_pass();
        
      release(&ptable.lock);
      return 1;
  }
    /* update stride for every process*/
  update_stride();
    /* initialize all pass */
  init_pass();
    
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
  struct cpu *c = mycpu();
  c->proc = 0; 
  
  struct proc *temp_proc = 0;
  int temp_pr;
  enum mlfq_lev temp_level = LOW;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
    int min_pass = 99999999;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE || p->mode != STRIDE_S)
        continue;
      if(min_pass >= p->stride.pass){
      	min_pass = p->stride.pass;
      	temp_proc = p;
     	}
    }
    
    //checking procs in MLFQ first
    int mlfq_count = get_proc_count(MLFQ_S);
    if(mlfq_count>0 && min_pass>=mlfq_queue.pass){
    	min_pass = mlfq_queue.pass;
    	temp_pr = 99999999;
    	temp_level = LOW;
    	
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if((p->state != RUNNABLE) || (p->mode != MLFQ_S))
          continue;
        
        if(p->mlfq.level <= temp_level) {
          if(p->mlfq.priority <= temp_pr) {
            temp_level = p->mlfq.level;
            temp_pr = p->mlfq.priority;
            temp_proc = p;
          }
        }
      }
        
      p = temp_proc;
      c->proc = p;
      switchuvm(p); //switch process with temp_proc (mlfq)
      p->state = RUNNING;
      
      swtch(&c->scheduler, p->context);
      switchkvm();

      if(p->mode == MLFQ_S) {
        p->stride.pass += p->stride.stride;
        p->mlfq.curr_runtime++;
        
        mlfq_queue.pass += (p->stride.stride / mlfq_count);
        mlfq_queue.total_runtime++;
      } else {
        panic("p->mode != MLFQ_S\n");
      }
      
      //updating each process on every level
      if(p->mlfq.level == TOP) {
        
        //TOP level allot check
        if(p->mlfq.curr_runtime >= TOP_ALLOT) {
          p->mlfq.level = MID;
          p->mlfq.curr_runtime = 0;
          mlfq_queue.top_proc_count--;
          mlfq_queue.mid_proc_count++;
        //TOP level time slice check
        } else if(p->mlfq.curr_runtime % TOP_TS == 0) {
          p->mlfq.priority++;
        }
        
        
      } else if(p->mlfq.level == MID) {
        //MID level allot check
        if(p->mlfq.curr_runtime >= MID_TS) {
          p->mlfq.level = LOW;
          p->mlfq.curr_runtime = 0;
          mlfq_queue.mid_proc_count--;
          mlfq_queue.low_proc_count++;
        //TOP level time slice check
        } else if(p->mlfq.curr_runtime % MID_TS == 0) {
          p->mlfq.priority++;
        }
        
        
      } else if(p->mlfq.level == LOW) {
        //LOW level allot check
        if(p->mlfq.curr_runtime % LOW_TS == 0) {
          p->mlfq.priority++;
        }
      } else {
        panic("p->mlfq.level == ?\n");
      }
      
      //priority boost for every 100 ticks
      if(mlfq_queue.total_runtime >= PR_BOOST_TICK) {
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
          if((p->state != RUNNABLE) || (p->mode != MLFQ_S))
            continue;
          p->mlfq.curr_runtime = 0;
          p->mlfq.priority = 0;
          p->mlfq.level = TOP;
        }
        mlfq_queue.total_runtime = 0;
        mlfq_queue.top_proc_count = mlfq_count;
        mlfq_queue.mid_proc_count = 0;
        mlfq_queue.low_proc_count = 0;    
      }
      
		}
		// if no MLFQ proc, then run STRIDE sched
		else if (mlfq_count==0 || (mlfq_count>0 && min_pass<mlfq_queue.pass) ){
			p = temp_proc;
      c->proc = p;
      
      switchuvm(p);
      p->state = RUNNING;
      
      swtch(&c->scheduler, p->context);
      switchkvm();

      p->stride.pass += p->stride.stride; //update pass val
		
		}else {
      panic("get_num(MLFQ_SCHED) < 0\n");
  	}
  	
  	update_stride();
    
    if(c->proc->stride.pass > 100000000) {
      init_pass();
    }

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    
  	release(&ptable.lock);

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
