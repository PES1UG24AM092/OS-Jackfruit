/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
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
    int stop_requested;
    char log_path[PATH_MAX];
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
    int fd;
    char id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} pipe_reader_arg_t;

static supervisor_ctx_t *g_ctx = NULL;

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

static int parse_mib_flag(const char *flag, const char *value,
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

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start_index)
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
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
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
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ================================================================
 * Bounded Buffer
 * ================================================================ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    if (buf->count == 0) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ================================================================
 * Logging Consumer Thread
 * ================================================================ */

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    mkdir(LOG_DIR, 0755);
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX];
        int fd;
        snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/* ================================================================
 * Pipe Reader (Producer Thread)
 * ================================================================ */

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *p = (pipe_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;
    while ((n = read(p->fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        strncpy(item.container_id, p->id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';
        bounded_buffer_push(p->buf, &item);
    }
    close(p->fd);
    free(p);
    return NULL;
}

/* ================================================================
 * Clone Child Entrypoint
 * ================================================================ */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *args[2];

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    sethostname(cfg->id, strlen(cfg->id));

    if (chroot(cfg->rootfs) != 0) { perror("chroot"); return 1; }
    if (chdir("/") != 0)          { perror("chdir");  return 1; }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc");

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    args[0] = cfg->command;
    args[1] = NULL;
    execv(cfg->command, args);
    perror("execv");
    return 1;
}

/* ================================================================
 * Monitor Registration
 * ================================================================ */

static int register_with_monitor(int monitor_fd, const char *container_id,
                                 pid_t host_pid, unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id,
                                   pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

/* ================================================================
 * Signal Handlers
 * ================================================================ */

static void handle_sigchld(int sig) { (void)sig; }
static void handle_shutdown(int sig) { (void)sig; if (g_ctx) g_ctx->should_stop = 1; }

/* ================================================================
 * Supervisor Helpers
 * ================================================================ */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        container_record_t *c;
        pthread_mutex_lock(&ctx->metadata_lock);
        c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->exit_code = WEXITSTATUS(wstatus);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    c->exit_signal = WTERMSIG(wstatus);
                    c->state = c->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int launch_container(supervisor_ctx_t *ctx, control_request_t *req)
{
    int pipefd[2];
    child_config_t *cfg;
    char *stack;
    pid_t pid;
    int flags;
    container_record_t *rec;
    pipe_reader_arg_t *pra;
    pthread_t reader;
    char log_path[PATH_MAX];

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "Container '%s' already exists\n", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) != 0) { perror("pipe"); return -1; }

    cfg = malloc(sizeof(child_config_t));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->id[CONTAINER_ID_LEN - 1]      = '\0';
    cfg->rootfs[PATH_MAX - 1]           = '\0';
    cfg->command[CHILD_COMMAND_LEN - 1] = '\0';
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return -1; }

    flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid   = clone(child_fn, stack + STACK_SIZE, flags, cfg);

    close(pipefd[1]);
    free(stack);
    free(cfg);

    if (pid < 0) { perror("clone"); close(pipefd[0]); return -1; }

    mkdir(LOG_DIR, 0755);
    snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    rec = calloc(1, sizeof(container_record_t));
    if (!rec) { close(pipefd[0]); return -1; }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->stop_requested   = 0;
    strncpy(rec->log_path, log_path, PATH_MAX - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    pra = malloc(sizeof(pipe_reader_arg_t));
    if (pra) {
        pra->fd  = pipefd[0];
        pra->buf = &ctx->log_buffer;
        strncpy(pra->id, req->container_id, CONTAINER_ID_LEN - 1);
        pra->id[CONTAINER_ID_LEN - 1] = '\0';
        pthread_create(&reader, NULL, pipe_reader_thread, pra);
        pthread_detach(reader);
    } else {
        close(pipefd[0]);
    }

    return pid;
}

/* ================================================================
 * Supervisor Event Loop
 * ================================================================ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: kernel monitor not available\n");

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(ctx.server_fd, 8);

    signal(SIGCHLD, handle_sigchld);
    signal(SIGINT,  handle_shutdown);
    signal(SIGTERM, handle_shutdown);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    fprintf(stderr, "[supervisor] Started. rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    while (!ctx.should_stop) {
        fd_set fds;
        struct timeval tv = {1, 0};
        int r, client_fd;
        control_request_t  req;
        control_response_t resp;

        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);
        r = select(ctx.server_fd + 1, &fds, NULL, NULL, &tv);
        reap_children(&ctx);
        if (r <= 0) continue;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        memset(&resp, 0, sizeof(resp));
        if (read(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
            close(client_fd); continue;
        }

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            int pid = launch_container(&ctx, &req);
            if (pid < 0) {
                resp.status = -1;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "Failed to start container '%s'", req.container_id);
            } else {
                resp.status = 0;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "Container '%s' started (pid=%d)", req.container_id, pid);
                if (req.kind == CMD_RUN) {
                    int wstatus;
                    write(client_fd, &resp, sizeof(resp));
                    close(client_fd);
                    waitpid(pid, &wstatus, 0);
                    reap_children(&ctx);
                    continue;
                }
            }

        } else if (req.kind == CMD_PS) {
            char *p = resp.message;
            int left = CONTROL_MESSAGE_LEN - 1;
            container_record_t *c;
            pthread_mutex_lock(&ctx.metadata_lock);
            c = ctx.containers;
            if (!c) {
                snprintf(p, left, "(no containers)");
            } else {
                while (c && left > 0) {
                    int n = snprintf(p, left, "%-16s pid=%-6d state=%s\n",
                                     c->id, c->host_pid,
                                     state_to_string(c->state));
                    p += n; left -= n;
                    c = c->next;
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            resp.status = 0;

        } else if (req.kind == CMD_LOGS) {
            container_record_t *c;
            pthread_mutex_lock(&ctx.metadata_lock);
            c = find_container(&ctx, req.container_id);
            if (c)
                snprintf(resp.message, CONTROL_MESSAGE_LEN, "%s", c->log_path);
            else
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "Container '%s' not found", req.container_id);
            resp.status = c ? 0 : -1;
            pthread_mutex_unlock(&ctx.metadata_lock);

        } else if (req.kind == CMD_STOP) {
            container_record_t *c;
            pthread_mutex_lock(&ctx.metadata_lock);
            c = find_container(&ctx, req.container_id);
            if (c && c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                c->state          = CONTAINER_STOPPED;
                kill(c->host_pid, SIGTERM);
                resp.status = 0;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "Sent SIGTERM to '%s' (pid=%d)",
                         req.container_id, c->host_pid);
            } else {
                resp.status = -1;
                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                         "Container '%s' not found or not running",
                         req.container_id);
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    {
        container_record_t *c;
        pthread_mutex_lock(&ctx.metadata_lock);
        c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                kill(c->host_pid, SIGTERM);
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    sleep(1);
    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    {
        container_record_t *c, *next;
        pthread_mutex_lock(&ctx.metadata_lock);
        c = ctx.containers;
        while (c) { next = c->next; free(c); c = next; }
        ctx.containers = NULL;
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Exited cleanly.\n");
    return 0;
}

/* ================================================================
 * CLI Client
 * ================================================================ */

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s "
                        "(is it running?)\n", CONTROL_PATH);
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write"); close(fd); return 1;
    }
    if (read(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("read"); close(fd); return 1;
    }
    close(fd);

    if (req->kind == CMD_LOGS && resp.status == 0) {
        FILE *f = fopen(resp.message, "r");
        if (f) {
            char buf[4096];
            while (fgets(buf, sizeof(buf), f)) fputs(buf, stdout);
            fclose(f);
        } else {
            fprintf(stderr, "Log file not found: %s\n", resp.message);
            return 1;
        }
    } else {
        printf("%s\n", resp.message);
    }

    return resp.status == 0 ? 0 : 1;
}

/* ================================================================
 * CLI Entry Points
 * ================================================================ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
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
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
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
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
