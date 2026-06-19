#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <cstdint>

class RingingAudioProvider {
private:
    std::vector<int16_t> ringing_buffer;
    size_t current_position;
    bool loaded;
    std::mutex buffer_mutex;

public:
    explicit RingingAudioProvider(int sample_rate);
    bool loadRingingFile(int sample_rate);  // Loads ringing-8k.wav or ringing-16k.wav
    void getRingingFrame(std::vector<uint8_t>& audio_data, size_t frame_size);
    bool isReady() const;
    void reset();
};