// ═══════════════════════════════════════════════════════════════════════
//  KLIPPUTER v1.0 — pocket KlipperScreen for the M5Stack Cardputer ADV
//
//  Remote control + live status for the Flashforge AD5M (community
//  Klipper) via Moonraker's JSON-RPC WebSocket at MOONRAKER_HOST:PORT
//  (see secrets.h). Planned in knowledge-base/projects/klipputer/
//  handoff.md before this code was written.
//
//  Reuses the established patterns from radar/range-toolkit/cardmeleon on
//  this same hardware, deliberately unchanged for muscle-memory:
//   - M5Canvas offscreen double-buffer, one pushSprite per 50ms frame
//   - keysState() iteration (NOT isKeyPressed())
//   - ";/." navigate, ",//" adjust with AdjustRepeater (tap = 1 step,
//     hold ramps 220ms -> 50ms; includes the v1.3 staleness guard)
//   - 'e' direct numeric entry ('c' clears, backspace 0x2A best-effort)
//   - ESC = HID 0x29 OR 0x35 (0x35 is what this keyboard actually sends —
//     confirmed on hardware in range-toolkit), 'h' guaranteed backup
//   - battery % top-right, red below 20%
//   - NO PSRAM flags anywhere: this unit's PSRAM is broken
//
//  Audio is speaker-only (no mic), so range-toolkit's codec-arbiter
//  problem doesn't apply here. If beeps are ever silent, check the 3.5mm
//  jack-detect switch first (range-toolkit, 2026-06-28).
//
//  Screens (number keys jump directly):
//   [1] Dashboard  [2] Temps  [3] Move  [4] Files  [5] Macros  [6] Log
//
//  STATUS: built 2026-07-03, not yet hardware-tested.
// ═══════════════════════════════════════════════════════════════════════

#include <M5Cardputer.h>
#include <WiFi.h>
#include "secrets.h"
#include "printer_state.h"
#include "moonraker.h"
#include "ui.h"

M5Canvas canvas(&M5Cardputer.Display);
Moonraker mr;

// webcam defaults — override in secrets.h if your cam differs
#ifndef WEBCAM_PORT
#define WEBCAM_PORT 8080
#endif
#ifndef WEBCAM_PATH
#define WEBCAM_PATH "/stream"
#endif

// OBJECTS ('7') and WEBCAM ('8') live off the tab bar — DASH-family views
enum class Screen { DASH, TEMP, MOVE, FILES, MACROS, LOG, OBJECTS, WEBCAM };
Screen screen = Screen::DASH;

uint32_t lastDrawMs = 0, lastHistMs = 0;

// ── confirm modal ────────────────────────────────────────────────────────
// Anything that can wreck an active print sits behind one of these:
// cancel, e-stop, pause/resume, mid-print temp changes, exclude object.
enum class Act { NONE, CANCEL_PRINT, ESTOP, START_PRINT, FW_RESTART, HOST_RESTART,
                 PAUSE, RESUME, SET_TEMP, EXCLUDE_OBJ };
struct {
    bool active = false;
    Act act = Act::NONE;
    String l1, l2, arg;
    int argA = 0;
    float argB = 0;
} modal;

void openConfirm(Act a, const String &l1, const String &l2, const String &arg = "",
                 int argA = 0, float argB = 0) {
    modal.active = true;
    modal.act = a;
    modal.l1 = l1;
    modal.l2 = l2;
    modal.arg = arg;
    modal.argA = argA;
    modal.argB = argB;
}

// ── beep melodies (speaker only — non-blocking note queue) ──────────────
struct Note { uint16_t f, d; };
Note beepQ[6];
int beepLen = 0, beepIdx = 0;
uint32_t beepNextMs = 0;

void playNotes(const Note *notes, int n) {
    beepLen = min(n, 6);
    for (int i = 0; i < beepLen; i++) beepQ[i] = notes[i];
    beepIdx = 0;
    beepNextMs = millis();
}
void tickBeep() {
    if (beepIdx >= beepLen) return;
    if (millis() < beepNextMs) return;
    M5Cardputer.Speaker.tone(beepQ[beepIdx].f, beepQ[beepIdx].d);
    beepNextMs = millis() + beepQ[beepIdx].d + 30;
    beepIdx++;
}
void chirpComplete() { static const Note n[] = {{880, 120}, {1175, 120}, {1568, 260}}; playNotes(n, 3); }
void chirpError()    { static const Note n[] = {{220, 300}, {160, 450}}; playNotes(n, 2); }
void chirpWarn()     { static const Note n[] = {{440, 90}}; playNotes(n, 1); }
void chirpAck()      { static const Note n[] = {{1400, 45}}; playNotes(n, 1); }

// ── direct numeric entry (range-toolkit's proven 'e' pattern) ───────────
bool  editingField = false;
char  editBuf[12] = {0};
int   editLen = 0;
float *editTarget = nullptr;
float editMinV = 0, editMaxV = 0;
bool  *editDirtyFlag = nullptr;

void startEdit(float *target, float minV, float maxV, bool *dirtyFlag) {
    editingField = true;
    editTarget = target;
    editMinV = minV;
    editMaxV = maxV;
    editDirtyFlag = dirtyFlag;
    snprintf(editBuf, sizeof(editBuf), "%g", (double)*target);
    editLen = strlen(editBuf);
}
void cancelEdit() { editingField = false; editTarget = nullptr; }
void confirmEdit() {
    if (editTarget && editLen > 0) {
        *editTarget = constrain((float)atof(editBuf), editMinV, editMaxV);
        if (editDirtyFlag) *editDirtyFlag = true;
    }
    editingField = false;
    editTarget = nullptr;
}

template <typename KeysState>
void handleEditKeys(const KeysState &state) {
    for (auto key : state.word) {
        if ((key >= '0' && key <= '9') || key == '.') {
            if (editLen < (int)sizeof(editBuf) - 1) { editBuf[editLen++] = key; editBuf[editLen] = 0; }
        } else if (key == 'c' || key == 'C') {
            editLen = 0;
            editBuf[0] = 0;   // clear-buffer fallback — backspace HID code is best-effort
        } else if (key == 'h' || key == 'H') {
            cancelEdit();
            return;
        }
    }
    for (auto key : state.hid_keys) {
        if (key == 0) continue;
        if (key == 0x28) { confirmEdit(); return; }               // Enter
        if (key == 0x2A && editLen > 0) { editLen--; editBuf[editLen] = 0; }  // Backspace (best-effort)
        if (key == 0x29 || key == 0x35) { cancelEdit(); return; } // ESC (0x35 = this keyboard's real code)
    }
}

// ── press-and-hold auto-repeat for ,// (verbatim from range-toolkit v1.3,
//    including the staleness guard for missed key releases) ─────────────
struct AdjustRepeater {
    char activeKey = 0;
    uint32_t pressedAt = 0, lastFireAt = 0, lastPollAt = 0;

    int poll(bool decHeld, bool incHeld) {
        char key = decHeld ? ',' : (incHeld ? '/' : 0);
        uint32_t now = millis();
        if (key == 0) { activeKey = 0; return 0; }
        if (key == activeKey && now - lastPollAt > 150) activeKey = 0;  // stale hold -> treat as fresh press
        lastPollAt = now;
        if (key != activeKey) {
            activeKey = key;
            pressedAt = lastFireAt = now;
            return (key == ',') ? -1 : 1;
        }
        const uint32_t INITIAL_DELAY = 400;
        uint32_t held = now - pressedAt;
        if (held < INITIAL_DELAY) return 0;
        uint32_t accelMs = min(held - INITIAL_DELAY, (uint32_t)1500);
        uint32_t interval = 220 - (uint32_t)((170.0f * accelMs) / 1500.0f);
        if (now - lastFireAt >= interval) {
            lastFireAt = now;
            return (key == ',') ? -1 : 1;
        }
        return 0;
    }
};
AdjustRepeater adjRepeater;

