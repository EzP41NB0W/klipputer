// ui.h — Klipputer palette + shared widgets.
// All drawing goes through the global M5Canvas (defined in main.cpp) —
// same offscreen double-buffer pattern as radar/range-toolkit/cardmeleon.
#pragma once

#include <M5Cardputer.h>
#include <WiFi.h>

#define SCR_W 240
#define SCR_H 135

extern M5Canvas canvas;

// ── palette (RGB565) ─────────────────────────────────────────────────────
#define C_BG      TFT_BLACK
#define C_BAR     0x0841   // topbar background
#define C_PANEL   0x10A2   // panel/tile fill
#define C_BORDER  0x39C7
#define C_GRID    0x18C3
#define C_TEXT    0xCE79
#define C_DIM     0x8410
#define C_FAINT   0x4208
#define C_ACCENT  0x07FF   // cyan
#define C_NOZ     0xFD20   // amber/orange — nozzle
#define C_NOZ_DIM 0x7A80
#define C_BED     0x34DF   // light blue — bed
#define C_BED_DIM 0x1A6F
#define C_OK      0x07E0
#define C_WARN    0xFFE0
#define C_ERR     0xF800

// ── formatting ──────────────────────────────────────────────────────────
inline String fmtDur(float secs) {
    if (secs < 0) return "--:--";
    int t = (int)secs;
    char b[16];
    if (t >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else           snprintf(b, sizeof(b), "%02d:%02d", t / 60, t % 60);
    return String(b);
}

inline String fmtSize(uint32_t bytes) {
    char b[16];
    if (bytes >= 1048576UL) snprintf(b, sizeof(b), "%.1fM", bytes / 1048576.0f);
    else                    snprintf(b, sizeof(b), "%luK", (unsigned long)((bytes + 1023) / 1024));
    return String(b);
}

// ── widgets ─────────────────────────────────────────────────────────────

// Tab strip + connection dot + WiFi bars + battery %. dotColor: green =
// WS up + klippy ready, yellow = WS only, red = no WS.
inline void drawTopBar(int active, uint16_t dotColor) {
    canvas.fillRect(0, 0, SCR_W, 13, C_BAR);
    // each tab shows its jump key: press 1-6 anywhere to switch screens
    static const char *tabs[6] = {"DSH", "TMP", "MOV", "FIL", "MAC", "CON"};
    canvas.setTextSize(1);
    int x = 1;
    for (int i = 0; i < 6; i++) {
        int w = 4 * 6 + 6;
        if (i == active) {
            canvas.fillRoundRect(x, 1, w, 11, 2, C_ACCENT);
            canvas.setTextColor(TFT_BLACK);
        } else {
            canvas.setTextColor(C_TEXT);
        }
        canvas.setCursor(x + 3, 3);
        canvas.print((char)('1' + i));
        if (i != active) canvas.setTextColor(C_DIM);
        canvas.setCursor(x + 9, 3);
        canvas.print(tabs[i]);
        x += w + 1;
    }
    canvas.fillCircle(x + 4, 6, 3, dotColor);

    int bx = x + 10;
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : rssi > -90 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 2;
        canvas.fillRect(bx + i * 4, 11 - bh, 3, bh, i < bars ? C_OK : C_FAINT);
    }

    int batt = M5.Power.getBatteryLevel();   // -1 if unknown — just skip
    if (batt >= 0) {
        canvas.setTextColor(batt < 20 ? C_ERR : C_DIM);
        canvas.setCursor(SCR_W - 28, 3);
        canvas.printf("%d%%", batt);
    }
}

inline void drawProgressBar(int x, int y, int w, int h, float frac, uint16_t color) {
    frac = constrain(frac, 0.0f, 1.0f);
    canvas.drawRoundRect(x, y, w, h, 3, C_BORDER);
    int fw = (int)((w - 4) * frac);
    if (fw > 2) canvas.fillRoundRect(x + 2, y + 2, fw, h - 4, 2, color);
    char b[8];
    snprintf(b, sizeof(b), "%.0f%%", frac * 100.0f);
    int tw = strlen(b) * 6;
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT);
    canvas.setCursor(x + (w - tw) / 2, y + (h - 8) / 2 + 1);
    canvas.print(b);
}

