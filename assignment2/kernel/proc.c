#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "procstat.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;




int nextpid = 1;
struct spinlock pid_lock;

int batch_tot_size = 0;
int batch_size = 0;
uint batch_start_time;
uint batch_exec_time = 0;
int batch_sched = 0;

int submitjob_pid;


int avg_turn_time = 0;
int avg_wait_time;
int avg_comp_time = 0;
int min_comp_time = -1;
int max_comp_time = 0;

int cpu_bursts = 0;
int avg_cpu_burst = 0;
int max_cpu_burst = 0;
int min_cpu_burst = -1;

int diff_cpu_burst = 0;
int est_cpu_bursts = 0;
int avg_est_cpu_burst = 0;
int max_est_cpu_burst = 0;
int min_est_cpu_burst = -1;
uint batch_ready = 0;
int cpu_burst_est_error = 0;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  uint xticks;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.


  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  p->base_priority = 0;
  p->priority = 0;
  p->made_forkp = 0;
  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  p->sticks = 0;
  p->eticks = 0;
  p->next_burst = 0;
  p->ctime = xticks;
  p->stime = -1;
  p->endtime = -1;
  p->cpu_usage = 0;
  p->wait_time = 0;

  p->rticks = 0;
  p->wticks = 0;
  
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  //printf("%d created\n", p->pid);
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  np->base_priority = p->base_priority;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