// ── per-screen state ─────────────────────────────────────────────────────
// TEMP: pending targets track actuals until made dirty by adjust/edit
int   tempSel = 0;                    // 0 nozzle, 1 bed
float pendTemp[2] = {0, 0};
bool  tempDirty[2] = {false, false};
int   presetIdx = -1;
struct Preset { const char *name; int noz, bed; };
const Preset PRESETS[] = {{"PLA", 210, 60}, {"PETG", 240, 80}, {"ABS", 250, 100}, {"OFF", 0, 0}};

// DASH tune row (speed/flow/fan % + Z-offset babystep) — debounced sends
int      tuneSel = 0;
float    pendTune[3] = {100, 100, 0};
bool     tuneDirty[3] = {false, false, false};
uint32_t lastTuneAdjMs = 0;
// Z babystep accumulates locally, then one SET_GCODE_OFFSET Z_ADJUST per
// debounce window — 0.005mm per step, hold-to-ramp for bigger moves
float    zAdjPending = 0;
bool     zDirty = false;

// OBJECTS (screen 7) — exclude-object plate map
int  objSel = 0, objTop = 0;
bool objectsPending = false;

// WEBCAM (screen 8) — grabs JPEG frames straight out of the MJPEG stream
WiFiClient camClient;
uint8_t   *camBuf = nullptr;
size_t     camFill = 0, camScanned = 0;
int        camSoi = -1;
bool       camFrameShown = false;
uint32_t   camLastFrameMs = 0;
int        camFps10 = 0;
const size_t CAM_BUF_SIZE = 56 * 1024;   // biggest frame accepted — no PSRAM on this unit

void camStop() {
    camClient.stop();
    if (camBuf) { free(camBuf); camBuf = nullptr; }
    camFill = camScanned = 0;
    camSoi = -1;
    camFrameShown = false;
    camLastFrameMs = 0;
}

bool camStart() {
    camStop();
    camBuf = (uint8_t *)malloc(CAM_BUF_SIZE);
    if (!camBuf) return false;
    if (!camClient.connect(MOONRAKER_HOST, WEBCAM_PORT)) { camStop(); return false; }
    camClient.print(String("GET ") + WEBCAM_PATH + " HTTP/1.1\r\nHost: " MOONRAKER_HOST "\r\n\r\n");
    return true;
}

// Called every loop() while on the webcam screen: reads a slice of the
// stream, and when a complete FFD8..FFD9 JPEG is in the buffer, decodes
// it into the canvas (drawCurrent pushes it). HTTP/multipart headers pass
// through the scanner harmlessly — they never contain the SOI marker.
void camLoop() {
    if (!camBuf || !camClient.connected()) return;
    int n = camClient.read(camBuf + camFill, min((size_t)4096, CAM_BUF_SIZE - camFill));
    if (n > 0) camFill += n;
    for (size_t i = camScanned; i + 1 < camFill; i++) {
        if (camBuf[i] == 0xFF && camBuf[i + 1] == 0xD8) {
            camSoi = (int)i;
        } else if (camBuf[i] == 0xFF && camBuf[i + 1] == 0xD9 && camSoi >= 0) {
            canvas.fillRect(0, 13, SCR_W, SCR_H - 13, TFT_BLACK);
            canvas.drawJpg(camBuf + camSoi, (i + 2) - camSoi, 0, 13, SCR_W, SCR_H - 23,
                           0, 0, 0.0f, 0.0f);   // zoom 0 = auto-fit to maxWidth/maxHeight
            camFrameShown = true;
            uint32_t now = millis();
            if (camLastFrameMs) camFps10 = (int)(10000UL / max(now - camLastFrameMs, (uint32_t)1));
            camLastFrameMs = now;
            size_t tail = camFill - (i + 2);
            memmove(camBuf, camBuf + i + 2, tail);
            camFill = tail;
            camScanned = 0;
            camSoi = -1;
            return;   // one frame per pass
        }
    }
    camScanned = camFill > 0 ? camFill - 1 : 0;
    if (camFill >= CAM_BUF_SIZE - 4096) {   // overrun without a frame — resync
        camFill = camScanned = 0;
        camSoi = -1;
    }
}

// MOVE
const float STEPS[] = {0.1f, 1.0f, 10.0f, 25.0f};
int  stepIdx = 2;
bool homeMenu = false;
int  homeSel = 0;

// FILES
bool    fileDetail = false;
int     fileSel = 0, fileTop = 0;
String  detailPath;
FileMeta meta;
bool    filesPending = false, metaPending = false;

// MACROS
int macroSel = 0, macroTop = 0;

// CONSOLE (screen 6) — log view + manual gcode entry
bool   restartMenu = false;
int    restartSel = 0;
int    logScroll = 0;          // entries scrolled back from the newest
bool   consoleInput = false;   // typing a command — captures ALL keys
String cmdBuf, lastCmd;

// help overlay — '0' from any screen except TEMP (where 0 = heater off)
bool helpActive = false;

// ── toast — transient feedback banner, drawn over any screen ────────────
String   toastMsg;
uint16_t toastColor = C_ACCENT;
uint32_t toastUntil = 0;

void showToast(const String &msg, uint16_t color, uint32_t ms) {
    toastMsg = msg;
    toastColor = color;
    toastUntil = millis() + ms;
}

// Fixed-width box (no per-frame width changes — the old dynamic width
// pulsed when the busy dots animated), lines centered, guaranteed
// word-wrap to two lines. 31 chars/line leaves room for 3 busy dots.
void drawToast() {
    if (millis() >= toastUntil) return;
    const int CPL = 31;
    String t = toastMsg;
    String l1 = t, l2 = "";
    if ((int)t.length() > CPL) {
        int cut = CPL;
        for (int i = CPL; i > 12; i--) if (t.charAt(i) == ' ') { cut = i; break; }
        l1 = t.substring(0, cut);
        l2 = t.substring(t.charAt(cut) == ' ' ? cut + 1 : cut);
        if ((int)l2.length() > CPL) l2 = l2.substring(0, CPL - 2) + "..";
    }
    int w = SCR_W - 8, x = 4;
    int h = l2.length() ? 28 : 18;
    // console keeps its input line at the bottom — toast moves up top there
    int y = (screen == Screen::LOG) ? 15 : SCR_H - 12 - h;
    canvas.fillRoundRect(x, y, w, h, 3, C_PANEL);
    canvas.drawRoundRect(x, y, w, h, 3, toastColor);
    canvas.setTextSize(1);
    canvas.setTextColor(toastColor);
    canvas.setCursor(x + (w - (int)l1.length() * 6) / 2, y + 5);
    canvas.print(l1);
    if (l2.length()) {
        canvas.setCursor(x + (w - (int)l2.length() * 6) / 2, y + 15);
        canvas.print(l2);
    }
    // animated dots while the tracked gcode is still executing — appended
    // after the last line so the box itself never changes size
    if (mr.gcodeBusy) {
        int dots = (millis() / 300) % 4;
        for (int i = 0; i < dots; i++) canvas.print('.');
    }
}

// ── helpers ──────────────────────────────────────────────────────────────
uint16_t stateColor(const String &s) {
    if (s == "printing") return C_OK;
    if (s == "paused")   return C_WARN;
    if (s == "complete") return C_ACCENT;
    if (s == "error")    return C_ERR;
    return C_DIM;
}

uint16_t connDotColor() {
    if (!mr.wsUp) return C_ERR;
    return (mr.st.klippy == PrinterState::Klippy::READY) ? C_OK : C_WARN;
}

