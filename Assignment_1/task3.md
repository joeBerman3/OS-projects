# Task 3 — Coroutine System Call (`co_yield`)

This document walks through everything we changed to implement Task 3
and **why** each change was needed. Read top-to-bottom; later sections
build on the protocol explained in earlier ones.

---

## 1. What the assignment asks for

Implement a new syscall:

```c
int co_yield(int pid, int value);
```

Semantics:
- Hand the CPU directly to process `pid`, passing it `value`.
- The target's matching `co_yield` call returns `value`.
- The caller sleeps until its peer yields back.
- On success: return the (positive) value the peer sent.
- On any error: return `-1`.

Error conditions:
- `pid <= 0`
- non-existent `pid`
- killed/zombie target
- self-yield (`pid == caller's pid`)

Hard constraints from the PDF:
- No modifications to `struct proc`, no new process states or fields.
- No new global or process-level kernel data structures.
- Do not break the existing scheduler/process model/syscalls.
- Run with `CPUS=1` — correctness only required on a single CPU.
- **Skip the `RUNNABLE` state entirely:** when the peer is already
  waiting, swtch directly to it, bypassing `scheduler()`.

---

## 2. Files touched (quick map)

| File | Role | What we added |
| --- | --- | --- |
| `kernel/syscall.h` | syscall number table | `#define SYS_co_yield 23` |
| `kernel/syscall.c` | dispatch table | extern decl + `[SYS_co_yield] sys_co_yield` |
| `kernel/defs.h` | kernel-internal prototypes | `sys_co_yield`, `co_yield` |
| `kernel/sysproc.c` | argument unpacking | `sys_co_yield()` wrapper |
| `kernel/proc.c` | **the real work** | `co_yield()` + one-line scheduler fix |
| `user/user.h` | user-space prototype | `int co_yield(int, int);` |
| `user/usys.pl` | user→kernel stub generator | `entry("co_yield");` |
| `user/co_test.c` | test program | ping-pong + error tests |
| `Makefile` | fs image contents | `$U/_co_test` in `UPROGS` |

`CPUS := 1` in the `Makefile` was already the default, so no change
needed there.

---

## 3. Syscall plumbing (the mechanical parts)

Every new xv6 syscall needs the same four-to-five boilerplate edits to
wire a user-space function call through the trap into kernel code.

### 3.1 `kernel/syscall.h`
```c
#define SYS_co_yield 23
```
Assigns the syscall a unique number. User-space `ecall` passes this
number in register `a7`; the kernel's dispatch table uses it as an
index.

### 3.2 `kernel/syscall.c`
```c
extern uint64 sys_co_yield(void);
...
[SYS_co_yield] sys_co_yield,
```
Declares the kernel-side handler and registers it at index
`SYS_co_yield` in the `syscalls[]` table that `syscall()` in the same
file indexes with `p->trapframe->a7`.

### 3.3 `kernel/defs.h`
```c
uint64 sys_co_yield(void);   // syscall wrapper
int    co_yield(int, int);   // actual logic, in proc.c
```
So `sysproc.c` and other kernel files can see both prototypes.

### 3.4 `kernel/sysproc.c`
```c
uint64 sys_co_yield(void) {
  int pid, value;
  argint(0, &pid);
  argint(1, &value);
  return co_yield(pid, value);
}
```
Unpacks the two `int` arguments from the user trapframe and delegates
to the real implementation in `proc.c`. This split matches xv6's
existing convention (e.g. `sys_kill`).

### 3.5 `user/user.h`
```c
int co_yield(int, int);
```
Lets user programs call the syscall like a regular C function.

### 3.6 `user/usys.pl`
```perl
entry("co_yield");
```
Generates the actual `ecall`-based assembly stub in `usys.S` at build
time. The stub is:
```
co_yield:
    li  a7, SYS_co_yield
    ecall
    ret
```

### 3.7 `Makefile`
```
UPROGS += $U/_co_test
```
Bundles the test binary into `fs.img` so it's available inside xv6.

None of these seven files contain interesting logic — they are the
standard xv6 recipe for adding a syscall. The real content is next.

---

## 4. The core implementation — `kernel/proc.c`

### 4.1 Constraint-driven design

The assignment's most demanding line:

> "You will need to manually change the state of both the yielding and
> target processes, **skip the RUNNABLE state entirely** and make the
> current CPU switch directly to the target process."

Combined with:
- no new fields on `struct proc`,
- no new globals,
- no breaking scheduler/syscalls.