int forkf(uint64 faddr)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;
  // Make child to jump to function
  np->trapframe->epc = faddr;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  np->base_priority = p->base_priority;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();
  uint xticks;

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);
  // if(p == submitjob_proc) submitjob_proc = 0;
  p->xstate = status;
  p->state = ZOMBIE;
  //printf("[%d] converted to zombie\n", p->pid);
  release(&wait_lock);

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);

  p->endtime = xticks;
  p->eticks = xticks;
  int curr_cpu_burst = p->eticks - p->sticks;


    if(p->pid==submitjob_pid){
      // sumbitjob_ended = 1;
      submitjob_pid = -1;
      // printf("submitjob ended\n");
    }
  if(p->made_forkp) {
    avg_turn_time += (p->endtime - p->rticks);
    avg_comp_time += p->endtime;
    avg_wait_time += p->wait_time;
    
     if(min_comp_time == -1) {
      min_comp_time = p->endtime;
    }
    else {
      if(min_comp_time > p->endtime) min_comp_time = p->endtime;
    }
    if(max_comp_time < p->endtime) max_comp_time = p->endtime;
  
    
    // statistics collection
    if(curr_cpu_burst > 0 && p->made_forkp == 1) {
      // stats for SJF cpu bursts
      cpu_bursts += 1;
      avg_cpu_burst += curr_cpu_burst;
      if(max_cpu_burst < curr_cpu_burst) max_cpu_burst = curr_cpu_burst;
      if(min_cpu_burst == -1) {
        min_cpu_burst = curr_cpu_burst;
        //seen_cpu_burst = 1;
      }
      else {
        if(min_cpu_burst > curr_cpu_burst) min_cpu_burst = curr_cpu_burst;
      }
      
      if(p->next_burst != 0) {
        cpu_burst_est_error += 1;
        if(p->next_burst > curr_cpu_burst) {
          diff_cpu_burst = diff_cpu_burst + (p->next_burst - (p->eticks - p->sticks));
        }
        else {
          diff_cpu_burst = diff_cpu_burst + ( (p->eticks - p->sticks) - p->next_burst);
        }
      }
      // stats for cpu burst estimation
      if(p->next_burst != 0) {
        est_cpu_bursts += 1;
        avg_est_cpu_burst += p->next_burst;
        if(max_est_cpu_burst < p->next_burst) max_est_cpu_burst = p->next_burst;
        if(min_est_cpu_burst == -1) {
          min_est_cpu_burst = p->next_burst;
        }
        else {
          if(min_est_cpu_burst > p->next_burst) min_est_cpu_burst = p->next_burst;
        }
        
      }
      // calculating the next cpu burst
      p->next_burst = p->eticks - p->sticks - ((p->eticks - p->sticks) * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM + (p->next_burst * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM;
      
    }
  }
  if(batch_size > 0 && p->made_forkp ) {
    
    batch_size -= 1;
    //printf("batch size: %d\n", batch_size);
    if(batch_size == 0) {
      batch_exec_time = xticks - batch_start_time;
      
      getstats();
      sched_policy = SCHED_PREEMPT_RR;
      
    }
  }
  p->next_burst = p->eticks - p->sticks - ((p->eticks - p->sticks) * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM + (p->next_burst * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM;
  // Jump into the scheduler, never to return.
  
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

int waitpid(int pid, uint64 addr)
{
  struct proc *np;
  struct proc *p = myproc();
  int found = 0;

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for child with pid
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if ((np->parent == p) && (np->pid == pid))
      {
        found = 1;
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        if (np->state == ZOMBIE)
        {
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }

        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!found || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{

  struct proc *p;
  struct cpu *c = mycpu();
  //c->policy = 2;
  sched_policy = 2;
  c->proc = 0;
  
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    int pol = sched_policy;
    if (sched_policy == SCHED_PREEMPT_RR || sched_policy == SCHED_NPREEMPT_FCFS)
    {
      
      if(pol != sched_policy) continue;
      for (p = proc; p < &proc[NPROC]; p++)
      {
        
        
        int pol = sched_policy;
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          if(p->made_forkp == 1) {
             
            uint xticks;
            if (!holding(&tickslock)) {
              acquire(&tickslock);
              xticks = ticks;
              release(&tickslock);
            }
            else xticks = ticks;
            p->sticks = xticks;
            p->wait_time += (xticks - p->wticks);
            //printf("****xtixcks : %d, wticjs: %d\n", xticks, p->wticks);
          }
          // printf("pid: %d\n", p->pid);
          swtch(&c->context, &p->context);
          
          if (sched_policy != pol)
          {
            //c->pol_change = 0;
            release(&p->lock);
            break;
          }
          if(batch_sched == 0 && p->made_forkp == 1) {
            batch_start_time = p->stime;
            batch_sched = 1;
          }
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
        }
        release(&p->lock);
      }
    }
    else if (sched_policy == SCHED_NPREEMPT_SJF)
    {
      
      for (p = proc; p < &proc[NPROC]; p++)
      {
        int pol = sched_policy;
        acquire(&p->lock);
        //printf("p->pid = %d, p->state = %d \n", p->pid, p->state);
        if (p->state == RUNNABLE ) {
          if(p->made_forkp == 0) {
            {
              // Switch to chosen process.  It is the process's job
              // to release its lock and then reacquire it
              // before jumping back to us.
              
              p->state = RUNNING;
              c->proc = p;
              swtch(&c->context, &p->context);
              if (sched_policy != pol)
              {
                release(&p->lock);
                break;
              }
              c->proc = 0;
            }
          }
        }
        release(&p->lock);
      }
      // if(sched_policy != pol )continue;
      int min_b = -1;
      struct proc *minp = 0;
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if(p->state == RUNNABLE && p->made_forkp) {
          if(min_b < 0) {
            min_b = p->next_burst;
            minp = p;

          }
          else {
            if(min_b > p->next_burst) {
              min_b = p->next_burst;
              minp = p;
            }
          }
        }
            
        release(&p->lock); 
      }
           
      
      if (minp != 0)
      {
        acquire(&minp->lock);
        // printf("pid of minp: %d\n", minp->pid);
        if(minp->state != RUNNABLE) {
          //printf("hi\n");
          release(&minp->lock);
          continue;
        }
        minp->state = RUNNING;
        c->proc = minp;
        
        uint xticks;
        if (!holding(&tickslock)) {
          acquire(&tickslock);
          xticks = ticks;
          release(&tickslock);
        }
        else xticks = ticks;
        //uint st = xticks;
        minp->wait_time += (xticks - minp->wticks);
        // initialize start time of current cpu burst
        minp->sticks = xticks;
        
        swtch(&c->context, &minp->context);

        if(batch_sched == 0) {
          batch_sched = 1;
          batch_start_time = minp->stime;
        }
      
        c->proc = 0;
        release(&minp->lock);
      }
        
      
      
    
    }
    else if (sched_policy == SCHED_PREEMPT_UNIX)
    {
      
      
      if(pol != sched_policy) continue;
      
      
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if(p->state==RUNNABLE) {
          p->cpu_usage = p->cpu_usage / 2;
          p->priority = p->base_priority + p->cpu_usage / 2;
        }
        
        release(&p->lock);
      }
      int seen_first_process = 0;
      int min = 1000000000;
      struct proc *minp = 0;
      for (p = proc; p < &proc[NPROC]; p++)
      {
        
        acquire(&p->lock);
        int pol = sched_policy;
        if (p->state == RUNNABLE && p->made_forkp == 0)
        {
          
          p->state=RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);
          if(pol != sched_policy) {
            release(&p->lock);
            c->proc = 0;
            break;
          }
          c->proc = 0;
        }
        else if(p->state ==RUNNABLE && p->made_forkp == 1) {
          if(seen_first_process == 0) {
            min = p->priority;
            minp = p;
            seen_first_process = 1;
          }
          else {
            if(min > p->priority) {
              min = p->priority;
              minp = p;
            }
          }
        }
        release(&p->lock);
      }
      if (minp != 0)
      {
        acquire(&minp->lock);
        minp->state = RUNNING;
        c->proc = minp;
        
        uint xticks;
            if (!holding(&tickslock)) {
              acquire(&tickslock);
              xticks = ticks;
              release(&tickslock);
            }
            else xticks = ticks;
            minp->sticks = xticks;
            minp->wait_time += (xticks - minp->wticks);
        swtch(&c->context, &minp->context);
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        if(batch_sched == 0) {
              batch_sched = 1;
              batch_start_time = minp->stime;
        }
        c->proc = 0;
        release(&minp->lock);
      }
    }
  }
  //printf("bye\n");
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();
  
  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  uint xticks;
  if (!holding(&tickslock))
  {
    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
  }
  else
    xticks = ticks;
  p->eticks = xticks;
  p->wticks = xticks;

  // calculate the current cpu burst
  int curr_cpu_burst = p->eticks - p->sticks;
  
  // statistics collection
  if(curr_cpu_burst > 0 && p->made_forkp == 1) {
    // stats for SJF cpu bursts
    cpu_bursts += 1;
    avg_cpu_burst += curr_cpu_burst;
    if(max_cpu_burst < curr_cpu_burst) max_cpu_burst = curr_cpu_burst;
    if(min_cpu_burst == -1) {
      min_cpu_burst = curr_cpu_burst;
      //seen_cpu_burst = 1;
    }
    else {
      if(min_cpu_burst > curr_cpu_burst) min_cpu_burst = curr_cpu_burst;
    }
    
    if(p->next_burst != 0) {
      cpu_burst_est_error += 1;
      if(p->next_burst > curr_cpu_burst) {
        diff_cpu_burst = diff_cpu_burst + (p->next_burst - (p->eticks - p->sticks));
      }
      else {
        diff_cpu_burst = diff_cpu_burst + ( (p->eticks - p->sticks) - p->next_burst);
      }
    }
    // stats for cpu burst estimation
    if(p->next_burst != 0) {
      est_cpu_bursts += 1;
      avg_est_cpu_burst += p->next_burst;
      if(max_est_cpu_burst < p->next_burst) max_est_cpu_burst = p->next_burst;
      if(min_est_cpu_burst == -1) {
        min_est_cpu_burst = p->next_burst;
      }
      else {
        if(min_est_cpu_burst > p->next_burst) min_est_cpu_burst = p->next_burst;
      }
      
    }
    // calculating the next cpu burst
    p->next_burst = p->eticks - p->sticks - ((p->eticks - p->sticks) * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM + (p->next_burst * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM;
    
  }
  
  //if(curr_cpu_burst > 0)
   // p->next_burst = p->eticks - p->sticks - ((p->eticks - p->sticks) * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM + (p->next_burst * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM;
  p->cpu_usage += SCHED_PARAM_CPU_USAGE;
  sched();
  
  //p->cpu_usage = p->cpu_usage / 2;
  //p->priority = p->base_priority + p->cpu_usage / 2;
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;
  uint xticks;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);

  myproc()->stime = xticks;
  myproc()->sticks = xticks;

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  // printf("pid: %d\n", p->pid);
  uint xticks;
  if (!holding(&tickslock))
  {
    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
  }
  else
    xticks = ticks;
  p->eticks = xticks;
  
  // calculate the current cpu burst
  int curr_cpu_burst = p->eticks - p->sticks;
  
  // statistics collection
  if(curr_cpu_burst > 0 && p->made_forkp == 1) {
    // stats for SJF cpu bursts
    cpu_bursts += 1;
    avg_cpu_burst += curr_cpu_burst;
    if(max_cpu_burst < curr_cpu_burst) max_cpu_burst = curr_cpu_burst;
    if(min_cpu_burst == -1) {
      min_cpu_burst = curr_cpu_burst;
      //seen_cpu_burst = 1;
    }
    else {
      if(min_cpu_burst > curr_cpu_burst) min_cpu_burst = curr_cpu_burst;
    }
    
    if(p->next_burst != 0) {
      cpu_burst_est_error += 1;
      if(p->next_burst > curr_cpu_burst) {
        diff_cpu_burst = diff_cpu_burst + (p->next_burst - (p->eticks - p->sticks));
      }
      else {
        diff_cpu_burst = diff_cpu_burst + ( (p->eticks - p->sticks) - p->next_burst);
      }
    }
    // stats for cpu burst estimation
    if(p->next_burst != 0) {
      est_cpu_bursts += 1;
      avg_est_cpu_burst += p->next_burst;
      if(max_est_cpu_burst < p->next_burst) max_est_cpu_burst = p->next_burst;
      if(min_est_cpu_burst == -1) {
        min_est_cpu_burst = p->next_burst;
      }
      else {
        if(min_est_cpu_burst > p->next_burst) min_est_cpu_burst = p->next_burst;
      }
      
    }
    // calculating the next cpu burst
    p->next_burst = p->eticks - p->sticks - ((p->eticks - p->sticks) * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM + (p->next_burst * SCHED_PARAM_SJF_A_NUMER) / SCHED_PARAM_SJF_A_DENOM;
    
  }
  p->cpu_usage += SCHED_PARAM_CPU_USAGE / 2;
  sched();

  // Tidy up.
  p->chan = 0;
  
  //p->cpu_usage = p->cpu_usage / 2;
  //p->priority = p->base_priority + p->cpu_usage / 2;
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
        uint xticks;
        if (!holding(&tickslock)) {
          acquire(&tickslock);
          xticks = ticks;
          release(&tickslock);
        }
        else xticks = ticks;
        p->wticks = xticks;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
        uint xticks;
        if (!holding(&tickslock)) {
          acquire(&tickslock);
          xticks = ticks;
          release(&tickslock);
        }
        else xticks = ticks;
        p->wticks = xticks;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// Print a process listing to console with proper locks held.
// Caution: don't invoke too often; can slow down the machine.
int ps(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep",
      [RUNNABLE] "runble",
      [RUNNING] "run",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;
  int ppid, pid;
  uint xticks;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      release(&p->lock);
      continue;
    }
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    pid = p->pid;
    release(&p->lock);
    acquire(&wait_lock);
    if (p->parent)
    {
      acquire(&p->parent->lock);
      ppid = p->parent->pid;
      release(&p->parent->lock);
    }
    else
      ppid = -1;
    release(&wait_lock);

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);

    printf("pid=%d, ppid=%d, state=%s, cmd=%s, ctime=%d, stime=%d, etime=%d, size=%p", pid, ppid, state, p->name, p->ctime, p->stime, (p->endtime == -1) ? xticks - p->stime : p->endtime - p->stime, p->sz);
    printf("\n");
  }
  return 0;
}

int pinfo(int pid, uint64 addr)
{
  struct procstat pstat;

  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep",
      [RUNNABLE] "runble",
      [RUNNING] "run",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;
  uint xticks;
  int found = 0;

  if (pid == -1)
  {
    p = myproc();
    acquire(&p->lock);
    found = 1;
  }
  else
  {
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if ((p->state == UNUSED) || (p->pid != pid))
      {
        release(&p->lock);
        continue;
      }
      else
      {
        found = 1;
        break;
      }
    }
  }
  if (found)
  {
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    pstat.pid = p->pid;
    release(&p->lock);
    acquire(&wait_lock);
    if (p->parent)
    {
      acquire(&p->parent->lock);
      pstat.ppid = p->parent->pid;
      release(&p->parent->lock);
    }
    else
      pstat.ppid = -1;
    release(&wait_lock);

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);

    safestrcpy(&pstat.state[0], state, strlen(state) + 1);
    safestrcpy(&pstat.command[0], &p->name[0], sizeof(p->name));
    pstat.ctime = p->ctime;
    pstat.stime = p->stime;
    pstat.etime = (p->endtime == -1) ? xticks - p->stime : p->endtime - p->stime;
    pstat.size = p->sz;
    if (copyout(myproc()->pagetable, addr, (char *)&pstat, sizeof(pstat)) < 0)
      return -1;
    return 0;
  }
  else
    return -1;
}