void switchScreen(Screen s) {
    if (screen == Screen::WEBCAM && s != Screen::WEBCAM) camStop();
    bool enteringCam = (s == Screen::WEBCAM && screen != Screen::WEBCAM);
    screen = s;
    homeMenu = restartMenu = fileDetail = false;
    consoleInput = false;
    cmdBuf = "";
    logScroll = 0;
    if (s == Screen::FILES && !mr.filesLoaded) filesPending = true;
    if (s == Screen::MACROS && !mr.macrosLoaded && mr.wsUp) mr.requestMacros();
    if (s == Screen::OBJECTS) objectsPending = true;   // refresh every entry — objects change per job
    if (enteringCam && !camStart()) showToast("webcam connect failed", C_ERR, 3000);
}

void goBack() {
    if (modal.active) { modal.active = false; return; }
    if (homeMenu)    { homeMenu = false; return; }
    if (restartMenu) { restartMenu = false; return; }
    if (fileDetail)  { fileDetail = false; return; }
    if (screen != Screen::DASH) switchScreen(Screen::DASH);   // via switchScreen so the webcam socket closes
}

void doSetTemp(int heater, int t) {
    mr.sendGcode(heater == 0 ? "M104 S" + String(t) : "M140 S" + String(t));
    tempDirty[heater] = false;
    chirpAck();
}

void execConfirm() {
    Act a = modal.act;
    modal.active = false;
    switch (a) {
        case Act::CANCEL_PRINT: mr.cancelPrint(); break;
        case Act::ESTOP:        mr.emergencyStop(); chirpError(); break;
        case Act::START_PRINT:  mr.startPrint(modal.arg); switchScreen(Screen::DASH); break;
        case Act::FW_RESTART:   mr.firmwareRestart(); break;
        case Act::HOST_RESTART: mr.hostRestart(); break;
        case Act::PAUSE:        mr.pausePrint(); break;
        case Act::RESUME:       mr.resumePrint(); break;
        case Act::SET_TEMP:     doSetTemp(modal.argA, (int)modal.argB); break;
        case Act::EXCLUDE_OBJ:
            mr.sendGcode("EXCLUDE_OBJECT NAME=" + modal.arg);
            showToast("excluded: " + modal.arg, C_WARN, 2500);
            break;
        default: break;
    }
}

// Mid-print temp changes go through a confirm — an accidental target
// change during a running job is a print-killer. Idle/paused sends stay
// immediate.
void requestTempSend(int heater) {
    int t = (int)pendTemp[heater];
    if (mr.st.printState == "printing")
        openConfirm(Act::SET_TEMP,
                    String(heater == 0 ? "NOZZLE" : "BED") + " -> " + String(t) + "C MID-PRINT?",
                    "confirm temp change during the job", "", heater, (float)t);
    else
        doSetTemp(heater, t);
}

void jog(char axis, float dist) {
    if (mr.st.printState == "printing") { mr.pushLog("!! motion locked while printing", 1); chirpWarn(); return; }
    char lower = tolower(axis);
    if (mr.st.homedAxes.indexOf(lower) < 0) {
        mr.pushLog(String("!! home ") + axis + " first ([g] home menu)", 1);
        chirpWarn();
        return;
    }
    float feed = (axis == 'Z') ? 900 : 6000;
    char buf[64];
    snprintf(buf, sizeof(buf), "G91\nG1 %c%.2f F%.0f\nG90", axis, dist, feed);
    mr.sendGcode(buf);
}

void extrude(float mm) {
    if (mr.st.printState == "printing") { mr.pushLog("!! locked while printing", 1); chirpWarn(); return; }
    if (mr.st.nozzleTemp < 170) {
        mr.pushLog("!! nozzle too cold to extrude (<170C)", 1);
        chirpWarn();
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "M83\nG1 E%.1f F150", mm);
    mr.sendGcode(buf);
}

void doHome(int sel) {
    static const char *cmds[] = {"G28", "G28 X", "G28 Y", "G28 Z"};
    if (mr.st.printState == "printing") { mr.pushLog("!! motion locked while printing", 1); chirpWarn(); return; }
    mr.sendGcode(cmds[sel]);
}

void applyPreset() {
    presetIdx = (presetIdx + 1) % 4;
    const Preset &p = PRESETS[presetIdx];
    mr.sendGcode("M104 S" + String(p.noz));
    mr.sendGcode("M140 S" + String(p.bed), false);
    mr.pushLog(String("[preset] ") + p.name + " " + p.noz + "/" + p.bed, 3);
    tempDirty[0] = tempDirty[1] = false;
    chirpAck();
}

// Pending values track live values until the user starts adjusting.
void syncPendings() {
    if (!tempDirty[0]) pendTemp[0] = mr.st.nozzleTarget;
    if (!tempDirty[1]) pendTemp[1] = mr.st.bedTarget;
    if (!tuneDirty[0]) pendTune[0] = mr.st.speedFactor * 100.0f;
    if (!tuneDirty[1]) pendTune[1] = mr.st.extrudeFactor * 100.0f;
    if (!tuneDirty[2]) pendTune[2] = mr.st.fanSpeed * 100.0f;
}

// ── console command entry ────────────────────────────────────────────────
void sendConsoleCmd() {
    cmdBuf.trim();
    if (!cmdBuf.length()) { consoleInput = false; return; }
    lastCmd = cmdBuf;
    mr.sendGcode(cmdBuf, true, /*track=*/true);
    if (mr.gcodeBusy) chirpAck();   // response lands in the log + OK/FAIL toast
    else { showToast("SEND FAILED - not connected", C_ERR, 3000); chirpWarn(); }
    cmdBuf = "";
    logScroll = 0;   // snap back to the newest lines to watch the response
    // stay in input mode for follow-up commands; ESC exits
}

// Captures ALL keys while typing (letters must not trigger shortcuts, and
// 1-6 must not switch screens mid-command). Klipper command/parameter
// names are case-insensitive, so lowercase typing is fine as-is.
template <typename KeysState>
void handleConsoleKeys(const KeysState &state) {
    // Fn+arrow = terminal-style history: fn+';' (up) recalls the last
    // command any time, fn+'.' (down) clears the line. Verified against
    // the installed M5Cardputer lib source: fn is a plain flag and the
    // key's base char still arrives in .word, so fn combos must be
    // handled first and never fall through into typing.
    if (state.fn) {
        for (auto key : state.word) {
            if (key == ';' && lastCmd.length()) cmdBuf = lastCmd;
            if (key == '.') cmdBuf = "";
        }
        for (auto key : state.hid_keys) {   // ESC still exits even with fn held
            if (key == 0x29 || key == 0x35) { consoleInput = false; cmdBuf = ""; return; }
        }
        return;
    }
    for (auto key : state.word) {
        if (cmdBuf.length() < 64 && key >= 32 && key < 127) cmdBuf += (char)key;
    }
    if (state.del && cmdBuf.length()) cmdBuf.remove(cmdBuf.length() - 1);
    for (auto key : state.hid_keys) {
        if (key == 0) continue;
        // backspace via raw HID only if the lib's del flag didn't already fire
        if (key == 0x2A && !state.del && cmdBuf.length()) cmdBuf.remove(cmdBuf.length() - 1);
        if (key == 0x28) { sendConsoleCmd(); return; }
        if (key == 0x29 || key == 0x35) { consoleInput = false; cmdBuf = ""; return; }
    }
}

