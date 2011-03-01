/*
 * Copyright (C) 2011 Piotr Jaroszyński
 */

#ifndef _CUDARAMD_PRINT_H_
#define _CUDARAMD_PRINT_H_

#include <syslog.h> /* for LOG_* */

/* Whether to use syslog or not */
extern int use_syslog;

__attribute__ ((format (printf, 2, 3)))
extern void print(int prio, const char *fmt, ...);

static inline __attribute__ ((format (printf, 2, 3)))
void no_print(int prio, const char *fmt, ...)
{
}

#define pr_err(fmt, ...) print(LOG_ERR, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) print(LOG_INFO, fmt, ##__VA_ARGS__)

#ifdef DEBUG
#define pr_debug(fmt, ...) print(LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) no_print(LOG_DEBUG, fmt, ##__VA_ARGS__)
#endif

#endif /* _CUDARAMD_PRINT_H_ */
