/*
 * LANCommander Comms - client API.
 *
 * Typical game-client flow:
 *   comms_client_config cfg; comms_client_config_init(&cfg);
 *   comms_client_create(&cfg, &client);
 *   comms_connect_info info; comms_connect_info_init(&info);
 *   info.host = ...; info.token = token_from_game_server;
 *   comms_client_connect(client, &info);
 *   on key down/up:  comms_client_set_push_to_talk(client, 0, 1/0)
 *   every frame:     drain comms_client_poll_event(...), optionally comms_client_get_active_speakers(...)
 */
#ifndef COMMS_CLIENT_H
#define COMMS_CLIENT_H

#include "comms.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comms_client comms_client;

/* Number of independent push-to-talk bindings (e.g. 0 = proximity, 1 = team, 2 = squad). */
#define COMMS_MAX_PTT_SLOTS 8

typedef enum comms_capture_mode {
    COMMS_CAPTURE_DEVICE   = 0, /* library opens a microphone */
    COMMS_CAPTURE_EXTERNAL = 1  /* game pushes PCM via comms_client_push_capture */
} comms_capture_mode;

typedef enum comms_playback_mode {
    COMMS_PLAYBACK_DEVICE   = 0, /* library mixes, spatializes and plays to an output device */
    COMMS_PLAYBACK_EXTERNAL = 1  /* game pulls per-speaker PCM via comms_client_read_speaker_audio */
} comms_playback_mode;

/*
 * How the default talk targets (comms_client_set_talk_targets) are gated:
 *   PUSH_TO_TALK:   only while a PTT slot is held.
 *   VOICE_ACTIVITY: whenever voice is detected (see comms_voice_activation_settings).
 *   CONTINUOUS:     always (open mic).
 * In every mode, holding a PTT slot also transmits to that slot's targets, bypassing voice
 * detection - so "voice activation for proximity + push-to-talk for team" works out of the box.
 */
typedef enum comms_transmit_mode {
    COMMS_TRANSMIT_PUSH_TO_TALK   = 0,
    COMMS_TRANSMIT_VOICE_ACTIVITY = 1,
    COMMS_TRANSMIT_CONTINUOUS     = 2
} comms_transmit_mode;

typedef enum comms_connection_state {
    COMMS_STATE_DISCONNECTED = 0,
    COMMS_STATE_CONNECTING,
    COMMS_STATE_AUTHENTICATING,
    COMMS_STATE_CONNECTED
} comms_connection_state;

typedef struct comms_voice_activation_settings {
    uint32_t struct_size;
    float    threshold_db;   /* open the gate above this input level (dBFS), e.g. -45 */
    uint32_t attack_ms;      /* level must stay above threshold this long before opening */
    uint32_t hold_ms;        /* keep transmitting this long after dropping below threshold */
    int      auto_threshold; /* effective threshold = max(threshold_db, noise floor + 12 dB) */
} comms_voice_activation_settings;

typedef struct comms_client_config {
    uint32_t                        struct_size;
    comms_capture_mode              capture_mode;
    comms_playback_mode             playback_mode;
    comms_transmit_mode             transmit_mode;
    comms_voice_activation_settings voice_activation;
    uint32_t                        ptt_release_delay_ms; /* keep sending briefly after PTT release so last words aren't clipped */
    const char*                     capture_device_id;    /* NULL = system default */
    const char*                     playback_device_id;   /* NULL = system default */
    uint32_t                        jitter_target_ms;
} comms_client_config;

typedef struct comms_connect_info {
    uint32_t    struct_size;
    const char* host;
    uint16_t    port;           /* 0 = COMMS_DEFAULT_PORT */
    const char* token;          /* COMMS_AUTH_TOKEN */
    const char* password;       /* COMMS_AUTH_PASSWORD, optionally COMMS_AUTH_CLAIM */
    uint64_t    game_player_id; /* COMMS_AUTH_CLAIM; 0 = claim by player_name */
    const char* player_name;    /* COMMS_AUTH_CLAIM fallback; display name for PASSWORD/OPEN */
    const char* admin_key;      /* optional: grants remote admin (comms_client_send_admin_command) */
    int         admin_only;     /* with admin_key: connect as an admin console only - no player, no voice */
} comms_connect_info;

