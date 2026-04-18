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
- Implemented `co_yield(int pid, int value)` using **direct process-to-process context switching** (via `swtch`) when the peer is already blocked waiting for us — bypasses `scheduler()` and skips the `RUNNABLE` state entirely, as the assignment requires.
- Does **not** use xv6's `sleep()` primitive. When the peer is not yet ready, we manually flip our own state to `SLEEPING`, set `p->chan`, and call `sched()` directly.
- Updates `mycpu()->proc = t` before the direct swtch, so `scheduler()` can identify which process is actually handing control back.
- Minor companion change in `scheduler()`: after `swtch` returns, release the lock of `c->proc` (which tracks the real returning process) instead of the stale local `p`. Without this, a direct-switch followed later by any call to `sched()` (e.g., from `wait`/`exit`) panics with `panic: release` because `scheduler`'s local `p` no longer matches the process whose lock is actually held.
- Enforces all required error checks: `pid <= 0`, self-yield, non-existent pid, killed/zombie target.

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
### 4.1 Locks used
- `wait_lock` serializes the whole rendezvous attempt (find target, check states, flip states) so a peer cannot half-observe us in transition.
- `t->lock` + `p->lock` protect each process's own state/chan/trapframe fields while we set them.

### 4.2 Rendezvous protocol — direct hand-off path
When `t->state == SLEEPING && t->chan == p` (peer is blocked waiting for us), we:
1. Write the payload into `t->trapframe->a0` (becomes the peer's `co_yield` return value).
2. Flip `t->state = RUNNING` and `p->state = SLEEPING`, skipping `RUNNABLE` entirely.
3. Set `p->chan = t` (so a later `co_yield(p, ...)` from `t` matches).
4. Update `mycpu()->proc = t` so the CPU and the scheduler agree on who is running.
5. Release `wait_lock` and our own `p->lock`. The target's lock stays held; by symmetry the target's own code will release it when it resumes.
6. Call `swtch(&p->context, &t->context)` — direct process-to-process context switch, bypassing `scheduler()`.
7. When we are later resumed (by a peer's direct swtch into us), `p->lock` is held for us and our peer has stored the incoming value in `p->trapframe->a0`. We read `killed` + `a0`, release `p->lock`, return.

Note on locking: we deliberately drop `p->lock` before `swtch`. The usual xv6 convention (hold own lock across `swtch`, let `scheduler` release it) does not apply — there is no scheduler thread in this path to release it for us, and keeping it held would deadlock any peer that later tries to `acquire(p->lock)` to hand control back. Single-CPU (`CPUS=1`) makes this safe: no other thread runs on this CPU between the release and the `swtch`.

### 4.3 Rendezvous protocol — peer not ready
If the target is not yet blocked waiting for us, we:
1. Release `t->lock`.
2. Set our own `p->chan = t` and `p->state = SLEEPING` manually (no call to `sleep()`).
3. Release `wait_lock`.
4. Call `sched()` with `p->lock` still held, per `sched()`'s precondition.
5. On resume — either a peer's direct swtch or the scheduler re-picking us after `kill()` — `p->lock` is held; we read `killed` + `a0`, release, return.

### 4.4 Scheduler companion change
`scheduler()` keeps a local `p` across its `swtch`. After a direct hand-off the process that later returns control via `sched()` is not the one scheduler originally swtched to, so the local `p` is stale. We replace scheduler's post-swtch `release(&p->lock)` target with `c->proc`, which co_yield keeps current. Standard (non-coroutine) flows are unaffected: `c->proc == p` in that case.

### 4.5 Return semantics
- Success: returns the positive value delivered by the peer.
- Invalid/error conditions (including being killed while suspended): `-1`.

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
