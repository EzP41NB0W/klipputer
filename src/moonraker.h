// moonraker.h — Moonraker JSON-RPC client for Klipputer.
//
// Two channels, deliberately:
//  - WebSocket (ws://host:7125/websocket) for everything live:
//    printer.objects.subscribe with a tight per-field filter so Moonraker
//    pushes small notify_status_update diffs. This unit has NO working
//    PSRAM, so every parse happens in internal heap — keep messages lean.
//  - Plain HTTP + ArduinoJson stream filters (useHTTP10) for the bulky
//    one-shots: gcode file list and file metadata. A big gcode folder
//    never has to fit inside a WS frame buffer this way.
//
// Wire shapes per Moonraker's official API docs (moonraker.readthedocs.io):
//  - subscribe result:        {"status": {...}, "eventtime": ...}
//  - notify_status_update:    params [diff-object, eventtime]
//  - notify_gcode_response:   params [string]
//  - notify_klippy_ready / notify_klippy_shutdown / notify_klippy_disconnected
// notify_proc_stat_update arrives every second (~700B of host process
// stats we don't use) — dropped via strstr before any JSON parse.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include "printer_state.h"

struct FileEntry {
    String path;
    double modified = 0;
    uint32_t sizeB = 0;
};

struct FileMeta {
    bool valid = false;
    float estTime = -1;      // seconds
    float filamentMM = -1;
    float height = -1;       // object_height, mm
    float bedTemp = -1, extTemp = -1;
    String slicer;
};

class Moonraker {
public:
    PrinterState st;
    bool wsUp = false;

    std::vector<String> macros;
    bool macrosLoaded = false;
    std::vector<FileEntry> files;
    bool filesLoaded = false;

    // console ring — colors: 0 dim (printer output), 1 red (errors),
    // 2 cyan (things we sent), 3 green (connection/info)
    static const int LOGN = 30;
    String logLines[LOGN];
    uint8_t logColors[LOGN] = {0};
    int logHead = 0, logCount = 0;

    // one-shot events for the UI to consume (beeps)
    bool evComplete = false, evError = false;

    // tracked-gcode feedback (macros): Moonraker answers a gcode.script
    // RPC only AFTER the script finishes executing, so busy -> ok/fail is
    // real "did it actually run" feedback, not just "was it sent"
    bool gcodeBusy = false;
    bool evGcodeOk = false, evGcodeFail = false;
    String evGcodeLabel;

    void begin(const char *host, uint16_t port) {
        host_ = host;
        port_ = port;
        ws_.begin(host_, port_, "/websocket");
        ws_.onEvent([this](WStype_t t, uint8_t *p, size_t l) { onEvent(t, p, l); });
        ws_.setReconnectInterval(3000);
        ws_.enableHeartbeat(15000, 4000, 2);
    }

    void loop() { ws_.loop(); }

    const char *host() const { return host_.c_str(); }
    uint16_t port() const { return port_; }

    // ── console ─────────────────────────────────────────────────────────
    void pushLog(const String &msg, int color = -1) {
        // split multi-line messages; auto-color klipper "!!" errors
        int pos = 0;
        while (pos < (int)msg.length()) {
            int nl = msg.indexOf('\n', pos);
            String line = (nl < 0) ? msg.substring(pos) : msg.substring(pos, nl);
            pos = (nl < 0) ? msg.length() : nl + 1;
            if (!line.length()) continue;
            if (line.length() > 96) line = line.substring(0, 96);
            uint8_t c = (color >= 0) ? (uint8_t)color : (line.startsWith("!!") ? 1 : 0);
            logLines[logHead] = line;
            logColors[logHead] = c;
            logHead = (logHead + 1) % LOGN;
            if (logCount < LOGN) logCount++;
        }
    }
    // i = 0 is the oldest line currently held
    const String &logAt(int i, uint8_t &colorOut) const {
        int idx = (logHead - logCount + i + LOGN) % LOGN;
        colorOut = logColors[idx];
        return logLines[idx];
    }

    // ── commands ────────────────────────────────────────────────────────
    // track=true wires this send into the busy/ok/fail feedback events
    // (used by the Macros screen; plain jogs/temp-sets stay log-only)
    void sendGcode(const String &script, bool echo = true, bool track = false) {
        JsonDocument d;
        uint32_t id = nextId_++;
        d["jsonrpc"] = "2.0";
        d["method"] = "printer.gcode.script";
        d["params"]["script"] = script;
        d["id"] = (int)id;
        String first = script;
        int nl = first.indexOf('\n');
        if (nl >= 0) first = first.substring(0, nl) + " ...";
        bool sent = sendDoc(d);
        if (sent && track) {
            gcodeId_ = id;
            gcodeLabel_ = first;
            gcodeBusy = true;
        }
        if (echo && sent) pushLog("> " + first, 2);
    }
    void pausePrint()      { simpleRpc("printer.print.pause");      pushLog("> pause", 2); }
    void resumePrint()     { simpleRpc("printer.print.resume");     pushLog("> resume", 2); }
    void cancelPrint()     { simpleRpc("printer.print.cancel");     pushLog("> cancel", 2); }
    void emergencyStop()   { simpleRpc("printer.emergency_stop");   pushLog("> EMERGENCY STOP", 1); }
    void firmwareRestart() { simpleRpc("printer.firmware_restart"); pushLog("> firmware_restart", 2); }
    void hostRestart()     { simpleRpc("printer.restart");          pushLog("> restart", 2); }

