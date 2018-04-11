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

  case T_PGFLT:
     cprintf("page fault! - %s - 0x%x", myproc()->name, rcr2());
     // A number of things should be checked upon page fault
     // (1) Check totalPages of the calling process
     //	   (i) If totalPages >= MAX_TOTAL_PAGES, kill the process
     //	(2) Check if the faulting address is swapped out
     //    (i) If not, increment totalPages
     // (3) Check totalPhysicalPages of the calling process
     //    (i) If totalPhysicalPages is less than MAX_PSYC_PAGES,
     //	       increment and allocate a new physical page
     //    (ii) Otherwise, select a victim page and swap out
     // (4) If the faulting address was swapped out, read it back in.

     struct proc *curproc = myproc();
     void* addr = (void*) rcr2();
     char buf[PGSIZE];
     pte_t *fpage, *spage;
     int index;

     // Check totalPages and kill process if appropriate
     if(curproc->totalPages >= MAX_TOTAL_PAGES) kill(curproc->pid);

     // Retrieve PTE and determine if paged out
     fpage = walkpgdir(curproc->pgdir, addr, 0);

     // If not, increment totalPages
     if(!(*fpage & PTE_PG)) curproc->totalPages++;

     // Check if totalPhysicalPages < MAX_PSYC_PAGES
     // If so, increment totalPhysicalPages and allocate
     // a new physical page
     if(curproc->totalPhysicalPages < MAX_PSYC_PAGES) {
	curproc->totalPhysicalPages++;
     }
     // If not, select a victim page, write out, and update
     else {
	spage = get_victim(); 				 	   // Select victim page
	memmove((void*) buf, (void*) PTE_ADDR(*spage), PGSIZE); // Get physical contents of page
	index = add_page(addr, curproc);			   // Find available index in swap file
	writeToSwapFile(curproc, buf, index, PGSIZE);		   // Write physical memory contents to swap file
	
	// Mark victim's PTE as not present and paged out
	*spage &= ~PTE_P;
	*spage |= PTE_PG;

	// Call mappages()
	mappages(curproc->pgdir, addr, PGSIZE, PTE_ADDR(*spage), 0);

	// Check if the faulted address was swapped out
	// If so, read in from swap file.
	if(*fpage & PTE_PG) {
	   index = remove_page(addr, curproc);
	   readFromSwapFile(curproc, buf, index, PGSIZE);
	   memmove((void*) PTE_ADDR(*fpage), (void*) buf, PGSIZE);
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