That rules out the obvious "use `sleep()`/`wakeup()`" approach,
because `sleep()`/`wakeup()` go `SLEEPING → RUNNABLE → RUNNING` via
`scheduler()`. We need a direct `SLEEPING → RUNNING` transition.

We use only existing fields: `state`, `chan`, `trapframe->a0` (for
the payload and the eventual syscall return value), and `context`
(the save slot `swtch` uses). No struct changes, no globals.

### 4.2 The rendezvous protocol

Two branches:

**(a) Peer already waiting for us** — `t->state == SLEEPING && t->chan == p`:

1. `t->trapframe->a0 = value` — this becomes the peer's `co_yield`
   return value when `syscall()` later reads `p->trapframe->a0` for
   the restored syscall return.
2. `t->state = RUNNING` — skip `RUNNABLE`, as required.
3. `p->chan = t; p->state = SLEEPING` — we are now waiting for a
   future `co_yield(p, …)` from `t`.
4. `mycpu()->proc = t` — the CPU now points at the target (critical
   for the scheduler fix in §4.5).
5. Release `wait_lock`, then release our own `p->lock`.
6. `swtch(&p->context, &t->context)` — direct, process-to-process,
   bypassing `scheduler()`.
7. When we are later resumed, read `p->trapframe->a0` (the peer's
   payload) and return it.

**(b) Peer not yet ready** — anything else:

1. Release `t->lock`.
2. Set our own `p->chan = t` and `p->state = SLEEPING` **manually**
   (no call to `sleep()` — that was the user's preference and also
   the clean pattern).
3. Release `wait_lock`.
4. Call `sched()` with `p->lock` held — `sched()`'s precondition.
5. On resume, return `p->trapframe->a0` (either a direct swtch set
   it, or we were killed and `kill()` just flipped us to `RUNNABLE`).

In both branches, after resume we check `p->killed` and return `-1`
if set, so a kill during suspension surfaces as an error rather than
a bogus value.

### 4.3 Locking model

Three locks are relevant:

- `wait_lock` — a global lock already in xv6. We borrow it to
  serialize the whole rendezvous attempt. With only one rendezvous
  in flight at a time, we cannot race with another `co_yield`
  half-observing us mid-transition.
- `t->lock` — the target process's own lock. Held while we check
  `t->state`/`t->chan` and while we flip `t->state`.
- `p->lock` — our own lock. Protects our state transitions.

Acquire order: `wait_lock → t->lock → p->lock`. Consistent order to
avoid deadlock (redundant on single-CPU, but good hygiene).

### 4.4 Why we drop `p->lock` **before** `swtch` in the direct path

The usual xv6 convention (per book §7.3) is "caller of `swtch`
holds `target->lock` across `swtch`; caller's own lock is released
later by the `scheduler` thread that `swtch`ed into `scheduler`."

That convention **cannot** apply to our direct swtch:
- We are not `swtch`-ing into `scheduler`, so no scheduler thread is
  going to release our lock for us.
- If we kept `p->lock` held across `swtch`, then any future peer that
  tries to `co_yield(p, …)` to hand control back would block on
  `acquire(p->lock)` — and the only way `p->lock` would ever get
  released is if *we* resume, which requires that peer. **Deadlock.**

So we deliberately release `p->lock` right before `swtch`. The window
between `release(&p->lock)` and `swtch` has no other thread running
on this CPU (`CPUS=1`), and our state is already `SLEEPING`, so the
(other-CPU) scheduler wouldn't try to pick us anyway. Single-CPU
makes this safe.

`t->lock` stays held across `swtch`; by symmetry, `t`'s own code
releases it after `t` resumes (in its own `sched`-branch or
direct-branch epilogue — same pattern).

### 4.5 The companion fix in `scheduler()`

Original xv6 scheduler (simplified):

```c
for(p = proc; p < &proc[NPROC]; p++) {
  acquire(&p->lock);
  if(p->state == RUNNABLE) {
    p->state = RUNNING;
    c->proc = p;
    swtch(&c->context, &p->context);
    c->proc = 0;
  }
  release(&p->lock);       // <-- releases the SAME p it swtched to
}
```

The scheduler keeps a **local** `p` variable across its suspend
(`swtch`) and expects `p->lock` to be held when it is resumed. In
stock xv6 this invariant is automatic: the process that calls
`sched()` to return to `scheduler` is always the same `p` that
`scheduler` originally swtched to, and `sched()` requires that
process to hold its own lock.

With our direct swtches this invariant **breaks**:

