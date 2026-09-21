#ifndef PICORUBY_MULTICORE_H_
#define PICORUBY_MULTICORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The contract between the VM glue (src/mruby, src/mrubyc) and a port
 * (ports/<board>/multicore.c). The VM stays on its own core and only submits
 * byte strings; a worker on the other core looks a kernel up by name in the
 * table below and calls it. The worker never touches the VM.
 *
 * Build-time constants (override with -D on the compiler command line of the
 * port). Each job slot holds one input and one output buffer plus 28 bytes of
 * bookkeeping. The slots (and on rp2040 core 1's stack) are allocated with
 * malloc by MULTICORE_start on the VM's core and freed by MULTICORE_stop once
 * the worker has really stopped; nothing is reserved while no worker runs. */
#ifndef MULTICORE_IN_CAP
#define MULTICORE_IN_CAP 4096
#endif
#ifndef MULTICORE_OUT_CAP
#define MULTICORE_OUT_CAP 4096
#endif
#ifndef MULTICORE_QUEUE_DEPTH
#define MULTICORE_QUEUE_DEPTH 4
#endif
#ifndef MULTICORE_STACK_BYTES
#define MULTICORE_STACK_BYTES 8192
#endif

/* The kernel table. The firmware build must define `multicore_kernels`:
 * sendairk03's scripts/kernels_build.rb generates it (picoruby-kernel_registry),
 * empty when there are no kernels. The last entry is all NULL. There is no
 * default, so a build without the table fails to link. */
typedef int32_t (*multicore_kernel_fn)(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap);
typedef struct {
  const char *name;                    /* method name ("scale_sum") */
  multicore_kernel_fn call;            /* <lib>_<method>_call */
  const char *(*signature)(void);      /* <lib>_<method>_signature */
  void (*init)(void);                  /* <lib>_init: once before the first call (idempotent) */
  const char *(*error_message)(void);  /* <lib>_error_message: the message of the exception a -3 call raised */
} multicore_kernel_t;
extern const multicore_kernel_t multicore_kernels[];

/* Status a kernel's call returns (>= 0 is the number of bytes written). */
#define MULTICORE_K_MALFORMED (-1)
#define MULTICORE_K_NOSPACE   (-2)
#define MULTICORE_K_RAISED    (-3)
#define MULTICORE_K_RANGE     (-4)

/* MULTICORE_start() status. The Ruby layer turns a non-zero status into an
 * exception, so no port raises by itself. */
#define MULTICORE_OK           0
#define MULTICORE_CORE_BUSY    2
#define MULTICORE_START_FAILED 3
#define MULTICORE_NO_MEMORY    4  /* malloc returned NULL; nothing is left allocated */

/* MULTICORE_submit() failures (a job id is >= 0). */
#define MULTICORE_E_NOT_RUNNING   (-1)
#define MULTICORE_E_UNKNOWN       (-2)
#define MULTICORE_E_INPUT_TOO_BIG (-3)
#define MULTICORE_E_QUEUE_FULL    (-4)

/* MULTICORE_poll() */
#define MULTICORE_JOB_PENDING 0
#define MULTICORE_JOB_DONE    1
#define MULTICORE_JOB_UNKNOWN (-1)

/* Take the core, allocate the slots and start the worker. Idempotent while running. */
int MULTICORE_start(void);

/* Stop the worker, release the core and free the slots; every job is dropped. False when the
 * worker is still inside a kernel after the bounded wait: everything stays allocated,
 * call again later. */
bool MULTICORE_stop(void);

bool MULTICORE_running(void);

/* Queue one call of kernel `name` with the MessagePack bytes `in`. Returns a
 * job id, or one of MULTICORE_E_*. Never blocks. */
int32_t MULTICORE_submit(const char *name, const uint8_t *in, int32_t in_len);

/* Never blocks. */
int MULTICORE_poll(int32_t job);

/* Result of a finished job: `*data` / `*len` are the output bytes (for status
 * MULTICORE_K_RAISED, the exception's message), `*status` the kernel's status.
 * `data` stays valid until MULTICORE_release(). 0 when done, -1 otherwise. */
int MULTICORE_result(int32_t job, const uint8_t **data, int32_t *len, int32_t *status);

/* Free a finished job's slot. */
void MULTICORE_release(int32_t job);

/* Give up on a job whose result is not wanted: a finished job is freed at once,
 * a pending one is freed when it ends. */
void MULTICORE_forget(int32_t job);

/* Signature of kernel `name` (NULL when the table has no such kernel), and the
 * kernel names joined with ", " (truncated to fit `cap`, always NUL terminated). */
const char *MULTICORE_signature(const char *name);
void MULTICORE_names(char *buf, int cap);

int MULTICORE_in_cap(void);
int MULTICORE_out_cap(void);
int MULTICORE_queue_depth(void);

/* Milliseconds from a monotonic clock; wraps at 2^32. */
uint32_t MULTICORE_now_ms(void);

#endif /* PICORUBY_MULTICORE_H_ */