int schedpolicy(int policy)
{
  int old_policy = sched_policy;
  sched_policy = policy;
  printf("Policy changed from %d to %d\n", old_policy, policy);
  return old_policy;
}

int forkp(int prior)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  // if(submitjob_proc == 0) submitjob_proc = p;
  // printf("forkp called\n");
  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }
  np->made_forkp = 1;
  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  
  release(&wait_lock);

  acquire(&np->lock);
  
  np->base_priority = prior;
  np->priority = prior;
  uint xticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
  }
  else xticks = ticks;
  
    np->rticks = xticks;
    //printf("ready time: %d\n", np->rticks);
    np->wticks = xticks;
  np->state = RUNNABLE;
  release(&np->lock);
  if(batch_size == 0 ) {
    //printf("resetting stats\n");
    
    batch_sched= 0;
    batch_exec_time = 0;
    batch_start_time = 0;
    batch_tot_size = 0;
    avg_comp_time = 0;
    avg_turn_time = 0;
    max_comp_time = 0;
    avg_cpu_burst = 0;
    cpu_bursts = 0;
    max_cpu_burst = 0;
    batch_ready = xticks;
    //
    //seen_comp = 0;
    avg_wait_time = 0;
    avg_est_cpu_burst = 0;

    avg_turn_time = 0;
    //avg_wait_time;
    avg_comp_time = 0;
    min_comp_time = -1;
    max_comp_time = 0;
    //seen_comp = 0;
    cpu_bursts = 0;
    avg_cpu_burst = 0;
    max_cpu_burst = 0;
    min_cpu_burst = -1;
    //seen_cpu_burst = 0;
    diff_cpu_burst = 0;
    est_cpu_bursts = 0;
    avg_est_cpu_burst = 0;
    max_est_cpu_burst = 0;
    min_est_cpu_burst = -1;
    batch_ready = 0;
    cpu_burst_est_error = 0;
    submitjob_pid = -1;
    // sumbitjob_ended = 0;
    // submitjob_proc = 0;
    
  }
  submitjob_pid=p->pid;
  batch_size += 1;
  // printf("batch size : %d\n", batch_size);
  batch_tot_size += 1;
  return pid;
}


int getstats()
{
  printf("Batch execution time: %d\n", batch_exec_time);
  printf("Average turn-around time: %d\n", avg_turn_time / batch_tot_size );
  printf("Average waiting time: %d\n", avg_wait_time / batch_tot_size);
  printf("Completion time: avg: %d, max: %d, min: %d\n", avg_comp_time / batch_tot_size, max_comp_time, min_comp_time);
  //printf("CPU bursts: count: %")
  if(sched_policy == SCHED_NPREEMPT_SJF)  {
    printf("CPU bursts: count: %d, avg: %d, max: %d, min: %d\n", cpu_bursts, avg_cpu_burst / cpu_bursts, max_cpu_burst, min_cpu_burst);
    printf("CPU burst estimates: count: %d, avg: %d, max: %d, min: %d\n", est_cpu_bursts, avg_est_cpu_burst / est_cpu_bursts, max_est_cpu_burst, min_est_cpu_burst);
    printf("CPU burst estimation error: count: %d, avg: %d\n", cpu_burst_est_error, diff_cpu_burst / cpu_burst_est_error);

  }
  return 0;
}