    void startPrint(const String &fn) {
        JsonDocument d;
        d["jsonrpc"] = "2.0";
        d["method"] = "printer.print.start";
        d["params"]["filename"] = fn;
        d["id"] = (int)nextId_++;
        sendDoc(d);
        pushLog("> print " + fn, 2);
    }

    void requestMacros() {
        JsonDocument d;
        d["jsonrpc"] = "2.0";
        d["method"] = "printer.objects.list";
        listId_ = nextId_++;
        d["id"] = (int)listId_;
        sendDoc(d);
    }

    // ── bulky one-shots over HTTP (blocking, call from loop context) ────
    bool fetchFiles() {
        HTTPClient http;
        http.useHTTP10(true);   // no chunked encoding -> stream-parseable
        http.setTimeout(6000);
        http.begin("http://" + host_ + ":" + String(port_) + "/server/files/list?root=gcodes");
        int code = http.GET();
        bool ok = false;
        if (code == 200) {
            JsonDocument filter;
            filter["result"][0]["path"] = true;
            filter["result"][0]["size"] = true;
            filter["result"][0]["modified"] = true;
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, http.getStream(),
                                                       DeserializationOption::Filter(filter));
            if (!err) {
                files.clear();
                for (JsonObjectConst e : doc["result"].as<JsonArrayConst>()) {
                    FileEntry fe;
                    fe.path = e["path"].as<const char *>();
                    fe.sizeB = e["size"] | 0UL;
                    fe.modified = e["modified"] | 0.0;
                    files.push_back(fe);
                }
                std::sort(files.begin(), files.end(),
                          [](const FileEntry &a, const FileEntry &b) { return a.modified > b.modified; });
                if (files.size() > 80) files.resize(80);   // heap cap — no PSRAM on this unit
                filesLoaded = true;
                ok = true;
                pushLog("[files] " + String(files.size()) + " gcodes", 3);
            } else {
                pushLog(String("[files] parse: ") + err.c_str(), 1);
            }
        } else {
            pushLog("[files] HTTP " + String(code), 1);
        }
        http.end();
        return ok;
    }

    FileMeta fetchMeta(const String &path) {
        FileMeta m;
        HTTPClient http;
        http.useHTTP10(true);
        http.setTimeout(6000);
        http.begin("http://" + host_ + ":" + String(port_) +
                   "/server/files/metadata?filename=" + urlEncode(path));
        int code = http.GET();
        if (code == 200) {
            JsonDocument filter;
            JsonObject fr = filter["result"].to<JsonObject>();
            fr["estimated_time"] = true;
            fr["filament_total"] = true;
            fr["object_height"] = true;
            fr["first_layer_bed_temp"] = true;
            fr["first_layer_extr_temp"] = true;
            fr["slicer"] = true;
            JsonDocument doc;
            if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
                JsonObjectConst r = doc["result"];
                m.estTime    = r["estimated_time"] | -1.0f;
                m.filamentMM = r["filament_total"] | -1.0f;
                m.height     = r["object_height"] | -1.0f;
                m.bedTemp    = r["first_layer_bed_temp"] | -1.0f;
                m.extTemp    = r["first_layer_extr_temp"] | -1.0f;
                if (r["slicer"].is<const char *>()) m.slicer = r["slicer"].as<const char *>();
                m.valid = true;
            }
        }
        http.end();
        return m;
    }

