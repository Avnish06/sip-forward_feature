#include "human_ringing.hpp"
#include <iostream>
#include <fstream>
#include <cstring>

RingingAudioProvider::RingingAudioProvider(int sample_rate)
    : current_position(0), loaded(false) {
}

bool RingingAudioProvider::loadRingingFile(int sample_rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    
    // Choose file based on sample rate
    std::string file_path;
    if (sample_rate == 8000) {
        // file_path = "assets/ringing-8k.wav";
        file_path = "assets/queue_music_8k.wav";
    } else if (sample_rate == 16000) {
        file_path = "assets/queue_music_16k.wav";
    } else {
        std::cerr << "Unsupported sample rate for ringing: " << sample_rate << std::endl;
        return false;
    }
    
    // Load WAV file (assumes 16-bit mono PCM)
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open ringing file: " << file_path << std::endl;
        return false;
    }
    
    // Skip WAV header (44 bytes for simple PCM)
    file.seekg(44);
    
    // Read audio data
    ringing_buffer.clear();
    int16_t sample;
    while (file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
        ringing_buffer.push_back(sample);
    }
    
    file.close();
    
    if (ringing_buffer.empty()) {
        std::cerr << "No audio data found in ringing file: " << file_path << std::endl;
        return false;
    }
    
    current_position = 0;
    loaded = true;
    
    std::cout << "Loaded ringing audio: " << ringing_buffer.size() << " samples from " << file_path << std::endl;
    return true;
}

void RingingAudioProvider::getRingingFrame(std::vector<uint8_t>& audio_data, size_t frame_size) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    
    if (!loaded || ringing_buffer.empty()) {
        audio_data.assign(frame_size, 0);
        return;
    }
    
    size_t samples_needed = frame_size / sizeof(int16_t);
    audio_data.clear();
    audio_data.reserve(frame_size);
    
    for (size_t i = 0; i < samples_needed; ++i) {
        // Get sample from buffer
        int16_t sample = ringing_buffer[current_position];
        
        // Convert to bytes (little-endian)
        uint8_t low_byte = static_cast<uint8_t>(sample & 0xFF);
        uint8_t high_byte = static_cast<uint8_t>((sample >> 8) & 0xFF);
        
        audio_data.push_back(low_byte);
        audio_data.push_back(high_byte);
        
        // Loop the ringing audio
        current_position++;
        if (current_position >= ringing_buffer.size()) {
            current_position = 0;
        }
    }
}

bool RingingAudioProvider::isReady() const {
    return loaded && !ringing_buffer.empty();
}

void RingingAudioProvider::reset() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    current_position = 0;
}