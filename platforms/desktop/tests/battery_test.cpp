// SystemBattery's /sys/class/power_supply reading, on fake directories laid out like the
// uConsole's (AXP battery and AC), a laptop's (energy and power, no capacity), a desktop's
// (no battery, or only a mouse's), and then the real battery of this machine.
//
//   battery_test <scratch dir>

#include "SystemBattery.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using Supply = std::map<std::string, std::string>;

std::string makeDir(const std::string& root, const std::map<std::string, Supply>& supplies)
{
    std::filesystem::remove_all(root);
    for (const auto& [name, files] : supplies)
    {
        std::filesystem::create_directories(root + "/" + name);
        for (const auto& [file, value] : files)
            std::ofstream(root + "/" + name + "/" + file) << value << "\n";
    }
    return root;
}

void print(const char* what, const SystemBattery::Reading& r)
{
    std::printf("%-22s present %d, %d%%, %.2f V, charger %d, %d min\n", what, r.present, r.level, r.voltage, r.charging, r.minutes);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: battery_test <scratch dir>\n");
        return 2;
    }
    const std::string scratch = argv[1];

    // The uConsole: the AC supply sorts after the battery.
    Supply axpBattery = { { "type", "Battery" }, { "present", "1" }, { "status", "Discharging" },
                          { "capacity", "62" }, { "voltage_now", "3812000" } };
    Supply axpAc = { { "type", "Mains" }, { "online", "0" }, { "present", "1" } };
    std::string dir = makeDir(scratch + "/uconsole", { { "axp20x-battery", axpBattery }, { "axp22x-ac", axpAc } });
    SystemBattery::Reading r = SystemBattery::readPowerSupplyDir(dir);
    print("uConsole on battery", r);
    CHECK(r.present && r.level == 62 && !r.charging);
    CHECK(r.voltage > 3.81f && r.voltage < 3.82f);

    // Plugged in: charging, and still "charging" (charger present) once full.
    axpAc["online"] = "1";
    axpBattery["status"] = "Charging";
    dir = makeDir(scratch + "/uconsole", { { "axp20x-battery", axpBattery }, { "axp22x-ac", axpAc } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("uConsole charging", r);
    CHECK(r.present && r.charging);
    axpBattery["status"] = "Full";
    axpBattery["capacity"] = "100";
    dir = makeDir(scratch + "/uconsole", { { "axp20x-battery", axpBattery }, { "axp22x-ac", axpAc } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("uConsole full", r);
    CHECK(r.present && r.level == 100 && r.charging && r.minutes == 0);

    // A laptop: level and time from energy and power; 30 Wh of 60 at 15 W is 2 h.
    Supply bat0 = { { "type", "Battery" }, { "status", "Discharging" }, { "energy_now", "30000000" },
                    { "energy_full", "60000000" }, { "power_now", "15000000" } };
    dir = makeDir(scratch + "/laptop", { { "AC", { { "type", "Mains" }, { "online", "0" } } }, { "BAT0", bat0 } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("laptop discharging", r);
    CHECK(r.present && r.level == 50 && !r.charging && r.minutes == 120);
    bat0["status"] = "Charging";
    dir = makeDir(scratch + "/laptop", { { "AC", { { "type", "Mains" }, { "online", "1" } } }, { "BAT0", bat0 } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("laptop charging", r);
    CHECK(r.present && r.charging && r.minutes == 120);

    // Charge instead of energy, and the driver's own estimate.
    dir = makeDir(scratch + "/charge", { { "BAT1", { { "type", "Battery" }, { "status", "Discharging" }, { "charge_now", "1000000" },
                                                     { "charge_full", "4000000" }, { "time_to_empty_now", "5400" } } } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("charge counter", r);
    CHECK(r.present && r.level == 25 && r.minutes == 90);

    // No battery: mains only, a mouse's battery, a removed battery, nothing at all.
    dir = makeDir(scratch + "/desktop",
                  { { "AC", { { "type", "Mains" }, { "online", "1" } } },
                    { "hidpp_battery_0", { { "type", "Battery" }, { "scope", "Device" }, { "capacity", "70" } } },
                    { "BAT0", { { "type", "Battery" }, { "present", "0" }, { "capacity", "0" } } } });
    r = SystemBattery::readPowerSupplyDir(dir);
    print("desktop", r);
    CHECK(!r.present && !r.charging);
    r = SystemBattery::readPowerSupplyDir(scratch + "/missing");
    CHECK(!r.present);

    // The driver interface caches and reports the same.
    SystemBattery battery(scratch + "/laptop");
    CHECK(battery.getState() && battery.getLevel() == 50 && battery.getCharging() && !battery.isEmpty());

    SystemBattery real;
    print("this machine", real.read());

    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all battery checks passed\n");
    return 0;
}
