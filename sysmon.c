/*
 * sysmon - lightweight system monitor for embedded Linux
 *
 * Reads /proc and statvfs() only; no libraries beyond libc. Works with glibc,
 * musl, uClibc and BusyBox-based systems.
 *
 * Monitors: CPU, memory, disk, network throughput, process count, uptime,
 * load average. Raises threshold alerts and optionally logs them to a file.
 *
 * Usage: sysmon [options]
 *   -i SEC    refresh interval in seconds        (default 2)
 *   -n COUNT  stop after COUNT refreshes         (default: run forever)
 *   -p PATH   mount point for disk usage         (default /)
 *   -I IFACE  network interface                  (default: first non-lo)
 *   -C PCT    CPU alert threshold                (default 90, 0 = off)
 *   -M PCT    memory alert threshold             (default 90, 0 = off)
 *   -D PCT    disk alert threshold               (default 90, 0 = off)
 *   -l FILE   append alert events to FILE
 *   -h        show help
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define BAR "========================================"
#define NUM_ALERTS 3

/* ---- types ----------------------------------------------------------- */

typedef struct {
    unsigned long long idle;  /* idle + iowait */
    unsigned long long total; /* sum of all CPU time fields */
} cpu_sample_t;

typedef struct {
    double interval;
    long count;
    const char *disk_path;
    char iface[32];
    double cpu_limit, mem_limit, disk_limit;
    const char *log_path;
} config_t;

typedef struct {
    double cpu, mem, load;
    int procs;
    unsigned long uptime;
    int disk_ok;
    double disk_pct;
    unsigned long long disk_used, disk_total;
    int net_ok;
    double rx_rate, tx_rate; /* bytes per second */
} metrics_t;

typedef struct {
    const char *name;
    int active;
} alert_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---- CPU / memory / processes / uptime / load ------------------------ */

static int read_cpu(cpu_sample_t *s)
{
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;

    /* user nice system idle iowait irq softirq steal */
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return -1;

    s->idle = v[3] + v[4];
    s->total = 0;
    for (int i = 0; i < 8; i++)
        s->total += v[i];
    return 0;
}

static double cpu_percent(const cpu_sample_t *prev, const cpu_sample_t *cur)
{
    unsigned long long dtotal = cur->total - prev->total;
    unsigned long long didle = cur->idle - prev->idle;
    if (dtotal == 0)
        return 0.0;
    return 100.0 * (double)(dtotal - didle) / (double)dtotal;
}

/* Uses MemAvailable when present (kernel >= 3.14), otherwise estimates it. */
static int read_mem_percent(double *pct)
{
    char line[128];
    unsigned long long total = 0, avail = 0, free_kb = 0, buffers = 0, cached = 0, val;
    int have_avail = 0;

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %llu", &val) == 1)
            total = val;
        else if (sscanf(line, "MemAvailable: %llu", &val) == 1) {
            avail = val;
            have_avail = 1;
        } else if (sscanf(line, "MemFree: %llu", &val) == 1)
            free_kb = val;
        else if (sscanf(line, "Buffers: %llu", &val) == 1)
            buffers = val;
        else if (sscanf(line, "Cached: %llu", &val) == 1)
            cached = val;
    }
    fclose(f);

    if (total == 0)
        return -1;
    if (!have_avail)
        avail = free_kb + buffers + cached;
    if (avail > total)
        avail = total;

    *pct = 100.0 * (double)(total - avail) / (double)total;
    return 0;
}

/* Counts numeric directories in /proc (processes, not threads). */
static int count_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (isdigit((unsigned char)e->d_name[0]))
            count++;
    }
    closedir(d);
    return count;
}

static int read_uptime(unsigned long *secs)
{
    double up;
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return -1;
    int n = fscanf(f, "%lf", &up);
    fclose(f);
    if (n != 1)
        return -1;
    *secs = (unsigned long)up;
    return 0;
}

static int read_load(double *l1)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return -1;
    int n = fscanf(f, "%lf", l1);
    fclose(f);
    return n == 1 ? 0 : -1;
}

/* ---- disk ------------------------------------------------------------ */

/* Same math as df: used / (used + space available to unprivileged users). */
static int read_disk(const char *path, double *pct,
                     unsigned long long *used, unsigned long long *total)
{
    struct statvfs sv;
    if (statvfs(path, &sv) != 0)
        return -1;

    unsigned long long frsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    unsigned long long size = (unsigned long long)sv.f_blocks * frsize;
    unsigned long long freeb = (unsigned long long)sv.f_bfree * frsize;
    unsigned long long availb = (unsigned long long)sv.f_bavail * frsize;
    if (size == 0)
        return -1;

    *used = size - freeb;
    *total = *used + availb;
    *pct = *total ? 100.0 * (double)*used / (double)*total : 0.0;
    return 0;
}

/* ---- network --------------------------------------------------------- */

