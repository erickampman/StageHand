#include "Engine/EngineGraph.h"

#include <utility>

namespace stagehand {

EngineGraph::EngineGraph(std::vector<SlotConfig> slotConfigs, std::uint32_t maxEventsPerSlotBlock)
    : configs(std::move(slotConfigs)), states(configs.size()) {
    for (std::size_t i = 0; i < configs.size(); ++i) {
        SlotState& state = states[i];
        state.events.resize(maxEventsPerSlotBlock);
        if (!configs[i].plugin) continue;

        const PortLayout layout = configs[i].plugin->portLayout();
        if (configs[i].notePort < layout.noteInputs.size())
            state.dialects = layout.noteInputs[configs[i].notePort].supportedDialects;
        if (const int main = mainAudioOutput(layout); main >= 0)
            state.audioChannels = layout.audioOutputs[static_cast<std::size_t>(main)].channelCount;

        state.audio.assign(std::size_t{state.audioChannels} * kMaxPluginBlockFrames, 0.0f);
        state.channels.resize(state.audioChannels);
        for (std::uint32_t c = 0; c < state.audioChannels; ++c)
            state.channels[c] = state.audio.data() + std::size_t{c} * kMaxPluginBlockFrames;
    }
}

bool EngineGraph::contains(const HostedPlugin* plugin) const noexcept {
    for (const SlotConfig& slot : configs)
        if (slot.plugin.get() == plugin) return true;
    return false;
}

} // namespace stagehand