private:
    WebSocketsClient ws_;
    String host_;
    uint16_t port_ = 7125;
    uint32_t nextId_ = 100;
    uint32_t subId_ = 0, listId_ = 0, infoId_ = 0;
    uint32_t gcodeId_ = 0;
    String gcodeLabel_;

    bool sendDoc(JsonDocument &d) {
        if (!wsUp) { pushLog("[ws] not connected", 1); return false; }
        String out;
        serializeJson(d, out);
        ws_.sendTXT(out);
        return true;
    }

    void simpleRpc(const char *method) {
        JsonDocument d;
        d["jsonrpc"] = "2.0";
        d["method"] = method;
        d["id"] = (int)nextId_++;
        sendDoc(d);
    }

    void queryServerInfo() {
        JsonDocument d;
        d["jsonrpc"] = "2.0";
        d["method"] = "server.info";
        infoId_ = nextId_++;
        d["id"] = (int)infoId_;
        sendDoc(d);
    }

    void subscribe() {
        JsonDocument d;
        d["jsonrpc"] = "2.0";
        d["method"] = "printer.objects.subscribe";
        subId_ = nextId_++;
        d["id"] = (int)subId_;
        JsonObject objs = d["params"]["objects"].to<JsonObject>();
        auto arr = [&](const char *name, std::initializer_list<const char *> fields) {
            JsonArray a = objs[name].to<JsonArray>();
            for (const char *f : fields) a.add(f);
        };
        arr("webhooks",       {"state", "state_message"});
        arr("print_stats",    {"state", "filename", "print_duration", "total_duration", "info"});
        arr("display_status", {"progress", "message"});
        arr("virtual_sdcard", {"progress"});
        arr("extruder",       {"temperature", "target", "power"});
        arr("heater_bed",     {"temperature", "target", "power"});
        arr("toolhead",       {"position", "homed_axes"});
        arr("gcode_move",     {"speed_factor", "extrude_factor"});
        arr("fan",            {"speed"});
        sendDoc(d);
    }

    void applyAndTrack(JsonObjectConst status) {
        String prevState = st.printState;
        PrinterState::Klippy prevK = st.klippy;
        st.applyStatus(status);
        if (prevState != st.printState) {
            pushLog("[job] " + prevState + " -> " + st.printState, 3);
            if (st.printState == "complete") evComplete = true;
            if (st.printState == "error") evError = true;
        }
        if (prevK != st.klippy && st.klippy == PrinterState::Klippy::SHUTDOWN) evError = true;
    }

    void onEvent(WStype_t type, uint8_t *payload, size_t len) {
        switch (type) {
            case WStype_CONNECTED:
                wsUp = true;
                pushLog("[ws] connected", 3);
                queryServerInfo();
                subscribe();
                requestMacros();
                break;
            case WStype_DISCONNECTED:
                if (wsUp) pushLog("[ws] disconnected", 1);
                wsUp = false;
                if (gcodeBusy) {   // response is never coming — fail the tracked send
                    gcodeBusy = false;
                    evGcodeFail = true;
                    evGcodeLabel = gcodeLabel_;
                }
                break;
            case WStype_TEXT:
                onText(payload, len);
                break;
            default:
                break;
        }
    }

    void onText(uint8_t *payload, size_t len) {
        // payload is NUL-terminated by the WebSockets lib for TEXT frames
        if (len > 24 * 1024) return;   // heap guard — nothing we expect is this big
        if (strstr((const char *)payload, "notify_proc_stat_update")) return;

        JsonDocument doc;
        if (deserializeJson(doc, (const char *)payload, len)) return;

        const char *method = doc["method"];
        if (method) {
            if (!strcmp(method, "notify_status_update")) {
                applyAndTrack(doc["params"][0]);
            } else if (!strcmp(method, "notify_gcode_response")) {
                const char *msg = doc["params"][0];
                if (msg) pushLog(msg);
            } else if (!strcmp(method, "notify_klippy_ready")) {
                st.klippy = PrinterState::Klippy::READY;
                pushLog("[klippy] ready", 3);
                subscribe();          // subscriptions reset across klippy restarts
                requestMacros();
            } else if (!strcmp(method, "notify_klippy_shutdown")) {
                st.klippy = PrinterState::Klippy::SHUTDOWN;
                pushLog("[klippy] shutdown", 1);
                evError = true;
            } else if (!strcmp(method, "notify_klippy_disconnected")) {
                st.klippy = PrinterState::Klippy::DISCONNECTED;
                pushLog("[klippy] disconnected", 1);
            }
            return;
        }

        uint32_t id = doc["id"] | 0UL;
        if (!doc["error"].isNull()) {
            const char *emsg = doc["error"]["message"] | "unknown";
            pushLog(String("[rpc] err: ") + emsg, 1);
            if (gcodeBusy && id == gcodeId_) {
                gcodeBusy = false;
                evGcodeFail = true;
                evGcodeLabel = gcodeLabel_;
            }
            return;
        }
        if (gcodeBusy && id == gcodeId_) {
            gcodeBusy = false;
            evGcodeOk = true;
            evGcodeLabel = gcodeLabel_;
        }
        if (id == subId_) {
            applyAndTrack(doc["result"]["status"]);
        } else if (id == infoId_) {
            const char *ks = doc["result"]["klippy_state"] | "?";
            pushLog(String("[moonraker] klippy: ") + ks, 3);
        } else if (id == listId_) {
            macros.clear();
            for (const char *name : doc["result"]["objects"].as<JsonArrayConst>()) {
                if (name && !strncmp(name, "gcode_macro ", 12) && name[12] != '_')
                    macros.push_back(String(name + 12));
            }
            std::sort(macros.begin(), macros.end(),
                      [](const String &a, const String &b) { return a.compareTo(b) < 0; });
            macrosLoaded = true;
        }
    }

    static String urlEncode(const String &s) {
        static const char *hex = "0123456789ABCDEF";
        String o;
        o.reserve(s.length() + 8);
        for (unsigned i = 0; i < s.length(); i++) {
            char c = s[i];
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
                o += c;
            else {
                o += '%';
                o += hex[(c >> 4) & 0xF];
                o += hex[c & 0xF];
            }
        }
        return o;
    }
};
