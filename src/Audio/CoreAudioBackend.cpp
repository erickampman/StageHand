#include "Audio/CoreAudioBackend.h"

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "Core/Clock.h"

namespace stagehand {

namespace {

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector, T& value,
                 AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const AudioObjectPropertyAddress where = address(selector, scope);
    UInt32                           size  = sizeof(T);
    return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr;
}

template <typename T>
bool setProperty(AudioObjectID object, AudioObjectPropertySelector selector, const T& value) {
    const AudioObjectPropertyAddress where = address(selector);
    return AudioObjectSetPropertyData(object, &where, 0, nullptr, sizeof(T), &value) == noErr;
}

std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    if (!getProperty(object, selector, value) || value == nullptr) return {};
    std::string text;
    const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    if (CFStringGetCString(value, buffer.data(), size, kCFStringEncodingUTF8)) text = buffer.data();
    CFRelease(value);
    return text;
}

std::uint32_t outputChannelCount(AudioObjectID device) {
    const AudioObjectPropertyAddress where = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32                           size  = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<std::byte> storage(size);
    auto* const            list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr) return 0;
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Property changes come on the HAL's own notification thread (see
// useHalThread()), never the IOProc's.
OSStatus propertyListener(AudioObjectID, UInt32 count, const AudioObjectPropertyAddress* addresses, void* client) {
    auto* const self = static_cast<CoreAudioBackend*>(client);
    for (UInt32 i = 0; i < count; ++i) self->onPropertyChanged(addresses[i].mSelector);
    return noErr;
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp* now, const AudioBufferList*, const AudioTimeStamp*,
                AudioBufferList* output, const AudioTimeStamp*, void* client) {
    static_cast<CoreAudioBackend*>(client)->render(now, output);
    return noErr;
}

// The HAL posts notifications on a run loop it's given, or its own thread if
// it's given none. None: they then come whatever loop the app runs (or none).
void useHalThread() {
    static const bool done = [] {
        CFRunLoopRef none = nullptr;
        setProperty(kAudioObjectSystemObject, kAudioHardwarePropertyRunLoop, none);
        return true;
    }();
    (void)done;
}

constexpr AudioObjectPropertySelector kWatched[] = {
    kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyNominalSampleRate,
    kAudioDeviceProcessorOverload, kAudioDevicePropertyDeviceIsAlive};

} // namespace

CoreAudioBackend::CoreAudioBackend(std::string deviceChoice) : choice(std::move(deviceChoice)) {
    useHalThread();
}

CoreAudioBackend::~CoreAudioBackend() {
    close();
}

// --- devices ---------------------------------------------------------------------------

std::vector<AudioDeviceInfo> CoreAudioBackend::outputDevices() {
    std::vector<AudioDeviceInfo> devices;
    const AudioObjectPropertyAddress where = address(kAudioHardwarePropertyDevices);
    UInt32                           size  = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &where, 0, nullptr, &size) != noErr) return devices;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, ids.data()) != noErr)
        return devices;

    AudioObjectID defaultOutput = 0;
    getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, defaultOutput);
    for (const AudioObjectID id : ids) {
        AudioDeviceInfo info;
        info.id             = id;
        info.outputChannels = outputChannelCount(id);
        if (info.outputChannels == 0) continue;   // an input-only device
        info.name      = stringProperty(id, kAudioObjectPropertyName);
        info.uid       = stringProperty(id, kAudioDevicePropertyDeviceUID);
        info.isDefault = id == defaultOutput;
        Float64 rate   = 0.0;
        if (getProperty(id, kAudioDevicePropertyNominalSampleRate, rate)) info.sampleRate = rate;
        devices.push_back(info);
    }
    return devices;
}

