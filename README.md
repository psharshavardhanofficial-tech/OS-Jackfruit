# OS-Jackfruit: Multi-Container Runtime
**Course:** UE24CS242B - Operating Systems
**University:** PES University

## 1. Project Summary
OS-Jackfruit is a custom lightweight container engine that provides process isolation, resource monitoring, and asynchronous logging. The system is split into a **User-Space Supervisor** (`engine.c`), which manages the container lifecycle and logging, and a **Kernel-Space Monitor** (`monitor.c`), which enforces memory limits via a custom Linux Kernel Module (LKM).

---

## 2. Technical Architecture & Design Decisions

### A. Namespace Isolation
We achieved process and filesystem isolation by utilizing the `clone()` system call with specific flags:
* **CLONE_NEWPID**: Isolates the process ID space so the container cannot see host processes.
* **CLONE_NEWUTS**: Allows the container to have its own hostname.
* **CLONE_NEWNS**: Isolates mount points.
* **Filesystem**: We used `chroot()` to jail the process into a unique, writable Alpine rootfs.

### B. Bounded-Buffer Logging (Path A)
To prevent logging from blocking the containerized application, we implemented a producer-consumer model:
* **Mechanism**: A shared circular buffer with a fixed capacity.
* **Synchronization**: Utilized `pthread_mutex` to protect the buffer and `pthread_cond` (condition variables) to handle full/empty states without busy-waiting.
* **Tradeoff**: A fixed buffer size protects supervisor memory but requires producers to block if the consumer (disk writer) falls behind.

### C. Control Plane IPC (Path B)
CLI commands (start, stop, ps, logs) communicate with the long-running supervisor via a **Unix Domain Socket** (`/tmp/mini_runtime.sock`).
* **Justification**: Sockets provide a reliable, bidirectional channel compared to FIFOs, allowing the supervisor to send structured success/error responses back to the CLI.

### D. Kernel Memory Monitoring
The `monitor.ko` module tracks container Resident Set Size (RSS).
* **Policy**: Implements a dual-limit policy where exceeding the **Soft Limit** logs a kernel warning, while exceeding the **Hard Limit** triggers a `SIGKILL`.
* **Synchronization**: Used a `mutex` within a kernel workqueue to protect the linked list of monitored PIDs, ensuring thread safety during registration and periodic checks.

---

## 3. Build and Run Instructions

### Compilation
```bash
make
```

### Loading the Kernel Module
```bash
sudo insmod monitor.ko
```

### Starting the Supervisor
```bash
sudo ./engine supervisor ./rootfs-base
```

### Launching Containers
```bash
# Terminal 2
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
```
# Multi-Container Runtime

## 1. Team Information

- Team Member 1: VAGEESH PES2UG24CS663
- Team Member 2: ARYA V D PES2UG24CS663

## 2. Build, Load, and Run Instructions

Environment used:

- Ubuntu 22.04/24.04 VM
- Secure Boot: OFF
- Kernel headers installed for running kernel

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Build everything:

```bash
cd boilerplate
make
```

CI-safe build command (used by inherited workflow):

```bash
make -C boilerplate ci
```

Load kernel module and verify control device:

```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

Prepare rootfs and per-container copies:

```bash
cd boilerplate
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Copy workload binaries into container rootfs if needed:

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/
cp ./memory_hog ./rootfs-alpha/
```

Start supervisor (Terminal 1):

```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

Use CLI commands (Terminal 2):

```bash
cd boilerplate

# Required contract commands
sudo ./engine start alpha ./rootfs-alpha "/bin/sh" --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta "/bin/sh" --soft-mib 64 --hard-mib 96
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta

# Foreground execution
sudo ./engine run gamma ./rootfs-alpha "./cpu_hog" --nice 10
```

Cleanup:

```bash
cd boilerplate
sudo ./engine stop alpha
sudo ./engine stop beta
sudo rmmod monitor
sudo make clean
```

## 3. CLI Contract Implemented

Implemented command interface:

