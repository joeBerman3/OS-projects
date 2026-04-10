# Assignment 1 - Task 3 (Coroutine System Call)

## 1. Task Description
Task 3 requires implementing a coroutine-style syscall in xv6:

`int co_yield(int pid, int value);`

The syscall lets a process cooperate with a target process by:
- attempting a handoff to `pid`,
- passing `value` to the target,
- sleeping until it receives a value back from a matching `co_yield` call.

Required behavior from the assignment:
- On success: return the positive value passed by the peer process.
- On error: return `-1`.

Required error conditions:
- invalid PID (<= 0),
- non-existent PID,
- killed target process,
- self-yield (target PID is same as caller PID).

---

## 2. Files Changed For Task 3
Kernel syscall plumbing:
- `kernel/syscall.h`
- `kernel/syscall.c`
- `kernel/defs.h`
- `kernel/sysproc.c`
- `kernel/proc.c`

User-space syscall plumbing:
- `user/user.h`
- `user/usys.pl`

User-space test and build integration:
- `user/co_test.c`
- `Makefile`

---

## 3. Step-by-Step Implementation
### Step 1 - Add syscall number
In `kernel/syscall.h`:
- Added `SYS_co_yield` as syscall number `23`.

### Step 2 - Register syscall handler
In `kernel/syscall.c`:
- Added `extern uint64 sys_co_yield(void);`
- Added `[SYS_co_yield] sys_co_yield` to the syscall dispatch table.

### Step 3 - Add kernel prototypes
In `kernel/defs.h`:
- Added `uint64 sys_co_yield(void);`
- Added `int co_yield(int, int);`

### Step 4 - Add syscall wrapper
In `kernel/sysproc.c`:
- Implemented `sys_co_yield()`.
- Read syscall arguments using `argint(0, &pid)` and `argint(1, &value)`.
- Delegated logic to `co_yield(pid, value)`.

### Step 5 - Add kernel co_yield logic
In `kernel/proc.c`:
- Implemented `co_yield(int pid, int value)` with synchronization around `wait_lock` and process locks.
- Implemented rendezvous behavior using sleep/wakeup channels and process state checks.
- Enforced all required error checks.

### Step 6 - Add user-facing syscall declaration and stub
In `user/user.h`:
- Added `int co_yield(int, int);`

In `user/usys.pl`:
- Added `entry("co_yield");` to generate user syscall stub.

### Step 7 - Add test program and include in fs image
In `user/co_test.c`:
- Added ping-pong style parent/child test for cooperative yield.
- Added all required error tests.

In `Makefile`:
- Added `$U/_co_test` to `UPROGS` so it is built into `fs.img`.

---

## 4. Core Logic Explanation
### 4.1 Why wait_lock is used
`co_yield` uses `wait_lock` to serialize rendezvous and avoid missed wakeups between:
- checking peer state,
- sleeping,
- waking a matching peer.

### 4.2 Rendezvous protocol
Each `co_yield(pid, value)` call:
1. Validates arguments.
2. Locates target process by PID.
3. Verifies target is alive/not zombie.
4. Delivers `value` only when peer is actually waiting on caller channel.
5. Sleeps until reciprocal handoff is completed.
6. Returns received positive value.

### 4.3 Return semantics
- Success path returns a positive value received from peer.
- Any invalid/error condition returns `-1`.

---

## 5. Test Program Logic (`user/co_test.c`)
### Ping-pong test
- Parent and child repeatedly call `co_yield` against each other.
- Parent sends `2`, child sends `1`.
- Parent prints received value.
- Child prints received value.

### Error tests
- `co_yield(99999, 7)` => non-existent PID => expect `-1`.
- `co_yield(getpid(), 8)` => self-yield => expect `-1`.
- Fork child, kill it, then `co_yield(child_pid, 9)` => killed target => expect `-1`.

---

## 6. Validation Output (Observed)
Observed successful run (representative):

- `parent received: 1`
- `child received: 2`
- repeated across ping-pong iterations
- `error non-existent pid: -1`
- `error self-yield: -1`
- `error killed pid: -1`
- return to shell prompt `$`

This confirms functional behavior and required error handling for Task 3.

---
