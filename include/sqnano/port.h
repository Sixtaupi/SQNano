/*
 * Copyright (c) 2026 Romain Pelletant
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Operating-system abstraction: mutexes, condition variables, time.
 *
 * Zephyr uses k_mutex and k_condvar. Host builds (tests, tools) use POSIX
 * threads.
 */

#ifndef SQNANO_PORT_H_
#define SQNANO_PORT_H_

#include <stdint.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>

typedef struct k_mutex sqnano_mutex_t;
typedef struct k_condvar sqnano_cond_t;

#define SQNANO_MUTEX_DEFINE(name) K_MUTEX_DEFINE(name)

static inline void sqnano_mutex_init(sqnano_mutex_t *m)
{
	k_mutex_init(m);
}

static inline void sqnano_mutex_lock(sqnano_mutex_t *m)
{
	(void)k_mutex_lock(m, K_FOREVER);
}

static inline void sqnano_mutex_unlock(sqnano_mutex_t *m)
{
	(void)k_mutex_unlock(m);
}

static inline void sqnano_cond_init(sqnano_cond_t *c)
{
	k_condvar_init(c);
}

/** Wait at most @p ms milliseconds; returns 0 when signalled, -1 on timeout. */
static inline int sqnano_cond_wait_ms(sqnano_cond_t *c, sqnano_mutex_t *m, int32_t ms)
{
	return k_condvar_wait(c, m, ms < 0 ? K_FOREVER : K_MSEC(ms)) == 0 ? 0 : -1;
}

static inline void sqnano_cond_broadcast(sqnano_cond_t *c)
{
	(void)k_condvar_broadcast(c);
}

static inline int64_t sqnano_uptime_ms(void)
{
	return k_uptime_get();
}

static inline void sqnano_yield(void)
{
	k_yield();
}

#else /* host */
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef pthread_mutex_t sqnano_mutex_t;
typedef pthread_cond_t sqnano_cond_t;

#define SQNANO_MUTEX_DEFINE(name) sqnano_mutex_t name = PTHREAD_MUTEX_INITIALIZER

static inline void sqnano_mutex_init(sqnano_mutex_t *m)
{
	(void)pthread_mutex_init(m, NULL);
}

static inline void sqnano_mutex_lock(sqnano_mutex_t *m)
{
	(void)pthread_mutex_lock(m);
}

static inline void sqnano_mutex_unlock(sqnano_mutex_t *m)
{
	(void)pthread_mutex_unlock(m);
}

static inline void sqnano_cond_init(sqnano_cond_t *c)
{
	(void)pthread_cond_init(c, NULL);
}

static inline int sqnano_cond_wait_ms(sqnano_cond_t *c, sqnano_mutex_t *m, int32_t ms)
{
	struct timespec ts;

	if (ms < 0) {
		return pthread_cond_wait(c, m) == 0 ? 0 : -1;
	}
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(c, m, &ts) == 0 ? 0 : -1;
}

static inline void sqnano_cond_broadcast(sqnano_cond_t *c)
{
	(void)pthread_cond_broadcast(c);
}

static inline int64_t sqnano_uptime_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
}

static inline void sqnano_yield(void)
{
	(void)sched_yield();
}
#endif

#endif /* SQNANO_PORT_H_ */