typedef enum comms_device_type {
    COMMS_DEVICE_CAPTURE  = 0,
    COMMS_DEVICE_PLAYBACK = 1
} comms_device_type;

typedef struct comms_audio_device {
    char id[256];
    char name[256];
    int  is_default;
} comms_audio_device;

typedef struct comms_client_player_info {
    comms_player_id id;
    uint64_t        game_player_id;
    char            name[64];
    int             speaking;     /* currently heard by the local player */
    int             locally_muted;
    float           local_volume; /* 0..2, 1 = unchanged */
    int             server_muted;
} comms_client_player_info;

/* A player the local client is currently hearing. */
typedef struct comms_speaker_info {
    comms_player_id   player;
    comms_target_kind kind;          /* how they reached us: proximity, group or whisper */
    comms_group_id    group;         /* kind == COMMS_TARGET_GROUP */
    float             level_db;      /* smoothed speech level, for animated talk indicators */
    float             distance;      /* kind == COMMS_TARGET_PROXIMITY; -1 if unknown */
    float             pan;           /* -1 = left .. +1 = right, as applied by device playback; 0 for non-proximity */
    int               locally_muted; /* still reported so UIs can show "muted but talking" */
} comms_speaker_info;

/* The local player's own transmit state, for "you are talking" indicators and settings UIs. */
typedef struct comms_transmit_state {
    int      transmitting;   /* audio is currently being sent */
    int      voice_detected; /* VAD gate open (independent of mode) */
    uint32_t ptt_slots_held; /* bitmask of held PTT slots */
    float    input_level_db; /* current mic level after gain, dBFS */
    float    noise_floor_db; /* estimated, used by auto_threshold */
    int      self_muted;
    int      self_deafened;
} comms_transmit_state;

typedef struct comms_client_group_info {
    comms_group_id   id;
    comms_group_type type;
    char             name[64];
    comms_player_id  owner;        /* PRIVATE groups; COMMS_INVALID_ID otherwise */
    comms_group_id   parent;       /* CHANNEL hierarchy; COMMS_INVALID_ID = root */
    uint32_t         member_count; /* connected members */
    uint32_t         max_members;  /* 0 = unlimited */
    int              is_member;
    int              listening;    /* local player is a member and hasn't paused it via comms_client_set_group_listening */
    int              can_speak;    /* local player's permission (set by the server) */
    int              has_password;
    int              can_leave;
} comms_client_group_info;

typedef struct comms_discovered_server {
    char            name[64];
    char            host[64];
    uint16_t        port;
    uint32_t        player_count;
    uint32_t        max_players;
    comms_auth_mode auth_mode;
    uint32_t        ping_ms;
} comms_discovered_server;

typedef enum comms_client_event_type {
    COMMS_CLIENT_EVENT_NONE = 0,
    COMMS_CLIENT_EVENT_STATE_CHANGED,             /* state, reason */
    COMMS_CLIENT_EVENT_PLAYER_JOINED,             /* player */
    COMMS_CLIENT_EVENT_PLAYER_LEFT,               /* player */
    COMMS_CLIENT_EVENT_PLAYER_STARTED_SPEAKING,   /* player, target_kind, group */
    COMMS_CLIENT_EVENT_PLAYER_STOPPED_SPEAKING,   /* player */
    COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STARTED,    /* local player started sending audio */
    COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STOPPED,
    COMMS_CLIENT_EVENT_GROUP_ADDED,               /* group */
    COMMS_CLIENT_EVENT_GROUP_REMOVED,             /* group */
    COMMS_CLIENT_EVENT_GROUP_MEMBER_JOINED,       /* group, player */
    COMMS_CLIENT_EVENT_GROUP_MEMBER_LEFT,         /* group, player */
    COMMS_CLIENT_EVENT_GROUP_INVITE,              /* group, player = inviter */
    COMMS_CLIENT_EVENT_REQUEST_FAILED,            /* reason = comms_result of an async request, text = details */
    COMMS_CLIENT_EVENT_KICKED,                    /* text = reason */
    COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST,
    COMMS_CLIENT_EVENT_DISCOVERY_RESULT,          /* call comms_client_get_discovered_servers */
    COMMS_CLIENT_EVENT_ADMIN_RESPONSE,            /* request_id, reason = result; full output via comms_client_get_admin_response */
    COMMS_CLIENT_EVENT_CONFIG_CHANGED             /* server changed codec settings (bitrate/frame size) */
} comms_client_event_type;

