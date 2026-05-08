#include "AudioFileLoader.h"

#include <juce_audio_formats/juce_audio_formats.h>

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

radium::AudioEncodeFunc make_juce_flac_encode_func() {
    return [](const radium::AudioBuffer& source) -> radium::EmbeddedAudioBlob {
        radium::EmbeddedAudioBlob blob;
        blob.sample_rate = source.sample_rate;
        blob.channels = source.channels;

        if (source.channels <= 0 || source.sample_rate <= 0 || source.samples.empty()) {
            return blob;
        }

        const auto frameCount = source.samples.size() / static_cast<std::size_t>(source.channels);

        // Deinterleave the radium::AudioBuffer into a JUCE buffer for the encoder.
        juce::AudioBuffer<float> juceBuffer(source.channels, static_cast<int>(frameCount));
        for (int frame = 0; frame < static_cast<int>(frameCount); ++frame) {
            for (int ch = 0; ch < source.channels; ++ch) {
                juceBuffer.setSample(ch, frame,
                    source.samples[static_cast<std::size_t>(frame) * source.channels + ch]);
            }
        }

        juce::MemoryBlock encoded;
        {
            juce::FlacAudioFormat flac;
            // FLAC writer expects an output stream; wrap a MemoryOutputStream
            // that will write into `encoded`. The writer takes ownership of
            // the stream pointer, so release after construction.
            auto rawStream = std::make_unique<juce::MemoryOutputStream>(encoded, false);
            std::unique_ptr<juce::AudioFormatWriter> writer(
                flac.createWriterFor(rawStream.get(),
                                     static_cast<double>(source.sample_rate),
                                     static_cast<unsigned int>(source.channels),
                                     24, // bit depth — FLAC supports up to 24
                                     {},
                                     0));

            if (writer != nullptr) {
                rawStream.release();  // writer now owns the stream
                writer->writeFromAudioSampleBuffer(juceBuffer, 0, juceBuffer.getNumSamples());
                writer.reset();  // flushes and closes the stream
            }
        }

        blob.bytes.assign(static_cast<const std::uint8_t*>(encoded.getData()),
                          static_cast<const std::uint8_t*>(encoded.getData()) + encoded.getSize());
        return blob;
    };
}

radium::AudioDecodeBytesFunc make_juce_flac_decode_bytes_func() {
    return [](const radium::EmbeddedAudioBlob& blob) -> radium::FixtureAudioDecodeResult {
        radium::FixtureAudioDecodeResult result;
        if (blob.bytes.empty()) {
            result.diagnostics.push_back("Embedded audio blob is empty.");
            return result;
        }

        // JUCE wants a heap-allocated InputStream — the reader takes ownership.
        auto inputStream = std::make_unique<juce::MemoryInputStream>(
            blob.bytes.data(), blob.bytes.size(), false);

        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatReader> reader(
            flac.createReaderFor(inputStream.get(), false));

        if (reader == nullptr) {
            result.diagnostics.push_back("Embedded audio could not be decoded.");
            return result;
        }
        inputStream.release();  // reader now owns the stream

        const int numChannels = static_cast<int>(reader->numChannels);
        const auto numSamples = static_cast<int>(reader->lengthInSamples);

        if (numSamples == 0 || numChannels == 0) {
            result.diagnostics.push_back("Embedded audio blob contained no samples.");
            return result;
        }

        juce::AudioBuffer<float> juceBuffer(numChannels, numSamples);
        reader->read(&juceBuffer, 0, numSamples, 0, true, true);

        result.audio.sample_rate = static_cast<int>(reader->sampleRate);
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
