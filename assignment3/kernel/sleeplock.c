// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, name);
  if(name == 0)
    name = "noname";
  else
    lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{

  if(holding(&lk->lk))
    printf("acquiresleep: %s: already holding",lk->name);
  acquire(&lk->lk);
  
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  if(holding(&lk->lk))
    printf("acquiresleep: %s: already holding",lk->name);
  acquire(&lk->lk);
  // printf("release sleep lock by %s\n", lk->name);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
  // printf("release sleep lock by %d\n", myproc()->pid);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}



