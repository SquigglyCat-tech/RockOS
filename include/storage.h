#ifndef ROCKOS_STORAGE_H
#define ROCKOS_STORAGE_H

#include "block.h"

#include <stddef.h>

/* The final sector is reserved for the self-test. It writes only an empty
 * sector or its own previous pattern; future filesystems must exclude it. */
#define STORAGE_TEST_RESERVED_SECTORS 1ULL

block_result_t storage_init(void);
block_result_t storage_initialization_result(void);
const block_device_t* storage_get_device(void);
size_t storage_get_device_count(void);
const block_device_t* storage_get_device_at(size_t index);
block_result_t storage_self_test(void);

#endif