typedef struct comms_client_event {
    comms_client_event_type type;
    comms_connection_state  state;
    comms_player_id         player;
    comms_group_id          group;
    comms_target_kind       target_kind;
    int32_t                 reason;
    uint32_t                request_id; /* ADMIN_RESPONSE */
    char                    text[256];  /* may be truncated; see comms_client_get_admin_response for admin output */
} comms_client_event;

/* ---- Lifecycle ---------------------------------------------------------- */

COMMS_API void         COMMS_CALL comms_client_config_init(comms_client_config* config);
COMMS_API comms_result COMMS_CALL comms_client_create(const comms_client_config* config, comms_client** out_client);
COMMS_API void         COMMS_CALL comms_client_destroy(comms_client* client);

COMMS_API void         COMMS_CALL comms_connect_info_init(comms_connect_info* info);
/* Asynchronous; watch for STATE_CHANGED events. */
COMMS_API comms_result COMMS_CALL comms_client_connect(comms_client* client, const comms_connect_info* info);
COMMS_API comms_result COMMS_CALL comms_client_disconnect(comms_client* client);
COMMS_API comms_connection_state COMMS_CALL comms_client_get_state(comms_client* client);
COMMS_API comms_player_id COMMS_CALL comms_client_get_local_player(comms_client* client);

/* Returns COMMS_OK and fills *out_event if one was pending, COMMS_ERR_NOT_FOUND if the queue is empty. */
COMMS_API comms_result COMMS_CALL comms_client_poll_event(comms_client* client, comms_client_event* out_event);

/* ---- Discovery ---------------------------------------------------------- */

/* Broadcasts a LAN probe; results arrive as DISCOVERY_RESULT events. port 0 = default. */
COMMS_API comms_result COMMS_CALL comms_client_discover(comms_client* client, uint16_t discovery_port, uint32_t timeout_ms);
COMMS_API comms_result COMMS_CALL comms_client_get_discovered_servers(comms_client* client, comms_discovered_server* out_servers,
                                                                      uint32_t capacity, uint32_t* out_count);

/* ---- Audio devices ------------------------------------------------------ */

COMMS_API comms_result COMMS_CALL comms_list_audio_devices(comms_device_type type, comms_audio_device* out_devices,
                                                           uint32_t capacity, uint32_t* out_count);
COMMS_API comms_result COMMS_CALL comms_client_set_capture_device(comms_client* client, const char* device_id);
COMMS_API comms_result COMMS_CALL comms_client_set_playback_device(comms_client* client, const char* device_id);

/* ---- Transmit: mode, push-to-talk, voice activation --------------------- */

COMMS_API comms_result COMMS_CALL comms_client_set_transmit_mode(comms_client* client, comms_transmit_mode mode);
COMMS_API comms_transmit_mode COMMS_CALL comms_client_get_transmit_mode(comms_client* client);

/* Default targets for VOICE_ACTIVITY/CONTINUOUS and for PTT slots without their own targets. */
COMMS_API comms_result COMMS_CALL comms_client_set_talk_targets(comms_client* client, const comms_talk_target* targets,
                                                                uint32_t count);
/* Bind a PTT slot to its own targets (count 0 = use default talk targets). */
COMMS_API comms_result COMMS_CALL comms_client_set_ptt_targets(comms_client* client, uint32_t slot,
                                                               const comms_talk_target* targets, uint32_t count);
/* Call on key down (pressed = 1) and key up (pressed = 0). Holding several slots sends to the union. */
COMMS_API comms_result COMMS_CALL comms_client_set_push_to_talk(comms_client* client, uint32_t slot, int pressed);
COMMS_API comms_result COMMS_CALL comms_client_set_ptt_release_delay(comms_client* client, uint32_t delay_ms);

