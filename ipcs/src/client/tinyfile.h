#ifndef __TINY_FILE__
#define __TINY_FILE__
#include "protocols.h"
#include <cstddef>
// ---------Structures---------
struct TF_result {
    uint64_t req_id;
    uint8_t *data;
    size_t   len;
};

// ---------External APIs---------
tf_err TF_init();
tf_err TF_compress_sync(const uint8_t *data, size_t len, uint8_t **out_compressed, size_t *out_len);
tf_err TF_compress_async(const uint8_t *data, size_t len, uint64_t *out_req_id);
tf_err TF_check_async(TF_result **out_results, size_t *out_count);
tf_err TF_wait_async(TF_result **out_results, size_t *out_count);
tf_err TF_cleanup();
void TF_free(uint8_t *data);
void TF_free_results(TF_result *results, size_t count);

#endif