/* Parses /proc/net/dev. If iface is NULL, writes the first non-lo name to out. */
static int net_scan(const char *iface, char *out, size_t outlen,
                    unsigned long long *rx, unsigned long long *tx)
{
    char line[256];
    int found = -1;

    FILE *f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':'); /* header lines have no colon */
        if (!colon)
            continue;
        *colon = '\0';
        char *name = line;
        while (*name == ' ')
            name++;

        if (iface == NULL) {
            if (strcmp(name, "lo") != 0) {
                snprintf(out, outlen, "%.15s", name); /* IFNAMSIZ-1 */
                found = 0;
                break;
            }
            continue; /* skip loopback, keep looking */
        }
        if (strcmp(name, iface) == 0) {
            /* fields: rx_bytes ... (7 skipped) ... tx_bytes */
            if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", rx, tx) == 2)
                found = 0;
            break;
        }
    }
    fclose(f);

    if (iface == NULL && found != 0) {
        snprintf(out, outlen, "lo"); /* nothing but loopback */
        found = 0;
    }
    return found;
}

/* ---- formatting ------------------------------------------------------ */

static void format_uptime(unsigned long s, char *out, size_t len)
{
    unsigned long d = s / 86400;
    unsigned long h = (s % 86400) / 3600;
    unsigned long m = (s % 3600) / 60;

    if (d > 0)
        snprintf(out, len, "%lud %luh %lum", d, h, m);
    else
        snprintf(out, len, "%luh %lum", h, m);
}

static void format_size(unsigned long long bytes, char *out, size_t len)
{
    static const char units[] = "BKMGT";
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, len, "%.0f%c", v, units[u]);
    else
        snprintf(out, len, "%.1f%c", v, units[u]);
}

static void format_rate(double bps, char *out, size_t len)
{
    if (bps < 1024.0)
        snprintf(out, len, "%.0f B/s", bps);
    else if (bps < 1024.0 * 1024.0)
        snprintf(out, len, "%.1f KB/s", bps / 1024.0);
    else
        snprintf(out, len, "%.1f MB/s", bps / (1024.0 * 1024.0));
}

/* ---- alerts & logging ------------------------------------------------ */

static void log_event(FILE *log, const char *kind, const char *name,
                      double value, double limit)
{
    if (!log)
        return;

    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(log, "%s %-7s %s %.1f%% (limit %.1f%%)\n", ts, kind, name, value, limit);
    fflush(log);
}

/*
 * Returns 1 while the value is over the limit. Logs only on state changes
 * (ALERT when it goes over, CLEARED when it recovers) so the log is not
 * flooded on every refresh.
 */
static int check_alert(alert_t *a, double value, double limit, FILE *log)
{
    int over = (limit > 0.0 && value > limit);

    if (over && !a->active)
        log_event(log, "ALERT", a->name, value, limit);
    else if (!over && a->active)
        log_event(log, "CLEARED", a->name, value, limit);

    a->active = over;
    return over;
}

/* ---- display --------------------------------------------------------- */

static void render(const metrics_t *m, const config_t *cfg, const char *iface,
                   char msgs[][64], int nmsg, int clear)
{
    char up[32], used[16], total[16], rx[24], tx[24];

    format_uptime(m->uptime, up, sizeof up);

    if (clear)
        fputs("\033[H\033[J", stdout); /* home + clear screen */

    printf("%s\n", BAR);
    printf("       EMBEDDED LINUX SYSTEM MONITOR\n");
    printf("%s\n\n", BAR);

    printf("CPU Usage       : %.1f%%\n", m->cpu);
    printf("Memory Usage    : %.1f%%\n", m->mem);

    if (m->disk_ok) {
        format_size(m->disk_used, used, sizeof used);
        format_size(m->disk_total, total, sizeof total);
        printf("Disk Usage      : %.1f%% (%s / %s) [%s]\n",
               m->disk_pct, used, total, cfg->disk_path);
    } else {
        printf("Disk Usage      : n/a [%s]\n", cfg->disk_path);
    }

    if (m->net_ok) {
        format_rate(m->rx_rate, rx, sizeof rx);
        format_rate(m->tx_rate, tx, sizeof tx);
        printf("Network         : %s  RX %s  TX %s\n", iface, rx, tx);
    } else {
        printf("Network         : n/a (%s)\n", iface);
    }

    printf("Processes       : %d\n", m->procs);
    printf("Uptime          : %s\n", up);
    printf("Load Average    : %.2f\n\n", m->load);

    if (nmsg == 0) {
        printf("Alerts          : none\n");
    } else {
        printf("Alerts          : %d active\n", nmsg);
        for (int i = 0; i < nmsg; i++)
            printf("  [!] %s\n", msgs[i]);
    }

    printf("\nRefreshing every %g seconds...\n", cfg->interval);
    printf("%s\n", BAR);
    fflush(stdout);
}