COMMS_API void         COMMS_CALL comms_voice_activation_settings_init(comms_voice_activation_settings* settings);
COMMS_API comms_result COMMS_CALL comms_client_set_voice_activation(comms_client* client,
                                                                    const comms_voice_activation_settings* settings);
COMMS_API comms_result COMMS_CALL comms_client_get_voice_activation(comms_client* client,
                                                                    comms_voice_activation_settings* out_settings);

COMMS_API comms_result COMMS_CALL comms_client_get_transmit_state(comms_client* client, comms_transmit_state* out_state);

COMMS_API comms_result COMMS_CALL comms_client_set_self_muted(comms_client* client, int muted);
COMMS_API comms_result COMMS_CALL comms_client_set_self_deafened(comms_client* client, int deafened);
COMMS_API comms_result COMMS_CALL comms_client_set_input_gain(comms_client* client, float gain);
COMMS_API comms_result COMMS_CALL comms_client_set_master_volume(comms_client* client, float volume);

/* ---- Other players & talk indicators ------------------------------------ */

COMMS_API comms_result COMMS_CALL comms_client_set_player_muted(comms_client* client, comms_player_id player, int muted);
COMMS_API comms_result COMMS_CALL comms_client_set_player_volume(comms_client* client, comms_player_id player, float volume);
COMMS_API comms_result COMMS_CALL comms_client_get_player_info(comms_client* client, comms_player_id player,
                                                               comms_client_player_info* out_info);
COMMS_API comms_result COMMS_CALL comms_client_list_players(comms_client* client, comms_player_id* out_ids,
                                                            uint32_t capacity, uint32_t* out_count);
/* Maps a game's own player id to the voice id (e.g. to draw a speaker icon over a player model). */
COMMS_API comms_result COMMS_CALL comms_client_find_player(comms_client* client, uint64_t game_player_id,
                                                           comms_player_id* out_player);

/* Snapshot of everyone currently being heard. Cheap; safe to call every frame. */
COMMS_API comms_result COMMS_CALL comms_client_get_active_speakers(comms_client* client, comms_speaker_info* out_speakers,
                                                                   uint32_t capacity, uint32_t* out_count);
COMMS_API int          COMMS_CALL comms_client_is_player_speaking(comms_client* client, comms_player_id player);

/* ---- Groups ------------------------------------------------------------- */

/*
 * Visible groups: every CHANNEL, SERVER groups the local player belongs to (other teams stay hidden),
 * and PRIVATE groups the local player belongs to or has been invited to. Member lists only contain
 * connected players. GROUP_ADDED/REMOVED fire as groups become visible/hidden.
 */
COMMS_API comms_result COMMS_CALL comms_client_list_groups(comms_client* client, comms_group_id* out_ids,
                                                           uint32_t capacity, uint32_t* out_count);
COMMS_API comms_result COMMS_CALL comms_client_get_group_info(comms_client* client, comms_group_id group,
                                                              comms_client_group_info* out_info);
COMMS_API comms_result COMMS_CALL comms_client_list_group_members(comms_client* client, comms_group_id group,
                                                                  comms_player_id* out_ids, uint32_t capacity,
                                                                  uint32_t* out_count);
/*
 * Requests are asynchronous: success shows up as GROUP_* events, failure as REQUEST_FAILED with
 * reason = comms_result and group = the requested group (COMMS_INVALID_ID for create).
 */

/* Name is 1-63 bytes. The creator becomes owner; the new group arrives as GROUP_ADDED with owner = local player. */
COMMS_API comms_result COMMS_CALL comms_client_create_private_group(comms_client* client, const char* name, const char* password);
/* CHANNEL: free to join (password if set). PRIVATE: requires an invite or the password. SERVER: never. */
COMMS_API comms_result COMMS_CALL comms_client_join_group(comms_client* client, comms_group_id group, const char* password);
/* SERVER groups only if the server allows it. Ownership of a PRIVATE group passes on; empty PRIVATE groups are destroyed. */
COMMS_API comms_result COMMS_CALL comms_client_leave_group(comms_client* client, comms_group_id group);
/* Members may invite to PRIVATE and CHANNEL groups; an invite bypasses the password. */
COMMS_API comms_result COMMS_CALL comms_client_invite_to_group(comms_client* client, comms_group_id group, comms_player_id player);
/* Stop hearing a group without leaving it (the server stops forwarding it, saving bandwidth). */
COMMS_API comms_result COMMS_CALL comms_client_set_group_listening(comms_client* client, comms_group_id group, int listening);

