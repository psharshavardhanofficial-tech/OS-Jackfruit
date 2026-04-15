cat << 'EOF' > README.md
# 📦 Multi-Container Runtime (OS-Jackfruit)

## 👨‍💻 Team Information

- **Name:** Harshavardhan P.S  
- **SRN:** PES2UG25CS817  
- **Course:** B.Tech CSE – PES University  

---

## ⚙️ Project Overview

This project implements a **lightweight multi-container runtime** using core Operating System concepts.

It simulates how container engines (like Docker) work internally by building features from scratch.

### 🔹 Features

- Process isolation using namespaces  
- Supervisor-based container lifecycle management  
- IPC using UNIX sockets and pipes  
- Kernel-level memory monitoring  
- CPU scheduling experimentation  

---

## 🏗️ System Architecture

\`\`\`
User (CLI)
   ↓
Engine (CLI Tool)
   ↓
Supervisor Process
   ↓
Containers (Namespaces + chroot)
   ↓
Kernel Module (Memory Monitor)
\`\`\`

---

## 🛠️ Setup & Execution Guide

### 🔹 1. Prerequisites

\`\`\`bash
sudo apt update
sudo apt install -y build-essential linux-headers-\$(uname -r)
\`\`\`

---

### 🔹 2. Build Project

\`\`\`bash
cd boilerplate
make
\`\`\`

(Optional check)

\`\`\`bash
make -C boilerplate ci
\`\`\`

---

### 🔹 3. Load Kernel Module

\`\`\`bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
\`\`\`

---

### 🔹 4. Setup Root Filesystem

\`\`\`bash
mkdir -p rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
\`\`\`

---

### 🔹 5. Start Supervisor (Terminal 1)

\`\`\`bash
sudo ./engine supervisor ./rootfs-base
\`\`\`

---

### 🔹 6. Run Containers (Terminal 2)

\`\`\`bash
# Start containers
sudo ./engine start alpha ./rootfs-alpha "/bin/sh" --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta "/bin/sh" --soft-mib 64 --hard-mib 96

# Commands
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
\`\`\`

---

### 🔹 7. Foreground Execution

\`\`\`bash
sudo ./engine run gamma ./rootfs-alpha "./cpu_hog" --nice 10
\`\`\`

---

### 🔹 8. Cleanup

\`\`\`bash
sudo ./engine stop alpha
sudo ./engine stop beta

dmesg | tail -n 50

sudo rmmod monitor
sudo make clean
\`\`\`

---

## 📸 Demo Description

- Multiple containers running under one supervisor  
- \`engine ps\` shows metadata and states  
- Logs captured using bounded-buffer system  
- IPC communication via UNIX sockets  
- Memory limits enforced via kernel module  
- Scheduler behavior observed using \`nice` values  
- Clean shutdown without zombie processes  

---

## 🧠 Core Concepts Explained

### 🔸 Container Isolation

- PID Namespace → Isolates process IDs  
- UTS Namespace → Custom hostname  
- Mount Namespace → Isolated filesystem view  
- \`chroot\` → Restricts root directory  

---

### 🔸 Supervisor & Lifecycle

- Single long-running supervisor process  
- Manages container creation and cleanup  
- Uses \`waitpid()\` to prevent zombie processes  

---

### 🔸 IPC & Synchronization

- UNIX Domain Socket → CLI communication  
- Pipes → Logging system  
- Mutex + condition variables → synchronization  

---

### 🔸 Memory Management

- RSS (Resident Set Size) tracking  
- Soft Limit → Warning  
- Hard Limit → Process killed (SIGKILL)  

---

### 🔸 Scheduling (CFS)

- \`nice\` controls CPU priority  

| Nice Value | Effect |
|-----------|--------|
| -10 | Faster |
| 19 | Slower |

---

## 📊 Experiment Results

- CPU-bound tasks vary with priority  
- I/O tasks remain stable  
- Demonstrates real Linux scheduling behavior  

---

## 📂 Project Structure

\`\`\`
boilerplate/
├── engine.c
├── monitor.c
├── monitor_ioctl.h
├── cpu_hog.c
├── io_pulse.c
├── memory_hog.c
├── Makefile
\`\`\`

---

## 🎯 Conclusion

This project demonstrates:

- Container runtime fundamentals  
- OS-level isolation  
- Kernel interaction  
- Scheduling and memory control  

---

## 🚀 Future Improvements

- Add network namespaces  
- Implement cgroups  
- Add container image support  
- Build UI dashboard  

---

## 📚 References

- Linux Kernel Docs  
- OS Concepts  
- Namespace Documentation  

EOF