// Big heater tile: label, current temp (large), /target, heater-power bar.
inline void drawTempTile(int x, int y, int w, int h, const char *label,
                         float cur, float tgt, float power, uint16_t color) {
    canvas.fillRoundRect(x, y, w, h, 3, C_PANEL);
    canvas.drawRoundRect(x, y, w, h, 3, C_BORDER);
    canvas.setTextSize(1);
    canvas.setTextColor(C_DIM);
    canvas.setCursor(x + 5, y + 3);
    canvas.print(label);
    canvas.setTextSize(2);
    canvas.setTextColor(color);
    canvas.setCursor(x + 5, y + 12);
    canvas.printf("%3.0f", cur);
    canvas.setTextSize(1);
    canvas.setTextColor(tgt > 0 ? C_TEXT : C_FAINT);
    canvas.setCursor(x + 48, y + 19);
    canvas.printf("/%3.0fC", tgt);
    int bw = (int)((w - 10) * constrain(power, 0.0f, 1.0f));
    canvas.fillRect(x + 5, y + h - 5, w - 10, 2, C_FAINT);
    if (bw > 0) canvas.fillRect(x + 5, y + h - 5, bw, 2, color);
}

// Scrolling text when it doesn't fit (character-rotation marquee).
inline void drawMarquee(int x, int y, int w, const String &text, uint16_t color) {
    canvas.setTextSize(1);
    canvas.setTextColor(color);
    int chars = w / 6;
    if ((int)text.length() <= chars) {
        canvas.setCursor(x, y);
        canvas.print(text);
        return;
    }
    String s = text + "   ";
    int off = (int)((millis() / 250) % s.length());
    char buf[48];
    int n = min(chars, 47);
    for (int i = 0; i < n; i++) buf[i] = s[(off + i) % s.length()];
    buf[n] = 0;
    canvas.setCursor(x, y);
    canvas.print(buf);
}

// Naive width-based word wrap; returns lines drawn.
inline int drawWrapped(int x, int y, int w, const String &text, uint16_t color, int maxLines) {
    int cpl = w / 6;
    if (cpl < 4) return 0;
    canvas.setTextSize(1);
    canvas.setTextColor(color);
    int line = 0, pos = 0;
    while (pos < (int)text.length() && line < maxLines) {
        int end = min((int)text.length(), pos + cpl);
        String seg = text.substring(pos, end);
        int nl = seg.indexOf('\n');
        if (nl >= 0) { seg = seg.substring(0, nl); pos += nl + 1; }
        else pos = end;
        canvas.setCursor(x, y + line * 10);
        canvas.print(seg);
        line++;
    }
    return line;
}

// Centered yes/no confirmation box (drawn over the current screen).
// The detail line word-wraps to two lines instead of truncating.
inline void drawConfirmBox(const String &l1, const String &l2) {
    const int CPL = 32;
    String a = l2, b = "";
    if ((int)l2.length() > CPL) {
        int cut = CPL;
        for (int i = CPL; i > 12; i--) if (l2.charAt(i) == ' ') { cut = i; break; }
        a = l2.substring(0, cut);
        b = l2.substring(l2.charAt(cut) == ' ' ? cut + 1 : cut);
        if ((int)b.length() > CPL) b = b.substring(0, CPL - 2) + "..";
    }
    int x = 14, w = SCR_W - 28;
    int h = b.length() ? 78 : 66;
    int y = (SCR_H - h) / 2;
    canvas.fillRoundRect(x, y, w, h, 4, C_PANEL);
    canvas.drawRoundRect(x, y, w, h, 4, C_WARN);
    canvas.setTextSize(1);
    canvas.setTextColor(C_WARN);
    String t1 = l1.substring(0, CPL);
    canvas.setCursor(x + (w - (int)t1.length() * 6) / 2, y + 10);
    canvas.print(t1);
    canvas.setTextColor(C_DIM);
    if (a.length()) {
        canvas.setCursor(x + (w - (int)a.length() * 6) / 2, y + 26);
        canvas.print(a);
    }
    if (b.length()) {
        canvas.setCursor(x + (w - (int)b.length() * 6) / 2, y + 36);
        canvas.print(b);
    }
    canvas.setTextColor(C_TEXT);
    canvas.setCursor(x + (w - 20 * 6) / 2, y + h - 16);
    canvas.print("[ENT]yes  [ESC/h]no");
}

