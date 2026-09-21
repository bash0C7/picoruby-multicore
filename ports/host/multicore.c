/* host port: a pthread worker. The kernel really runs on another core, so the
 * host tests exercise the same submit / poll / result path the boards use.
 *
 * MULTICORE_HOST_CORE_BUSY=1 in the environment (any other value clears it) makes MULTICORE_start report
 * that the core is taken, the way a board reports it when another user holds
 * core 1. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../../include/multicore.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static bool wake_pending;
static bool stop_requested;
static bool worker_done;  /* atomic: read without the mutex */
static bool thread_alive;
static pthread_t thread;

#define STOP_WAIT_MS 2000

static void
host_wake(void)
{
  pthread_mutex_lock(&mu);
  wake_pending = true;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
}

#define MC_WAKE() host_wake()
#include "../../include/multicore_engine.h"

static bool
core_busy_requested(void)
{
  const char *v = getenv("MULTICORE_HOST_CORE_BUSY");
  return v != NULL && v[0] == '1';
}

static void *
worker_main(void *arg)
{
  (void)arg;
  pthread_mutex_lock(&mu);
  while (!stop_requested) {
    if (!wake_pending) {
      pthread_cond_wait(&cv, &mu);
      continue;
    }
    wake_pending = false;
    pthread_mutex_unlock(&mu);
    while (mc_worker_run_one()) {
    }
    pthread_mutex_lock(&mu);
  }
  __atomic_store_n(&worker_done, true, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&mu);
  return NULL;
}

uint32_t
MULTICORE_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

int
MULTICORE_start(void)
{
  if (mc_running) {
    return MULTICORE_OK;
  }
  if (thread_alive) {
    /* A stop that timed out left the thread behind. */
    if (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)) {
      return MULTICORE_CORE_BUSY;
    }
    pthread_join(thread, NULL);
    thread_alive = false;
  }
  if (core_busy_requested()) {
    return MULTICORE_CORE_BUSY;
  }
  mc_reset_slots();
  stop_requested = false;
  wake_pending = false;
  __atomic_store_n(&worker_done, false, __ATOMIC_RELEASE);
  if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
    return MULTICORE_START_FAILED;
  }
  thread_alive = true;
  mc_running = true;
  return MULTICORE_OK;
}

bool
MULTICORE_stop(void)
{
  if (!thread_alive) {
    mc_running = false;
    return true;
  }
  mc_running = false;
  pthread_mutex_lock(&mu);
  stop_requested = true;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  int waited = 0;
  while (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) && waited < STOP_WAIT_MS) {
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
    waited++;
  }
  if (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)) {
    return false;
  }
  pthread_join(thread, NULL);
  thread_alive = false;
  mc_reset_slots();
  return true;
}
