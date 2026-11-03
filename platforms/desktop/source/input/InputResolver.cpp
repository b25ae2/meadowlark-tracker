#include "InputResolver.h"

#include <algorithm>

namespace input
{
namespace
{
bool contains(const Binding& binding, InputId input)
{
    return std::find(binding.inputs.begin(), binding.inputs.end(), input) != binding.inputs.end();
}
} // namespace

void InputResolver::setBindings(std::vector<Binding> newBindings)
{
    releaseAll();
    bindings = std::move(newBindings);
    active.assign(bindings.size(), false);
}

void InputResolver::inputDown(InputId input)
{
    if (!down.insert(input).second)
        return;

    bool chordActivated = false;
    for (size_t i = 0; i < bindings.size(); i++)
    {
        const Binding& binding = bindings[i];
        if (active[i] || binding.inputs.size() < 2 || !contains(binding, input))
            continue;
        bool allDown = std::all_of(binding.inputs.begin(), binding.inputs.end(), [this](InputId other) {
            return down.count(other) != 0;
        });
        if (allDown)
        {
            activate(i);
            chordActivated = true;
        }
    }

    if (chordActivated)
    {
        consumed.insert(input);
        return;
    }

    for (size_t i = 0; i < bindings.size(); i++)
    {
        if (!active[i] && bindings[i].inputs.size() == 1 && bindings[i].inputs[0] == input)
            activate(i);
    }
}

void InputResolver::inputUp(InputId input)
{
    if (down.erase(input) == 0)
        return;
    consumed.erase(input);

    for (size_t i = 0; i < bindings.size(); i++)
    {
        if (active[i] && contains(bindings[i], input))
            deactivate(i);
    }
}

void InputResolver::releaseAll()
{
    for (size_t i = 0; i < bindings.size(); i++)
    {
        if (active[i])
            deactivate(i);
    }
    down.clear();
    consumed.clear();
}

bool InputResolver::isActionHeld(ActionId action) const
{
    auto it = heldCount.find(action);
    return it != heldCount.end() && it->second > 0;
}

void InputResolver::activate(size_t binding)
{
    active[binding] = true;
    const ActionId action = bindings[binding].action;
    if (heldCount[action]++ == 0 && sink)
        sink(action, true);
}

void InputResolver::deactivate(size_t binding)
{
    active[binding] = false;
    const ActionId action = bindings[binding].action;
    if (--heldCount[action] == 0 && sink)
        sink(action, false);
}
} // namespace input
