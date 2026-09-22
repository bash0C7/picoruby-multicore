/* esp32 port: one FreeRTOS task pinned to core 1.
 *
 * The task runs kernels only; it never touches the VM, never allocates and
 * never calls back into Ruby. core 0 wakes it with a task notification after
 * it publishes a job in the shared slots (include/multicore_engine.h).
 *
 * The slots are allocated with malloc in MULTICORE_start (ESP-IDF's heap is safe from both
 * cores) and freed in MULTICORE_stop once the task is gone; the task itself never allocates.
 *
 * This file needs the ESP-IDF include paths, so it is not compiled into
 * libmruby: the firmware build definition adds it to the IDF component's SRCS.
 * MULTICORE_STACK_BYTES is the task's stack (default 8192). */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "../../include/multicore.h"

#define WORKER_CORE         1
#define POLL_TICKS          pdMS_TO_TICKS(50)
#define SHUTDOWN_WAIT_TICKS 300

#define TAG "multicore"

static TaskHandle_t worker = NULL;
static volatile bool stop_requested = false;
static volatile bool worker_done = false;

#define MC_WAKE() do { if (worker != NULL) { xTaskNotifyGive(worker); } } while (0)
#include "../../include/multicore_engine.h"

static void
worker_main(void *arg)
{
  (void)arg;
  while (!stop_requested) {
    ulTaskNotifyTake(pdTRUE, POLL_TICKS);
    while (!stop_requested && mc_worker_run_one()) {
    }
  }
  worker_done = true;
  vTaskDelete(NULL);
}

uint32_t
MULTICORE_now_ms(void)
{
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* The three heap figures the start logs, so a NoMemory on a board can be read against the headroom. */
#define HEAP_FMT "largest free block %u, free heap %u, minimum free heap ever %u"
#define HEAP_ARGS (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), \
                  (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size()

int
MULTICORE_start(void)
{
  if (mc_running) {
    return MULTICORE_OK;
  }
  if (worker != NULL) {
    /* A stop that timed out left the task behind, still inside a kernel. */
    return MULTICORE_CORE_BUSY;
  }
  ESP_LOGI(TAG, "start: " HEAP_FMT " (slots need %u bytes in %d blocks, the largest %u bytes; task stack %u bytes)", HEAP_ARGS,
           (unsigned)MC_SLOTS_BYTES, MC_BLOCKS,
           (unsigned)(MULTICORE_IN_CAP > MULTICORE_OUT_CAP ? MULTICORE_IN_CAP : MULTICORE_OUT_CAP),
           (unsigned)MULTICORE_STACK_BYTES);
  if (!mc_alloc_slots()) {
    ESP_LOGE(TAG, "job slots: malloc of %u bytes failed for the %s of slot %d (%d of %d blocks allocated before it): " HEAP_FMT,
             (unsigned)mc_alloc_failure.bytes, mc_alloc_failure.what, mc_alloc_failure.index,
             mc_alloc_failure.blocks_before, MC_BLOCKS, HEAP_ARGS);
    return MULTICORE_NO_MEMORY;
  }
  stop_requested = false;
  worker_done = false;
  if (xTaskCreatePinnedToCore(worker_main, "multicore1", MULTICORE_STACK_BYTES, NULL,
                              tskIDLE_PRIORITY + 1, &worker, WORKER_CORE) != pdPASS) {
    worker = NULL;
    mc_free_slots();
    ESP_LOGE(TAG, "task stack: creating the task (%u byte stack) failed: " HEAP_FMT,
             (unsigned)MULTICORE_STACK_BYTES, HEAP_ARGS);
    return MULTICORE_START_FAILED;
  }
  mc_running = true;
  return MULTICORE_OK;
}

bool
MULTICORE_stop(void)
{
  if (worker == NULL) {
    mc_running = false;
    return true;
  }
  mc_running = false;
  stop_requested = true;
  xTaskNotifyGive(worker);
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
  mc_free_slots();
  return true;
}