```bash
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

Semantics implemented:

- Default limits: soft = 40 MiB, hard = 64 MiB
- `start` is asynchronous and returns after supervisor accepts request
- `run` waits for container completion and returns final status
- Container logs are captured through bounded-buffer logging pipeline
- `stop` sets stop intent and triggers graceful termination path

## 4. Demo with Screenshots

### 4.1 Multi-Container Supervision
![Multi-container supervision](screenshots/os1.png)
Caption: Two containers run under one supervisor process with distinct host PIDs.

### 4.2 Metadata Tracking
![Metadata tracking](screenshots/os2.png)
Caption: `engine ps` shows tracked metadata and state transitions.

### 4.3 Bounded-Buffer Logging
![Bounded-buffer logging](screenshots/os3.png)
Caption: `logs/alpha.log` shows output captured through producer-consumer logging.

### 4.4 CLI and IPC
![CLI and IPC](screenshots/os4.png)
Caption: CLI command sent to supervisor over control IPC and response received.

### 4.5 Soft-Limit Warning
![Soft-limit warning](screenshots/os5and6.png)
Caption: `dmesg` shows soft-limit warning when RSS crosses configured soft limit.

### 4.6 Hard-Limit Enforcement
![Hard-limit enforcement](screenshots/os5and6.png)
Caption: `dmesg` shows hard-limit kill and supervisor metadata reflects forced termination.

### 4.7 Scheduling Experiment
![Scheduling experiment](screenshots/os7.png)
Caption: Different scheduling configurations show observable runtime differences.

### 4.8 Clean Teardown
![Clean teardown](screenshots/os8.png)
Caption: Containers are reaped, monitor entries removed, and teardown completes without stale state.

## 5. Engineering Analysis

### 5.1 Isolation Mechanisms

The runtime uses PID, UTS, and mount namespaces at clone time. PID namespace isolates process IDs inside each container. UTS namespace isolates hostname. Mount namespace isolates mount tables so each container can mount its own `/proc` without affecting host/global mount state. `chroot` changes the visible filesystem root for the process tree to the container rootfs. Containers still share the host kernel, scheduler, and physical memory.

### 5.2 Supervisor and Process Lifecycle

A long-running supervisor centralizes lifecycle management, metadata tracking, reaping, and policy enforcement. Child containers are created with `clone`, tracked by host PID and ID, and reaped by `waitpid` on `SIGCHLD`. This avoids zombie accumulation and keeps a single source of truth for final state and exit reason.

### 5.3 IPC, Threads, and Synchronization

Two IPC paths are used:

- Control plane: CLI to supervisor via UNIX domain socket.
- Logging plane: container stdout/stderr via pipes.

Logging uses a bounded buffer with mutex + condition variables (`not_full`, `not_empty`) to coordinate producers and consumer. Metadata has a separate lock to avoid races between command handling and child reaping. Without these locks, races could corrupt container list state, lose log chunks, or deadlock producer/consumer paths.

### 5.4 Memory Management and Enforcement

The kernel module measures RSS, which reflects resident physical pages currently mapped by a process. RSS does not include all virtual address space reservations and can change dynamically with paging. Soft limits are warnings for observation and diagnostics. Hard limits enforce safety by terminating memory-abusive workloads. Kernel-space enforcement is required for trusted, non-bypassable policy application.

### 5.5 Scheduling Behavior

Experiments with `cpu_hog` and `io_pulse` under different `nice` values show CFS tradeoffs between fairness and priority bias. Higher-priority tasks (lower nice) receive proportionally more CPU share and complete faster; I/O-bound workloads often preserve responsiveness due to frequent sleep/wake behavior.

## 6. Design Decisions and Tradeoffs

### 6.1 Namespace Isolation

- Choice: PID/UTS/mount namespaces with `chroot` and private `/proc` mount.
- Tradeoff: `chroot` is simpler than `pivot_root` but less strict against certain escape patterns if misconfigured.
- Justification: Simpler bring-up with enough isolation for project scope and demos.

### 6.2 Supervisor Architecture

- Choice: Single long-running parent supervisor handling all container metadata.
- Tradeoff: Central loop can become a bottleneck under heavy control traffic.
- Justification: Deterministic lifecycle management and easier debugging.

### 6.3 IPC and Logging Pipeline

- Choice: UNIX socket for control path and pipe plus bounded-buffer for logs.
- Tradeoff: More moving parts than direct file writes.
- Justification: Separates concerns, improves concurrency, and demonstrates IPC design clearly.

### 6.4 Kernel Monitor

- Choice: Linked-list tracked entries with lock protection and periodic RSS checks.
- Tradeoff: Periodic sampling can miss very short spikes.
- Justification: Predictable implementation with low complexity and clear soft/hard policy enforcement.

### 6.5 Scheduling Experiments

- Choice: Compare CPU-bound and I/O-bound workloads across different nice levels.
- Tradeoff: Results can vary slightly by VM host load.
- Justification: Directly demonstrates scheduler behavior in observable, repeatable tests.

## 7. Scheduler Experiment Results

### 7.1 Raw Measurements

Replace with your measured values:

| Workload | Config A | Config B | Metric | Observation |
| --- | --- | --- | --- | --- |
| CPU-bound (`cpu_hog`) | `nice=19` | `nice=-10` | Completion time | High-priority finishes much faster |
| Mixed (`cpu_hog` + `io_pulse`) | same nice | different nice | Throughput/latency | I/O task remains responsive while CPU shares shift |

### 7.2 Comparison and Interpretation

Your results should explain how Linux CFS balances fairness and responsiveness. Show at least one side-by-side numeric comparison and connect it to priority (`nice`), blocking behavior (I/O sleep/wake), and CPU share.

## 8. Source Files Included

Required files present:

- `boilerplate/engine.c`
- `boilerplate/monitor.c`
- `boilerplate/monitor_ioctl.h`
- `boilerplate/cpu_hog.c`
- `boilerplate/io_pulse.c`
- `boilerplate/memory_hog.c`
- `boilerplate/Makefile`