/* ---- helpers --------------------------------------------------------- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_for(double seconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL); /* returns early on signal, which is what we want */
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -i SEC    refresh interval in seconds   (default 2)\n"
            "  -n COUNT  stop after COUNT refreshes    (default: forever)\n"
            "  -p PATH   mount point for disk usage    (default /)\n"
            "  -I IFACE  network interface             (default: first non-lo)\n"
            "  -C PCT    CPU alert threshold           (default 90, 0 = off)\n"
            "  -M PCT    memory alert threshold        (default 90, 0 = off)\n"
            "  -D PCT    disk alert threshold          (default 90, 0 = off)\n"
            "  -l FILE   append alert events to FILE\n"
            "  -h        show this help\n",
            prog);
}

static int parse_args(int argc, char **argv, config_t *cfg)
{
    int opt;

    while ((opt = getopt(argc, argv, "i:n:p:I:C:M:D:l:h")) != -1) {
        switch (opt) {
        case 'i':
            cfg->interval = strtod(optarg, NULL);
            if (cfg->interval < 0.1) {
                fprintf(stderr, "interval must be >= 0.1 seconds\n");
                return -1;
            }
            break;
        case 'n':
            cfg->count = strtol(optarg, NULL, 10);
            break;
        case 'p':
            cfg->disk_path = optarg;
            break;
        case 'I':
            snprintf(cfg->iface, sizeof cfg->iface, "%s", optarg);
            break;
        case 'C':
            cfg->cpu_limit = strtod(optarg, NULL);
            break;
        case 'M':
            cfg->mem_limit = strtod(optarg, NULL);
            break;
        case 'D':
            cfg->disk_limit = strtod(optarg, NULL);
            break;
        case 'l':
            cfg->log_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    config_t cfg = {
        .interval = 2.0, .count = 0, .disk_path = "/", .iface = "",
        .cpu_limit = 90.0, .mem_limit = 90.0, .disk_limit = 90.0,
        .log_path = NULL,
    };

    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    FILE *log = NULL;
    if (cfg.log_path) {
        log = fopen(cfg.log_path, "a");
        if (!log) {
            perror("sysmon: cannot open log file");
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (cfg.iface[0] == '\0')
        net_scan(NULL, cfg.iface, sizeof cfg.iface, NULL, NULL);

    int clear = isatty(STDOUT_FILENO);
    alert_t alerts[NUM_ALERTS] = {
        {"CPU usage", 0}, {"Memory usage", 0}, {"Disk usage", 0},
    };

    cpu_sample_t cpu_prev, cpu_cur;
    unsigned long long rx_prev = 0, tx_prev = 0, rx_cur = 0, tx_cur = 0;

    if (read_cpu(&cpu_prev) != 0) {
        fprintf(stderr, "sysmon: cannot read /proc/stat\n");
        return 1;
    }
    int net_prev_ok = (net_scan(cfg.iface, NULL, 0, &rx_prev, &tx_prev) == 0);
    double t_prev = now_seconds();

    sleep_for(0.25); /* short gap so the first CPU/network reading is real */

    for (long i = 0; g_running && (cfg.count == 0 || i < cfg.count); i++) {
        metrics_t m;
        memset(&m, 0, sizeof m);

        if (read_cpu(&cpu_cur) != 0)
            break;
        m.cpu = cpu_percent(&cpu_prev, &cpu_cur);
        cpu_prev = cpu_cur;

        read_mem_percent(&m.mem);
        read_uptime(&m.uptime);
        read_load(&m.load);
        m.procs = count_processes();
        m.disk_ok = (read_disk(cfg.disk_path, &m.disk_pct,
                               &m.disk_used, &m.disk_total) == 0);

        double t_cur = now_seconds();
        double dt = t_cur - t_prev;
        int net_cur_ok = (net_scan(cfg.iface, NULL, 0, &rx_cur, &tx_cur) == 0);
        m.net_ok = net_cur_ok;
        if (net_cur_ok && net_prev_ok && dt > 0.0) {
            /* guard against counter wrap/reset */
            m.rx_rate = rx_cur >= rx_prev ? (double)(rx_cur - rx_prev) / dt : 0.0;
            m.tx_rate = tx_cur >= tx_prev ? (double)(tx_cur - tx_prev) / dt : 0.0;
        }
        rx_prev = rx_cur;
        tx_prev = tx_cur;
        net_prev_ok = net_cur_ok;
        t_prev = t_cur;

        /* threshold checks */
        double vals[NUM_ALERTS] = {m.cpu, m.mem, m.disk_ok ? m.disk_pct : 0.0};
        double lims[NUM_ALERTS] = {cfg.cpu_limit, cfg.mem_limit, cfg.disk_limit};
        char msgs[NUM_ALERTS][64];
        int nmsg = 0;
        for (int a = 0; a < NUM_ALERTS; a++) {
            if (check_alert(&alerts[a], vals[a], lims[a], log))
                snprintf(msgs[nmsg++], sizeof msgs[0], "%s %.1f%% > %.1f%%",
                         alerts[a].name, vals[a], lims[a]);
        }

        render(&m, &cfg, cfg.iface, msgs, nmsg, clear);

        if (cfg.count == 0 || i + 1 < cfg.count)
            sleep_for(cfg.interval);
    }

    if (clear)
        fputs("\n", stdout);
    if (log)
        fclose(log);
    return 0;
}
