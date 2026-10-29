/*
 * LANCommander Comms - common definitions.
 *
 * Plain C ABI. Include <comms/server.h> and/or <comms/client.h>.
 *
 * Kept C89-compatible (no // comments, no stdbool, declarations-first friendly) so it can be
 * consumed by mod SDKs of older games built with old toolchains. Available as x86 and x64.
 */
#ifndef COMMS_H
#define COMMS_H

#include <stddef.h>

#if defined(_MSC_VER) && _MSC_VER < 1600
   /* Pre-VS2010 toolchains (e.g. older game SDKs) lack <stdint.h>. */
   typedef signed   __int32 int32_t;
   typedef unsigned __int8  uint8_t;
   typedef unsigned __int16 uint16_t;
   typedef unsigned __int32 uint32_t;
   typedef unsigned __int64 uint64_t;
#else
#  include <stdint.h>
#endif

#if defined(_WIN32)
#  if defined(COMMS_STATIC)
#    define COMMS_API
#  elif defined(COMMS_BUILDING)
#    define COMMS_API __declspec(dllexport)
#  else
#    define COMMS_API __declspec(dllimport)
#  endif
   /* Explicit so 32-bit consumers built with /Gz or /Gr still call correctly. */
#  define COMMS_CALL __cdecl
#else
#  define COMMS_API __attribute__((visibility("default")))
#  define COMMS_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any ABI-breaking change. Compare with comms_get_api_version(). */
#define COMMS_API_VERSION 1

#define COMMS_DEFAULT_PORT           27800
#define COMMS_DEFAULT_DISCOVERY_PORT 27801

typedef enum comms_result {
    COMMS_OK                    =  0,
    COMMS_ERR_UNKNOWN           = -1,
    COMMS_ERR_INVALID_ARGUMENT  = -2,
    COMMS_ERR_OUT_OF_MEMORY     = -3,
    COMMS_ERR_NOT_IMPLEMENTED   = -4,
    COMMS_ERR_NOT_FOUND         = -5,
    COMMS_ERR_ALREADY_EXISTS    = -6,
    COMMS_ERR_LIMIT_REACHED     = -7,
    COMMS_ERR_PERMISSION_DENIED = -8,
    COMMS_ERR_INVALID_STATE     = -9,
    COMMS_ERR_NETWORK           = -10,
    COMMS_ERR_AUDIO_DEVICE      = -11,
    COMMS_ERR_BUFFER_TOO_SMALL  = -12,
    COMMS_ERR_VERSION_MISMATCH  = -13,
    COMMS_ERR_AUTH_FAILED       = -14,
    COMMS_ERR_TIMEOUT           = -15
} comms_result;

/* Ids are assigned by the voice server and are unique for its lifetime. 0 is never valid. */
typedef uint32_t comms_player_id;
typedef uint32_t comms_group_id;
typedef uint32_t comms_zone_id;

#define COMMS_INVALID_ID 0u

typedef struct comms_vec3 {
    float x, y, z;
} comms_vec3;

typedef enum comms_auth_mode {
    COMMS_AUTH_TOKEN    = 0, /* client presents a join token issued by the game server (games with netcode access) */
    COMMS_AUTH_CLAIM    = 1, /* client claims a registered game player by id/name, optionally + password (bridged/legacy games) */
    COMMS_AUTH_PASSWORD = 2, /* anyone with the server password (standalone voice apps) */
    COMMS_AUTH_OPEN     = 3  /* anyone */
} comms_auth_mode;

typedef enum comms_position_authority {
    COMMS_POSITION_SERVER = 0, /* only comms_server_update_player_position is used; client reports ignored (recommended) */
    COMMS_POSITION_CLIENT = 1  /* client reports accepted, subject to server sanity checks (legacy games with no server data) */
} comms_position_authority;

typedef enum comms_group_type {
    COMMS_GROUP_PROXIMITY = 0, /* implicit, one per zone */
    COMMS_GROUP_SERVER    = 1, /* server-managed membership (teams) */
    COMMS_GROUP_PRIVATE   = 2, /* player-created, invite/password */
    COMMS_GROUP_CHANNEL   = 3  /* persistent, free join/leave */
} comms_group_type;

typedef enum comms_target_kind {
    COMMS_TARGET_PROXIMITY = 0, /* id ignored; uses the speaker's current zone */
    COMMS_TARGET_GROUP     = 1, /* id = comms_group_id */
    COMMS_TARGET_WHISPER   = 2  /* id = comms_player_id */
} comms_target_kind;

typedef struct comms_talk_target {
    comms_target_kind kind;
    uint32_t          id;
} comms_talk_target;

/* Maximum targets in one target set (default targets or a PTT slot). */
#define COMMS_MAX_TALK_TARGETS 16

typedef enum comms_rolloff {
    COMMS_ROLLOFF_LINEAR      = 0,
    COMMS_ROLLOFF_INVERSE     = 1,
    COMMS_ROLLOFF_EXPONENTIAL = 2
} comms_rolloff;

typedef enum comms_log_level {
    COMMS_LOG_TRACE = 0,
    COMMS_LOG_DEBUG = 1,
    COMMS_LOG_INFO  = 2,
    COMMS_LOG_WARN  = 3,
    COMMS_LOG_ERROR = 4,
    COMMS_LOG_OFF   = 5
} comms_log_level;

/* May be invoked from any library thread; must be thread-safe and must not call back into the library. */
typedef void (COMMS_CALL *comms_log_fn)(comms_log_level level, const char* message, void* user_data);

COMMS_API uint32_t     COMMS_CALL comms_get_api_version(void);
COMMS_API const char*  COMMS_CALL comms_get_version_string(void);
COMMS_API const char*  COMMS_CALL comms_result_to_string(comms_result result);

/* Global, process-wide. Pass NULL to disable. */
COMMS_API void         COMMS_CALL comms_set_log_handler(comms_log_fn fn, comms_log_level min_level, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* COMMS_H */