// Debounced tune sends — fire 450ms after the last ,// adjustment so a
// hold-to-ramp doesn't spam Moonraker with an RPC per step.
void flushTune() {
    if (millis() - lastTuneAdjMs < 450) return;
    if (tuneDirty[0]) { mr.sendGcode("M220 S" + String((int)pendTune[0])); tuneDirty[0] = false; }
    if (tuneDirty[1]) { mr.sendGcode("M221 S" + String((int)pendTune[1])); tuneDirty[1] = false; }
    if (tuneDirty[2]) { mr.sendGcode("M106 S" + String((int)roundf(pendTune[2] * 2.55f))); tuneDirty[2] = false; }
    if (zDirty) {
        char b[56];
        snprintf(b, sizeof(b), "SET_GCODE_OFFSET Z_ADJUST=%.3f MOVE=1", zAdjPending);
        mr.sendGcode(b);
        zAdjPending = 0;
        zDirty = false;
    }
}

// ═══ drawing ═════════════════════════════════════════════════════════════

void drawLegend(const char *text) {
    canvas.setTextSize(1);
    canvas.setTextColor(C_FAINT);
    canvas.setCursor(2, SCR_H - 9);
    canvas.print(text);
}

void drawDash() {
    canvas.fillScreen(C_BG);
    drawTopBar(0, connDotColor());
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    bool active = mr.st.isActive();

    String stateStr = mr.st.printState;
    stateStr.toUpperCase();
    uint16_t sc = stateColor(mr.st.printState);
    int bw = stateStr.length() * 6 + 10;
    canvas.fillRoundRect(2, 15, bw, 12, 3, sc);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_BLACK);
    canvas.setCursor(7, 17);
    canvas.print(stateStr);
    if (mr.st.filename.length()) drawMarquee(bw + 8, 17, SCR_W - bw - 12, mr.st.filename, C_TEXT);

    if (!wifiUp || !mr.wsUp || mr.st.klippy != PrinterState::Klippy::READY) {
        // connection / klippy problem panel instead of the live dashboard
        canvas.drawRoundRect(6, 32, SCR_W - 12, 76, 4, C_BORDER);
        canvas.setTextSize(1);
        if (!wifiUp) {
            canvas.setTextColor(C_WARN);
            canvas.setCursor(14, 40);
            canvas.print("WiFi: connecting...");
            canvas.setTextColor(C_DIM);
            canvas.setCursor(14, 54);
            canvas.printf("SSID %s", WIFI_SSID);
        } else if (!mr.wsUp) {
            canvas.setTextColor(C_WARN);
            canvas.setCursor(14, 40);
            canvas.print("Moonraker: connecting...");
            canvas.setTextColor(C_DIM);
            canvas.setCursor(14, 54);
            canvas.printf("%s:%u", mr.host(), (unsigned)mr.port());
            canvas.setCursor(14, 66);
            canvas.printf("IP %s  RSSI %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            canvas.setTextColor(C_ERR);
            canvas.setCursor(14, 40);
            canvas.print("Klipper: not ready");
            drawWrapped(14, 54, SCR_W - 28, mr.st.stateMessage.length() ? mr.st.stateMessage
                                                                        : String("waiting for klippy..."),
                        C_DIM, 4);
        }
        canvas.setTextColor(C_NOZ);
        canvas.setCursor(10, 114);
        canvas.printf("NOZ %.0f/%.0f", mr.st.nozzleTemp, mr.st.nozzleTarget);
        canvas.setTextColor(C_BED);
        canvas.setCursor(120, 114);
        canvas.printf("BED %.0f/%.0f", mr.st.bedTemp, mr.st.bedTarget);
        drawLegend("[6]console  [1-6]screens  [0]help");
        return;
    }

    drawTempTile(2, 30, 116, 34, "NOZZLE", mr.st.nozzleTemp, mr.st.nozzleTarget, mr.st.nozzlePower, C_NOZ);
    drawTempTile(122, 30, 116, 34, "BED", mr.st.bedTemp, mr.st.bedTarget, mr.st.bedPower, C_BED);

    float p = mr.st.prog();
    drawProgressBar(2, 68, 236, 13, p, active ? C_OK : C_FAINT);

    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(2, 85);
    canvas.printf("elapsed %s", fmtDur(mr.st.printDuration).c_str());
    String eta = "--:--";
    if (p > 0.02f && mr.st.printDuration > 1) eta = fmtDur(mr.st.printDuration * (1.0f / p - 1.0f));
    canvas.setCursor(148, 85);
    canvas.printf("eta %s", eta.c_str());

    canvas.setTextColor(C_TEXT);
    canvas.setCursor(2, 96);
    if (mr.st.totalLayer > 0) canvas.printf("L %d/%d  ", mr.st.currentLayer, mr.st.totalLayer);
    canvas.printf("Z %.2f", mr.st.posZ);
    if (mr.st.m117.length()) drawMarquee(110, 96, 126, mr.st.m117, C_ACCENT);

    // tune row — S/F/FAN % + live Z-offset babystep, selectable while active
    int tx = 2;
    for (int i = 0; i < 4; i++) {
        bool sel = active && i == tuneSel;
        bool dirty = (i < 3) ? tuneDirty[i] : zDirty;
        char buf[16];
        switch (i) {
            case 0: snprintf(buf, sizeof(buf), "S%3.0f%%", pendTune[0]); break;
            case 1: snprintf(buf, sizeof(buf), "F%3.0f%%", pendTune[1]); break;
            case 2: snprintf(buf, sizeof(buf), "FAN%3.0f%%", pendTune[2]); break;
            case 3: snprintf(buf, sizeof(buf), "Z%+.3f", mr.st.zOffset + zAdjPending); break;
        }
        int w = strlen(buf) * 6 + 8;
        if (sel) {
            canvas.fillRoundRect(tx, 106, w, 12, 2, C_PANEL);
            canvas.drawRoundRect(tx, 106, w, 12, 2, C_ACCENT);
        }
        canvas.setTextColor(dirty ? C_WARN : (sel ? C_TEXT : C_DIM));
        canvas.setCursor(tx + 4, 108);
        canvas.print(buf);
        tx += w + 6;
    }

    if (active)
        drawLegend(mr.st.printState == "paused" ? "[p]resume [c]ncl [x]stop [;.][,/]tune+Z"
                                                : "[p]ause [c]ncl [x]stop [;.][,/]tune+Z");
    else
        drawLegend("[4]files [7]objects [8]cam [0]help");
}