bool CoreAudioBackend::findDevice(const std::string& wanted, AudioDeviceInfo& found, std::string& problem) {
    const std::vector<AudioDeviceInfo> devices = outputDevices();
    if (wanted.empty()) {
        for (const AudioDeviceInfo& device : devices)
            if (device.isDefault) {
                found = device;
                return true;
            }
        problem = "there's no default output device";
        return false;
    }
    for (const AudioDeviceInfo& device : devices)
        if (device.name == wanted || device.uid == wanted) {
            found = device;
            return true;
        }
    const std::string            part = lowercase(wanted);
    std::vector<AudioDeviceInfo> matches;
    for (const AudioDeviceInfo& device : devices)
        if (lowercase(device.name).find(part) != std::string::npos) matches.push_back(device);
    if (matches.size() == 1) {
        found = matches.front();
        return true;
    }
    if (matches.empty()) {
        problem = "no output device '" + wanted + "' (see --list-audio)";
    } else {
        problem = "'" + wanted + "' matches several output devices:";
        for (const AudioDeviceInfo& device : matches) problem += " '" + device.name + "'";
    }
    return false;
}

// --- AudioBackend ------------------------------------------------------------------------

bool CoreAudioBackend::open(const AudioConfig& requested, AudioConfig& negotiated) {
    if (procId != nullptr) return false;   // already open

    std::string problem;
    if (!findDevice(choice, opened, problem)) {
        std::fprintf(stderr, "CoreAudioBackend: %s\n", problem.c_str());
        return false;
    }
    const AudioObjectID device = opened.id;

    // The device keeps its rate unless one is asked for.
    if (requested.sampleRate > 0.0 && requested.sampleRate != opened.sampleRate &&
        !setProperty(device, kAudioDevicePropertyNominalSampleRate, Float64{requested.sampleRate}))
        std::fprintf(stderr, "CoreAudioBackend: '%s' won't run at %.0f Hz\n", opened.name.c_str(), requested.sampleRate);

    AudioValueRange range{};
    getProperty(device, kAudioDevicePropertyBufferFrameSizeRange, range);
    restoreBufferSize = 0;
    if (requested.bufferSize != 0) {
        // This process's buffer on the device stays as set until set again, so
        // close() puts back what was there.
        UInt32 before = 0;
        if (getProperty(device, kAudioDevicePropertyBufferFrameSize, before)) restoreBufferSize = before;
        const auto frames = static_cast<UInt32>(
            std::clamp<double>(requested.bufferSize, range.mMinimum, range.mMaximum > 0 ? range.mMaximum : 8192));
        if (frames != requested.bufferSize)
            std::fprintf(stderr, "CoreAudioBackend: '%s' takes %.0f to %.0f frames; using %u\n", opened.name.c_str(),
                         range.mMinimum, range.mMaximum, frames);
        if (!setProperty(device, kAudioDevicePropertyBufferFrameSize, frames))
            std::fprintf(stderr, "CoreAudioBackend: couldn't set a %u-frame buffer\n", frames);
    }

    Float64 rate   = 0.0;
    UInt32  frames = 0;
    getProperty(device, kAudioDevicePropertyNominalSampleRate, rate);
    getProperty(device, kAudioDevicePropertyBufferFrameSize, frames);
    currentSampleRate.store(rate, std::memory_order_relaxed);
    currentBufferSize.store(frames, std::memory_order_relaxed);

    const std::uint32_t outputs = std::min(requested.outputChannels, opened.outputChannels);
    if (outputs < requested.outputChannels)
        std::fprintf(stderr, "CoreAudioBackend: '%s' has %u outputs; using %u of the %u asked for\n",
                     opened.name.c_str(), opened.outputChannels, outputs, requested.outputChannels);
    const std::uint32_t aux = requested.auxOutputChannels;

    // Planar buffers for the largest block the device allows, so a buffer-size
    // change never needs more.
    capacityFrames = std::max<std::uint32_t>(frames, static_cast<std::uint32_t>(range.mMaximum));
    planar.assign(static_cast<std::size_t>(outputs + aux) * capacityFrames, 0.0f);
    outputPointers.clear();
    auxPointers.clear();
    for (std::uint32_t c = 0; c < outputs; ++c) outputPointers.push_back(planar.data() + std::size_t{c} * capacityFrames);
    for (std::uint32_t c = 0; c < aux; ++c)
        auxPointers.push_back(planar.data() + std::size_t{outputs + c} * capacityFrames);

    AudioDeviceIOProcID id = nullptr;
    if (const OSStatus result = AudioDeviceCreateIOProcID(device, &ioProc, this, &id); result != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't run on '%s' (error %d)\n", opened.name.c_str(),
                     static_cast<int>(result));
        return false;
    }
    procId = reinterpret_cast<void*>(id);
    addListeners();

    negotiatedConfig                   = AudioConfig{};
    negotiatedConfig.sampleRate        = rate;
    negotiatedConfig.bufferSize        = frames;
    negotiatedConfig.inputChannels     = 0;   // outputs only (decision 2)
    negotiatedConfig.outputChannels    = outputs;
    negotiatedConfig.auxOutputChannels = aux;
    lost.store(false);
    rateChanged.store(false);
    negotiated = negotiatedConfig;
    return true;
}

