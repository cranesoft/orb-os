// The Big Orb's knob is the glass.
//
// The Waveshare ESP32-P4 3.4C has no encoder, and the whole UI is built around one: turn,
// press, the Rock (a quick back-and-forth that opens the app menu) and an 8 s hold that
// forces a recovery reboot. Rather than teach every screen about touch, this file speaks
// the knob:: API from the touch panel, so app_shell, input_router and every view see the
// same events a 466 Orb's knob produces.
//
//   circle a finger around the dial   one detent per DETENT_DEG of arc, clockwise = right
//   tap                               press
//   hold still MENU_HOLD_MS           the Rock: opens the app menu
//   hold still LONG_PRESS_MS          long press: recovery reboot, as on the knob
//
// The panel is a GT9271 speaking the GT911 protocol, polled over I2C because its INT line
// is not routed to the P4. knob.cpp (the real encoder) is left out of this build.
#include "config.h"

#if defined(ORB_BOARD_P4_34C)

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "knob.h"
#include "display.h"   // orb_log_quiet()

namespace {

constexpr uint32_t POLL_MS        = 15;     // ~66 Hz: smooth enough to track a circling finger
constexpr float    DETENT_DEG     = 12.0f;  // 30 detents per lap, close to a KY-040's feel
constexpr float    RING_MIN_FRAC  = 0.30f;  // ignore angle changes nearer the centre than this
constexpr int      TAP_SLOP_PX    = ORB_PX(18);   // movement that still counts as a tap
constexpr uint32_t TAP_MAX_MS     = 450;
constexpr uint32_t MENU_HOLD_MS   = 600;
constexpr uint32_t LONG_PRESS_MS  = 8000;   // same as knob.cpp

// GT911 registers (16-bit, big-endian on the wire).
constexpr uint16_t GT_REG_STATUS  = 0x814E;   // bit7 = buffer ready, low nibble = points
constexpr uint16_t GT_REG_POINT1  = 0x814F;   // 8 bytes per point: id, xl, xh, yl, yh, sl, sh, -
constexpr uint16_t GT_REG_PRODUCT = 0x8140;

uint8_t  s_addr = 0;          // 0 = no controller found

// Knob state, the same shapes knob.cpp keeps.
int32_t  s_detent       = 0;
int32_t  s_pendingDelta = 0;
bool     s_pendingPress = false;
bool     s_pendingLong  = false;
uint32_t s_rockMs       = 0;
uint32_t s_rockGapMs    = 0;
int32_t  s_rockDetent   = 0;

// The current touch.
bool     s_down      = false;
uint32_t s_downMs    = 0;
int      s_x0 = 0, s_y0 = 0;        // where it landed
float    s_lastAng   = 0.0f;        // degrees, of the last sample that was on the ring
bool     s_angValid  = false;
float    s_angAccum  = 0.0f;        // arc travelled toward the next detent
bool     s_moved     = false;       // past the tap slop, or turned a detent
bool     s_menuFired = false;
bool     s_longFired = false;
uint32_t s_lastPollMs = 0;

bool gt_read(uint16_t reg, uint8_t *buf, size_t n) {
    Wire.beginTransmission(s_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)s_addr, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)Wire.read();
    return true;
}

