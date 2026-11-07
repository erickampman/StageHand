#include "Engine/EngineGraph.h"

#include <algorithm>
#include <utility>

namespace stagehand {

namespace {

// The plugin's main audio input (the first flagged main, else the first), or -1.
int mainAudioInput(const PortLayout& layout) noexcept {
    for (std::size_t i = 0; i < layout.audioInputs.size(); ++i)
        if (layout.audioInputs[i].isMain) return static_cast<int>(i);
    return layout.audioInputs.empty() ? -1 : 0;
}

} // namespace

EngineGraph::EngineGraph(std::vector<SlotConfig> slotConfigs, std::vector<BusConfig> buses,
                         std::uint32_t maxEventsPerSlotBlock)
    : configs(std::move(slotConfigs)), states(configs.size()), busConfigs(std::move(buses)),
      gainRamp(kMaxPluginBlockFrames), sendRamp(kMaxPluginBlockFrames) {
    if (busConfigs.size() > kBusCount) busConfigs.resize(kBusCount);
    busStates.resize(busConfigs.size());
    for (std::size_t b = 0; b < busConfigs.size(); ++b) {
        BusConfig& config = busConfigs[b];
        BusState&  state  = busStates[b];
        if (!config.runtime) config.runtime = std::make_shared<SlotRuntime>();
        state.input.assign(2 * std::size_t{kMaxPluginBlockFrames}, 0.0f);
        for (std::size_t c = 0; c < 2; ++c) state.inputChannels[c] = state.input.data() + c * kMaxPluginBlockFrames;

        // Chain the stages: each reads what the one before produced.
        std::vector<const float*> source{state.inputChannels[0], state.inputChannels[1]};
        for (std::size_t e = 0; e < BusConfig::kEffects; ++e) {
            if (!config.effects[e]) continue;   // an empty slot passes the audio on
            BusState::Stage& stage  = state.stages[e];
            const PortLayout layout = config.effects[e]->portLayout();
            stage.events.resize(kMaxBusEffectEvents);
            if (const int in = mainAudioInput(layout); in >= 0 && !source.empty()) {
                const std::uint32_t channels = layout.audioInputs[static_cast<std::size_t>(in)].channelCount;
                for (std::uint32_t c = 0; c < channels; ++c) stage.in.push_back(source[std::min<std::size_t>(c, source.size() - 1)]);
            }
            std::uint32_t outChannels = 0;
            if (const int out = mainAudioOutput(layout); out >= 0)
                outChannels = layout.audioOutputs[static_cast<std::size_t>(out)].channelCount;
            stage.audio.assign(std::size_t{outChannels} * kMaxPluginBlockFrames, 0.0f);
            stage.out.resize(outChannels);
            source.clear();
            for (std::uint32_t c = 0; c < outChannels; ++c) {
                stage.out[c] = stage.audio.data() + std::size_t{c} * kMaxPluginBlockFrames;
                source.push_back(stage.out[c]);
            }
        }
        state.silent = source.empty();
        for (std::size_t c = 0; c < 2 && !source.empty(); ++c)
            state.returnChannels[c] = source[std::min<std::size_t>(c, source.size() - 1)];
    }

    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (!configs[i].runtime) configs[i].runtime = std::make_shared<SlotRuntime>();
        SlotState& state = states[i];
        state.events.resize(maxEventsPerSlotBlock);
        if (!configs[i].plugin) continue;

        const PortLayout layout = configs[i].plugin->portLayout();
        if (configs[i].notePort < layout.noteInputs.size())
            state.pluginDialects = layout.noteInputs[configs[i].notePort].supportedDialects;
        state.dialects = state.pluginDialects;

        const std::vector<std::shared_ptr<HostedPlugin>>& effects = configs[i].midiEffects;
        state.effects.resize(effects.size());
        for (std::size_t e = 0; e < effects.size(); ++e) {
            EffectState&     effect = state.effects[e];
            const PortLayout own    = effects[e]->portLayout();
            if (!own.noteInputs.empty()) effect.dialects = own.noteInputs[0].supportedDialects;
            if (const int main = mainAudioOutput(own); main >= 0)
                effect.audioChannels = own.audioOutputs[static_cast<std::size_t>(main)].channelCount;
            effect.params.resize(kMaxBusEffectEvents);
            effect.audio.assign(std::size_t{effect.audioChannels} * kMaxPluginBlockFrames, 0.0f);
            effect.channels.resize(effect.audioChannels);
            for (std::uint32_t c = 0; c < effect.audioChannels; ++c)
                effect.channels[c] = effect.audio.data() + std::size_t{c} * kMaxPluginBlockFrames;
        }
        for (std::size_t e = effects.size(); e-- > 0;)   // the first effect not bypassed takes the row's MIDI
            if (!configs[i].bypassed(e)) state.dialects = state.effects[e].dialects;
        if (!effects.empty()) {
            for (std::vector<Event>& buffer : state.chain) buffer.resize(maxEventsPerSlotBlock);
            state.merged.resize(std::size_t{maxEventsPerSlotBlock} + kMaxBusEffectEvents);
        }
        if ((state.pluginDialects & NoteDialect::Midi2) != 0)   // only MIDI 2.0 carries a Pitch 7.25
            state.withResets.resize(std::size_t{maxEventsPerSlotBlock} + kMaxBusEffectEvents + kMaxRetuneResets);
        if (const int main = mainAudioOutput(layout); main >= 0)
            state.audioChannels = layout.audioOutputs[static_cast<std::size_t>(main)].channelCount;

        state.audio.assign(std::size_t{state.audioChannels} * kMaxPluginBlockFrames, 0.0f);
        state.channels.resize(state.audioChannels);
        for (std::uint32_t c = 0; c < state.audioChannels; ++c)
            state.channels[c] = state.audio.data() + std::size_t{c} * kMaxPluginBlockFrames;
    }
}

bool EngineGraph::contains(const HostedPlugin* plugin) const noexcept {
    for (const SlotConfig& slot : configs) {
        if (slot.plugin.get() == plugin) return true;
        for (const auto& effect : slot.midiEffects)
            if (effect.get() == plugin) return true;
    }
    for (const BusConfig& bus : busConfigs)
        for (const auto& effect : bus.effects)
            if (effect.get() == plugin) return true;
    return false;
}

} // namespace stagehand
