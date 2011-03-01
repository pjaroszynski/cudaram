/*
 * Copyright (C) 2011 Piotr Jaroszyński
 */

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include "print.h"

int use_syslog = 0;

__attribute__ ((format (printf, 2, 3)))
void print(int prio, const char *fmt, ...)
{
	va_list args;
	char msg[256];
	char *prefix;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	switch (prio) {
	case LOG_INFO:
		prefix = "[INF] ";
		break;
	case LOG_DEBUG:
		prefix = "[DBG] ";
		break;
	case LOG_ERR:
		prefix = "[ERR] ";
		break;
	default:
		prefix = "[???] ";
		break;
	}

	if (use_syslog)
		syslog(prio, "%s%s", prefix, msg);
	else
		fprintf(stderr, "%s%s", prefix, msg);
}
