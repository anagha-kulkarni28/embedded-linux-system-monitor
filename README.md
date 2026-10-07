# Embedded Linux System Monitor

A lightweight system monitoring utility written in C for Linux and embedded
Linux targets. It reads the `/proc` filesystem and `statvfs()` directly, so it
needs nothing beyond libc and runs on BusyBox-based systems (glibc, musl, uClibc).

```
========================================
       EMBEDDED LINUX SYSTEM MONITOR
========================================

CPU Usage       : 32.5%
Memory Usage    : 58.7%
Disk Usage      : 46.1% (8.5G / 18.5G) [/]
Network         : eth0  RX 12.3 KB/s  TX 4.5 KB/s
Processes       : 186
Uptime          : 6h 21m
Load Average    : 1.18

Alerts          : none

Refreshing every 2 seconds...
========================================
```

## Features

- CPU utilization from `/proc/stat` (delta between samples, iowait counted as idle)
- Memory utilization from `/proc/meminfo` using `MemAvailable` (cache is not counted as used)
- Disk usage for any mount point via `statvfs()`, using the same math as `df`
- Network throughput (RX/TX per second) from `/proc/net/dev`, auto-selecting the first non-loopback interface
- Process count, uptime, and load average
- Threshold alerts for CPU, memory, and disk, shown on screen
- Alert logging to a file; events are written only on state changes (ALERT / CLEARED), so the log is not flooded
- Configurable refresh interval, clean shutdown on SIGINT/SIGTERM
- Makefile build automation with ARM cross-compilation and QEMU test targets

## Build

```
make              # native
make arm          # static ARM binary (sysmon-arm)
make test-arm     # build for ARM and run under qemu-arm
```

ARM toolchain on Debian/Ubuntu: `sudo apt install gcc-arm-linux-gnueabihf qemu-user`

For other targets: `make CROSS_COMPILE=<toolchain-prefix>-`

## Usage

```
sysmon [options]
  -i SEC    refresh interval in seconds   (default 2)
  -n COUNT  stop after COUNT refreshes    (default: run forever)
  -p PATH   mount point for disk usage    (default /)
  -I IFACE  network interface             (default: first non-lo)
  -C PCT    CPU alert threshold           (default 90, 0 = off)
  -M PCT    memory alert threshold        (default 90, 0 = off)
  -D PCT    disk alert threshold          (default 90, 0 = off)
  -l FILE   append alert events to FILE
```

Examples:

```
./sysmon                                  # defaults
./sysmon -i 5 -I wlan0 -p /data           # custom interval, interface, mount
./sysmon -C 80 -M 85 -l /var/log/sysmon.log
```

Example log output:

```
2026-10-05 17:28:29 ALERT   Memory usage 91.3% (limit 90.0%)
2026-10-05 17:31:02 CLEARED Memory usage 74.8% (limit 90.0%)
```

## Notes

- Load average shows the 1-minute value; interpret it relative to the CPU core count.
- Process count is the number of numeric entries in `/proc` (processes, not threads).
- Tested natively on x86-64 and as a static ARM EABI5 binary under QEMU.