void drawTemp() {
    canvas.fillScreen(C_BG);
    drawTopBar(1, connDotColor());

    for (int i = 0; i < 2; i++) {
        int y = 16 + i * 16;
        uint16_t color = i ? C_BED : C_NOZ;
        if (tempSel == i) canvas.fillRoundRect(0, y - 2, SCR_W, 15, 2, C_PANEL);
        canvas.setTextSize(1);
        canvas.setTextColor(color);
        canvas.setCursor(6, y);
        canvas.printf("%-6s %6.1fC", i ? "BED" : "NOZZLE", i ? mr.st.bedTemp : mr.st.nozzleTemp);
        // pending target — yellow + brackets while unsent
        canvas.setTextColor(tempDirty[i] ? C_WARN : C_TEXT);
        canvas.setCursor(110, y);
        if (editingField && editTarget == &pendTemp[i]) canvas.printf("> %s_", editBuf);
        else if (tempDirty[i])                          canvas.printf("[%3.0f]", pendTemp[i]);
        else                                            canvas.printf(" %3.0f", pendTemp[i]);
        // heater power mini-bar
        float pw = i ? mr.st.bedPower : mr.st.nozzlePower;
        canvas.fillRect(170, y + 2, 60, 4, C_FAINT);
        int bw2 = (int)(60 * constrain(pw, 0.0f, 1.0f));
        if (bw2 > 0) canvas.fillRect(170, y + 2, bw2, 4, color);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(6, 50);
    canvas.printf("[p]reset cycle: %s", presetIdx < 0 ? "PLA>PETG>ABS>OFF" : PRESETS[presetIdx].name);

    drawTempGraph(2, 60, 236, 62, mr.st);
    drawLegend("[;.]sel [,/]adj [e]type [ENT]send [0]off");
}

void drawMove() {
    canvas.fillScreen(C_BG);
    drawTopBar(2, connDotColor());
    bool locked = mr.st.printState == "printing";

    if (locked) {
        canvas.fillRoundRect(2, 15, SCR_W - 4, 12, 2, C_ERR);
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_BLACK);
        canvas.setCursor(8, 17);
        canvas.print("PRINTING - MOTION LOCKED");
    }

    // position block, per-axis homed indicator
    struct { const char *n; float v; char a; } axes[3] = {
        {"X", mr.st.posX, 'x'}, {"Y", mr.st.posY, 'y'}, {"Z", mr.st.posZ, 'z'}};
    for (int i = 0; i < 3; i++) {
        int y = 32 + i * 15;
        bool homed = mr.st.homedAxes.indexOf(axes[i].a) >= 0;
        canvas.setTextSize(1);
        canvas.setTextColor(homed ? C_OK : C_ERR);
        canvas.setCursor(6, y);
        canvas.print(axes[i].n);
        canvas.setTextSize(2);
        canvas.setTextColor(C_TEXT);
        canvas.setCursor(20, y - 3);
        canvas.printf("%8.2f", axes[i].v);
    }

    // right panel: step + nozzle temp (cold-extrude awareness)
    canvas.drawRoundRect(146, 30, 92, 44, 3, C_BORDER);
    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(152, 34);
    canvas.print("STEP [t]");
    canvas.setTextSize(2);
    canvas.setTextColor(C_ACCENT);
    canvas.setCursor(152, 45);
    canvas.printf("%g", (double)STEPS[stepIdx]);
    canvas.setTextSize(1);
    canvas.print(" mm");
    canvas.setTextColor(mr.st.nozzleTemp >= 170 ? C_OK : C_DIM);
    canvas.setCursor(152, 64);
    canvas.printf("NOZ %.0f/%.0f", mr.st.nozzleTemp, mr.st.nozzleTarget);

    canvas.setTextColor(C_DIM);
    canvas.setCursor(6, 84);
    canvas.print("[w/s]Y+/-  [a/d]X-/+  [;/.]Z+/-");
    canvas.setCursor(6, 96);
    canvas.print("[g]home  [m]motors off");
    canvas.setCursor(6, 108);
    canvas.print("[e]xtrude 5  [r]etract 5");

    drawLegend("[t]step  [0]help  [h/ESC]back");
    if (homeMenu) {
        static const char *items[4] = {"Home ALL", "Home X", "Home Y", "Home Z"};
        drawListMenu("HOME", items, 4, homeSel);
    }
}

void drawFiles() {
    canvas.fillScreen(C_BG);
    drawTopBar(3, connDotColor());

    if (fileDetail) {
        canvas.setTextSize(1);
        int used = drawWrapped(6, 17, SCR_W - 12, detailPath, C_ACCENT, 2);
        int y = 17 + used * 10 + 4;
        canvas.setTextColor(C_DIM);
        if (metaPending) {
            canvas.setCursor(6, y);
            canvas.print("reading metadata...");
        } else if (meta.valid) {
            canvas.setCursor(6, y);
            canvas.printf("est time  %s", meta.estTime > 0 ? fmtDur(meta.estTime).c_str() : "?");
            canvas.setCursor(6, y + 11);
            canvas.printf("filament  %s", meta.filamentMM > 0 ? (String(meta.filamentMM / 1000.0f, 1) + " m").c_str() : "?");
            canvas.setCursor(6, y + 22);
            canvas.printf("height    %s", meta.height > 0 ? (String(meta.height, 1) + " mm").c_str() : "?");
            canvas.setCursor(6, y + 33);
            if (meta.extTemp > 0) canvas.printf("temps     %.0f / %.0f", meta.extTemp, meta.bedTemp);
            canvas.setCursor(6, y + 44);
            if (meta.slicer.length()) canvas.printf("slicer    %s", meta.slicer.c_str());
        } else {
            canvas.setCursor(6, y);
            canvas.print("(no metadata)");
        }
        canvas.fillRoundRect(50, 106, 140, 15, 3, C_OK);
        canvas.setTextColor(TFT_BLACK);
        canvas.setCursor(74, 110);
        canvas.print("[ENT] PRINT");
        drawLegend("[ESC/h]back");
        return;
    }

    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(4, 16);
    if (!mr.filesLoaded) canvas.print("gcodes/  (loading...)");
    else                 canvas.printf("gcodes/  %d files (newest first)", (int)mr.files.size());

    const int ROWS = 8;
    if (fileSel < fileTop) fileTop = fileSel;
    if (fileSel >= fileTop + ROWS) fileTop = fileSel - ROWS + 1;
    for (int r = 0; r < ROWS; r++) {
        int idx = fileTop + r;
        if (idx >= (int)mr.files.size()) break;
        int y = 27 + r * 12;
        bool sel = idx == fileSel;
        if (sel) canvas.fillRoundRect(0, y - 2, SCR_W, 12, 2, C_PANEL);
        canvas.setTextColor(sel ? C_TEXT : C_DIM);
        String name = mr.files[idx].path;
        if (name.length() > 33) name = name.substring(0, 32) + "~";
        canvas.setCursor(4, y);
        canvas.print(name);
        canvas.setCursor(SCR_W - 34, y);
        canvas.print(fmtSize(mr.files[idx].sizeB));
    }
    drawLegend("[;.]nav [ENT]open [r]efresh [0]help");
}

void drawMacros() {
    canvas.fillScreen(C_BG);
    drawTopBar(4, connDotColor());
    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(4, 16);
    if (!mr.macrosLoaded) canvas.print("loading macros...");
    else                  canvas.printf("%d macros (underscore-prefixed hidden)", (int)mr.macros.size());

    const int ROWS = 8;
    if (macroSel < macroTop) macroTop = macroSel;
    if (macroSel >= macroTop + ROWS) macroTop = macroSel - ROWS + 1;
    for (int r = 0; r < ROWS; r++) {
        int idx = macroTop + r;
        if (idx >= (int)mr.macros.size()) break;
        int y = 27 + r * 12;
        bool sel = idx == macroSel;
        if (sel) canvas.fillRoundRect(0, y - 2, SCR_W, 12, 2, C_PANEL);
        canvas.setTextColor(sel ? C_ACCENT : C_DIM);
        canvas.setCursor(4, y);
        String name = mr.macros[idx];
        if (name.length() > 38) name = name.substring(0, 37) + "~";
        canvas.print(name);
    }
    drawLegend("[;.]nav [ENT]run [r]eload [0]help");
}

void drawLog() {
    canvas.fillScreen(C_BG);
    drawTopBar(5, connDotColor());
    static const uint16_t logPalette[4] = {C_DIM, C_ERR, C_ACCENT, C_OK};
    canvas.setTextSize(1);

    // log area: newest anchored at the bottom, long lines word-wrapped
    // across as many 39-char rows as they need (no more cut-off text)
    const int CPL = 39;
    const int inputY = SCR_H - 20;
    int y = inputY - 12;
    for (int i = mr.logCount - 1 - logScroll; i >= 0 && y >= 15; i--) {
        uint8_t c;
        const String &line = mr.logAt(i, c);
        int len = (int)line.length();
        int segs = max(1, (len + CPL - 1) / CPL);
        for (int s = segs - 1; s >= 0 && y >= 15; s--) {
            canvas.setTextColor(logPalette[c & 3]);
            canvas.setCursor(2, y);
            canvas.print(line.substring(s * CPL, min(len, (s + 1) * CPL)));
            y -= 9;
        }
    }
    if (logScroll > 0) {
        canvas.setTextColor(C_WARN);
        canvas.setCursor(SCR_W - 42, inputY - 12);
        canvas.printf("v %d", logScroll);
    }

    // command entry line
    canvas.drawFastHLine(0, inputY - 3, SCR_W, C_BORDER);
    canvas.setCursor(2, inputY);
    if (consoleInput) {
        canvas.setTextColor(C_ACCENT);
        canvas.print("> ");
        String shown = cmdBuf;
        const int maxc = 35;
        if ((int)shown.length() > maxc) shown = shown.substring(shown.length() - maxc);
        canvas.setTextColor(C_TEXT);
        canvas.print(shown);
        if ((millis() / 400) % 2) canvas.print("_");
    } else {
        canvas.setTextColor(C_FAINT);
        canvas.print("[ENT] type a command");
    }

    drawLegend(consoleInput ? "[ENT]send [ESC]exit [fn+up]recall last"
                            : "[;.]scroll [r]estart [x]stop [0]help");
    if (restartMenu) {
        static const char *items[2] = {"Firmware restart", "Klipper restart"};
        drawListMenu("RESTART", items, 2, restartSel);
    }
}

