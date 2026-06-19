#pragma once
#include <pjsua2.hpp>
#include <string>
#include <mutex>
#include <memory>
#include <queue>
#include <chrono>
#include <vector>
#include <deque>
#include <future>
#include <iomanip>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

class AudioMixer {
private:
    size_t sample_rate;  // Sample rate in Hz
    static constexpr size_t FRAME_DURATION_MS = 20;  // Standard PJSIP frame duration
    size_t samples_per_frame;  // Calculated based on sample rate
    static constexpr size_t MAX_BUFFER_SIZE = 50;  // Maximum number of frames to buffer
    static constexpr size_t MAX_SILENCE_FRAMES = 250;  // Maximum number of silent frames before considering stream inactive

    struct AudioFrame {
        std::vector<int16_t> samples;
        std::chrono::steady_clock::time_point timestamp;
        bool is_silence;
    };

    std::mutex mix_mutex;
    std::deque<AudioFrame> agent_buffer;
    std::deque<AudioFrame> user_buffer;
    std::vector<int16_t> audio_buffer;  // Mixed audio buffer
    std::vector<int16_t> user_only_buffer;  // User-only audio buffer
    std::vector<int16_t> mix_buffer;
    
    double agent_gain = 1.0;
    double user_gain = 1.0;

    size_t agent_silence_count = 0;
    size_t user_silence_count = 0;
    bool agent_stream_active = false;
    bool user_stream_active = false;
    std::chrono::steady_clock::time_point last_agent_frame;
    std::chrono::steady_clock::time_point last_user_frame;
    std::string mixed_audio_folder = "sip_recordings";
    std::string user_only_audio_folder = "sip_user_recordings";

public:
    explicit AudioMixer(size_t sample_rate = 8000);
    const std::vector<int16_t>& getAudioBuffer() const { return audio_buffer; }
    const std::vector<int16_t>& getUserOnlyBuffer() const { return user_only_buffer; }
    void addFrame(const pj::MediaFrame& frame, bool is_agent);
    void clearBuffers();  // drop all accumulated + pending audio (used at human transfer)

private:
    void processBuffers();
    void mixFrames(const std::vector<int16_t>& agent_samples, 
                  const std::vector<int16_t>& user_samples);
    void writeSingleStream(const std::vector<int16_t>& samples);
    void writeUserOnlyStream(const std::vector<int16_t>& samples);
    bool isFrameSilent(const std::vector<uint8_t>& frame_data);
    void checkStreamActivity();
};

class R2Uploader {
private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    std::vector<uint8_t> convertToWav(const std::vector<int16_t>& raw_data, size_t sample_rate);

public:
    static R2Uploader& getInstance(size_t sample_rate = 8000) {
        static R2Uploader instance;
        return instance;
    }

    // Perform an S3 PUT using a caller-owned CURL handle. The handle is
    // NOT init'd or cleaned up here — UploadManager keeps one per worker
    // so curl's connection cache reuses TCP+TLS across uploads.
    bool uploadWithHandle(CURL* curl,
                          const std::string& call_id,
                          const std::vector<uint8_t>& wav_data,
                          const std::string& folder,
                          int max_retries = 3);

    std::future<bool> uploadAsync(const std::string& call_id, const std::vector<int16_t>& raw_data, size_t sample_rate, const std::string& folder = "sip_recordings");

    // Persist raw PCM as a local .wav file (so recordings are inspectable even
    // when the S3 upload is unavailable, e.g. dummy creds). Returns true on success.
    bool writeWavToFile(const std::string& path, const std::vector<int16_t>& raw_data, size_t sample_rate);

private:
    R2Uploader() = default;
    R2Uploader(const R2Uploader&) = delete;
    R2Uploader& operator=(const R2Uploader&) = delete;
};

class AudioRecorder {
private:
    std::string call_id;
    std::mutex mutex;
    bool is_recording;
    size_t sample_rate;
    std::unique_ptr<AudioMixer> mixer;
    std::future<bool> upload_future_mixed;  // Future for mixed audio upload
    std::future<bool> upload_future_user;   // Future for user-only audio upload

public:
    AudioRecorder(const std::string& call_id, size_t sample_rate = 8000);
    ~AudioRecorder();
    bool startRecording();
    void stopRecording();
    std::future<bool> uploadMixedToR2();
    std::future<bool> uploadUserOnlyToR2();
    void writeFrame(const pj::MediaFrame& frame, bool is_agent);
    void discardBufferedAudio();  // drop everything recorded so far (keep only audio after this point)
    bool isRecording() const { return is_recording; }
};