void CoreAudioBackend::setProcessor(AudioProcessor* audioProcessor) {
    if (running) {
        std::fprintf(stderr, "CoreAudioBackend: setProcessor() ignored while running\n");
        return;
    }
    processor = audioProcessor;
}

bool CoreAudioBackend::start() {
    if (procId == nullptr || running) return false;
    if (const OSStatus result = AudioDeviceStart(opened.id, reinterpret_cast<AudioDeviceIOProcID>(procId));
        result != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't start '%s' (error %d)\n", opened.name.c_str(),
                     static_cast<int>(result));
        return false;
    }
    running = true;
    return true;
}

void CoreAudioBackend::stop() {
    if (!running) return;
    AudioDeviceStop(opened.id, reinterpret_cast<AudioDeviceIOProcID>(procId));   // returns once the IOProc is done
    running = false;
}

void CoreAudioBackend::close() {
    stop();
    if (procId != nullptr) {
        removeListeners();
        AudioDeviceDestroyIOProcID(opened.id, reinterpret_cast<AudioDeviceIOProcID>(procId));
        procId = nullptr;
        if (restoreBufferSize != 0) setProperty(opened.id, kAudioDevicePropertyBufferFrameSize, UInt32{restoreBufferSize});
        restoreBufferSize = 0;
    }
    planar.clear();
    outputPointers.clear();
    auxPointers.clear();
    capacityFrames = 0;
}

AudioConfig CoreAudioBackend::config() const {
    AudioConfig current = negotiatedConfig;
    current.bufferSize  = currentBufferSize.load(std::memory_order_relaxed);
    current.sampleRate  = currentSampleRate.load(std::memory_order_relaxed);
    return current;
}

// From a period's start to its sound at the outputs: the buffer, then the
// device's safety offset and latency, and the stream's.
double CoreAudioBackend::outputLatencySeconds() const {
    const double rate = currentSampleRate.load(std::memory_order_relaxed);
    if (procId == nullptr || rate <= 0.0) return 0.0;
    UInt32 deviceLatency = 0, safetyOffset = 0, streamLatency = 0;
    getProperty(opened.id, kAudioDevicePropertyLatency, deviceLatency, kAudioObjectPropertyScopeOutput);
    getProperty(opened.id, kAudioDevicePropertySafetyOffset, safetyOffset, kAudioObjectPropertyScopeOutput);
    AudioStreamID stream = 0;   // the first output stream's
    if (getProperty(opened.id, kAudioDevicePropertyStreams, stream, kAudioObjectPropertyScopeOutput) && stream != 0)
        getProperty(stream, kAudioStreamPropertyLatency, streamLatency);
    const std::uint32_t frames = currentBufferSize.load(std::memory_order_relaxed) + deviceLatency + safetyOffset +
                                 streamLatency;
    return frames / rate;
}

// --- notifications -----------------------------------------------------------------------

void CoreAudioBackend::addListeners() {
    for (const AudioObjectPropertySelector selector : kWatched) {
        const AudioObjectPropertyAddress where = address(selector);
        AudioObjectAddPropertyListener(opened.id, &where, &propertyListener, this);
    }
    listening = true;
}

void CoreAudioBackend::removeListeners() {
    if (!listening) return;
    for (const AudioObjectPropertySelector selector : kWatched) {
        const AudioObjectPropertyAddress where = address(selector);
        AudioObjectRemovePropertyListener(opened.id, &where, &propertyListener, this);
    }
    listening = false;
}

