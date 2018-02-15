/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#pragma once

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @defgroup types Builtin Types
 *  @ingroup contractdev
 *  @brief Specifies typedefs and aliases
 *
 *  @{
 */

typedef uint64_t account_name;
typedef uint64_t permission_name;
typedef uint64_t token_name;
typedef uint64_t table_name;
typedef uint32_t time;
typedef uint64_t scope_name;
typedef uint64_t action_name;
typedef uint16_t region_id;

typedef uint64_t asset_symbol;
typedef int64_t share_type;

#define PACKED(X) __attribute((packed)) X

struct public_key {
   uint8_t data[33];
};

struct signature {
   uint8_t data[65];
};

struct checksum {
   uint64_t hash[4];
};

struct fixed_string16 {
   uint8_t len;
   char str[16];
};

typedef struct fixed_string16 field_name;

struct fixed_string32 {
   uint8_t len;
   char str[32];
};

typedef struct fixed_string32 type_name;

struct account_permission {
   account_name account;
   permission_name permission;
};

typedef union {
   uint8_t   bytes[32];
   uint16_t  uint16s[16];
   uint32_t  uint32s[8];
   uint64_t  uint64s[4];
   uint128_t uint128s[2];
} uint256;

#ifdef __cplusplus
} /// extern "C"
#endif
/// @}