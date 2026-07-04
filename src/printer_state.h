// printer_state.h — Klipputer's live data model.
//
// One struct holding everything the UI draws, updated by moonraker.h from
// the printer.objects.subscribe initial snapshot AND from every
// notify_status_update diff — both use the same applyStatus(), since
// Moonraker uses the same shape for both (fields simply absent when
// unchanged). Every field check is presence-guarded so a diff touching
// only extruder.temperature never clobbers anything else.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct PrinterState {
    // ── klippy (the Klipper host process) ──────────────────────────────
    enum class Klippy { UNKNOWN, STARTUP, READY, SHUTDOWN, KERROR, DISCONNECTED };
    Klippy klippy = Klippy::UNKNOWN;
    String stateMessage;          // webhooks.state_message — shown when not ready

    // ── heaters ────────────────────────────────────────────────────────
    float nozzleTemp = 0, nozzleTarget = 0, nozzlePower = 0;
    float bedTemp = 0,    bedTarget = 0,    bedPower = 0;

    // ── print job ──────────────────────────────────────────────────────
    String printState = "standby";  // standby/printing/paused/complete/cancelled/error
    String filename;
    float printDuration = 0;        // print_stats.print_duration (excludes pauses)
    float totalDuration = 0;
    float dsProgress = 0;           // display_status.progress   (0..1)
    float sdProgress = 0;           // virtual_sdcard.progress   (0..1)
    int currentLayer = -1, totalLayer = -1;   // print_stats.info — only if the slicer/macros set them
    String m117;                    // display_status.message

    // ── motion / tuning ────────────────────────────────────────────────
    float posX = 0, posY = 0, posZ = 0;
    String homedAxes;               // e.g. "xyz", lowercase
    float speedFactor = 1.0f;       // gcode_move.speed_factor (1.0 = 100%)
    float extrudeFactor = 1.0f;
    float fanSpeed = 0;             // fan.speed (0..1)

    // ── temp history ring for the graph (1 sample/sec, 2 minutes) ─────
    static const int HIST = 120;
    float nozzleHist[HIST] = {0};
    float bedHist[HIST] = {0};
    int histCount = 0, histHead = 0;

    void pushHist() {
        nozzleHist[histHead] = nozzleTemp;
        bedHist[histHead] = bedTemp;
        histHead = (histHead + 1) % HIST;
        if (histCount < HIST) histCount++;
    }
    // i = 0 is the oldest sample currently held
    float histAt(const float *buf, int i) const {
        int start = (histHead - histCount + HIST) % HIST;
        return buf[(start + i) % HIST];
    }

    bool isActive() const { return printState == "printing" || printState == "paused"; }
    float prog() const { return sdProgress > 0.0005f ? sdProgress : dsProgress; }

    // Apply a status object (subscribe snapshot or notify diff).
    void applyStatus(JsonObjectConst s) {
        if (s.isNull()) return;
        JsonObjectConst o;

        o = s["webhooks"];
        if (!o.isNull()) {
            const char *ks = o["state"];
            if (ks) {
                if      (!strcmp(ks, "ready"))    klippy = Klippy::READY;
                else if (!strcmp(ks, "shutdown")) klippy = Klippy::SHUTDOWN;
                else if (!strcmp(ks, "startup"))  klippy = Klippy::STARTUP;
                else                              klippy = Klippy::KERROR;
            }
            if (o["state_message"].is<const char *>()) stateMessage = o["state_message"].as<const char *>();
        }

        o = s["extruder"];
        if (!o.isNull()) {
            if (o["temperature"].is<float>()) nozzleTemp   = o["temperature"];
            if (o["target"].is<float>())      nozzleTarget = o["target"];
            if (o["power"].is<float>())       nozzlePower  = o["power"];
        }

        o = s["heater_bed"];
        if (!o.isNull()) {
            if (o["temperature"].is<float>()) bedTemp   = o["temperature"];
            if (o["target"].is<float>())      bedTarget = o["target"];
            if (o["power"].is<float>())       bedPower  = o["power"];
        }

        o = s["print_stats"];
        if (!o.isNull()) {
            if (o["state"].is<const char *>())    printState = o["state"].as<const char *>();
            if (o["filename"].is<const char *>()) filename   = o["filename"].as<const char *>();
            if (o["print_duration"].is<float>())  printDuration = o["print_duration"];
            if (o["total_duration"].is<float>())  totalDuration = o["total_duration"];
            JsonObjectConst inf = o["info"];
            if (!inf.isNull()) {
                if (inf["current_layer"].is<int>()) currentLayer = inf["current_layer"];
                if (inf["total_layer"].is<int>())   totalLayer   = inf["total_layer"];
            }
        }

        o = s["display_status"];
        if (!o.isNull()) {
            if (o["progress"].is<float>()) dsProgress = o["progress"];
            if (o["message"].is<const char *>()) m117 = o["message"].as<const char *>();
            else if (o.containsKey("message")) m117 = "";  // key present but null -> M117 cleared
        }

        o = s["virtual_sdcard"];
        if (!o.isNull()) {
            if (o["progress"].is<float>()) sdProgress = o["progress"];
        }

        o = s["toolhead"];
        if (!o.isNull()) {
            JsonArrayConst p = o["position"];
            if (!p.isNull() && p.size() >= 3) { posX = p[0]; posY = p[1]; posZ = p[2]; }
            if (o["homed_axes"].is<const char *>()) homedAxes = o["homed_axes"].as<const char *>();
        }

        o = s["gcode_move"];
        if (!o.isNull()) {
            if (o["speed_factor"].is<float>())   speedFactor   = o["speed_factor"];
            if (o["extrude_factor"].is<float>()) extrudeFactor = o["extrude_factor"];
        }

        o = s["fan"];
        if (!o.isNull()) {
            if (o["speed"].is<float>()) fanSpeed = o["speed"];
        }
    }
};
