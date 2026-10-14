#ifndef __SHM_COMMON_H__
#define __SHM_COMMON_H__
#include <string>
#include "regions.h"
#include "protocols.h"

// ---- Region sizing / addressing ----
size_t region_total_size(uint32_t n_sms, uint32_t sms_size);

inline uint8_t *segment_ptr(shared_region_t *region, uint32_t segment_id) {
    return region->seg_pool + (size_t)segment_id * region->sms_size;
}

tf_err validate_region_params(uint32_t n_sms, uint32_t sms_size);

// ---- Server info file ----
struct ServerInfo {
    int shm_id;
};

tf_err write_server_info(const std::string &path, const ServerInfo &info);
tf_err read_server_info(const std::string &path, ServerInfo &out);

#endif