/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    void *child_stack;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_arg_t;

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes);

static int unregister_from_monitor(int monitor_fd,
                                   const char *container_id,
                                   pid_t host_pid);

static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_supervisor_reap = 0;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t rc = write(fd, buf + written, len - written);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += (size_t)rc;
    }

    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t rc = read(fd, (char *)buf + total, len - total);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return -1;
        total += (size_t)rc;
    }

    return 0;
}

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    snprintf(dst, dst_size, "%s", src);
}

static int apply_nice_value(int nice_value)
{
    if (nice_value == 0)
        return 0;

    if (setpriority(PRIO_PROCESS, 0, nice_value) < 0) {
        perror("setpriority");
        return -1;
    }

    return 0;
}

static int setup_container_root(const char *id, const char *rootfs)
{
    if (sethostname(id, strnlen(id, CONTAINER_ID_LEN)) < 0) {
        perror("sethostname");
        return -1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private /");
        return -1;
    }

    if (chdir(rootfs) < 0 || chroot(".") < 0 || chdir("/") < 0) {
        perror("container rootfs setup");
        return -1;
    }

    return 0;
}

static int setup_container_proc(void)
{
    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return -1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return -1;
    }

    return 0;
}

static int redirect_container_logs(int log_write_fd)
{
    if (dup2(log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return -1;
    }

    if (close(log_write_fd) < 0) {
        perror("close log fd");
        return -1;
    }

    return 0;
}

static void *producer_thread(void *arg)
{
    producer_arg_t *producer = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t nread;

    memset(&item, 0, sizeof(item));
    copy_cstr(item.container_id, sizeof(item.container_id), producer->container_id);

    for (;;) {
        nread = read(producer->read_fd, item.data, sizeof(item.data));
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (nread == 0)
            break;

        item.length = (size_t)nread;
        if (bounded_buffer_push(&producer->ctx->log_buffer, &item) != 0)
            break;
    }

    close(producer->read_fd);
    free(producer);
    return NULL;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    for (;;) {
        char path[PATH_MAX];
        int fd;

        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 1)
            break;
        if (rc != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;

        (void)write_all(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (apply_nice_value(cfg->nice_value) < 0)
        return 1;

    if (setup_container_root(cfg->id, cfg->rootfs) < 0)
        return 1;

    if (setup_container_proc() < 0)
        return 1;

    if (redirect_container_logs(cfg->log_write_fd) < 0)
        return 1;

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 1;
}

static void sigchld_handler(int signum)
{
    (void)signum;
    g_supervisor_reap = 1;
}

static void stop_handler(int signum)
{
    (void)signum;
    g_supervisor_stop = 1;
}

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx,
                                                        const char *id)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0)
            return cur;
        cur = cur->next;
    }

    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }

    return NULL;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    pid_t pid;
    int status;

    for (;;) {
        char id[CONTAINER_ID_LEN] = {0};
        pid_t host_pid = -1;
        container_record_t *record;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_by_pid_locked(ctx, pid);
        if (record) {
            copy_cstr(id, sizeof(id), record->id);
            host_pid = record->host_pid;
            if (WIFEXITED(status)) {
                record->exit_code = WEXITSTATUS(status);
                record->exit_signal = 0;
                record->state = record->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                record->exit_code = -1;
                record->exit_signal = WTERMSIG(status);
                record->state = record->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
            }
            if (record->child_stack) {
                free(record->child_stack);
                record->child_stack = NULL;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (record && ctx->monitor_fd >= 0)
            (void)unregister_from_monitor(ctx->monitor_fd, id, host_pid);
    }
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           pid_t *out_pid,
                           char *error_message,
                           size_t error_message_len)
{
    int pipefd[2] = {-1, -1};
    void *stack = NULL;
    child_config_t *cfg = NULL;
    container_record_t *record = NULL;
    producer_arg_t *producer = NULL;
    pthread_t producer_tid;
    int rc;
    pid_t child_pid;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_by_id_locked(ctx, req->container_id);
    if (record && (record->state == CONTAINER_STARTING || record->state == CONTAINER_RUNNING)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(error_message, error_message_len, "container id already running: %s", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        snprintf(error_message, error_message_len, "failed to create logs directory");
        return -1;
    }

    if (pipe(pipefd) < 0) {
        snprintf(error_message, error_message_len, "pipe failed: %s", strerror(errno));
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    stack = malloc(STACK_SIZE);
    record = calloc(1, sizeof(*record));
    producer = calloc(1, sizeof(*producer));
    if (!cfg || !stack || !record || !producer) {
        snprintf(error_message, error_message_len, "out of memory");
        goto fail;
    }

    copy_cstr(cfg->id, sizeof(cfg->id), req->container_id);
    copy_cstr(cfg->rootfs, sizeof(cfg->rootfs), req->rootfs);
    copy_cstr(cfg->command, sizeof(cfg->command), req->command);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    child_pid = clone(child_fn,
                      (char *)stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        snprintf(error_message, error_message_len, "clone failed: %s", strerror(errno));
        goto fail;
    }

    close(pipefd[1]);
    pipefd[1] = -1;
    free(cfg);
    cfg = NULL;

    copy_cstr(record->id, sizeof(record->id), req->container_id);
    record->host_pid = child_pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    record->child_stack = stack;
    stack = NULL;

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd,
                                  req->container_id,
                                  child_pid,
                                  req->soft_limit_bytes,
                                  req->hard_limit_bytes) < 0) {
            perror("register_with_monitor");
        }
    }

    producer->ctx = ctx;
    producer->read_fd = pipefd[0];
    copy_cstr(producer->container_id, sizeof(producer->container_id), req->container_id);

    rc = pthread_create(&producer_tid, NULL, producer_thread, producer);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create(producer)");
        close(pipefd[0]);
        free(producer);
    } else {
        (void)pthread_detach(producer_tid);
    }

    *out_pid = child_pid;
    return 0;

