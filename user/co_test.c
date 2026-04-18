#include "kernel/types.h"
#include "user/user.h"

static void
ping_pong_test(void)
{
  int parent = getpid(); // get our own pid to use as the "target" of our co_yield calls, since we want to yield to each other, and the child will inherit the parent's pid, so both can use the same pid to yield to each other
  int child = fork(); // fork a child to yield to. The child will inherit the parent's pid, so both can use the same pid to yield to each other

  if(child < 0){ // fork failed
    printf("co_test: fork failed\n");
    exit(1);
  }

  if(child == 0){ // child: yield to parent in a loop, sending a different value each time, and print what we receive back from the parent. After the loop, exit to end the test.
    for(;;){
      int v = co_yield(parent, 1);
      printf("child received: %d\n", v);
    }
    exit(0);
  }

  for(int i = 0; i < 5; i++){ // parent: yield to child in a loop, sending a different value each time, and print what we receive back from the child. After the loop, kill the child and wait for it to exit to finish the test.
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
  int me = getpid(); // get our own pid to use as the "target" of our co_yield calls, since we want to test error cases like yielding to a non-existent pid, yielding to ourselves, and yielding to a killed pid, and in the last case we can kill ourselves to get a valid killed pid to test with

  r = co_yield(99999, 7); // try to yield to a non-existent pid, and print the result, which should be an error
  printf("error non-existent pid: %d\n", r);

  r = co_yield(me, 8); // try to yield to ourselves, and print the result, which should be an error
  printf("error self-yield: %d\n", r);

  int child = fork();
  if(child < 0){ // fork failed
    printf("co_test: fork for killed test failed\n");
    exit(1);
  }

  if(child == 0){ // child: kill ourselves to get a valid killed pid to test with, then try to yield to our own pid, which should
    sleep(1000);
    exit(0);
  }

  kill(child);
  r = co_yield(child, 9); // try to yield to a killed pid, and print the result, which should be an error
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