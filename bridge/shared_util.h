#pragma once

#include <stdio.h>

extern FILE *g_log;

extern void log_error(const char *fmt, ...);
extern void log_console_impl(const char *prefix, const char *fmt, ...);
extern void log_console_always_impl(const char *prefix, const char *fmt, ...);
extern void delete_log_file(void);

#ifndef LOG_PREFIX
#define LOG_PREFIX ""
#endif

#define log_console(fmt, ...) log_console_impl(LOG_PREFIX, fmt, ##__VA_ARGS__)
#define log_always(fmt, ...)                                                   \
  log_console_always_impl(LOG_PREFIX, fmt, ##__VA_ARGS__)

#include <stdint.h>
#include <time.h>

static inline uint64_t bridge_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void bridge_cpu_relax(void)
{
#if defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#endif
}
