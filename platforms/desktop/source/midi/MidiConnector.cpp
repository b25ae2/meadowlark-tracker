#include "MidiBackend.h"

#include <cstdio>

namespace midi
{
void MidiConnector::update()
{
    if (!config.enabled)
        return;
    const std::vector<Endpoint> sources = backend.sources();
    const std::vector<Endpoint> destinations = backend.destinations();
    for (int port = 0; port < kPortCount; port++)
    {
        connect(port, true, sources);
        connect(port, false, destinations);
    }
}

void MidiConnector::connect(int port, bool input, const std::vector<Endpoint>& endpoints)
{
    const std::vector<std::string>& patterns = input ? config.ports[port].inputs : config.ports[port].outputs;
    std::set<std::pair<uint64_t, std::string>>& done = connected[input ? 0 : 1][port];

    std::set<std::pair<uint64_t, std::string>> present;
    for (const Endpoint& endpoint : endpoints)
    {
        const std::pair<uint64_t, std::string> key(endpoint.id, endpoint.name);
        present.insert(key);
        if (done.count(key))
            continue;
        // "*" leaves out Meadowlark's own ports; a name can still pick one (loopback tests).
        bool wanted = false;
        for (const std::string& pattern : patterns)
            wanted = wanted || (matchesPattern(pattern, endpoint.name, endpoint.hardware && !endpoint.own));
        if (!wanted)
            continue;

        const bool ok = input ? backend.connectInput(port, endpoint) : backend.connectOutput(port, endpoint);
        std::printf("meadowlark: midi %s %s %s%s\n",
                    portName(port),
                    input ? "<-" : "->",
                    endpoint.name.c_str(),
                    ok ? "" : " (failed)");
        // A failure isn't retried until the port goes away and comes back.
        done.insert(key);
    }

    for (auto it = done.begin(); it != done.end();)
        it = present.count(*it) ? std::next(it) : done.erase(it);
}

void printMidiPorts(MidiBackend& backend, const MidiConfig& config)
{
    auto print = [&](const char* title, const std::vector<Endpoint>& endpoints, bool input) {
        std::printf("%s:\n", title);
        if (endpoints.empty())
            std::printf("  (none)\n");
        for (const Endpoint& endpoint : endpoints)
        {
            std::string matches;
            for (int port = 0; port < kPortCount; port++)
            {
                const std::vector<std::string>& patterns =
                  input ? config.ports[port].inputs : config.ports[port].outputs;
                for (const std::string& pattern : patterns)
                {
                    if (matchesPattern(pattern, endpoint.name, endpoint.hardware && !endpoint.own))
                    {
                        matches += std::string(matches.empty() ? (input ? "  -> " : "  <- ") : ", ") + portName(port);
                        break;
                    }
                }
            }
            std::printf("  %-48s %s%s\n",
                        endpoint.name.c_str(),
                        endpoint.own ? "meadowlark" : (endpoint.hardware ? "hardware" : "software"),
                        matches.c_str());
        }
    };
    print("MIDI sources (for [usb]/[jack] inputs)", backend.sources(), true);
    print("MIDI destinations (for outputs)", backend.destinations(), false);
}
} // namespace midi