void gt_write8(uint16_t reg, uint8_t v) {
    Wire.beginTransmission(s_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(v);
    Wire.endTransmission();
}

bool gt_probe(uint8_t addr) {
    s_addr = addr;
    uint8_t id[4] = {0};
    if (!gt_read(GT_REG_PRODUCT, id, 4)) { s_addr = 0; return false; }
    Serial.printf("[touch] GT%c%c%c%c at 0x%02X\n", id[0], id[1], id[2], id[3], addr);
    return true;
}

// One sample: true with a position while a finger is down.
bool gt_sample(int &x, int &y) {
    uint8_t st = 0;
    if (!gt_read(GT_REG_STATUS, &st, 1)) return s_down;   // a missed read changes nothing
    if (!(st & 0x80)) return s_down;                      // no new frame yet
    const uint8_t n = st & 0x0F;
    bool have = false;
    if (n > 0 && n <= 10) {
        uint8_t p[8];
        if (gt_read(GT_REG_POINT1, p, sizeof p)) {
            x = (int)(p[1] | (p[2] << 8));
            y = (int)(p[3] | (p[4] << 8));
            have = true;
        }
    }
    gt_write8(GT_REG_STATUS, 0);                          // hand the buffer back
    return have;
}

void on_detent(int dir) {
    s_detent += dir;
    s_pendingDelta += dir;
    display::markInput(millis());
    if (!orb_log_quiet())
        Serial.printf("[knob] turned %s  (pos=%ld)  [touch]\n",
                      dir > 0 ? "RIGHT (CW)" : "LEFT (CCW)", (long)s_detent);
}

void touch_down(int x, int y, uint32_t now) {
    s_down = true;
    s_downMs = now;
    s_x0 = x; s_y0 = y;
    s_angValid = false;
    s_angAccum = 0.0f;
    s_moved = false;
    s_menuFired = false;
    s_longFired = false;
    display::noteActivity();
}

void touch_move(int x, int y, uint32_t now) {
    (void)now;
    const int dx0 = x - s_x0, dy0 = y - s_y0;
    if (!s_moved && dx0 * dx0 + dy0 * dy0 > TAP_SLOP_PX * TAP_SLOP_PX) s_moved = true;

    // Angle about the dial centre. Screen y grows downward, so atan2(dx, -dy) is 0 at
    // twelve o'clock and increases clockwise, which is the knob's "right".
    const float rx = (float)(x - SCREEN_CX), ry = (float)(y - SCREEN_CY);
    const float r = sqrtf(rx * rx + ry * ry);
    if (r < RING_MIN_FRAC * SCREEN_CX) { s_angValid = false; return; }   // too near the hub to steer
    const float ang = atan2f(rx, -ry) * (180.0f / (float)M_PI);
    if (s_angValid) {
        float d = ang - s_lastAng;
        if (d > 180.0f)  d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        s_angAccum += d;
        while (s_angAccum >=  DETENT_DEG) { s_angAccum -= DETENT_DEG; on_detent(+1); s_moved = true; }
        while (s_angAccum <= -DETENT_DEG) { s_angAccum += DETENT_DEG; on_detent(-1); s_moved = true; }
    }
    s_lastAng = ang;
    s_angValid = true;
}

void touch_up(uint32_t now) {
    s_down = false;
    if (!s_moved && !s_menuFired && !s_longFired && (now - s_downMs) <= TAP_MAX_MS) {
        s_pendingPress = true;
        display::markInput(now);
        Serial.println("[knob] press  [touch tap]");
    }
}

// Held, still: first the menu, much later the recovery reboot.
void touch_hold(uint32_t now) {
    if (s_moved) return;
    const uint32_t held = now - s_downMs;
    if (!s_menuFired && held >= MENU_HOLD_MS) {
        // Recorded exactly the way the encoder records a Rock, so input_router's settle
        // and run-back checks apply unchanged: a fast reversal, no detents since.
        s_menuFired  = true;
        s_rockGapMs  = 100;
        s_rockDetent = s_detent;
        s_rockMs     = now ? now : 1;
        Serial.println("[knob] rock  [touch hold]");
    }
    if (!s_longFired && held >= LONG_PRESS_MS) {
        s_longFired = true;
        s_pendingLong = true;
        Serial.println("[knob] long-press (reboot)  [touch hold]");
    }
}

}  // namespace

void knob::begin() {
    // The I2C bus is already up: imu_begin() (board_stubs_p4.cpp) starts it, as on the Orb.
    if (!gt_probe(I2C_ADDR_TOUCH) && !gt_probe(I2C_ADDR_TOUCH_ALT))
        Serial.println("[touch] no GT911-family controller at 0x5D or 0x14: no input");
    else
        Serial.println("[knob] touch knob ready: circle to turn, tap to press, hold for the menu");
}

void knob::poll() {
    if (!s_addr) return;
    const uint32_t now = millis();
    if (now - s_lastPollMs < POLL_MS) {
        if (s_down) touch_hold(now);
        return;
    }
    s_lastPollMs = now;
    int x = 0, y = 0;
    const bool pressed = gt_sample(x, y);
    if (pressed && !s_down)      touch_down(x, y, now);
    else if (pressed && s_down)  touch_move(x, y, now);
    else if (!pressed && s_down) touch_up(now);
    if (s_down) touch_hold(now);
}

int32_t knob::takeDelta()      { const int32_t d = s_pendingDelta; s_pendingDelta = 0; return d; }
bool    knob::takePress()      { const bool p = s_pendingPress; s_pendingPress = false; return p; }
bool    knob::takeLongPress()  { const bool p = s_pendingLong;  s_pendingLong  = false; return p; }
bool    knob::pendingPress()   { return s_pendingPress; }
int32_t knob::rawPosition()    { return s_detent; }
uint32_t knob::lastRockMs()    { return s_rockMs; }
uint32_t knob::lastRockGapMs() { return s_rockGapMs; }
int32_t  knob::lastRockDetent(){ return s_rockDetent; }
int32_t  knob::detentCount()   { return s_detent; }
// The 8 s recovery countdown in main.cpp reads this. A finger that has moved is steering,
// not holding, so it never counts toward the reboot.
uint32_t knob::heldMs()        { return (s_down && !s_moved) ? (millis() - s_downMs) : 0; }
uint32_t knob::longPressMs()   { return LONG_PRESS_MS; }

#endif  // ORB_BOARD_P4_34C
