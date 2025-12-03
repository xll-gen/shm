#pragma once
#include <cstdint>

namespace shm {

#pragma pack(push, 1)
struct TransportHeader {
    uint64_t req_id;
};
#pragma pack(pop)

}
