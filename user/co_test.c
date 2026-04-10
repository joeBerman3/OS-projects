#include "kernel/types.h"
#include "user/user.h"

static void
ping_pong_test(void)
{
  int parent = getpid();
  int child = fork();

  if(child < 0){
    printf("co_test: fork failed\n");
    exit(1);
  }

  if(child == 0){
    for(;;){
      int v = co_yield(parent, 1);
      printf("child received: %d\n", v);
    }
    exit(0);
  }

  for(int i = 0; i < 5; i++){
    int v = co_yield(child, 2);
    printf("parent received: %d\n", v);
  }

  kill(child);
  wait(0);
}

static void
error_tests(void)
{
  int r;
  int me = getpid();

  r = co_yield(99999, 7);
  printf("error non-existent pid: %d\n", r);

  r = co_yield(me, 8);
  printf("error self-yield: %d\n", r);

  int child = fork();
  if(child < 0){
    printf("co_test: fork for killed test failed\n");
    exit(1);
  }

  if(child == 0){
    sleep(1000);
    exit(0);
  }

  kill(child);
  r = co_yield(child, 9);
  printf("error killed pid: %d\n", r);
  wait(0);
}

int
main(int argc, char *argv[])
{
  ping_pong_test();
  error_tests();
  exit(0);
}