void drawHelp() {
    canvas.fillRoundRect(4, 2, SCR_W - 8, SCR_H - 4, 4, C_PANEL);
    canvas.drawRoundRect(4, 2, SCR_W - 8, SCR_H - 4, 4, C_ACCENT);
    canvas.setTextSize(1);
    // every description <= 29 chars: text starts at x=52 and the box's
    // inner edge is ~230px, so 52 + 29*6 = 226 is the hard fit limit —
    // longer rows wrap-smeared across the overlay on real hardware
    struct { const char *k, *d; } rows[] = {
        {"1-6",   "screens  7 objects  8 webcam"},
        {"h ESC", "back / close"},
        {";/.",   "navigate  ,// adjust (hold)"},
        {"DASH",  "p pause c cancel x e-stop"},
        {"Z",     "tune row babysteps z-offset"},
        {"TEMP",  "e type  0 off  ENT send"},
        {"MOVE",  "wasd+;. jog  t step  g home"},
        {"FILE",  "ENT print   MACR: ENT run"},
        {"OBJ",   "ENT exclude  CONS: ENT cmd"},
    };
    canvas.setTextColor(C_ACCENT);
    canvas.setCursor(12, 8);
    canvas.print("KEYS");
    canvas.setTextColor(C_FAINT);
    canvas.setCursor(160, 8);
    canvas.print("[0] help");
    for (int i = 0; i < 9; i++) {
        int y = 20 + i * 11;
        canvas.setTextColor(C_WARN);
        canvas.setCursor(12, y);
        canvas.print(rows[i].k);
        canvas.setTextColor(C_TEXT);
        canvas.setCursor(52, y);
        canvas.print(rows[i].d);
    }
    canvas.setTextColor(C_FAINT);
    canvas.setCursor(60, SCR_H - 14);
    canvas.print("press any key to close");
}

// exclude-object plate map — list left, scaled bed right (AD5M: 220x220)
void drawObjects() {
    canvas.fillScreen(C_BG);
    drawTopBar(-1, connDotColor());
    canvas.setTextSize(1);
    canvas.setTextColor(C_ACCENT);
    canvas.setCursor(4, 15);
    canvas.print("PRINT OBJECTS");

    if (!mr.objectsLoaded || mr.objects.empty()) {
        canvas.setTextColor(C_DIM);
        canvas.setCursor(8, 40);
        canvas.print("no object data");
        drawWrapped(8, 54, SCR_W - 16,
                    "objects appear once a job with exclude_object markers is printing",
                    C_FAINT, 3);
        drawLegend("[r]efresh  [ESC/h]back");
        return;
    }

    int n = (int)mr.objects.size();
    canvas.setTextColor(C_DIM);
    canvas.setCursor(100, 15);
    canvas.printf("%d obj  %d excl", n, (int)mr.st.excludedObjects.size());

    const int ROWS = 7;
    if (objSel < objTop) objTop = objSel;
    if (objSel >= objTop + ROWS) objTop = objSel - ROWS + 1;
    for (int r = 0; r < ROWS; r++) {
        int idx = objTop + r;
        if (idx >= n) break;
        int y = 27 + r * 13;
        bool sel = idx == objSel;
        bool exc = mr.st.isExcluded(mr.objects[idx].name);
        bool cur = mr.objects[idx].name == mr.st.currentObject && mr.st.currentObject.length();
        if (sel) canvas.fillRoundRect(0, y - 2, 130, 13, 2, C_PANEL);
        canvas.setTextColor(exc ? C_ERR : (cur ? C_OK : (sel ? C_TEXT : C_DIM)));
        canvas.setCursor(4, y);
        canvas.print(exc ? "X " : (cur ? "> " : "  "));
        String name = mr.objects[idx].name;
        if (name.length() > 19) name = name.substring(0, 18) + "~";
        canvas.print(name);
    }

    // bed map — y flipped so the bed front is at the bottom of the screen
    const int bx = 136, by = 17, bs = 96;
    canvas.drawRect(bx, by, bs, bs, C_BORDER);
    for (int i = 0; i < n; i++) {
        const ObjEntry &o = mr.objects[i];
        if (o.cx < 0) continue;
        int px = bx + (int)(constrain(o.cx, 0.0f, 220.0f) / 220.0f * (bs - 1));
        int py = by + bs - 1 - (int)(constrain(o.cy, 0.0f, 220.0f) / 220.0f * (bs - 1));
        bool exc = mr.st.isExcluded(o.name);
        bool cur = o.name == mr.st.currentObject && mr.st.currentObject.length();
        if (exc) {
            canvas.drawLine(px - 3, py - 3, px + 3, py + 3, C_ERR);
            canvas.drawLine(px - 3, py + 3, px + 3, py - 3, C_ERR);
        } else if (cur) {
            canvas.fillCircle(px, py, 3, C_OK);
        } else {
            canvas.drawCircle(px, py, 2, C_DIM);
        }
        if (i == objSel) canvas.drawCircle(px, py, 5, C_ACCENT);
    }
    canvas.setTextColor(C_FAINT);
    canvas.setCursor(bx + 20, by + bs + 2);
    canvas.print("bed 220x220");

    drawLegend("[;.]sel [ENT]exclude [r]efresh");
}

void drawWebcam() {
    // no fillScreen — the canvas keeps the last decoded video frame;
    // camLoop() paints new frames in as they finish decoding
    if (!camFrameShown) {
        canvas.fillScreen(C_BG);
        canvas.setTextSize(1);
        canvas.setTextColor(camClient.connected() ? C_DIM : C_ERR);
        canvas.setCursor(30, 55);
        canvas.print(camClient.connected() ? "waiting for first frame..." : "stream not connected");
        canvas.setTextColor(C_FAINT);
        canvas.setCursor(30, 70);
        canvas.printf("%s:%d%s", MOONRAKER_HOST, WEBCAM_PORT, WEBCAM_PATH);
    }
    drawTopBar(-1, connDotColor());
    canvas.fillRect(0, SCR_H - 10, SCR_W, 10, C_BAR);
    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(2, SCR_H - 9);
    if (camClient.connected() && camFrameShown)
        canvas.printf("WEBCAM  %d.%d fps   [ESC/h]back", camFps10 / 10, camFps10 % 10);
    else if (camClient.connected())
        canvas.print("WEBCAM  connecting...   [ESC/h]back");
    else
        canvas.print("WEBCAM  no stream   [r]etry [ESC]back");
}

void drawLoadingFrame(const char *msg) {
    // immediate feedback frame pushed outside the normal draw cadence,
    // right before a blocking HTTP fetch
    canvas.fillRoundRect(40, 55, 160, 24, 4, C_PANEL);
    canvas.drawRoundRect(40, 55, 160, 24, 4, C_ACCENT);
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT);
    canvas.setCursor(52, 63);
    canvas.print(msg);
    canvas.pushSprite(0, 0);
}

