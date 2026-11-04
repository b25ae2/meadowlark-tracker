#pragma once

// The computer's battery as the Mini's battery driver: level, voltage, charger and time
// left, for the title bar's icon and Config's battery line. getState() is false when the
// system has no battery (the title bar then hides the icon).
//
// Linux reads /sys/class/power_supply: the first system battery (scope "Device" ones, such
// as a mouse's, are skipped), and "charging" when any mains or USB supply is online or the
// battery says it is charging. macOS reads the internal battery from IOKit's power sources.
// MEADOWLARK_POWER_SUPPLY_DIR (or the constructor) points the Linux reader at another
// directory, which also works on macOS, for tests.
//
// Values are read at most every couple of seconds; isEmpty() is always false, since the
// computer shuts itself down.

#include "Platform.h"

#include <chrono>
#include <string>

class SystemBattery : public Battery
{
  public:
    struct Reading
    {
        bool present = false;
        int level = 0;          // percent
        float voltage = 0.0f;   // volts, 0 if unknown
        bool charging = false;  // a charger is plugged in
        int minutes = 0;        // to full when charging, to empty otherwise; 0 if unknown
    };

    SystemBattery();
    explicit SystemBattery(const std::string& powerSupplyDir);

    void enableMonitoring() override {}
    int getLevel() override { return current().level; }
    float getVoltage() override { return current().voltage; }
    bool getCharging() override { return current().charging; }
    int getTimeTo() override { return current().minutes; }
    bool getState() override { return current().present; }
    bool isEmpty() override { return false; }

    // A fresh reading, without the cache.
    Reading read() const;
    static Reading readPowerSupplyDir(const std::string& dir);

  private:
    const Reading& current();

    std::string powerSupplyDir;
    bool useIoKit = false;
    Reading cached;
    std::chrono::steady_clock::time_point nextRead{};
};