// Centered selection-list box (home menu, restart menu).
inline void drawListMenu(const char *title, const char *const *items, int n, int sel) {
    int w = 150, h = n * 14 + 30;
    int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    canvas.fillRoundRect(x, y, w, h, 4, C_PANEL);
    canvas.drawRoundRect(x, y, w, h, 4, C_ACCENT);
    canvas.setTextSize(1);
    canvas.setTextColor(C_ACCENT);
    canvas.setCursor(x + 6, y + 5);
    canvas.print(title);
    for (int i = 0; i < n; i++) {
        int iy = y + 18 + i * 14;
        if (i == sel) canvas.fillRoundRect(x + 4, iy - 2, w - 8, 13, 2, C_FAINT);
        canvas.setTextColor(i == sel ? C_TEXT : C_DIM);
        canvas.setCursor(x + 10, iy);
        canvas.print(items[i]);
    }
}

// Dual-line temperature history graph with dashed target lines.
inline void drawTempGraph(int x, int y, int w, int h, const PrinterState &st) {
    canvas.fillRect(x, y, w, h, C_BG);
    canvas.drawRect(x, y, w, h, C_BORDER);

    float maxT = 120;
    for (int i = 0; i < st.histCount; i++) {
        maxT = max(maxT, st.histAt(st.nozzleHist, i));
        maxT = max(maxT, st.histAt(st.bedHist, i));
    }
    maxT = max(maxT, max(st.nozzleTarget, st.bedTarget));
    maxT *= 1.12f;

    for (int g = 50; g < (int)maxT; g += 50) {
        int gy = y + h - 2 - (int)((g / maxT) * (h - 3));
        canvas.drawFastHLine(x + 1, gy, w - 2, C_GRID);
    }
    auto dashed = [&](float t, uint16_t c) {
        if (t <= 0) return;
        int ty = y + h - 2 - (int)((t / maxT) * (h - 3));
        for (int dx = x + 1; dx < x + w - 3; dx += 6) canvas.drawFastHLine(dx, ty, 3, c);
    };
    dashed(st.bedTarget, C_BED_DIM);
    dashed(st.nozzleTarget, C_NOZ_DIM);

    auto plot = [&](const float *buf, uint16_t c) {
        if (st.histCount < 2) return;
        for (int i = 1; i < st.histCount; i++) {
            int x0 = x + 1 + ((i - 1) * (w - 3)) / (PrinterState::HIST - 1);
            int x1 = x + 1 + (i * (w - 3)) / (PrinterState::HIST - 1);
            float v0 = st.histAt(buf, i - 1), v1 = st.histAt(buf, i);
            int y0 = y + h - 2 - (int)((constrain(v0, 0.0f, maxT) / maxT) * (h - 3));
            int y1 = y + h - 2 - (int)((constrain(v1, 0.0f, maxT) / maxT) * (h - 3));
            canvas.drawLine(x0, y0, x1, y1, c);
        }
    };
    plot(st.bedHist, C_BED);
    plot(st.nozzleHist, C_NOZ);

    canvas.setTextSize(1);
    canvas.setTextColor(C_NOZ);
    canvas.setCursor(x + w - 76, y + 3);
    canvas.printf("N%3.0f", st.nozzleTemp);
    canvas.setTextColor(C_BED);
    canvas.setCursor(x + w - 40, y + 3);
    canvas.printf("B%3.0f", st.bedTemp);
}