void drawCurrent() {
    switch (screen) {
        case Screen::DASH:    drawDash(); break;
        case Screen::TEMP:    drawTemp(); break;
        case Screen::MOVE:    drawMove(); break;
        case Screen::FILES:   drawFiles(); break;
        case Screen::MACROS:  drawMacros(); break;
        case Screen::LOG:     drawLog(); break;
        case Screen::OBJECTS: drawObjects(); break;
        case Screen::WEBCAM:  drawWebcam(); break;
    }
    // Overlays composite into the SAME frame, then ONE pushSprite per
    // frame. v1.2's screens pushed first and overlays pushed a second
    // frame, so the toast alternated with a toast-less frame every 50ms
    // tick — that was the rapid flicker reported from hardware.
    if (helpActive) {
        drawHelp();
    } else {
        if (millis() < toastUntil && !modal.active) drawToast();
        if (modal.active) drawConfirmBox(modal.l1, modal.l2);
    }
    canvas.pushSprite(0, 0);
}

// ═══ input ═══════════════════════════════════════════════════════════════

void screenEnter() {
    switch (screen) {
        case Screen::TEMP:
            requestTempSend(tempSel);
            break;
        case Screen::OBJECTS:
            if (mr.objectsLoaded && objSel < (int)mr.objects.size()) {
                const String &name = mr.objects[objSel].name;
                if (!mr.st.isActive()) showToast("no active print", C_WARN, 2000);
                else if (mr.st.isExcluded(name)) showToast("already excluded", C_WARN, 2000);
                else openConfirm(Act::EXCLUDE_OBJ, "EXCLUDE THIS OBJECT?",
                                 name + " - skipped for the rest of the job", name);
            }
            break;
        case Screen::MOVE:
            if (homeMenu) { doHome(homeSel); homeMenu = false; }
            break;
        case Screen::FILES:
            if (!fileDetail) {
                if (fileSel < (int)mr.files.size()) {
                    detailPath = mr.files[fileSel].path;
                    fileDetail = true;
                    meta = FileMeta();
                    metaPending = true;
                }
            } else {
                openConfirm(Act::START_PRINT, "START THIS PRINT?", detailPath, detailPath);
            }
            break;
        case Screen::MACROS:
            if (macroSel < (int)mr.macros.size()) {
                mr.sendGcode(mr.macros[macroSel], true, /*track=*/true);
                if (mr.gcodeBusy) {
                    // Moonraker replies to gcode.script only once the script
                    // has finished, so this toast sits (with animated dots)
                    // until the real ok/fail lands — capped at 5 min
                    showToast("running " + mr.macros[macroSel], C_ACCENT, 300000);
                    chirpAck();
                } else {
                    showToast("SEND FAILED - not connected", C_ERR, 3000);
                    chirpWarn();
                }
            }
            break;
        case Screen::LOG:
            if (restartMenu) {
                restartMenu = false;
                if (restartSel == 0) openConfirm(Act::FW_RESTART, "FIRMWARE RESTART?", "restarts MCU + klippy");
                else                 openConfirm(Act::HOST_RESTART, "KLIPPER RESTART?", "reloads klippy config");
            } else {
                consoleInput = true;   // open the command line
            }
            break;
        default:
            break;
    }
}

void handleDashKeys(char key) {
    bool active = mr.st.isActive();
    switch (key) {
        case 'p':
            // both directions confirmed — an accidental pause mid-print or
            // an accidental resume of a deliberately-paused job both hurt
            if (mr.st.printState == "printing")
                openConfirm(Act::PAUSE, "PAUSE THIS PRINT?", mr.st.filename);
            else if (mr.st.printState == "paused")
                openConfirm(Act::RESUME, "RESUME PRINT?", mr.st.filename);
            break;
        case 'c':
            if (active) openConfirm(Act::CANCEL_PRINT, "CANCEL THIS PRINT?", mr.st.filename);
            break;
        case 'x':
            openConfirm(Act::ESTOP, "EMERGENCY STOP?", "klipper will shut down");
            break;
        case ';': if (active) tuneSel = (tuneSel + 3) % 4; break;
        case '.': if (active) tuneSel = (tuneSel + 1) % 4; break;
    }
}

void handleObjectsKeys(char key) {
    int n = (int)mr.objects.size();
    switch (key) {
        case ';': if (n) objSel = (objSel + n - 1) % n; break;
        case '.': if (n) objSel = (objSel + 1) % n; break;
        case 'r': objectsPending = true; break;
    }
}

void handleTempKeys(char key) {
    switch (key) {
        case ';': tempSel = 1 - tempSel; break;
        case '.': tempSel = 1 - tempSel; break;
        case 'e': startEdit(&pendTemp[tempSel], 0, tempSel ? 110 : 300, &tempDirty[tempSel]); break;
        case '0':
            pendTemp[tempSel] = 0;
            tempDirty[tempSel] = true;
            requestTempSend(tempSel);   // heater-off mid-print goes through the confirm too
            break;
        case 'p':
            if (mr.st.printState == "printing") {
                showToast("presets locked while printing", C_WARN, 2500);
                chirpWarn();
            } else {
                applyPreset();
            }
            break;
    }
}

void handleMoveKeys(char key) {
    if (homeMenu) {
        if (key == ';') homeSel = (homeSel + 3) % 4;
        if (key == '.') homeSel = (homeSel + 1) % 4;
        return;
    }
    float st = STEPS[stepIdx];
    switch (key) {
        case 'w': jog('Y', st); break;
        case 's': jog('Y', -st); break;
        case 'a': jog('X', -st); break;
        case 'd': jog('X', st); break;
        case ';': jog('Z', st); break;
        case '.': jog('Z', -st); break;
        case 't': stepIdx = (stepIdx + 1) % 4; break;
        case 'g': homeMenu = true; homeSel = 0; break;
        case 'm': mr.sendGcode("M84"); break;
        case 'e': extrude(5.0f); break;
        case 'r': extrude(-5.0f); break;
    }
}

void handleFilesKeys(char key) {
    if (fileDetail) return;   // detail view: only ENT/ESC matter
    int n = (int)mr.files.size();
    switch (key) {
        case ';': if (n) fileSel = (fileSel + n - 1) % n; break;
        case '.': if (n) fileSel = (fileSel + 1) % n; break;
        case 'r': filesPending = true; break;
    }
}

void handleMacrosKeys(char key) {
    int n = (int)mr.macros.size();
    switch (key) {
        case ';': if (n) macroSel = (macroSel + n - 1) % n; break;
        case '.': if (n) macroSel = (macroSel + 1) % n; break;
        case 'r': if (mr.wsUp) { mr.macrosLoaded = false; mr.requestMacros(); } break;
    }
}

void handleLogKeys(char key) {
    if (restartMenu) {
        if (key == ';' || key == '.') restartSel = 1 - restartSel;
        return;
    }
    switch (key) {
        case ';': if (logScroll < mr.logCount - 1) logScroll++; break;   // older
        case '.': if (logScroll > 0) logScroll--; break;                 // newer
        case 'r': restartMenu = true; restartSel = 0; break;
        case 'x': openConfirm(Act::ESTOP, "EMERGENCY STOP?", "klipper will shut down"); break;
        case 'b': M5Cardputer.Speaker.tone(2000, 200); break;
    }
}

