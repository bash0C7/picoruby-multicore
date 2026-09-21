/* esp32 port: unit "core1_lcg".
 *
 * One FreeRTOS task pinned to core 1 and two int32 queues. The task runs the
 * kernel only; it never touches the VM, never allocates and never calls back
 * into Ruby. Every call is non-blocking except close, which waits a bounded
 * time for the task to leave its job.
 *
 * This file needs the ESP-IDF include paths, so it is not compiled into
 * libmruby: the firmware build definition adds it to the IDF component's SRCS.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "../../include/multicore.h"

#define WORKER_CORE         1
#define WORKER_STACK_BYTES  4096
#define QUEUE_DEPTH         8
#define POLL_TICKS          10
#define SHUTDOWN_WAIT_TICKS 300

static QueueHandle_t in_queue = NULL;
static QueueHandle_t out_queue = NULL;
static TaskHandle_t worker = NULL;
static volatile bool stop_requested = false;
static volatile bool worker_done = false;

static void
worker_main(void *arg)
{
  (void)arg;
  while (!stop_requested) {
    int32_t n;
    if (xQueueReceive(in_queue, &n, POLL_TICKS) != pdTRUE) {
      continue;
    }
    int32_t result = MULTICORE_lcg(n);
    while (!stop_requested) {
      if (xQueueSend(out_queue, &result, POLL_TICKS) == pdTRUE) {
        break;
      }
    }
  }
  worker_done = true;
  vTaskDelete(NULL);
}

int
MULTICORE_open(const char *unit)
{
  if (strcmp(unit, "core1_lcg") != 0) {
    return MULTICORE_UNKNOWN_UNIT;
  }
  if (worker != NULL) {
    return MULTICORE_CORE_BUSY;
  }
  in_queue = xQueueCreate(QUEUE_DEPTH, sizeof(int32_t));
  out_queue = xQueueCreate(QUEUE_DEPTH, sizeof(int32_t));
  if (in_queue == NULL || out_queue == NULL) {
    goto fail;
  }
  stop_requested = false;
  worker_done = false;
  if (xTaskCreatePinnedToCore(worker_main, "multicore1", WORKER_STACK_BYTES, NULL,
                              tskIDLE_PRIORITY + 1, &worker, WORKER_CORE) != pdPASS) {
    worker = NULL;
    goto fail;
  }
  return MULTICORE_OK;
fail:
  if (in_queue != NULL) { vQueueDelete(in_queue); in_queue = NULL; }
  if (out_queue != NULL) { vQueueDelete(out_queue); out_queue = NULL; }
  return MULTICORE_START_FAILED;
}

bool
MULTICORE_try_send(int32_t n)
{
  if (worker == NULL) {
    return false;
  }
  return xQueueSend(in_queue, &n, 0) == pdTRUE;
}

bool
MULTICORE_try_receive(int32_t *out)
{
  if (worker == NULL) {
    return false;
  }
  return xQueueReceive(out_queue, out, 0) == pdTRUE;
}

bool
MULTICORE_close(void)
{
  if (worker == NULL) {
    return true;
  }
  stop_requested = true;
  TickType_t waited = 0;
  while (!worker_done && waited < SHUTDOWN_WAIT_TICKS) {
    vTaskDelay(1);
    waited++;
  }
  if (!worker_done) {
    return false;
  }
  /* worker_done is set just before vTaskDelete(NULL); give the idle task a tick to reclaim it. */
  vTaskDelay(2);
  worker = NULL;
  vQueueDelete(in_queue);
  vQueueDelete(out_queue);
  in_queue = NULL;
  out_queue = NULL;
  return true;
}
