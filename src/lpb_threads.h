/*
 * lua-python-bridge: minimal portable threading primitives.
 */
#ifndef LPB_THREADS_H
#define LPB_THREADS_H

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef SRWLOCK lpb_mutex;
typedef CONDITION_VARIABLE lpb_cond;
#  define LPB_MUTEX_INIT SRWLOCK_INIT
#  ifdef _MSC_VER
#    define LPB_TLS __declspec(thread)
#  else
#    define LPB_TLS _Thread_local
#  endif
static inline void lpb_mutex_init(lpb_mutex *m) { InitializeSRWLock(m); }
static inline void lpb_mutex_destroy(lpb_mutex *m) { (void)m; }
static inline void lpb_mutex_lock(lpb_mutex *m) { AcquireSRWLockExclusive(m); }
static inline void lpb_mutex_unlock(lpb_mutex *m) { ReleaseSRWLockExclusive(m); }
static inline void lpb_cond_init(lpb_cond *c) { InitializeConditionVariable(c); }
static inline void lpb_cond_destroy(lpb_cond *c) { (void)c; }
static inline void lpb_cond_wait(lpb_cond *c, lpb_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static inline void lpb_cond_broadcast(lpb_cond *c) { WakeAllConditionVariable(c); }
static inline void lpb_sleep_ms(unsigned long ms) { Sleep(ms); }
#else
#  include <pthread.h>
#  include <time.h>
#  include <errno.h>
typedef pthread_mutex_t lpb_mutex;
typedef pthread_cond_t lpb_cond;
#  define LPB_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#  define LPB_TLS _Thread_local
static inline void lpb_mutex_init(lpb_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void lpb_mutex_destroy(lpb_mutex *m) { pthread_mutex_destroy(m); }
static inline void lpb_mutex_lock(lpb_mutex *m) { pthread_mutex_lock(m); }
static inline void lpb_mutex_unlock(lpb_mutex *m) { pthread_mutex_unlock(m); }
static inline void lpb_cond_init(lpb_cond *c) { pthread_cond_init(c, NULL); }
static inline void lpb_cond_destroy(lpb_cond *c) { pthread_cond_destroy(c); }
static inline void lpb_cond_wait(lpb_cond *c, lpb_mutex *m) { pthread_cond_wait(c, m); }
static inline void lpb_cond_broadcast(lpb_cond *c) { pthread_cond_broadcast(c); }
static inline void lpb_sleep_ms(unsigned long ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
#endif

#endif
