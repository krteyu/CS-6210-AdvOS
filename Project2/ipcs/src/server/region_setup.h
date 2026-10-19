#ifndef __REGION_SETUP_H__
#define __REGION_SETUP_H__
#include "regions.h"
#include "protocols.h"

// Creates the shm region + semaphores, zero-initializes everything,
// writes the info file. Exits the process on any failure.
tf_err create_and_init_region(uint32_t n_sms, uint32_t sms_size, int *out_shm_id, shared_region_t **out_region);

void destroy_region(shared_region_t *region, int shm_id);

#endif