# Task 3 — Extended Q&A (understanding the implementation)

Companion document to `task3.md`. Walks through every question asked
while studying the Task 3 code, in the order they were asked. Focus
is on **understanding** — not just what the code does, but *why* it
has to look that way. Read `task3.md` first for the top-down summary,
then this one for the bottom-up, question-driven walk-through.

---

## Table of contents

1. [Syscall plumbing — what each file does](#1-syscall-plumbing)
    1. [What is the dispatch table in `kernel/syscall.c`?](#11-dispatch-table)
    2. [What are "kernel-internal prototypes" in `kernel/defs.h`?](#12-kernel-internal-prototypes)
    3. [What does `sys_co_yield` in `kernel/sysproc.c` do?](#13-sys_co_yield)
    4. [When user calls `co_yield`, does `sys_co_yield` hold the pid/value from the real function?](#14-userkernel-flow)
    5. [What is the role of `user/usys.pl`?](#15-usyspl)
    6. [How does `user/usys.S` relate to our `co_yield`?](#16-usyss)
    7. [What would happen if we didn't use `usys.S`?](#17-without-usyss)
    8. [What is `ecall`?](#18-ecall)
2. [Process model background](#2-process-model-background)
    1. [What is the `chan` field?](#21-chan-field)
3. [Walking through `co_yield` in `kernel/proc.c`](#3-walking-through-co_yield)
    1. [The opening validation and `wait_lock` acquire](#31-opening-validation)
    2. [Locating the target in the proc table](#32-locating-target)
    3. [The liveness gate (`UNUSED`/`ZOMBIE`/`killed`)](#33-liveness-gate)
    4. [The direct hand-off branch](#34-direct-handoff-branch)
    5. [The peer-not-ready branch](#35-peer-not-ready-branch)
4. [What `swtch` and `sched` actually are](#4-swtch-and-sched)

---

## 1. Syscall plumbing

### 1.1 Dispatch table

> **Q:** In `kernel/syscall.c`, what is a dispatch table and what does it mean?

#### What it is

Look at `kernel/syscall.c:109-133`:

```c
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
...
[SYS_co_yield] sys_co_yield,
};
```

`syscalls[]` is an **array of function pointers**. Each slot holds
the address of a kernel function. The slot *index* is the syscall
number (`SYS_fork = 1`, `SYS_exit = 2`, …, `SYS_co_yield = 23`).

The `[SYS_fork] sys_fork` syntax is C's "designated initializer":
put `sys_fork` at index `SYS_fork` in the array.

#### What "dispatch" means

"Dispatch" = pick the right function to run based on some input.
Here the input is the syscall number, and the "right function" is
whichever kernel handler implements that syscall.

#### How it's used

```c
void syscall(void) {
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;              // user put syscall # in a7
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num](); // look up + call, store return
  } else {
    printf("... unknown sys call %d\n", ...);
    p->trapframe->a0 = -1;
  }
}
```

Flow when user calls `co_yield(...)`:

1. User stub does `li a7, 23; ecall`.
2. `ecall` traps into the kernel → `usertrap` → `syscall()`.
3. `syscall()` reads `a7` (= 23) from the saved user trapframe.
4. **Dispatch:** `syscalls[23]` → `sys_co_yield`. It calls it.
5. Return value goes back into `trapframe->a0`, which becomes `a0`
   in user-space when the trap returns — that's the syscall's return.

#### Why a table instead of a switch

You *could* write:

```c
switch(num) {
  case SYS_fork: return sys_fork();
  case SYS_exit: return sys_exit();
  ...
}
```

Functionally equivalent, but the table is:
- **O(1)** lookup instead of a branch chain.
- **Extensible** — adding a syscall is just adding an entry.
- **Data-driven** — the control flow is encoded in an array, not
  in code, which is a common OS pattern (interrupt vectors, VFS
  operations, device drivers all use the same trick).

#### Mental model

Think of it as a phone directory:
- The **syscall number** is the phone number.
- The **array** is the directory.
- `syscalls[num]()` is "look up that number and call it."

This is the same idea as C++ vtables, Linux's `sys_call_table`, and
x86 IDT (interrupt descriptor table): a fixed-size array of function
pointers indexed by some small integer.

---

### 1.2 Kernel-internal prototypes

> **Q:** What are "kernel-internal prototypes" in `kernel/defs.h`?

#### What a prototype is

In C, a **prototype** (forward declaration) is a line like:

```c
int co_yield(int, int);
```

It tells the compiler: *"a function named `co_yield` exists somewhere,
takes two `int`s, and returns `int`."* No body — just the signature.
The actual implementation lives in some `.c` file.

**Why you need them:** C compiles one file at a time. When the
compiler is building `sysproc.c` and hits `co_yield(pid, value)`,
it has never seen `co_yield` before — the definition is in `proc.c`,
a different compilation unit. Without a prototype the compiler would
either error out or guess wrong about arg/return types.

#### "Kernel-internal" part

`defs.h` is the header shared **between kernel `.c` files only**.
Compare:

| Header | Visible to | Purpose |
| --- | --- | --- |
| `kernel/defs.h` | other kernel `.c` files | prototypes for functions kernel code calls |
| `user/user.h` | user-space programs | prototypes user apps need (libc + syscalls) |
| `kernel/syscall.h` | both | just syscall number `#define`s |

"Kernel-internal" = "not exposed to user space." User programs
`#include "user/user.h"`, not `kernel/defs.h`.

#### Concretely, what we added

```c
int             co_yield(int, int);          // in the proc.c group
uint64          sys_co_yield(void);          // in the syscall.c group
```

- `co_yield` — needed so `sys_co_yield` in `sysproc.c` can call
  the real implementation in `proc.c`.
- `sys_co_yield` — needed so `syscall.c` can register it in the
  dispatch table.

#### Mental model

`defs.h` is the kernel's "table of contents": a flat list grouped
by source file. When you add a kernel function that another kernel
file needs to call, you add its signature to `defs.h` under the
right section. That's the whole convention.

---

### 1.3 `sys_co_yield`

> **Q:** What does `sys_co_yield` in `kernel/sysproc.c` do?
>
> ```c
> uint64 sys_co_yield(void) {
>   int pid, value;
>   argint(0, &pid);
>   argint(1, &value);
>   return co_yield(pid, value);
> }
> ```

#### Role

`sys_co_yield` is the **kernel-side entry point** for the `co_yield`
syscall — the bridge between "user called a syscall" and "the real
implementation runs." Every xv6 syscall has one (see `sys_fork`,
`sys_kill`, etc. in the same file).

The dispatch table calls `sys_co_yield()` with no arguments. But our
real logic, `co_yield(int pid, int value)` in `proc.c`, needs two.
This wrapper's job is to **pull those two arguments out of the
user's trapframe** and hand them to `co_yield`.

#### Why the wrapper takes no arguments

Every entry in the dispatch table has the same type:
`uint64 (*)(void)`. Uniformity is what lets `syscalls[num]()` work
for every syscall. The syscall number is the only thing passed by
the calling convention; the *arguments* are fetched separately
from saved user registers.

#### Line-by-line

```c
int pid, value;
```
Local kernel variables that will hold the unpacked arguments.

```c
argint(0, &pid);
argint(1, &value);
```

`argint(n, &x)` reads **the n-th syscall argument as an int** out of
the saved user trapframe. From `kernel/syscall.c:33-60`:

```c
static uint64 argraw(int n) {
  struct proc *p = myproc();
  switch (n) {
  case 0: return p->trapframe->a0;
  case 1: return p->trapframe->a1;
  ...
  }
}

void argint(int n, int *ip) { *ip = argraw(n); }
```

So `argint(0, &pid)` reads `trapframe->a0`, and
`argint(1, &value)` reads `trapframe->a1`. When user called
`co_yield(pid, value)`, the RISC-V calling convention put those
two args in registers `a0` and `a1`, and the trap machinery saved
user registers into the trapframe on entry.

```c
return co_yield(pid, value);
```

Calls the real implementation in `proc.c` with the unpacked args,
and returns whatever it returns. That return value becomes
`trapframe->a0` in `syscall()`:

```c
p->trapframe->a0 = syscalls[num]();   // syscall.c:145
```

…which becomes `a0` in user space when the trap returns.

---

### 1.4 User→kernel flow

> **Q:** So when user calls `co_yield`, does `sys_co_yield` hold the
> pid/value from the actual `co_yield` function?

Close, but there are actually **two** `co_yield` functions with the
same name:

1. The **user-space stub** `co_yield` (generated from `usys.pl`) —
   runs in user mode.
2. The **kernel function** `co_yield(int pid, int value)` in
   `proc.c` — runs in kernel mode.

`sys_co_yield` sits between them. Here's the full timeline:

```
USER MODE                                      KERNEL MODE
─────────                                      ───────────
int r = co_yield(5, 2);
   │ (normal C call, RISC-V ABI puts
   │  5 in register a0, 2 in a1)
   ▼
co_yield:                (user stub)
  li a7, 23              put syscall# in a7
  ecall  ─────── trap ──▶ usertrap()
                            │ (saves a0,a1,a7,… from
                            │  user CPU regs into p->trapframe)
                            ▼
                          syscall()
                            │ reads p->trapframe->a7 = 23
                            │ calls syscalls[23]()
                            ▼
                          sys_co_yield()       ← NOW this runs
                            int pid, value;
                            argint(0, &pid);   → reads trapframe->a0 = 5
                            argint(1, &value); → reads trapframe->a1 = 2
                            return co_yield(pid, value);
                                       │
                                       ▼
                                     co_yield() in proc.c   ← real work
                                       (the direct-switch logic)
                                       returns some int
                            ◀───────── back to sys_co_yield
                          ◀──────── back to syscall(), stores
                                    return in trapframe->a0
◀──── trap returns ──── usertrapret()
int r = …;  // r is whatever co_yield returned
```

So: **yes, `sys_co_yield` is the middleman.** Its only job is:

1. **Pull** `pid` and `value` out of the trapframe (where the trap
   code stashed them from the user's registers).
2. **Call** the real kernel `co_yield(pid, value)` with those values.
3. **Return** whatever that returns.

The trapframe is the key. You can think of it as a "frozen photo"
of the user's CPU registers taken the instant the trap happened.
All the user's args sit in that photo, and `argint` is how kernel
code develops the photo to read specific registers.

One subtlety: the user-space stub **doesn't need to do anything**
with the args — the RISC-V calling convention already put them in
`a0`/`a1` by the time the stub starts. The stub only needs to add
the syscall number in `a7` and `ecall`.

---

### 1.5 `usys.pl`

> **Q:** What is the role of `user/usys.pl`?

#### Role

`usys.pl` is a **code generator**. It's a Perl script that, at
build time, **writes another file** — `user/usys.S` — which contains
the user-space assembly stubs for every syscall.

It's not code that runs on xv6. It runs on your Linux host during
`make`.

#### The problem it solves

For every syscall, user-space needs a tiny function like:

```asm
co_yield:
    li  a7, SYS_co_yield   # put syscall number in a7
    ecall                  # trap into kernel
    ret                    # return to caller
```

Each syscall's stub differs only in **the name** and **the number**.
Writing 24 nearly-identical assembly functions by hand is tedious
and error-prone — so xv6 uses a script to generate them.

#### How it works

```perl
print "# generated by usys.pl - do not edit\n";
print "#include \"kernel/syscall.h\"\n";

sub entry {
    my $name = shift;
    print ".global $name\n";
    print "${name}:\n";
    print " li a7, SYS_${name}\n";
    print " ecall\n";
    print " ret\n";
}

entry("fork");
entry("exit");
...
entry("co_yield");
```

Each `entry("fork")` call makes the script **print** four lines of
assembly for that syscall. The output of running the whole script
is `usys.S`.

#### Where it plugs into the build

From the Makefile:

```
$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S
```

Chain:

```
usys.pl  ──perl──▶  usys.S  ──gcc -c──▶  usys.o  ──ld──▶  _co_test
(source)           (generated)          (object)         (user binary)
```

#### Why Perl and not C

Any scripting language would work — Perl is just what MIT used
historically. The script is doing pure text templating; nothing
about it is Perl-specific.

#### What changes when you add a syscall

Adding `entry("co_yield");` is enough — `make` re-runs the script,
the new stub appears in `usys.S`, `usys.o` gets rebuilt, and every
user program can call `co_yield` like any other C function.

---

### 1.6 `usys.S`

> **Q:** How does `user/usys.S` relate to our `co_yield`?

#### It's the generated file

`usys.S` is the output of running `perl usys.pl` during `make`.
The comment on line 1 says exactly that:
`# generated by usys.pl - do not edit`.

#### The `co_yield` chunk

```asm
.global co_yield        # make the symbol visible to the linker
co_yield:               # label = start of this function in memory
    li a7, SYS_co_yield # load immediate: put the syscall number (23) into a7
    ecall               # trap into the kernel
    ret                 # return to whoever called us
```

Four instructions. That's the **entire** user-space implementation
of `co_yield`. There is no user-space logic — it's a shim whose only
job is to trigger the trap with the right syscall number.

#### How it plugs into your test program

When `co_test.c` does:

```c
int v = co_yield(parent, 1);
```

The C compiler doesn't know or care that `co_yield` is a syscall.
It generates a plain function call, respecting the RISC-V calling
convention:

```asm
li   a0, <parent's pid>    # first arg → a0
li   a1, 1                 # second arg → a1
call co_yield              # ra = return address; jump to co_yield
```

Execution lands on the label `co_yield:` in `usys.S`. At this moment:
- `a0` already holds `parent`  ← placed by caller
- `a1` already holds `1`        ← same
- `ra` holds the return address inside `co_test`

Then the four-instruction stub runs:

| instr | effect |
| --- | --- |
| `li a7, SYS_co_yield` | `a7 = 23` — tells the kernel which syscall |
| `ecall` | trap! CPU jumps to the kernel's trap vector |
| `ret` | **after kernel returns.** `a0` now contains the return value; `ret` jumps back to `ra` |

#### What makes `co_yield` work

Three things all need to agree on the number **23**:

1. `#define SYS_co_yield 23` in `kernel/syscall.h`.
2. The dispatch-table entry actually pointing to `sys_co_yield`.
3. `sys_co_yield` actually calling the real `co_yield` in `proc.c`.

If any of those three goes out of sync — wrong number, missing entry,
wrong function — the user's call either triggers the wrong kernel
handler or panics with "unknown sys call 23."

#### Mental model

`usys.S` is the **thinnest possible layer**: user code thinks it's
calling a C function, but the "function" is really four lines of
assembly whose only purpose is to tell the kernel "here's a syscall
with this number, arguments are where you'd expect." Everything
else is the kernel's problem.

---

### 1.7 Without `usys.S`

> **Q:** If we didn't use `usys.S`, what would happen?

#### Short answer

The build would fail with a **linker error**, and even if you hacked
around that, user programs would have no way to actually trigger a
syscall.

#### Failure mode #1 — linker error

When `co_test.c` compiles, the C compiler sees `co_yield(parent, 1)`
and emits a `call co_yield` instruction. But the compiler has no
definition — it emits an **unresolved external reference** to the
symbol `co_yield`. That symbol has to be defined *somewhere* in an
object file linked into the user program.

If `usys.S` doesn't exist, `make qemu` fails with:

```
riscv64-linux-gnu-ld: user/co_test.o: in function `main':
co_test.c:(.text+0x...): undefined reference to `co_yield'
```

Same for every other syscall. Every user program fails to link.
`_sh` wouldn't exist. Nothing runs.

#### Failure mode #2 — even if hand-rolled, you still need the trap

Suppose you wrote the stub yourself in C:

```c
int co_yield(int pid, int value) {
    // ??? how do I trap into the kernel from C?
}
```

You **can't** — `ecall` is a privileged RISC-V instruction, not a
C operation. C has no way to express "execute `ecall`." You'd need:
- **Inline assembly** in a C file — works, but you've just
  re-implemented `usys.S` in uglier syntax.
- Or an **actual `.S` file** defining the stub — which is exactly
  what `usys.S` is.

"Not using `usys.S`" really means "writing the same assembly in a
different file." You cannot eliminate the layer — you can only
relocate it.

#### Why this layer *must* exist

The user↔kernel boundary is a **privilege boundary** enforced by
the CPU. Three things must happen to cross it:

1. **Tell the kernel which syscall you want** — load a number into
   a specific register (`a7` on RISC-V).
2. **Execute the trap instruction** — `ecall`.
3. **Arguments in the right registers** — which the C calling
   convention guarantees (`a0`, `a1`, …).

Step 1 and step 2 cannot be done from plain C. So *somewhere* in
the toolchain there has to be assembly that does it. `usys.S` is
that assembly. Removing it removes the only bridge from user-mode
C code to kernel-mode execution.

#### Design takeaway

- **Kernel side** of `co_yield` (the real logic in `proc.c`) is big
  and interesting.
- **User side** (in `usys.S`) is 4 lines of boilerplate and could be
  anywhere, as long as it exists.

That's deliberate. OS designers keep the user-side stub as thin as
possible so that user code looks like it's calling normal C
functions, while **all the policy and enforcement lives on the
kernel side of the trap**.

---

### 1.8 `ecall`

> **Q:** What is `ecall`?

#### Short answer

`ecall` = "**e**nvironment **call**." It's a single RISC-V CPU
instruction whose only job is to **deliberately trigger a trap** —
forcing the CPU to switch from user mode into supervisor (kernel)
mode and jump to the kernel's trap handler. Without it, there is
no way to invoke a syscall.

#### What the CPU actually does

Running this single instruction in user mode causes the hardware to:

1. **Save the current program counter** into `sepc` (so the kernel
   can resume you after the trap).
2. **Record why the trap happened** by writing a code (`8` =
   "ecall from user mode") into `scause`.
3. **Save the current privilege bits** into `sstatus`.
4. **Switch the CPU to supervisor mode**.
5. **Jump to the address in `stvec`** — which the kernel set up at
   boot to point at its trap vector.

That's it. The CPU does not know or care about "syscalls," arguments,
or `co_yield`. All it does is "promote me to kernel mode and jump
to the registered trap handler." Everything else — figuring out
which syscall was requested, reading arguments, dispatching — is
*software* that the kernel runs **after** `ecall` lands.

#### Why a special instruction is needed

User programs run in a restricted CPU mode that **cannot**:
- access kernel memory,
- disable interrupts,
- touch hardware registers,
- execute privileged instructions.

If user code could just `jal kernel_function` to any kernel address,
the kernel's protection would be meaningless. So the CPU has
exactly **one** controlled door from user to kernel: trap
instructions like `ecall`. When you walk through that door, the
CPU forces you to land at a specific address the kernel picked —
not anywhere you want.

The complementary instruction is `sret` ("supervisor return"), which
the kernel uses on the way back out: it restores user mode, puts
the PC back at `sepc`, and resumes your user code.

#### The `ecall` in `co_yield`, end to end

```
user mode                              kernel mode
─────────                              ───────────
a0 = pid, a1 = value, a7 = 23
        │
        ▼
      ecall  ───── HARDWARE ─────▶  uservec (trampoline.S)
                   (sepc ← PC)         │ saves all user regs to trapframe
                   (scause ← 8)        │ switches to kernel page table
                   (mode → S)          │ loads kernel stack
                   (PC ← stvec)        ▼
                                     usertrap()
                                       │ scause == 8 → it's a syscall
                                       │ epc += 4 (skip the ecall)
                                       ▼
                                     syscall()
                                       │ reads trapframe->a7 = 23
                                       │ dispatches to sys_co_yield
                                       ▼
                                     sys_co_yield()
                                       ...work happens...
                                       returns value
                                     syscall() stores it in trapframe->a0
                                     usertrap() returns
                                     usertrapret()
                                       restores registers from trapframe
                                       executes `sret`
◀──── HARDWARE ───── (mode → U, PC ← sepc)
`ret` in the stub runs
```

#### Why add 4 to `epc`?

`kernel/trap.c` does `p->trapframe->epc += 4` in the syscall branch.
`ecall` is a 4-byte instruction; `sepc` stored the address **of**
the ecall itself. If you restored user PC to that address and
resumed, you'd re-execute `ecall` forever. Adding 4 skips past it.

#### `ecall` vs other kinds of traps

`ecall` is a **synchronous, intentional** trap. Contrast:

| Trap source | Cause | `scause` |
| --- | --- | --- |
| `ecall` from user | syscall request | 8 |
| Page fault | bad memory access | 13/15 |
| Illegal instruction | unknown opcode | 2 |
| Timer interrupt | hardware clock | (high bit set, code 5) |
| Device interrupt | UART / disk | (high bit set, code 9) |

`usertrap()` uses `scause` to tell them apart.

#### Mental model

Think of `ecall` as a **doorbell** on the kernel's front door:
- The door is locked from the outside (user mode can't cross it).
- The doorbell (`ecall`) is the one button user code is allowed
  to press.
- Pressing it summons the kernel (trap handler) to the door.
- User code doesn't decide who answers or what happens next —
  the kernel does.
- The kernel comes back out when it's done (`sret`) and resumes
  user code at PC `+ 4`.

That controlled handoff — one CPU instruction that forces an
architecturally-defined entry into kernel code — is the entire
foundation of user/kernel isolation on RISC-V.

---

## 2. Process model background

### 2.1 `chan` field

> **Q:** What is the `chan` field?

#### What it is

From `kernel/proc.h:90`:

```c
struct proc {
  struct spinlock lock;
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  ...
};
```

`chan` is a single pointer-sized field on every process. It means
**"if this process is SLEEPING, what is it waiting *for*?"** The
value is just an opaque pointer — sleep/wakeup code treats it as a
nameless identifier, never dereferences it.

#### How sleep/wakeup uses it

```c
void sleep(void *chan, struct spinlock *lk) {
  ...
  p->chan = chan;            // "I'm waiting for this thing"
  p->state = SLEEPING;
  sched();
  p->chan = 0;               // cleared on wake
  ...
}

void wakeup(void *chan) {
  for each proc p:
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;   // match: wake
}
```

So `chan` is the glue between "go to sleep waiting for X" and
"wake everyone waiting for X". The pointer is just a token —
both sides have to pass the *same* value for the match to happen.

#### What kinds of pointers people pass

Anything whose address is stable and meaningful will do. Common
xv6 uses:

| Caller | `chan` value | Meaning |
| --- | --- | --- |
| `wait()` | `p` (the parent proc) | "waiting for a child to exit" |
| Pipe read | `&pi->nread` | "waiting for bytes to be written" |
| Console read | `&cons.r` | "waiting for keyboard input" |
| `sleep(n)` syscall | `&ticks` | "waiting for clock tick" |
| Disk I/O | `&b->data` | "waiting for buffer to load" |

Notice: **the pointer doesn't have to point at anything related to
the wait**. It's just a unique ID. `&ticks` is used because "the
ticks variable" is a convenient globally-unique address.

#### Why it's `void *` and not `int`

1. **Addresses are naturally unique** — any allocated struct, any
   global variable, its address cannot collide with any other live
   one. You don't have to centrally assign channel numbers.
2. **Semantic locality** — the "what you're waiting for" usually
   already exists as an object. Using its address as the channel
   means the waker doesn't need to look up a number.

#### How `co_yield` uses `chan`

We follow the same convention:

```c
p->chan = t;             // I'm waiting for process t to yield to me
p->state = SLEEPING;
```

`t` is a `struct proc *` — a perfectly good unique pointer. When
`t` later calls `co_yield(p_pid, val)`, it checks:

```c
if(t->state == SLEEPING && t->chan == p)   // p is the caller
```

i.e. "is my target asleep, *waiting specifically for me*?" If yes,
direct hand-off. The `chan` field is what lets us distinguish
"waiting for me" from "waiting for some third process."

This is where our use *departs* slightly from stock xv6: normally
many processes share a channel. For `co_yield` we intentionally make
the channel **point-to-point** by using the counterparty's proc
pointer — a pair of processes agreeing on each other's addresses.

#### Why `chan` is protected by `p->lock`

Reason: checking `chan` and checking `state` must be **atomic** in
`wakeup`:

```c
if (p->state == SLEEPING && p->chan == chan)
  p->state = RUNNABLE;
```

Without the lock, a racing `sleep` could flip state and chan
mid-check, and `wakeup` might miss a sleeper (the classic "lost
wakeup" bug — book §7.5).

#### Mental model

`chan` is a **bulletin board tag**. When a process goes to sleep,
it pins a tag on itself saying "wake me when this event happens" —
and the tag is just the address of some object. When an event
happens, whoever triggers it calls `wakeup(addr_of_that_object)`,
which scans the proc table and wakes everyone whose tag matches.

One pointer field replaces what would otherwise be a wait-queue
data structure per event. The tradeoff: `wakeup` has to scan all
procs (`O(NPROC)` each time), but `NPROC` is small in xv6.

---

## 3. Walking through `co_yield`

### 3.1 Opening validation

> **Q:** What do these lines at the beginning of `co_yield` mean?
>
> ```c
>   if(pid <= 0 || pid == p->pid)
>     return -1;
>
>   acquire(&wait_lock);
> ```

#### `if(pid <= 0 || pid == p->pid) return -1;`

**Input validation** — the first thing any syscall should do.
Reject obvious garbage before touching any shared state or taking
any locks.

Two conditions:

- **`pid <= 0`** — PIDs in xv6 start at 1 (`allocpid()`:
  `nextpid = 1`). Zero and negatives are never valid PIDs.
- **`pid == p->pid`** — self-yield. The assignment explicitly
  forbids it ("a process cannot yield to itself"). Beyond
  nonsensical, allowing it would cause ugly edge cases (acquire
  own lock twice, check `t->chan == p` where `t == p`, etc.).

Both conditions short-circuit with `||`: if either is true,
**return `-1` immediately**. No locks acquired, no side effects.

`p` here is `myproc()` — saved at the top of the function into a
local so we don't keep calling it.

#### `acquire(&wait_lock);`

Now we start real work. The very first real action is to **take a
lock**.

`wait_lock` is a global spinlock that already exists in stock xv6
(originally used for `wait`/`exit`/`fork` parent-child
synchronization). We borrow it for `co_yield`. Taking it gives:

1. **Mutual exclusion of rendezvous attempts.** Any other `co_yield`
   call blocks on `acquire(&wait_lock)` until we release. Our
   "find target → check state → flip states → swtch" sequence is
   serialized against other `co_yield`s.
2. **Interrupts disabled on this CPU** while the lock is held
   (`acquire` does `push_off()`). Prevents a timer interrupt from
   pre-empting us mid-transition.

#### Why the order matters

We validated **before** acquiring the lock:

- If we took the lock first, a bogus `pid` would cause us to
  acquire-then-release a lock for nothing.
- Discipline: locks should protect **real work**. Cheap checks that
  can't fail due to concurrent state don't need the lock.

Symmetrically, we must `release(&wait_lock)` on every exit path
once acquired. Forgetting this is a classic xv6 bug.

#### Why `wait_lock` specifically, not a new lock

The assignment forbids new global data structures. `wait_lock` is
the natural existing choice:

- Already serializes rare, coordinated events across the proc table.
- Acquired before per-process locks in stock xv6 convention —
  matches our desired order (`wait_lock` → `t->lock` → `p->lock`).
- Holding it for a short rendezvous doesn't hurt `wait`/`exit`
  performance noticeably.

#### Mental model

Standard "fail fast, then start the transaction" pattern:

```
validate arguments           ← quick, cheap, no side effects
start the critical section   ← acquire lock, disable interrupts
...do the real work...
end the critical section     ← release lock, re-enable interrupts
```

Every robust kernel function looks like this.

---

### 3.2 Locating the target

> **Q:**
> ```c
>   for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
>     acquire(&pp->lock);
>     if(pp->pid == pid){
>       t = pp;
>       break;
>     }
>     release(&pp->lock);
>   }
>   if(t == 0){
>     release(&wait_lock);
>     return -1;
>   }
>
>   acquire(&p->lock);
> ```

#### What this block does

Scan the process table for the one whose `pid` matches. If found,
keep its lock held and remember it as `t`. If not found, clean up
and fail. Finally, take our own process's lock.

Three things: the **scan**, the **not-found path**, and the
**acquire of `p->lock`**.

#### Why we need to scan

xv6 has no PID→proc hash map. `proc[]` is a fixed-size array of
`NPROC` entries (`NPROC = 64` in `param.h`), and each proc's `pid`
is just whatever `allocpid()` assigned when it was created. To find
a proc by PID you walk the array and compare. `O(NPROC)` but NPROC
is tiny.

```c
for(struct proc *pp = proc; pp < &proc[NPROC]; pp++)
```

Standard xv6 idiom — `proc` is the array, `&proc[NPROC]` is the
address just past the end, walk with a pointer.

#### Why we `acquire(&pp->lock)` on **every** iteration

```c
acquire(&pp->lock);
if(pp->pid == pid){ t = pp; break; }
release(&pp->lock);
```

You might think: "just read `pp->pid` without the lock." Three
reasons not to:

1. **Torn reads** — `pp->pid` could be mid-update if another CPU is
   inside `freeproc()` or `allocproc()`. Locks give a coherent
   snapshot.
2. **Stability** — even if `pp->pid == pid` at the moment we read
   it, by the next instruction `pp` might be freed and reallocated
   with a different pid.
3. **xv6 convention** — the comment at `kernel/proc.h:88` says
   `p->lock` must be held when using `state`, `chan`, `killed`,
   `xstate`, `pid`.

So: acquire → test → if no match, release and continue; if match,
**keep the lock held** and break out of the loop.

#### The asymmetric release

Subtle. After the loop:

- **Match**: `t == pp`, `pp->lock` still held.
- **No match**: walked off the end of the array, every lock was
  acquire/release'd individually, no lock held.

Both invariants are maintained by the fact that `break` jumps past
the `release(&pp->lock)`. Looks weird because normally every
`acquire` is matched by a `release` in the same block — but the
rule is really "every lock must be released *eventually*," not
"in the same scope." Here `t->lock` will be released later on
every exit path.

#### The not-found branch

```c
if(t == 0){
    release(&wait_lock);
    return -1;
}
```

If the loop exited without finding a match, `t` is still `0`. No
live process has the requested PID.

Before returning, we **release `wait_lock`** — we took it at the
top and are now bailing out. Forgetting would leave `wait_lock`
permanently held, and the next `wait`/`exit`/`fork`/`co_yield`
would deadlock.

Return `-1`. This is the "non-existent PID" error case.

#### Then: `acquire(&p->lock)`

We now take our own lock.

At this point three locks held in order:

```
wait_lock  →  t->lock  →  p->lock
```

Deliberate and consistent. If every `co_yield` caller acquires in
this order, we can never deadlock against another `co_yield`
caller. On single CPU, deadlock on spinlocks can't actually happen
anyway, but the discipline is good hygiene and would be required
on multi-CPU.

Why take `p->lock` now? Because:
- Not needed for the scan (we don't inspect our own state there).
- Taking it earlier would extend the window we hold it for no reason.
- Taking it after `t->lock` respects the ordering rule.

#### After this block, invariants are

```
wait_lock:  held
t->lock:    held
p->lock:    held
t:          non-null, points at the target proc
```

We have exclusive, coherent access to both processes' state fields.

#### Mental model

**"Look up the target in the phone book, and if found, keep your
finger on the page."** You flip through the directory (scan
`proc[]`), checking each entry under its local lock. When you
find a match, you don't close the book — you keep your finger in
it (`t->lock` held) so the entry can't disappear before you finish.
Then you take your own notebook out (`p->lock`) because the next
step involves both.

---

### 3.3 Liveness gate

> **Q:**
> ```c
>   if(t->state == UNUSED || t->state == ZOMBIE || t->killed){
>     release(&p->lock);
>     release(&t->lock);
>     release(&wait_lock);
>     return -1;
>   }
> ```

#### What this guards against

At this point we've confirmed a proc slot with matching pid exists
— but "the slot exists" is not "the target is a live, receivable
process." This block rejects three dead-or-dying cases.

#### The three conditions

##### `t->state == UNUSED`

Proc slot is free — not currently assigned to any running process.
Could happen if the process exited between argument validation and
our scan. `freeproc()` sets state to `UNUSED` (`kernel/proc.c:171`).
Treat UNUSED as "not a real process" → `-1`.

##### `t->state == ZOMBIE`

Process ran `exit()` and its slot is waiting for a parent `wait()`
call to reap it. It has a pid, it has a proc struct, but it
**cannot do anything** — it will never call `co_yield` back.
Yielding to it would mean sleeping forever.

##### `t->killed`

`killed` is a flag set by `kill(pid)` (`kernel/proc.c:582-604`).
Means "this process has been told to die; it will exit the next
time it hits a safe checkpoint." Until then, its `state` may still
be `RUNNABLE`/`RUNNING`/`SLEEPING`, so a check on `state` alone
would miss it.

A process with `killed == 1` is on its way out and:
- Can't reliably call `co_yield` back.
- Matches the "killed target" error case directly.

This is the scenario the third error test exercises:

```c
int child = fork();
if(child == 0){ sleep(1000); exit(0); }
kill(child);               // sets child->killed = 1
r = co_yield(child, 9);    // → -1 via this branch
```

#### Why all three at once

No reason to distinguish — the assignment says "no need to
distinguish between different error conditions in the return
value." All failures collapse to `-1`.

#### The release order

```c
release(&p->lock);
release(&t->lock);
release(&wait_lock);
```

**Reverse of acquire order.** Good habit because:

- Invariants are easy to read: locks nest like parentheses.
- On multi-CPU, minimizes the window where another thread might
  grab an "inner" lock (released earlier) but still block on an
  "outer" one you still hold.
- Matches how stack unwinding would release them.

On single-CPU xv6 the exact order doesn't matter for correctness,
but it's good style.

#### Accepted states (not blocked by this gate)

| state | interpretation | why it's OK |
| --- | --- | --- |
| `USED` | `allocproc` ran but fork not finished | pid valid; target will reach RUNNABLE soon |
| `RUNNABLE` | waiting for scheduler | may call `co_yield` when it runs |
| `RUNNING` | executing on another CPU | shouldn't happen on `CPUS=1` |
| `SLEEPING` | already blocked somewhere | the *most interesting* case — the next branch handles `SLEEPING && chan == p` |

So this check is a **blacklist of "definitely dead" states**, not a
whitelist of "ready to go." The direct-handoff branch does its own
finer check on the interesting SLEEPING sub-case.

#### Mental model

**Liveness gate**: "Can this target ever actually run again and
respond to our handoff?" If the answer is clearly no (UNUSED slot,
zombie, marked to die), bail out early. Otherwise fall through to
the two real branches.

---

### 3.4 Direct hand-off branch

> **Q:** Explain the whole direct hand-off block.

This is **the heart of Task 3** — the actual direct process-to-process
context switch.

#### The gate: `if(t->state == SLEEPING && t->chan == p)`

Two conditions, both required:

- **`t->state == SLEEPING`** — target is blocked, not running.
- **`t->chan == p`** — target is waiting *specifically for us*.
  The sleep-path below sets `p->chan = t` ("I'm waiting for t"),
  so `t->chan == p` means "t is waiting for p" — i.e. t earlier
  called `co_yield(our_pid, ...)` and is parked.

If either fails, we fall through to "peer not ready" and sleep.
**This is the single point where we decide: rendezvous or wait.**

#### The state flip

##### `t->trapframe->a0 = value;`

Write the payload into the **target's** trapframe register `a0`.
When `t` resumes and returns from its `co_yield`, the epilogue
picks up `p->trapframe->a0` as the return value. So this
**injects the value into t's future syscall return**.

##### `t->state = RUNNING;`

Here's the "skip RUNNABLE" the assignment requires. Normally a
sleeper wakes via SLEEPING → RUNNABLE → scheduler picks → RUNNING.
We bypass that: straight to RUNNING because we are about to `swtch`
into it ourselves.

##### `p->chan = t; p->state = SLEEPING;`

Mirror transformation on ourselves. We're giving up the CPU, so
SLEEPING. `chan = t` means when `t` later calls
`co_yield(our_pid, …)` back, t's gate condition matches and t can
deliver to us the same way.

**This `chan = t` is what sustains the ping-pong.** Without it,
our next resume would have no way to advertise "I'm waiting for t."

##### `mycpu()->proc = t;`

The CPU's "who is currently running" field. Normally only
`scheduler()` updates this. We update it manually because scheduler
isn't involved — the CPU is about to be running `t`'s code.

This is also **what makes the scheduler companion fix work**: when
scheduler later resumes via some future `sched()`, it uses `c->proc`
to figure out whose lock to release.

#### Locks around the swtch

```c
release(&wait_lock);   // no longer need rendezvous serialization
release(&p->lock);     // controversial — see below
// t->lock still held
swtch(&p->context, &t->context);
```

##### Why release `wait_lock` first?

Once we've committed to direct handoff (conditions checked, state
flipped), no system-wide serialization needed. Release early to let
other `co_yield`s proceed.

##### Why release `p->lock` **before** `swtch`?

The stock xv6 convention: "caller holds own `p->lock` across `swtch`,
scheduler releases it later."

That convention **cannot work here**:
- We're not swtching into scheduler — no scheduler thread will
  release our lock.
- If we kept `p->lock` held, any future peer trying
  `co_yield(us, …)` would block on `acquire(p->lock)`. The only
  way `p->lock` would ever release is if *we* resume, which
  requires that peer. **Deadlock.**

So we release it ourselves. The window between release and swtch
is safe on `CPUS=1`:
- No other thread runs on this CPU during the window.
- Scheduler never picks SLEEPING processes anyway.
- Interrupts still disabled (`t->lock` is still held).

##### Why keep `t->lock` held across `swtch`?

This **does** match stock xv6 convention — "target's lock is held
across the swtch into the target." When `t` resumes past *its own*
earlier swtch, `t` expects its lock held. t's resume code then
releases `t->lock`. If we released it before swtch, t would resume
into a lock it doesn't hold and subsequent releases would panic.

#### `swtch(&p->context, &t->context)` — what actually happens

Pure register save/restore. Not magic. From `kernel/swtch.S`
(only callee-saved registers — `ra`, `sp`, `s0`-`s11`):

- **Save** 14 callee-saved registers into `p->context`.
- **Load** those 14 registers from `t->context`.
- Return.

Key registers:
- `ra` (return address) — determines *where `ret` jumps*.
  `t->context.ra` is whatever was saved the last time `t` called
  swtch. When the swtch's `ret` executes, control jumps back into
  `t`'s saved call site.
- `sp` (stack pointer) — now points to `t`'s kernel stack.

After these two copies, the CPU is *running t*. Our own state is
frozen in `p->context`; we'll only resume when some future swtch
loads it back.

#### What "t resumes" means

`t` was suspended inside its own `co_yield` earlier. Two places it
could have saved context:

- **If t's earlier call went through "peer not ready"**, t's
  `context` was saved by `sched()`. On resume, t returns from
  `sched()`, continues at the `int k = p->killed; …` epilogue
  in the sleep branch.
- **If t's earlier call went through direct switch itself**
  (a later ping-pong round), t's `context` was saved by t's own
  `swtch(&p_self, &some_peer)`. On resume, t continues at the same
  `int k = p->killed; …` epilogue in the direct-switch branch.

Either way, t reads `t->trapframe->a0` (which *we* set to `value`)
and returns it to user code.

#### The other half — *our* resume

Eventually some future peer q calls `co_yield(our_pid, value2)`:

1. Finds us, acquires `our->lock`. (Free, we released it.)
2. Sees `our->state == SLEEPING && our->chan == q`. Gate passes.
3. Flips states: `our->trapframe->a0 = value2`, `our = RUNNING`,
   `q = SLEEPING`, …
4. `release(&wait_lock); release(&q->lock);` — q keeps **our**
   lock held across swtch.
5. `swtch(&q->context, &our->context)`.

Swtch loads our `ra` and `sp`; `ret` jumps to the instruction right
after **our own swtch**:

```c
int k = p->killed;
```

We wake at the epilogue with `p->lock` held (by q's acquisition)
and `p->trapframe->a0` containing q's value.

##### Epilogue line-by-line

```c
int k = p->killed;
uint64 v = p->trapframe->a0;
release(&p->lock);
return k ? -1 : (int)v;
```

- Snapshot `killed` and `a0` while holding `p->lock` — proper
  ordering, stable values.
- Release. `p->lock` lifecycle: held at top → released before
  swtch → q acquired → we got it back on resume → release now.
- If killed, return `-1`. Otherwise return the value.

#### Big picture

> We inject the payload into the target's future syscall return via
> `trapframe->a0`, skip `RUNNABLE` by setting `state = RUNNING`
> directly, re-parent the CPU pointer to the target, release locks
> in the exact order that preserves the lock-convention needed for
> our eventual resume, and finally `swtch` straight into the target's
> saved context — waking it up at *its* own `co_yield` epilogue,
> which reads the value we just wrote and returns it to user code,
> while we sit frozen in `p->context` waiting for some peer to swtch
> back into us.

---

### 3.5 Peer-not-ready branch

> **Q:** Explain the rest — the "peer not ready" branch.

The **fallback branch**: target isn't sitting there waiting for us,
so no direct handoff. Instead we park ourselves as "waiting for t"
and hand CPU back to scheduler.

#### When we get here

The gate `t->state == SLEEPING && t->chan == p` **failed**. Possible
reasons:

- t is `RUNNABLE` or `RUNNING` — hasn't called `co_yield` yet.
- t is `USED` — freshly forked, not yet RUNNABLE.
- t is `SLEEPING` but on a **different** `chan` (e.g. waiting for
  a pipe, or a third process — not us).

In all cases, t can't "receive" our handoff right now.

#### `release(&t->lock)`

We won't touch `t`'s state in this branch — only our own. Drop
`t->lock` early:

- Keeping it held while calling `sched()` would make `t`
  unavailable to other code for no benefit.
- `sched()` has a strict precondition: **exactly one lock held on
  entry**, and it must be `p->lock`. If we still held `t->lock`,
  `sched()` would panic with `panic("sched locks")`.

#### `p->chan = t; p->state = SLEEPING;`

Mark ourselves as **waiting** — the exact state the direct-handoff
branch is looking for.

- `p->chan = t` — "I am waiting for t specifically." When t later
  calls `co_yield(our_pid, ...)`, t's direct-handoff gate checks
  `our->state == SLEEPING && our->chan == t`. Our choice here makes
  that match happen.
- `p->state = SLEEPING` — we won't be picked by the scheduler's
  RUNNABLE scan. Only way to run again: (a) someone direct-switches
  into us, or (b) `kill()` flips us to RUNNABLE.

We don't touch `t`'s state (we already released t's lock), and we
don't touch `mycpu()->proc` — `sched()` will swtch to scheduler,
and scheduler's first action after resume is `c->proc = 0`.

#### `release(&wait_lock)`

`wait_lock` was serializing "find target, check state, flip state."
All done. Release so other `co_yield`s proceed while we sleep.

Why release `wait_lock` before `sched`? `sched()` requires
**exactly** `p->lock` held. With `wait_lock` still held, it would
panic.

#### `sched()` — what it does

From `kernel/proc.c:481`:

```c
void sched(void) {
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))     panic("sched p->lock");
  if(mycpu()->noff != 1)     panic("sched locks");
  if(p->state == RUNNING)    panic("sched running");
  if(intr_get())             panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}
```

Four preconditions (we satisfy all):
- `p->lock` held ✓
- Only one lock (`noff == 1`) ✓
- `state != RUNNING` (we set SLEEPING) ✓
- Interrupts off (we hold a spinlock) ✓

Then `swtch(&p->context, &mycpu()->context)`. Saves our callee-saved
registers, loads scheduler's context, resuming scheduler where it
last swtched out.

Scheduler resumes in `kernel/proc.c:463`. Thanks to our companion
fix (`p = c->proc;`), scheduler knows the returning process is `p`
(us), releases our `p->lock`, loops, picks the next RUNNABLE.

So: **we suspend with our lock held; scheduler releases it for us.**
Unlike the direct-handoff branch, here we *can* rely on the
convention because we *are* swtch-ing into scheduler.

#### Eventually we resume

Two ways:

##### Way A — peer does direct handoff into us

Some other process q calls `co_yield(our_pid, value)`. q's
direct-handoff path:
1. Finds us, acquires `our->lock`.
2. Sees `our->state == SLEEPING && our->chan == q`. Gate passes.
3. Writes `our->trapframe->a0 = value`.
4. `our = RUNNING`; q = SLEEPING; `mycpu()->proc = us`; releases
   `wait_lock` and own lock (keeps **our** lock held across swtch).
5. `swtch(&q->context, &our->context)`.

The swtch loads our `ra` and `sp`. `ra` points to the instruction
after **our** call to `sched()` — `int k = p->killed;`. So we
resume at the epilogue with `p->lock` held and `p->trapframe->a0`
holding q's value.

##### Way B — `kill()` flipped us to RUNNABLE

If someone calls `kill(our_pid)` while we're asleep,
`kernel/proc.c:582-604` does:

```c
if(p->state == SLEEPING){
  p->state = RUNNABLE;     // force wake
}
```

We're now RUNNABLE with no value delivered. Scheduler picks us:
1. `acquire(&p->lock)`.
2. `state == RUNNABLE`, so set RUNNING, swtch to us.
3. We resume at `int k = p->killed;` with `p->lock` held.

In both resume paths, **`p->lock` is held when we land at the
epilogue.**

#### The epilogue

```c
int k = p->killed;
uint64 v = p->trapframe->a0;
release(&p->lock);
return k ? -1 : (int)v;
```

- Snapshot killed + a0 while holding `p->lock`.
- In Way A, a0 is the delivered value. In Way B, a0 is stale — but
  `k == 1` so we return -1 anyway.
- Release.
- Return.

#### Why "without calling `sleep()`"

Stock `sleep()` in `kernel/proc.c:535`:

```c
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();
  acquire(&p->lock);
  release(lk);
  p->chan = chan;
  p->state = SLEEPING;
  sched();
  p->chan = 0;
  release(&p->lock);
  acquire(lk);
}
```

If we called `sleep(t, &wait_lock)`:
1. Acquires `p->lock` — but we already hold it. **Double-acquire
   → panic** (xv6 spinlocks are non-recursive).
2. Re-acquires `wait_lock` on return — we don't want that.

So we **inline the relevant parts of `sleep()`** (chan/state set +
`sched()` + lock release on the other side) and skip the parts that
don't fit. This also matches what the user wanted: "it's better not
to use sleep."

#### Contrast with direct handoff

| Aspect | Direct handoff | Peer not ready |
| --- | --- | --- |
| Target's state on entry | `SLEEPING` on us | anything else |
| Who ends up running next | the target | someone else |
| `c->proc` update | yes (`= t`) | no |
| Lock held across `swtch` | `t->lock` (target's) | `p->lock` (own) |
| Return to user | immediately (target's user code) | delayed until waker |
| Path through `scheduler()` | bypassed | used |

Direct handoff is the **fast case** (both processes agree right
now). This branch is the **slow case** (one side is early and has
to wait) — using stock xv6 sleep/wakeup machinery to wait, just
without calling `sleep()` itself.

#### Mental model

The two branches form a complete handshake:

- "Peer ready" = **the deliverer** — you find your peer already
  waiting; deliver and swtch.
- "Peer not ready" = **the waiter** — you go to sleep advertising
  "I'm waiting for t"; eventually t arrives and takes the
  deliverer role.

In ping-pong, every round alternates: one side is waiter, the other
deliverer. The first round begins with parent as waiter (child
hasn't started loop yet). Every subsequent round has the side that
just received become the deliverer for the return trip.

---

## 4. `swtch` and `sched`

> **Q:** What are `swtch` and `sched`?

Two fundamental context-switching primitives in xv6. They're a
pair — `swtch` is the hardware-level mechanic, `sched` is the safe
kernel-level wrapper around it.

### 4.1 `swtch` — the register swap

**File**: `kernel/swtch.S` — tiny, written in assembly.

**Signature**:
```c
void swtch(struct context *old, struct context *new);
```

**What it does**: save the CPU's callee-saved registers into
`*old`, then load registers from `*new`, then return.

**What it doesn't do**: anything else. It doesn't know about
processes, locks, scheduling, or even that it's a "context switch"
— it's just a register-save-then-restore between two memory blobs.

#### The struct

```c
struct context {
  uint64 ra;     // return address
  uint64 sp;     // stack pointer
  uint64 s0;     // callee-saved registers s0..s11
  uint64 s1;
  ...
  uint64 s11;
};
```

14 registers total. Why only these 14?
- **`ra`** and **`sp`** are the magic pair. `ra` controls where
  `ret` jumps (where we resume); `sp` controls which stack we use.
- **`s0`-`s11`** are the RISC-V callee-saved registers. By ABI,
  any function must preserve them across a call.
- **Everything else** (`a0`-`a7`, `t0`-`t6`) is caller-saved. The
  C compiler already saves those on the stack if needed.

#### The actual assembly

```asm
swtch:
    sd ra, 0(a0)     # save ra into old->ra
    sd sp, 8(a0)     # save sp into old->sp
    sd s0, 16(a0)    # save s0..s11 into old->s0..s11
    ...
    sd s11, 104(a0)

    ld ra, 0(a1)     # load ra from new->ra
    ld sp, 8(a1)     # load sp from new->sp
    ld s0, 16(a1)    # load s0..s11 from new->s0..s11
    ...
    ld s11, 104(a1)

    ret              # jump to whatever ra is now
```

~30 assembly instructions. The whole "context switch" concept in
xv6 is literally this.

#### Why this reshapes control flow

Magic is in `ra` and `sp`:
- `ret` at end of swtch jumps to `new->ra`. Completely different
  return address than we entered with.
- After `ld sp, ...`, we're using `new`'s stack.

So swtch "returns" to a different place than it was called from.
**Overwrite `ra` and `sp`, and a `ret` will transport you into
someone else's execution.**

#### Where swtch shows up

Only **two** callers in stock xv6 (three in our version):
1. `sched()` — a process giving up the CPU.
2. `scheduler()` — the scheduler picking a process.
3. **Our `co_yield`** — direct process-to-process, bypassing
   scheduler.

### 4.2 `sched` — the safe wrapper

**File**: `kernel/proc.c:481-499`:

```c
void sched(void) {
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))     panic("sched p->lock");
  if(mycpu()->noff != 1)     panic("sched locks");
  if(p->state == RUNNING)    panic("sched running");
  if(intr_get())             panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}
```

Thin wrapper around swtch that adds:
1. **Safety preconditions** (four panic checks).
2. **`intena` save/restore** — preserves the "were interrupts
   enabled before any locks?" bit. `intena` is a property of the
   kernel thread, not the CPU.

#### The four preconditions

| Check | Panic if... | Why |
| --- | --- | --- |
| `holding(&p->lock)` | we don't hold our own lock | swtch convention; wakeups can race |
| `noff != 1` | we hold more than just `p->lock` | extra locks stay held, blocking others; corrupts scheduler's expected lock state |
| `p->state == RUNNING` | forgot to change state | scheduler won't pick SLEEPING/RUNNABLE; other threads might mistakenly wake us |
| `intr_get()` | interrupts enabled | timer could pre-empt mid-transition; `intena` save captures wrong value |

#### The lock convention

"Must hold only p->lock and have changed proc->state":
- Caller has set `p->state` to something other than RUNNING.
- Caller holds `p->lock`.
- sched's swtch suspends caller, resumes scheduler.
- Scheduler releases `p->lock` on caller's behalf.

When caller is later resumed, `p->lock` is **re-acquired** (by
scheduler or by a peer's direct switch). So caller's code after
sched can assume `p->lock` is held.

### 4.3 The pair, compared

| Property | `swtch` | `sched` |
| --- | --- | --- |
| Language | assembly | C |
| Knows about processes | no | yes |
| Checks invariants | no | yes (4 panics) |
| Handles `intena` | no | yes |
| Callers | `sched`, `scheduler`, our `co_yield` | `yield`, `sleep`, `exit`, our `co_yield` sleep branch |
| Counterpart | symmetric — swaps two contexts | always swtches to scheduler |

**`swtch` is symmetric, `sched` is directional.** `sched` always
swtches *to the scheduler*. `swtch` can swtch between any two
contexts — which is exactly what we exploit in direct handoff
(swtch between two processes' contexts, skipping scheduler).

### 4.4 Why our direct swtch uses `swtch` not `sched`

`sched()` is hardcoded to swtch to `mycpu()->context` (scheduler).
We don't want that. We want `t->context` (the target). So we call
`swtch` directly.

But we lose the safety net: no preconditions, no `intena` handling.
That's why our direct-switch code is carefully commented and
requires `CPUS=1` — we bypass the guardrails and reason correctness
ourselves.

### 4.5 Control-flow picture

#### Stock xv6 flow (through scheduler)

```
process A           scheduler              process B
─────────           ─────────              ─────────
(running)
A calls sched:
  swtch(A_ctx, ──▶  resumes from its own swtch
      cpu_ctx)     c->proc = 0
                   release A->lock
                   loops, picks B
                   acquire B->lock
                   c->proc = B
                   swtch(cpu_ctx,  ──▶  B resumes at its last swtch
                       B_ctx)            B returns from sched
                                         release B->lock
                                         (running)
```

**Two swtches per transition.**

#### Our direct switch (co_yield)

```
process A (deliverer)                      process B (waiter)
─────────                                  ─────────
(running, about to deliver to B)
swtch(A_ctx,                         ──▶   B resumes at its previous
      B_ctx)                                 swtch site — the epilogue
                                             of its earlier co_yield
                                            B reads trapframe->a0
                                            returns value
                                            (running)
```

**One** swtch. Scheduler untouched. That's the whole point —
skipping RUNNABLE and the scheduler's table-scan overhead.

### 4.6 Why this is "coroutine-style"

The book §7.3 explicitly calls scheduler and any process-using-sched
"coroutines of each other":

> Procedures that intentionally transfer control to each other via
> thread switch are sometimes referred to as coroutines; in this
> example, sched and scheduler are co-routines of each other.

A coroutine is a function that can suspend and be resumed by another
coroutine at a specific point. `swtch` is the primitive that makes
that possible. The assignment's title, "Coroutine System Call,"
is earned by our use of `swtch` between two processes directly,
making those two processes *coroutines of each other*.

### 4.7 Mental model

- **`swtch`** is "save your current registers into box X, load from
  box Y, then `ret`." Pure plumbing.
- **`sched`** is "I want to stop running. Transfer control to
  scheduler, correctly, with all xv6's invariants preserved."

Use `sched` for normal xv6 semantics (scheduler picks next).
Use `swtch` directly when you want to bypass — rare, delicate, and
exactly what Task 3 asks.

---

## Appendix — study checklist

If you can answer these without rereading, you've got Task 3:

1. Why does every xv6 syscall need an entry in `syscalls[]`, a
   stub in `usys.S`, a prototype in `user/user.h`, and a number
   in `syscall.h` — what breaks if any of the four is missing?
2. Why does `sys_co_yield(void)` take no args even though user
   code calls `co_yield(pid, value)` with two?
3. Where is the user's `pid` argument at each stage — user reg,
   trapframe field, kernel local, kernel call arg?
4. What is `ecall` and why can't a user program invoke a syscall
   without it (or an equivalent privileged trap instruction)?
5. What is `chan` and why must `state` and `chan` be checked
   together under `p->lock` in `wakeup`?
6. Why is the self-yield check (`pid == p->pid`) done *before*
   `acquire(&wait_lock)`?
7. Why does the target-finding loop acquire `pp->lock` on every
   iteration, but only release on no-match?
8. What are the three "dead or dying" states the liveness gate
   rejects, and why `killed` has to be checked separately from
   `state`?
9. In the direct hand-off branch, why must we release `p->lock`
   **before** `swtch`, even though that violates the stock xv6
   convention? What single-CPU assumption makes it safe?
10. Why must we keep `t->lock` held **across** `swtch` (stock
    convention)? What panics if we don't?
11. Why do we update `mycpu()->proc = t` before the direct
    swtch, and what later `scheduler()` behavior depends on that?
12. Why does the scheduler's local `p` become stale after a direct
    handoff, and what exact sequence of events causes the
    `panic: release`?
13. In the peer-not-ready branch, why do we use `sched()` directly
    instead of `sleep()`?
14. What does `swtch` actually do in ~4 words? What register pair
    is the "magic"?
15. How is our `co_yield` "coroutine-style" in the sense the book
    uses the word?
