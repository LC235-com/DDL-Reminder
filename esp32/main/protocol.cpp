/**
 * @file protocol.cpp
 * @brief DDL protocol — JSON message parsing and building
 */

#include "protocol.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "Protocol";

// ── DDLEvent ─────────────────────────────────────────────────

DDLEvent DDLEvent::from_json(cJSON* obj) {
    DDLEvent e;
    if (!obj) return e;

    if (cJSON* v = cJSON_GetObjectItem(obj, "id")) {
        e.id = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "title")) {
        e.title = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "course")) {
        e.course = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "type")) {
        e.type = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "source")) {
        e.source = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "deadline")) {
        e.deadline = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "advance_minutes")) {
        e.advance_minutes = v->valueint;
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "url")) {
        e.url = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "rate")) {
        e.rate = v->valueint;
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "status")) {
        e.status = v->valuestring ? v->valuestring : "pending";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "tag")) {
        e.tag = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(obj, "minutes_remaining")) {
        e.minutes_remaining = v->valueint;
    }
    return e;
}

cJSON* DDLEvent::to_json() const {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id.c_str());
    cJSON_AddStringToObject(obj, "title", title.c_str());
    cJSON_AddStringToObject(obj, "course", course.c_str());
    cJSON_AddStringToObject(obj, "type", type.c_str());
    cJSON_AddStringToObject(obj, "source", source.c_str());
    cJSON_AddStringToObject(obj, "deadline", deadline.c_str());
    cJSON_AddNumberToObject(obj, "advance_minutes", advance_minutes);
    cJSON_AddStringToObject(obj, "url", url.c_str());
    cJSON_AddNumberToObject(obj, "rate", rate);
    cJSON_AddStringToObject(obj, "status", status.c_str());
    cJSON_AddStringToObject(obj, "tag", tag.c_str());
    cJSON_AddNumberToObject(obj, "minutes_remaining", minutes_remaining);
    return obj;
}

// ── ProtocolParser ────────────────────────────────────────────

ProtocolParser::MessageType ProtocolParser::get_message_type(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return MessageType::UNKNOWN;

    cJSON* cmd = cJSON_GetObjectItem(root, "cmd");
    MessageType type = MessageType::UNKNOWN;

    if (cmd && cmd->valuestring) {
        std::string cmd_str = cmd->valuestring;
        if (cmd_str == "sync") type = MessageType::SYNC;
        else if (cmd_str == "new_event") type = MessageType::NEW_EVENT;
        else if (cmd_str == "delete_event") type = MessageType::DELETE_EVENT;
        else if (cmd_str == "remind") type = MessageType::REMIND;
        else if (cmd_str == "speak") type = MessageType::SPEAK;
        else if (cmd_str == "emotion") type = MessageType::EMOTION;
        else if (cmd_str == "led") type = MessageType::LED;
        else if (cmd_str == "config") type = MessageType::CONFIG;
        else if (cmd_str == "pong") type = MessageType::PONG;
    }

    cJSON_Delete(root);
    return type;
}

ProtocolParser::SyncData ProtocolParser::parse_sync(const std::string& json_str) {
    SyncData data;
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return data;

    cJSON* events = cJSON_GetObjectItem(root, "events");
    if (events && cJSON_IsArray(events)) {
        int count = cJSON_GetArraySize(events);
        for (int i = 0; i < count; i++) {
            cJSON* item = cJSON_GetArrayItem(events, i);
            data.events.push_back(DDLEvent::from_json(item));
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed sync: %d events", (int)data.events.size());
    return data;
}

DDLEvent ProtocolParser::parse_new_event(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return DDLEvent();

    cJSON* event = cJSON_GetObjectItem(root, "event");
    DDLEvent e = DDLEvent::from_json(event);
    cJSON_Delete(root);
    return e;
}

std::string ProtocolParser::parse_delete_event(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return "";

    cJSON* id = cJSON_GetObjectItem(root, "id");
    std::string result = id && id->valuestring ? id->valuestring : "";
    cJSON_Delete(root);
    return result;
}

ProtocolParser::RemindData ProtocolParser::parse_remind(const std::string& json_str) {
    RemindData data;
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return data;

    cJSON* event = cJSON_GetObjectItem(root, "event");
    data.event = DDLEvent::from_json(event);
    cJSON_Delete(root);
    return data;
}

ProtocolParser::SpeakData ProtocolParser::parse_speak(const std::string& json_str) {
    SpeakData data;
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return data;

    if (cJSON* v = cJSON_GetObjectItem(root, "audio")) {
        data.audio_b64 = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(root, "text")) {
        data.text = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(root, "emotion")) {
        data.emotion = v->valuestring ? v->valuestring : "neutral";
    }

    cJSON_Delete(root);
    return data;
}

std::string ProtocolParser::parse_emotion(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return "neutral";

    cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
    std::string result = emotion && emotion->valuestring ? emotion->valuestring : "neutral";
    cJSON_Delete(root);
    return result;
}

void ProtocolParser::parse_led(const std::string& json_str, std::string& action, std::string& color) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) return;

    if (cJSON* v = cJSON_GetObjectItem(root, "action")) {
        action = v->valuestring ? v->valuestring : "";
    }
    if (cJSON* v = cJSON_GetObjectItem(root, "color")) {
        color = v->valuestring ? v->valuestring : "#FF0000";
    }
    cJSON_Delete(root);
}

// ── ProtocolBuilder ───────────────────────────────────────────

std::string ProtocolBuilder::build_audio_start() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "audio_start");
    cJSON_AddStringToObject(root, "format", "pcm16");
    cJSON_AddNumberToObject(root, "sample_rate", 16000);
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_audio_end() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "audio_end");
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_query(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "query");
    cJSON_AddStringToObject(root, "text", text.c_str());
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_event_action(const std::string& id, const std::string& action) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "event_action");
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "action", action.c_str());
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_request_sync() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "request_sync");
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_ping() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "ping");
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

std::string ProtocolBuilder::build_hello() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "hello");
    cJSON_AddStringToObject(root, "device", "esp32s3-ddl-reminder");
    cJSON_AddStringToObject(root, "version", "1.0.0");
    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
