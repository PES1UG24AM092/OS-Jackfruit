# OS-Jackfruit: Supervised Multi-Container Runtime

## Team

| Name | SRN |
|------|-----|
| Dilipan Y | PES1UG24AM092 |
| Sunay Hegde | PES1UG24AM903 |

---

## Project Overview

A from-scratch Linux container runtime consisting of:
- A **supervisor daemon** (`engine supervisor`) that manages container lifecycle using `clone()` with PID, UTS, and mount namespace isolation
- A **CLI client** (`engine start/run/ps/logs/stop`) that communicates with the supervisor over a UNIX domain socket
- A **bounded-buffer logging pipeline** with producer/consumer threads routing container stdout/stderr to per-container log files
- A **Linux kernel module** (`monitor.ko`) that tracks container memory usage and enforces soft/hard limits via `/dev/container_monitor`

---

## Build Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

### Clone and build

```bash
git clone https://github.com/PES1UG24AM092/OS-Jackfruit.git
cd OS-Jackfruit
```

Set up Alpine rootfs:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

Build all binaries and the kernel module:

```bash
cd boilerplate
make
```

Copy workload binaries into rootfs:

```bash
cp memory_hog cpu_hog io_pulse ../rootfs-alpha/
cp memory_hog cpu_hog io_pulse ../rootfs-beta/
```

---

## Running the Runtime

### 1. Load the kernel module

```bash
sudo insmod boilerplate/monitor.ko
dmesg | tail -3
```

### 2. Start the supervisor (Terminal 1)

```bash
cd boilerplate
sudo ./engine supervisor ../rootfs-base
```

### 3. Use the CLI (Terminal 2)

```bash
sudo ./engine start alpha ../rootfs-alpha /cpu_hog
sudo ./engine start beta  ../rootfs-beta  /cpu_hog --nice 10
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine run test ../rootfs-alpha /cpu_hog
sudo ./engine start memtest ../rootfs-alpha /memory_hog --soft-mib 5 --hard-mib 10
```

### 4. Unload the kernel module when done

```bash
sudo rmmod monitor
```

---

## Output Screenshots

### Screenshot 1: Two containers started and running

Both `alpha` and `beta` containers launched under the supervisor, shown via `engine ps`:
Container 'alpha' started (pid=9071)
Container 'beta' started (pid=9079)
beta             pid=9079   state=running
alpha            pid=9071   state=running

Both containers are isolated with their own PID, UTS, and mount namespaces. The kernel module confirms registration of both with soft=40MiB and hard=64MiB limits.

---

### Screenshot 2: Container log output

`engine logs alpha` reads the log file captured from the container's stdout via the bounded-buffer pipeline:
cpu_hog alive elapsed=1 accumulator=14103633593719469765
cpu_hog alive elapsed=2 accumulator=3887357026314302984
...
cpu_hog done duration=10 accumulator=5320461702344530175

Each line was written by the container process inside its namespace, piped to the supervisor's producer thread, pushed through the bounded buffer, and written to `logs/alpha.log` by the consumer thread.

---

### Screenshot 3: Containers stopped via CLI

SIGTERM sent to both containers while running; `engine ps` confirms `state=stopped`:
Sent SIGTERM to 'alpha' (pid=9071)
Sent SIGTERM to 'beta' (pid=9079)
beta             pid=9079   state=stopped
alpha            pid=9071   state=stopped

---

### Screenshot 4: Kernel module activity in dmesg
[94120.920494] [container_monitor] Module loaded. Device: /dev/container_monitor
[94905.028527] [container_monitor] Registered container=alpha pid=9071 soft=41943040 hard=67108864
[94905.041345] [container_monitor] Registered container=beta pid=9079 soft=41943040 hard=67108864
[94906.302724] [container_monitor] Stale entry removed: container=beta pid=9079
[94906.302730] [container_monitor] Stale entry removed: container=alpha pid=9071

---

### Screenshot 5: Memory limit enforcement (soft + hard)

`memory_hog` started with `--soft-mib 5 --hard-mib 10`:
Container 'memtest' started (pid=9236)
[container_monitor] Registered container=memtest pid=9236 soft=5242880 hard=10485760
[container_monitor] SOFT LIMIT container=memtest pid=9236 rss=9056256 limit=5242880
[container_monitor] HARD LIMIT container=memtest pid=9236 rss=17444864 limit=10485760 — sent SIGKILL
memtest          pid=9236   state=killed

---

## Engineering Analysis

