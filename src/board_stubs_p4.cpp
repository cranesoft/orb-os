// The Big Orb's answer for the parts it does not carry.
//
// The Waveshare ESP32-P4 3.4C has no AXP2101 PMIC, no QMI8658 IMU, no PCF85063 RTC and no
// GPS socket. Rather than scatter `#if` through every caller, the Big Orb build leaves
// battery.cpp, imu_qmi8658.cpp, rtc_pcf85063.cpp and gps.cpp out (build_src_filter in
// platformio.ini) and links these instead. Each says "not here" the way the real driver
// says it when the chip does not answer, so callers take the same path a 466 Orb takes
// with that part missing.
#include "config.h"

#if defined(ORB_BOARD_P4_34C)

#include <Arduino.h>
#include <Wire.h>
#include "battery.h"
#include "imu_qmi8658.h"
#include "rtc_pcf85063.h"
#include "gps.h"

// ---- PMIC: mains powered, no gauge --------------------------------------------------
bool battery_begin()              { return false; }
bool battery_present()            { return false; }
int  battery_percent()            { return -1; }
bool battery_charging()           { return false; }
// The ES8311's analog rail is always on here; there is no ALDO1 to switch.
void battery_enable_codec_rail()  {}

// ---- IMU: no face-down sleep, no knock-to-wake -------------------------------------
// imu_begin() is where the shared I2C bus comes up (see the note in imu_qmi8658.cpp:
// main.cpp's setup() relies on that order). The stub keeps the job and drops the probe.
bool imu_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    Serial.println("[imu] none on this board (I2C bus up on GPIO7/8)");
    return false;
}
int  imu_facedown() { return -1; }   // "read unavailable": callers leave their state alone
int  imu_motion()   { return 0; }

// ---- RTC: the clock comes from NTP alone -------------------------------------------
bool rtc_begin()                      { return false; }
bool rtc_present()                    { return false; }
bool rtc_read(struct tm *)            { return false; }
bool rtc_write(const struct tm *)     { return false; }

// ---- GPS ---------------------------------------------------------------------------
bool gps_begin()                       { return false; }
bool gps_present()                     { return false; }
void gps_poll()                        {}
bool gps_has_fix()                     { return false; }
bool gps_location(double *, double *)  { return false; }
int  gps_satellites()                  { return 0; }

#endif  // ORB_BOARD_P4_34C