void handleKeys() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto state = M5Cardputer.Keyboard.keysState();

    if (editingField) { handleEditKeys(state); return; }
    if (consoleInput && screen == Screen::LOG) { handleConsoleKeys(state); return; }
    if (helpActive) { helpActive = false; return; }   // any key closes help

    for (auto key : state.word) {
        if (modal.active) {
            if (key == 'h' || key == 'H') modal.active = false;
            continue;
        }
        if (key == 'h' || key == 'H') { goBack(); continue; }
        // number keys jump between screens from anywhere (except mid-edit;
        // '0' stays free as the TEMP heater-off key). 7 = objects, 8 = cam.
        if (key >= '1' && key <= '8') {
            switchScreen((Screen)(key - '1'));
            continue;
        }
        // help overlay — everywhere except TEMP, where 0 = heater off
        if (key == '0' && screen != Screen::TEMP) { helpActive = true; continue; }
        switch (screen) {
            case Screen::DASH:    handleDashKeys(key); break;
            case Screen::TEMP:    handleTempKeys(key); break;
            case Screen::MOVE:    handleMoveKeys(key); break;
            case Screen::FILES:   handleFilesKeys(key); break;
            case Screen::MACROS:  handleMacrosKeys(key); break;
            case Screen::LOG:     handleLogKeys(key); break;
            case Screen::OBJECTS: handleObjectsKeys(key); break;
            case Screen::WEBCAM:
                if (key == 'r' && !camStart()) showToast("webcam connect failed", C_ERR, 3000);
                break;
        }
    }

    for (auto key : state.hid_keys) {
        if (key == 0) continue;   // keysState() pads unused slots with 0
        if (key == 0x28) {        // Enter
            if (modal.active) execConfirm();
            else screenEnter();
        }
        if (key == 0x29 || key == 0x35) {   // ESC — 0x35 is this keyboard's real code
            goBack();
        }
    }
}

// ,// adjustments — polled every loop() iteration (NOT gated on
// isChange()) so press-and-hold ramps. Active on TEMP (pending targets)
// and DASH (tune row, only while a job is active).
void handleAdjustRepeat() {
    if (editingField || modal.active || consoleInput || helpActive) { adjRepeater.activeKey = 0; return; }
    bool dashTune = (screen == Screen::DASH && mr.st.isActive());
    if (screen != Screen::TEMP && !dashTune) { adjRepeater.activeKey = 0; return; }

    auto state = M5Cardputer.Keyboard.keysState();
    bool dec = false, inc = false;
    for (auto key : state.word) {
        if (key == ',') dec = true;
        if (key == '/') inc = true;
    }
    int dir = adjRepeater.poll(dec, inc);
    if (dir == 0) return;

    if (screen == Screen::TEMP) {
        float maxV = tempSel ? 110 : 300;
        pendTemp[tempSel] = constrain(pendTemp[tempSel] + dir, 0.0f, maxV);
        tempDirty[tempSel] = true;
    } else if (tuneSel == 3) {
        // Z babystep — 0.005mm per step, accumulated then sent as one
        // SET_GCODE_OFFSET Z_ADJUST after the debounce window
        zAdjPending += dir * 0.005f;
        zDirty = true;
        lastTuneAdjMs = millis();
    } else {
        float lo = (tuneSel == 2) ? 0 : 10;
        float hi = (tuneSel == 2) ? 100 : 300;
        pendTune[tuneSel] = constrain(pendTune[tuneSel] + dir, lo, hi);
        tuneDirty[tuneSel] = true;
        lastTuneAdjMs = millis();
    }
}

// ═══ boot splash — nozzle lays the title down like a first layer ═════════

void splash() {
    const char *title = "KLIPPUTER";
    const int n = strlen(title);
    const int cw = 22;                       // char cell at size 3 (18px glyph + gap)
    const int x0 = (SCR_W - n * cw) / 2;
    const int baseY = 52;

    for (int i = 0; i <= n; i++) {
        canvas.fillScreen(C_BG);
        for (int gx = 0; gx < SCR_W; gx += 16) canvas.drawFastVLine(gx, 0, SCR_H, 0x0861);
        for (int gy = 0; gy < SCR_H; gy += 16) canvas.drawFastHLine(0, gy, SCR_W, 0x0861);

        canvas.setTextSize(3);
        canvas.setTextColor(C_ACCENT);
        for (int j = 0; j < i && j < n; j++) {
            canvas.setCursor(x0 + j * cw, baseY);
            canvas.print(title[j]);
        }
        // deposited "first layer" under the printed letters
        if (i > 0) canvas.fillRect(x0, baseY + 26, min(i, n) * cw - 4, 3, C_NOZ);

        if (i < n) {
            // nozzle over the next letter: body, tip, filament drip
            int nx = x0 + i * cw + 8;
            canvas.fillRect(nx - 7, 18, 14, 10, C_DIM);
            canvas.fillTriangle(nx - 5, 28, nx + 5, 28, nx, 36, C_NOZ);
            canvas.drawFastVLine(nx, 36, baseY - 36 + 20, C_NOZ_DIM);
        }
        canvas.pushSprite(0, 0);
        M5Cardputer.Speaker.tone(600 + i * 90, 18);
        delay(95);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(x0 + 4, baseY + 36);
    canvas.print("pocket klipper console  v1.0");
    canvas.setTextColor(C_FAINT);
    canvas.setCursor(x0 + 4, baseY + 50);
    canvas.printf("wifi: %s ...", WIFI_SSID);
    canvas.pushSprite(0, 0);
    delay(900);
}

// ═══ setup / loop ════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(80);

    canvas.setColorDepth(16);
    canvas.createSprite(SCR_W, SCR_H);
    canvas.setTextWrap(false);   // overflow clips cleanly instead of wrap-smearing other rows

    M5Cardputer.Speaker.setVolume(128);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);   // connects in the background while the splash runs

    splash();

    mr.begin(MOONRAKER_HOST, MOONRAKER_PORT);
    mr.pushLog("[boot] klipputer v1.3", 3);

    // first thing a new user needs to know, right on the dashboard
    showToast("keys 1-8 = screens, 0 = help", C_WARN, 7000);
}

void loop() {
    M5Cardputer.update();
    mr.loop();
    handleKeys();
    handleAdjustRepeat();
    tickBeep();
    syncPendings();
    flushTune();

    // print-complete / error melodies
    if (mr.evComplete) { mr.evComplete = false; chirpComplete(); }
    if (mr.evError)    { mr.evError = false; chirpError(); }

    // tracked-gcode (macro) completion feedback
    if (mr.evGcodeOk) {
        mr.evGcodeOk = false;
        showToast("OK: " + mr.evGcodeLabel, C_OK, 2500);
        chirpAck();
    }
    if (mr.evGcodeFail) {
        mr.evGcodeFail = false;
        showToast("FAILED: " + mr.evGcodeLabel, C_ERR, 4000);
        chirpWarn();
    }

    // temp history sampling for the graph
    if (millis() - lastHistMs >= 1000) {
        lastHistMs = millis();
        mr.st.pushHist();
    }

    // queued blocking HTTP fetches — draw a loading frame first so the
    // screen doesn't just freeze silently
    if (filesPending && screen == Screen::FILES && WiFi.status() == WL_CONNECTED) {
        filesPending = false;
        drawLoadingFrame("loading file list...");
        mr.fetchFiles();
        fileSel = fileTop = 0;
    }
    if (metaPending && screen == Screen::FILES && fileDetail && WiFi.status() == WL_CONNECTED) {
        metaPending = false;
        drawLoadingFrame("reading metadata...");
        meta = mr.fetchMeta(detailPath);
    }
    if (objectsPending && screen == Screen::OBJECTS && WiFi.status() == WL_CONNECTED) {
        objectsPending = false;
        drawLoadingFrame("querying objects...");
        mr.fetchObjects();
        objSel = objTop = 0;
    }

    if (screen == Screen::WEBCAM) camLoop();

    if (millis() - lastDrawMs >= 50) {
        lastDrawMs = millis();
        drawCurrent();
    }
}
