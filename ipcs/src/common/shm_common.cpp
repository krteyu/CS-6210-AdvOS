#include "shm_common.h"
#include "include.h"

size_t region_total_size(uint32_t n_sms, uint32_t sms_size) {
    return sizeof(shared_region_t) + (size_t)n_sms * sms_size;
}

tf_err validate_region_params(uint32_t n_sms, uint32_t sms_size) {
    if (n_sms == 0 || n_sms > MAX_SEGMENTS) return tf_err::E_INVALID_ARG;
    if (sms_size == 0) return tf_err::E_INVALID_ARG;
    return tf_err::SUCCESS;
}

tf_err write_server_info(const std::string &path, const ServerInfo &info) {
    std::ofstream out(path);
    if (!out) return tf_err::E_FILE_NOT_FOUND;
    out << info.shm_id << "\n";
    out.close();
    return tf_err::SUCCESS;
}

tf_err read_server_info(const std::string &path, ServerInfo &out) {
    std::ifstream in(path);
    if (!in) return tf_err::E_FILE_NOT_FOUND;
    in >> out.shm_id;
    in.close();
    return tf_err::SUCCESS;
}