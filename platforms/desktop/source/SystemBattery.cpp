#include "SystemBattery.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

namespace
{
constexpr auto kReadInterval = std::chrono::seconds(2);

std::string readLine(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

// A number from a sysfs file; false if missing or not a number.
bool readNumber(const std::string& path, long long& value)
{
    const std::string text = readLine(path);
    if (text.empty())
        return false;
    char* end = nullptr;
    value = std::strtoll(text.c_str(), &end, 10);
    return end != text.c_str();
}

#ifdef __APPLE__
bool dictNumber(CFDictionaryRef dict, CFStringRef key, int& value)
{
    auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
    return number && CFGetTypeID(number) == CFNumberGetTypeID() && CFNumberGetValue(number, kCFNumberIntType, &value);
}

bool dictString(CFDictionaryRef dict, CFStringRef key, CFStringRef expected)
{
    auto string = static_cast<CFStringRef>(CFDictionaryGetValue(dict, key));
    return string && CFGetTypeID(string) == CFStringGetTypeID() && CFStringCompare(string, expected, 0) == kCFCompareEqualTo;
}

// The power sources leave the voltage out on Apple silicon; the battery's registry entry
// has it, in millivolts.
float smartBatteryVoltage()
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    if (!service)
        return 0.0f;
    int millivolts = 0;
    if (CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("Voltage"), kCFAllocatorDefault, 0))
    {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &millivolts);
        CFRelease(value);
    }
    IOObjectRelease(service);
    return millivolts / 1000.0f;
}

SystemBattery::Reading readIoKit()
{
    SystemBattery::Reading reading;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info)
        return reading;
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; sources && i < CFArrayGetCount(sources); i++)
    {
        CFDictionaryRef source = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, i));
        if (!source || !dictString(source, CFSTR(kIOPSTypeKey), CFSTR(kIOPSInternalBatteryType)))
            continue;
        auto present = static_cast<CFBooleanRef>(CFDictionaryGetValue(source, CFSTR(kIOPSIsPresentKey)));
        if (present && !CFBooleanGetValue(present))
            continue;
        int capacity = 0;
        int maxCapacity = 100;
        if (!dictNumber(source, CFSTR(kIOPSCurrentCapacityKey), capacity))
            continue;
        dictNumber(source, CFSTR(kIOPSMaxCapacityKey), maxCapacity);
        reading.present = true;
        reading.level = maxCapacity > 0 ? std::clamp(capacity * 100 / maxCapacity, 0, 100) : 0;
        reading.charging = dictString(source, CFSTR(kIOPSPowerSourceStateKey), CFSTR(kIOPSACPowerValue));
        int millivolts = 0;
        if (dictNumber(source, CFSTR(kIOPSVoltageKey), millivolts))
            reading.voltage = millivolts / 1000.0f;
        else
            reading.voltage = smartBatteryVoltage();
        int minutes = -1;
        dictNumber(source, reading.charging ? CFSTR(kIOPSTimeToFullChargeKey) : CFSTR(kIOPSTimeToEmptyKey), minutes);
        reading.minutes = std::max(minutes, 0); // -1 while the system is still estimating
        break;
    }
    if (sources)
        CFRelease(sources);
    CFRelease(info);
    return reading;
}
#endif
} // namespace

SystemBattery::SystemBattery()
{
    if (const char* dir = std::getenv("MEADOWLARK_POWER_SUPPLY_DIR"))
        powerSupplyDir = dir;
    else
    {
#ifdef __APPLE__
        useIoKit = true;
#else
        powerSupplyDir = "/sys/class/power_supply";
#endif
    }
}

SystemBattery::SystemBattery(const std::string& powerSupplyDir) : powerSupplyDir(powerSupplyDir) {}

const SystemBattery::Reading& SystemBattery::current()
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextRead)
    {
        cached = read();
        nextRead = now + kReadInterval;
    }
    return cached;
}

SystemBattery::Reading SystemBattery::read() const
{
#ifdef __APPLE__
    if (useIoKit)
        return readIoKit();
#endif
    return readPowerSupplyDir(powerSupplyDir);
}

SystemBattery::Reading SystemBattery::readPowerSupplyDir(const std::string& dir)
{
    Reading reading;
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* entry = readdir(d))
        {
            if (entry->d_name[0] != '.')
                names.push_back(entry->d_name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());

    bool chargerOnline = false;
    for (const std::string& name : names)
    {
        const std::string path = dir + "/" + name + "/";
        const std::string type = readLine(path + "type");
        long long value = 0;
        if (type == "Mains" || type == "USB" || type == "Wireless")
            chargerOnline = chargerOnline || (readNumber(path + "online", value) && value != 0);
    }

    for (const std::string& name : names)
    {
        const std::string path = dir + "/" + name + "/";
        const std::string type = readLine(path + "type");
        long long value = 0;
        if (type != "Battery" || reading.present || readLine(path + "scope") == "Device")
            continue;
        if (readNumber(path + "present", value) && value == 0)
            continue;

        // Level: capacity, else from energy or charge.
        long long now = 0;
        long long full = 0;
        if (readNumber(path + "capacity", value))
            reading.level = static_cast<int>(value);
        else if (readNumber(path + "energy_now", now) && readNumber(path + "energy_full", full) && full > 0)
            reading.level = static_cast<int>(now * 100 / full);
        else if (readNumber(path + "charge_now", now) && readNumber(path + "charge_full", full) && full > 0)
            reading.level = static_cast<int>(now * 100 / full);
        else
            continue;
        reading.present = true;
        reading.level = std::clamp(reading.level, 0, 100);

        if (readNumber(path + "voltage_now", value))
            reading.voltage = value / 1e6f;

        const std::string status = readLine(path + "status");
        reading.charging = status == "Charging";

        // Time left: the driver's estimate, else the energy (or charge) over the power (or
        // current) flowing.
        const bool towardsFull = reading.charging || chargerOnline;
        long long seconds = 0;
        if (readNumber(path + (towardsFull ? "time_to_full_now" : "time_to_empty_now"), seconds) && seconds > 0)
            reading.minutes = static_cast<int>(seconds / 60);
        else
        {
            long long rate = 0;
            const bool energy = readNumber(path + "energy_now", now) && readNumber(path + "power_now", rate);
            const bool charge = !energy && readNumber(path + "charge_now", now) && readNumber(path + "current_now", rate);
            rate = rate < 0 ? -rate : rate;
            if ((energy || charge) && rate > 0)
            {
                if (towardsFull)
                {
                    const bool haveFull = readNumber(path + (energy ? "energy_full" : "charge_full"), full);
                    reading.minutes = haveFull && full > now ? static_cast<int>((full - now) * 60 / rate) : 0;
                }
                else
                    reading.minutes = static_cast<int>(now * 60 / rate);
            }
        }
        if (status == "Full")
            reading.minutes = 0;
    }
    reading.charging = reading.present && (reading.charging || chargerOnline);
    return reading;
}
