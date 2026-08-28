#pragma once
#include <stdint.h>


inline int64_t GetEncryptedExpirationTimestamp() {
    uint64_t obfuscated_val = 1798761600ULL ^ 0x5A5A5A5AULL; 
    uint64_t secret_key = 0x5A5A5A5AULL;
    return (int64_t)(obfuscated_val ^ secret_key);
}
