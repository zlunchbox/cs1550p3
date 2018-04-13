#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;      
      #if LRU
      if (myproc() && myproc()->state == RUNNING)
        lru_update(myproc());
      #endif
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  // For Project 3 ******************************************************

  case T_PGFLT:;
    
    uint faddr = rcr2();
    struct proc* p = myproc();
    uint saddr;
    char buf[PGSIZE];
    pte_t* fpage, *spage;
    //int index;
    
    cprintf("page fault! - %s - 0x%x", p->name, faddr);
    //Check totalPages and kill process if appropriate
    if(p->totalPages >= MAX_TOTAL_PAGES) kill(p->pid);

    //Retrieve PTE and determine if paged out
    fpage = walkpgdir(p->pgdir, (void*) &faddr, 0);
    
    //If not, increment totalPages
    if(~(*fpage & PTE_PG)) p->totalPages++;
    
    //Check if totalPhysicalPages < MAX_PSYC_PAGES
    //If so, increment totalPhysicalPages and allocate
    //a new physical page
    if(p->totalPhysicalPages < MAX_PSYC_PAGES) {
      p->totalPhysicalPages++;
      allocuvm(p->pgdir, p->totalPhysicalPages * PGSIZE, 
               p->totalPhysicalPages * PGSIZE + PGSIZE);
      break;
    } else { //If not, select a victim, write out, and update
      spage = get_victim(p->pgdir);
      saddr = PTE_ADDR(*spage);
      memmove((void*) buf, (void*) saddr, PGSIZE); //Get physical contents of page

      index = add_page((void*) &saddr, p); //Find available index in swap file
      
      writeToSwapFile(p, buf, (uint) index * PGSIZE, PGSIZE);

      //Mark victim's PTE as not present and paged out
      *spage &= ~PTE_P;
      *spage |= PTE_PG;

      mappages(p->pgdir, (void*) &faddr, PGSIZE, saddr, 0);

      //Check if faulted address was swapped out and read in if so
      if (*fpage & PTE_PG) {
        index = remove_page((void*) &faddr, p);
        readFromSwapFile(p, buf, (uint) index * PGSIZE, PGSIZE);
        memmove((void*) &faddr, (void*) buf, PGSIZE);
      } else { //Otherwise set memory to 0
        memset((void*) &faddr, PGSIZE, 0);
      }
    }
    break;

  // ********************************************************************

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