### 1. Container Isolation

Each container is spawned using `clone()` with three namespace flags:

- `CLONE_NEWPID` — container gets its own PID namespace; its init process appears as PID 1 inside
- `CLONE_NEWUTS` — container gets its own hostname (set to container ID via `sethostname`)
- `CLONE_NEWNS` — container gets its own mount namespace; `/proc` is mounted fresh inside

After `clone()`, `child_fn` calls `chroot()` into the container's rootfs directory, then `chdir("/")`. The container cannot see the host filesystem or other containers.

**Tradeoff:** `chroot` was chosen over `pivot_root` for simplicity. `pivot_root` is more secure but requires the rootfs to be a separate mount point.

---

### 2. Container Lifecycle Management

The supervisor maintains a linked list of `container_record_t` structs protected by `metadata_lock`. State transitions:
STARTING → RUNNING → EXITED   (natural exit)
→ STOPPED  (SIGTERM from engine stop)
→ KILLED   (SIGKILL from kernel monitor)

`waitpid(-1, WNOHANG)` is called on every event-loop tick to reap all exited children and prevent zombies. On shutdown, SIGTERM is sent to all running containers, followed by a 1-second grace period before the log buffer is drained and the supervisor exits.

---

### 3. IPC and Synchronization

**Control plane:** UNIX domain socket (`/tmp/mini_runtime.sock`, `SOCK_STREAM`). The CLI sends a `control_request_t` struct and reads back a `control_response_t`. The supervisor uses `select()` with a 1-second timeout to multiplex between new connections and child reaping.

**Logging pipeline:** Each container has a pipe. The write end is passed to `child_fn` which redirects stdout/stderr into it. A producer thread per container reads from the pipe and calls `bounded_buffer_push()`. A single consumer thread (`logging_thread`) calls `bounded_buffer_pop()` and writes to the per-container log file.

**Bounded buffer:** 16-slot circular buffer. Producer blocks on `pthread_cond_wait(&not_full)` when full; consumer blocks on `pthread_cond_wait(&not_empty)` when empty. Shutdown is signalled via `shutting_down = 1` + `pthread_cond_broadcast` on both conditions for clean drain-and-exit.

**Why mutex over spinlock in kernel module:** The timer callback calls `get_rss_bytes()` which invokes `get_task_mm()` and `mmput()` — both sleepable. A spinlock cannot be held across sleepable operations, so `DEFINE_MUTEX` is required.

---

### 4. Memory Monitoring (Kernel Module)

The kernel module exposes `/dev/container_monitor` with two ioctls:

- `MONITOR_REGISTER` — adds a `monitored_entry` node with PID, soft limit, and hard limit
- `MONITOR_UNREGISTER` — removes the entry by PID

A kernel timer fires every second and iterates the list using `list_for_each_entry_safe`:

1. If process is gone: removes stale entry
2. If RSS ≥ hard limit: sends SIGKILL and removes entry
3. If RSS ≥ soft limit and not yet warned: emits `KERN_WARNING` and sets `soft_warned = 1`

---

### 5. Scheduler Experiment

Two `cpu_hog` containers ran concurrently — one at nice=0, one at nice=19. `cpu_hog` spins for 10 seconds and prints an accumulator each second.

**Results:**

| Container | Nice Value | Final Accumulator |
|-----------|-----------|-------------------|
| normal    | 0         | 12796951297270109463 |
| niced     | 19        | 5772039232732264222  |

The niced container's final accumulator is ~55% lower, confirming it received fewer CPU cycles. When run in isolation both containers produce similar accumulators (~15–16 × 10¹⁸). Under contention the CFS scheduler correctly deprioritises the nice=19 container by assigning it a lower scheduling weight.

---

## Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| IPC mechanism | UNIX domain socket | Bidirectional, message-framed, no polling. Simpler than shared memory; more robust than FIFO. |
| Namespace isolation | PID + UTS + Mount | Covers process, hostname, and filesystem isolation. |
| Rootfs isolation | `chroot` | Simpler than `pivot_root`; sufficient for project scope. |
| Log routing | Pipe + bounded buffer | Decouples container I/O speed from disk write speed. |
| Buffer full policy | Block producer | Prevents log loss at cost of back-pressure to container. |
| Kernel lock | `mutex` | Timer callback calls sleepable MM functions; spinlock would BUG. |
| Killed vs stopped state | Separate states | `ps` output clearly distinguishes voluntary stop from hard-limit kill. |
