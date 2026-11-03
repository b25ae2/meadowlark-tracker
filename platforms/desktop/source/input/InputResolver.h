#pragma once

// Turns input state into action presses and releases, independent of where inputs come from.
//
// A binding is one input or a chord of several. An action can have any number of bindings
// and is held while at least one of them is: the sink sees one press when the first binding
// activates and one release when the last one lets go.
//
// A chord activates when its last input goes down while the others are held. That input is
// then "consumed": its own single-input bindings do not fire until it is released, so
// "select + a" can mean something other than "a". The inputs held before it already did
// whatever they are bound to, like modifier keys.

#include "Actions.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace input
{
// Opaque input identifier; SdlInput defines the encoding.
using InputId = uint64_t;

struct Binding
{
    std::vector<InputId> inputs; // one input, or a chord
    ActionId action = 0;
};

class InputResolver
{
  public:
    using Sink = std::function<void(ActionId action, bool pressed)>;

    void setSink(Sink callback) { sink = std::move(callback); }
    // Releases everything held under the old bindings first.
    void setBindings(std::vector<Binding> newBindings);
    const std::vector<Binding>& getBindings() const { return bindings; }

    void inputDown(InputId input);
    void inputUp(InputId input);
    void releaseAll();

    bool isDown(InputId input) const { return down.count(input) != 0; }
    bool isActionHeld(ActionId action) const;

  private:
    void activate(size_t binding);
    void deactivate(size_t binding);

    std::vector<Binding> bindings;
    std::vector<bool> active;
    std::unordered_set<InputId> down;
    std::unordered_set<InputId> consumed;
    std::unordered_map<ActionId, int> heldCount;
    Sink sink;
};
} // namespace input
