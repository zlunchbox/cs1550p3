#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"

#define PGSIZE 4096

int main(int argc, char* argv[]) {
  int i, j;
  char* arr[18];
  char input[10];
  
  for (i = 0; i < 16; i++) {
    arr[i] = sbrk(PGSIZE);
    printf(1, "arr[%d]=0x%x\n", i, arr[i]);
  }
  arr[14] = sbrk(PGSIZE);
  printf(1, "arr[14]=0x%x\n", arr[12]); //Page fault should occur

  arr[15] = sbrk(PGSIZE);
  printf(1, "arr[15]=0x%x\n", arr[13]); //Page fault

  for (i = 0, i < 5; i++) {
    for (j = 0; j < PGSIZE; j++) arr[i][j] = 'q';
  }

  if (fork() == 0) {
    printf("Child\n");
    arr[5][0] = 'w';
    exit();
  } else {
    wait();
    sbrk(-16 * PGSIZE);
  }
  exit();
}
