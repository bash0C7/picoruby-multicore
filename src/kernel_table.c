/* Weak defaults so a build without generated kernels (an empty kernels/) still
 * links: the table has no entry and no kernel has a message hook. A build that
 * carries kernels (sendairk03's picoruby-kernel_registry) defines both
 * strongly. The host test build defines them in test/support instead. */
#include "../include/multicore.h"

#ifndef MULTICORE_TEST_TABLE
__attribute__((weak)) const multicore_kernel_t multicore_kernels[] = {
  { NULL, NULL, NULL, NULL }
};

__attribute__((weak)) const char *
multicore_kernel_error_message(const multicore_kernel_t *k)
{
  (void)k;
  return NULL;
}
#endif