fail:
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    free(cfg);
    free(stack);
    free(record);
    free(producer);
    return -1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    copy_cstr(req.container_id, sizeof(req.container_id), container_id);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    copy_cstr(req.container_id, sizeof(req.container_id), container_id);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sigaction sa;
    struct sockaddr_un addr;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        perror("mkdir logs");

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open /dev/container_monitor");

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_cstr(addr.sun_path, sizeof(addr.sun_path), CONTROL_PATH);

    unlink(CONTROL_PATH);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    (void)sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create(logger)");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fprintf(stderr, "Supervisor started (base-rootfs=%s, control=%s)\n", rootfs, CONTROL_PATH);

    while (!ctx.should_stop) {
        fd_set readfds;
        struct timeval tv;
        int sel;

        if (g_supervisor_stop)
            ctx.should_stop = 1;

        if (g_supervisor_reap) {
            reap_children(&ctx);
            g_supervisor_reap = 0;
        }

        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        sel = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (sel > 0 && FD_ISSET(ctx.server_fd, &readfds)) {
            control_request_t req;
            control_response_t resp;
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0)
                continue;

            memset(&resp, 0, sizeof(resp));
            resp.status = 1;

            if (read_all(client_fd, &req, sizeof(req)) != 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "failed to read request");
                (void)write_all(client_fd, (const char *)&resp, sizeof(resp));
                close(client_fd);
                continue;
            }

            switch (req.kind) {
            case CMD_START:
            case CMD_RUN:
            {
                pid_t pid = -1;
                if (start_container(&ctx, &req, &pid, resp.message, sizeof(resp.message)) != 0) {
                    resp.status = 1;
                    break;
                }

                if (req.kind == CMD_START) {
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message),
                             "started container=%s pid=%d", req.container_id, pid);
                    break;
                }

                for (;;) {
                    container_record_t *record;
                    int done = 0;

                    reap_children(&ctx);
                    pthread_mutex_lock(&ctx.metadata_lock);
                    record = find_container_by_pid_locked(&ctx, pid);
                    if (record && record->state != CONTAINER_RUNNING &&
                        record->state != CONTAINER_STARTING) {
                        if (record->exit_signal > 0)
                            resp.status = 128 + record->exit_signal;
                        else
                            resp.status = record->exit_code;
                        snprintf(resp.message, sizeof(resp.message),
                                 "container=%s finished status=%d", req.container_id, resp.status);
                        done = 1;
                    }
                    pthread_mutex_unlock(&ctx.metadata_lock);

                    if (done)
                        break;
                    usleep(100000);
                }
                break;
            }
            case CMD_PS:
            {
                container_record_t *cur;
                size_t used = 0;

                resp.status = 0;
                resp.message[0] = '\0';

                pthread_mutex_lock(&ctx.metadata_lock);
                cur = ctx.containers;
                if (!cur) {
                    snprintf(resp.message, sizeof(resp.message), "no containers tracked");
                } else {
                    while (cur && used < sizeof(resp.message) - 1) {
                        int n = snprintf(resp.message + used,
                                         sizeof(resp.message) - used,
                                         "%s pid=%d %s; ",
                                         cur->id,
                                         cur->host_pid,
                                         state_to_string(cur->state));
                        if (n <= 0 || (size_t)n >= sizeof(resp.message) - used)
                            break;
                        used += (size_t)n;
                        cur = cur->next;
                    }
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
                break;
            }
            case CMD_LOGS:
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "LOG_PATH %s/%s.log", LOG_DIR, req.container_id);
                break;
            case CMD_STOP:
            {
                container_record_t *record;
                pid_t pid = -1;

                pthread_mutex_lock(&ctx.metadata_lock);
                record = find_container_by_id_locked(&ctx, req.container_id);
                if (record) {
                    record->stop_requested = 1;
                    pid = record->host_pid;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);

                if (!record) {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "container not found: %s", req.container_id);
                    break;
                }

                if (kill(pid, SIGTERM) < 0) {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "failed to stop %s: %s", req.container_id, strerror(errno));
                    break;
                }

                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "stop requested for %s", req.container_id);
                break;
            }
            default:
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "unsupported command");
                break;
            }

            (void)write_all(client_fd, (const char *)&resp, sizeof(resp));
            close(client_fd);
        }
    }

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *cur = ctx.containers;
        while (cur) {
            if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
                cur->stop_requested = 1;
                (void)kill(cur->host_pid, SIGTERM);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    reap_children(&ctx);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    (void)pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    pthread_mutex_lock(&ctx.metadata_lock);
    while (ctx.containers) {
        container_record_t *next = ctx.containers->next;
        free(ctx.containers->child_stack);
        free(ctx.containers);
        ctx.containers = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_cstr(addr.sun_path, sizeof(addr.sun_path), CONTROL_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (write_all(sock, (const char *)req, sizeof(*req)) != 0) {
        perror("write request");
        close(sock);
        return 1;
    }

    if (read_all(sock, &resp, sizeof(resp)) != 0) {
        perror("read response");
        close(sock);
        return 1;
    }

    close(sock);

    if (req->kind == CMD_LOGS && resp.status == 0 &&
        strncmp(resp.message, "LOG_PATH ", 9) == 0) {
        const char *path = resp.message + 9;
        FILE *fp = fopen(path, "r");
        if (!fp) {
            perror("fopen logs");
            return 1;
        }

        for (;;) {
            char buf[LOG_CHUNK_SIZE];
            size_t n = fread(buf, 1, sizeof(buf), fp);
            if (n > 0)
                (void)fwrite(buf, 1, n, stdout);
            if (n < sizeof(buf))
                break;
        }

        fclose(fp);
        return 0;
    }

    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);

    if (req->kind == CMD_RUN)
        return resp.status;

    return resp.status == 0 ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);
    copy_cstr(req.rootfs, sizeof(req.rootfs), argv[3]);
    copy_cstr(req.command, sizeof(req.command), argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);
    copy_cstr(req.rootfs, sizeof(req.rootfs), argv[3]);
    copy_cstr(req.command, sizeof(req.command), argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