void CoreAudioBackend::onPropertyChanged(std::uint32_t selector) noexcept {
    switch (selector) {
        case kAudioDevicePropertyBufferFrameSize: {
            UInt32 frames = 0;
            if (getProperty(opened.id, kAudioDevicePropertyBufferFrameSize, frames))
                currentBufferSize.store(frames, std::memory_order_relaxed);
            break;
        }
        case kAudioDevicePropertyNominalSampleRate: {
            Float64 rate = 0.0;
            if (getProperty(opened.id, kAudioDevicePropertyNominalSampleRate, rate) &&
                rate != currentSampleRate.load(std::memory_order_relaxed)) {
                currentSampleRate.store(rate, std::memory_order_relaxed);
                rateChanged.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case kAudioDeviceProcessorOverload:
            overloads.fetch_add(1, std::memory_order_relaxed);
            break;
        case kAudioDevicePropertyDeviceIsAlive: {
            UInt32 alive = 1;
            if (!getProperty(opened.id, kAudioDevicePropertyDeviceIsAlive, alive) || alive == 0)
                lost.store(true, std::memory_order_relaxed);
            break;
        }
        default: break;
    }
}

// --- the IOProc (CoreAudio's real-time thread) -------------------------------------------

// No allocation, locks or system calls. The streams are the HAL's virtual
// format: Float32, interleaved within each stream.
void CoreAudioBackend::render(const void* nowStamp, void* outputBufferList) noexcept {
    const auto* const now    = static_cast<const AudioTimeStamp*>(nowStamp);
    auto* const       output = static_cast<AudioBufferList*>(outputBufferList);
    if (output == nullptr || output->mNumberBuffers == 0) return;

    const ::AudioBuffer& first = output->mBuffers[0];
    const std::uint32_t frames =
        first.mNumberChannels == 0 ? 0 : first.mDataByteSize / (first.mNumberChannels * sizeof(float));
    if (frames == 0 || frames > capacityFrames) {   // can't happen: the range's maximum was allocated
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
            std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
        return;
    }

    const std::int64_t   position = frameCounter.load(std::memory_order_relaxed);
    AudioCallbackContext ctx;
    ctx.frameCount     = frames;
    ctx.blockStartTime = now != nullptr && (now->mFlags & kAudioTimeStampHostTimeValid) != 0
                             ? hostTimeToNanoseconds(now->mHostTime)
                             : nowNanoseconds();
    ctx.time.sampleRate     = currentSampleRate.load(std::memory_order_relaxed);
    ctx.time.samplePosition = position;
    ctx.output = stagehand::AudioBuffer{outputPointers.data(), static_cast<std::uint32_t>(outputPointers.size()), frames};
    ctx.aux    = stagehand::AudioBuffer{auxPointers.data(), static_cast<std::uint32_t>(auxPointers.size()), frames};

    if (processor != nullptr) {
        processor->process(ctx);
    } else {
        for (float* channel : outputPointers) std::fill_n(channel, frames, 0.0f);
    }

    // Our outputs onto the device's channels, in order across its streams; any
    // channel beyond them is silent.
    std::uint32_t deviceChannel = 0;
    for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
        ::AudioBuffer&      buffer   = output->mBuffers[b];
        auto* const         samples  = static_cast<float*>(buffer.mData);
        const std::uint32_t channels = buffer.mNumberChannels;
        if (samples == nullptr || channels == 0) continue;
        const std::uint32_t bufferFrames = std::min<std::uint32_t>(frames, buffer.mDataByteSize / (channels * sizeof(float)));
        for (std::uint32_t c = 0; c < channels; ++c, ++deviceChannel) {
            if (deviceChannel < outputPointers.size()) {
                const float* source = outputPointers[deviceChannel];
                for (std::uint32_t f = 0; f < bufferFrames; ++f) samples[f * channels + c] = source[f];
            } else {
                for (std::uint32_t f = 0; f < bufferFrames; ++f) samples[f * channels + c] = 0.0f;
            }
        }
    }
    frameCounter.store(position + frames, std::memory_order_relaxed);
}

} // namespace stagehand