/* ---- Positions ---------------------------------------------------------- */

/*
 * Report the local player's position to the server. Only used for routing if the server grants
 * this player COMMS_POSITION_CLIENT authority (legacy games where the server has no position data);
 * otherwise ignored. Server-reported positions are recommended to prevent abuse.
 */
COMMS_API comms_result COMMS_CALL comms_client_report_position(comms_client* client, comms_zone_id zone, comms_vec3 position,
                                                               const comms_vec3* forward);

/*
 * Local spatialization (never affects routing or who can hear whom).
 *
 * By default stereo pan comes from the server, which only updates as often as the game reports positions.
 * A game client that already knows player positions can instead supply its own listener pose and speaker
 * positions every frame: proximity speakers with an override are then panned locally, so head turns are
 * reflected immediately. Volume attenuation still comes from the server (authoritative falloff/occlusion);
 * comms_speaker_info.distance reports the local distance.
 *
 * `right` is the listener's right vector in world space (e.g. Unity transform.right, Unreal GetRightVector);
 * passing it instead of an up vector avoids left/right-handed ambiguity. Vectors need not be normalized.
 */
COMMS_API comms_result COMMS_CALL comms_client_set_listener(comms_client* client, comms_vec3 position, comms_vec3 forward,
                                                            comms_vec3 right);
COMMS_API comms_result COMMS_CALL comms_client_clear_listener(comms_client* client);
COMMS_API comms_result COMMS_CALL comms_client_set_speaker_position_override(comms_client* client, comms_player_id player,
                                                                             const comms_vec3* position /* NULL = clear */);

/* ---- Remote admin ------------------------------------------------------- */

/*
 * Runs a server admin command (same language as comms_server_execute_command, e.g. "status",
 * "kick 12 spamming"). Requires connecting with a valid admin_key. Asynchronous: the result arrives as
 * an ADMIN_RESPONSE event carrying *out_request_id (may be NULL).
 */
COMMS_API comms_result COMMS_CALL comms_client_send_admin_command(comms_client* client, const char* command,
                                                                  uint32_t* out_request_id);
/* Full output of a recent admin response (the last 16 are kept). Fails with BUFFER_TOO_SMALL but still
 * writes a truncated, NUL-terminated copy if text_capacity is too small; *out_length (may be NULL)
 * receives the full length excluding the terminator. */
COMMS_API comms_result COMMS_CALL comms_client_get_admin_response(comms_client* client, uint32_t request_id,
                                                                  char* out_text, size_t text_capacity,
                                                                  size_t* out_length);
COMMS_API int          COMMS_CALL comms_client_is_admin(comms_client* client);

/* ---- External audio ----------------------------------------------------- */

/* COMMS_CAPTURE_EXTERNAL: mono float PCM at sample_rate (resampled internally to 48 kHz). */
COMMS_API comms_result COMMS_CALL comms_client_push_capture(comms_client* client, const float* samples,
                                                            uint32_t frame_count, uint32_t sample_rate);
/*
 * COMMS_PLAYBACK_EXTERNAL: pulls decoded mono 48 kHz float PCM for one speaker, already scaled by
 * per-player volume/mute (but NOT spatialized - feed it to an engine 3D source). Missing data is
 * filled with silence (PLC). *out_frames receives frames written.
 */
COMMS_API comms_result COMMS_CALL comms_client_read_speaker_audio(comms_client* client, comms_player_id player,
                                                                  float* out_samples, uint32_t frame_capacity,
                                                                  uint32_t* out_frames);

#ifdef __cplusplus
}
#endif

#endif /* COMMS_CLIENT_H */
