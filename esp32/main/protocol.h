/**
 * @file protocol.h
 * @brief DDL WebSocket protocol — message parsing and serialization
 */

#ifndef DDL_PROTOCOL_H
#define DDL_PROTOCOL_H

#include <string>
#include <vector>
#include "cJSON.h"

/**
 * @brief DDL event data structure (mirrors server-side DDLItem)
 */
struct DDLEvent {
    std::string id;
    std::string title;
    std::string course;
    std::string type;       // "作业", "考试", "实验", "自定义"
    std::string source;     // "zju", "pta", "manual"
    std::string deadline;   // ISO formatted deadline
    int advance_minutes;
    std::string url;
    int rate;
    std::string status;     // "pending", "done", "snoozed", "dismissed"
    std::string tag;        // urgency emoji
    int minutes_remaining;

    static DDLEvent from_json(cJSON* obj);
    cJSON* to_json() const;
};

/**
 * @brief Parse incoming JSON messages from server
 */
class ProtocolParser {
public:
    enum class MessageType {
        UNKNOWN,
        SYNC,           // Full data sync
        NEW_EVENT,      // New event added
        DELETE_EVENT,   // Event removed
        REMIND,         // Immediate reminder
        SPEAK,          // TTS audio + text + emotion
        EMOTION,        // Avatar expression control
        LED,            // LED control
        CONFIG,         // Config update
        PONG,           // Heartbeat response
    };

    struct SyncData {
        std::vector<DDLEvent> events;
    };

    struct SpeakData {
        std::string audio_b64;
        std::string text;
        std::string emotion;
    };

    struct RemindData {
        DDLEvent event;
    };

    static MessageType get_message_type(const std::string& json_str);
    static SyncData parse_sync(const std::string& json_str);
    static DDLEvent parse_new_event(const std::string& json_str);
    static std::string parse_delete_event(const std::string& json_str);
    static RemindData parse_remind(const std::string& json_str);
    static SpeakData parse_speak(const std::string& json_str);
    static std::string parse_emotion(const std::string& json_str);
    static void parse_led(const std::string& json_str, std::string& action, std::string& color);
};

/**
 * @brief Build outgoing JSON messages to server
 */
class ProtocolBuilder {
public:
    static std::string build_audio_start();
    static std::string build_audio_end();
    static std::string build_query(const std::string& text);
    static std::string build_event_action(const std::string& id, const std::string& action);
    static std::string build_request_sync();
    static std::string build_ping();
    static std::string build_hello();
};

#endif // DDL_PROTOCOL_H
