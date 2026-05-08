#include "AudioFileLoader.h"

namespace triggerfish {

radium::AudioDecodeFunc make_juce_audio_decode_func() {
    return [](const std::filesystem::path& audio_path,
              const std::filesystem::path& /*working_directory*/) -> radium::FixtureAudioDecodeResult {
        radium::FixtureAudioDecodeResult result;

        juce::File file(audio_path.string());
        if (!file.existsAsFile()) {
            result.diagnostics.push_back("File not found: " + audio_path.string());
            return result;
        }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(file));

        if (reader == nullptr) {
            result.diagnostics.push_back("Unsupported audio format: " + audio_path.string());
            return result;
        }

        const int numChannels = static_cast<int>(reader->numChannels);
        const auto numSamples = static_cast<int>(reader->lengthInSamples);
        const int sampleRate = static_cast<int>(reader->sampleRate);

        if (numSamples == 0 || numChannels == 0) {
            result.diagnostics.push_back("Empty audio file: " + audio_path.string());
            return result;
        }

        // Read into a JUCE buffer
        juce::AudioBuffer<float> juceBuffer(numChannels, numSamples);
        reader->read(&juceBuffer, 0, numSamples, 0, true, true);

        // Convert to radium::AudioBuffer (interleaved float format)
        result.audio.sample_rate = sampleRate;
        result.audio.channels = numChannels;
        result.audio.samples.resize(static_cast<std::size_t>(numSamples) * numChannels);

        for (int frame = 0; frame < numSamples; ++frame) {
            for (int ch = 0; ch < numChannels; ++ch) {
                result.audio.samples[static_cast<std::size_t>(frame) * numChannels + ch] =
                    juceBuffer.getSample(ch, frame);
            }
        }

        result.success = true;
        return result;
    };
}

}  // namespace triggerfish