1. `scheduler` swtches to child. (`scheduler`'s local `p = child`.)
2. Child `co_yield`s — direct swtch to parent. `scheduler` is still
   suspended with `p = child`.
3. Parent ping-pongs for a while, then calls `kill(child)` and
   `wait(0)`. `wait` calls `sleep`, which calls `sched`.
4. `sched` swtches into `scheduler`, which resumes with local
   `p = child` but the caller of `sched` was parent.
5. `scheduler` runs `release(&p->lock)` → releases **`child->lock`**,
   which no one holds.
6. `release` panics: `panic: release`.

The fix is one line. `mycpu()->proc` is already kept up-to-date by
our direct-switch code (`mycpu()->proc = t;`), so it is always the
process that actually just called `sched`. We use it:

```c
swtch(&c->context, &p->context);
p = c->proc;         // <-- NEW: re-read who actually returned
c->proc = 0;
```

Impact on normal (non-coroutine) code paths: none. In stock xv6
`c->proc == p` at the moment `scheduler` resumes, so `p = c->proc`
is a no-op.

### 4.6 Handling kills and edge cases

- `pid <= 0`, `pid == self` — rejected at the top of `co_yield`.
- PID not found in `proc[]` — return `-1` after releasing `wait_lock`.
- Target is `UNUSED`, `ZOMBIE`, or already has `killed == 1` — return
  `-1`. Catches the "killed target" error test.
- **We** get killed while suspended — `kill()` flips our state from
  `SLEEPING` to `RUNNABLE`; the scheduler eventually re-picks us; we
  resume at our `swtch`/`sched` return site, see `p->killed == 1`,
  and return `-1`.
- Target dies while we are suspended waiting for it — we do **not**
  attempt to wake in that case. Documented as an accepted
  non-handled edge case per the assignment ("choosing not to handle
  certain cases is a valid choice, as long as it is well-justified,
  does not break the system or the test code"). Not exercised by the
  supplied test.

---

## 5. The test program — `user/co_test.c`

Two parts:

### 5.1 Ping-pong (five rounds)

```c
int child = fork();
if(child == 0){
  for(;;){
    int v = co_yield(parent, 1);
    printf("child received: %d\n", v);
  }
}
for(int i = 0; i < 5; i++){
  int v = co_yield(child, 2);
  printf("parent received: %d\n", v);
}
kill(child); wait(0);
```

Expected: `parent received: 1` and `child received: 2` alternating,
five of parent's prints and four of child's (the fifth is pre-empted
by `kill` — benign).

### 5.2 Error tests

- `co_yield(99999, 7)` → non-existent pid → `-1`.
- `co_yield(getpid(), 8)` → self-yield → `-1`.
- `fork` + `kill(child)` + `co_yield(child, 9)` → killed target → `-1`.

---

## 6. How to run

From the repo root:

```
make qemu
```

Wait for `init: starting sh`, then at the `$` prompt:

```
co_test
```

Exit QEMU with `Ctrl-A` then `X`.

### Observed output (representative)

```
$ co_test
parent received: 1
child received: 2
parent received: 1
child received: 2
parent received: 1
child received: 2
parent received: 1
child received: 2
parent received: 1
error non-existent pid: -1
error self-yield: -1
error killed pid: -1
$
```

All required behaviors are present: bidirectional hand-off with
correct values, all three error cases return `-1`, and the shell
returns cleanly with no panic.

---

## 7. Summary of trade-offs and justifications

- **Direct swtch bypasses `scheduler`** — required by the assignment.
  We implement it by manually updating both processes' `state`,
  `chan`, and the CPU's `c->proc`, then calling `swtch` directly.
- **Released own lock before `swtch`** — deviates from the stock xv6
  convention but is necessary to avoid deadlock with future hand-off
  attempts. Safe because `CPUS=1`.
- **One-line `scheduler` change** — smallest possible fix to keep
  `scheduler`'s local view in sync with direct swtches. Does not
  affect standard flows (`c->proc == p` in those cases).
- **No new struct fields, no new globals** — respects the assignment's
  hard constraints. All per-process state lives in existing fields
  (`state`, `chan`, `trapframe->a0`, `context`).
- **Used `sched()` in the "peer not ready" branch, not `sleep()`** —
  avoids `sleep()`'s extra lock-manipulation ordering (it acquires
  `p->lock` then releases the arg lock in a fixed order), which would
  not match our protocol cleanly. `sched()` is the underlying
  primitive and gives us exactly what we need.
