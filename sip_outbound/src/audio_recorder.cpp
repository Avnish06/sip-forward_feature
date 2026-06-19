#include "../include/audio_recorder.hpp"
#include "../include/upload_manager.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <openssl/md5.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

// Add these declarations near the top of audio_recorder.cpp or in a header
static const std::string mixed_audio_folder = "sip_recordings";
static const std::string user_only_audio_folder = "sip_user_recordings"; 

// Read configuration from the process environment, which docker-compose
// injects from the host .env (consistent with the rest of the service).
std::string getEnvVar(const char* key) {
    const char* value = std::getenv(key);
    if (!value) {
        std::cerr << "[Config] Error: " << key << " is not set in the environment" << std::endl;
        return "";
    }
    return value;
}

// Global AWS S3 configuration
const std::string AWS_REGION = getEnvVar("AWS_REGION");
const std::string AWS_ACCESS_KEY = getEnvVar("AWS_ACCESS_KEY");
const std::string AWS_SECRET_KEY = getEnvVar("AWS_SECRET_KEY");
const std::string AWS_BUCKET_NAME = getEnvVar("AWS_BUCKET_NAME");

// Validate AWS S3 configuration
static bool validateR2Config() {
    bool valid = true;
    if (AWS_REGION.empty()) {
        std::cerr << "[Config] Error: AWS_REGION is empty" << std::endl;
        valid = false;
    }
    if (AWS_ACCESS_KEY.empty()) {
        std::cerr << "[Config] Error: AWS_ACCESS_KEY is empty" << std::endl;
        valid = false;
    }
    if (AWS_SECRET_KEY.empty()) {
        std::cerr << "[Config] Error: AWS_SECRET_KEY is empty" << std::endl;
        valid = false;
    }
    if (AWS_BUCKET_NAME.empty()) {
        std::cerr << "[Config] Error: AWS_BUCKET_NAME is empty" << std::endl;
        valid = false;
    }
    return valid;
}

AudioMixer::AudioMixer(size_t sample_rate) : sample_rate(sample_rate) {
    samples_per_frame = (sample_rate * FRAME_DURATION_MS) / 1000;
    mix_buffer.resize(samples_per_frame);
    last_agent_frame = std::chrono::steady_clock::now();
    last_user_frame = std::chrono::steady_clock::now();
}

AudioRecorder::AudioRecorder(const std::string& call_id, size_t sample_rate)
    : call_id(call_id), is_recording(false), sample_rate(sample_rate) {
    mixer = std::make_unique<AudioMixer>(sample_rate);
}

bool AudioMixer::isFrameSilent(const std::vector<uint8_t>& frame_data) {
    if (frame_data.empty()) return true;
    
    const int16_t* samples = reinterpret_cast<const int16_t*>(frame_data.data());
    size_t num_samples = frame_data.size() / 2;
    
    // Check if all samples are below a threshold
    constexpr int16_t SILENCE_THRESHOLD = 50;  // Adjust this value if needed
    for (size_t i = 0; i < num_samples; ++i) {
        if (std::abs(samples[i]) > SILENCE_THRESHOLD) {
            return false;
        }
    }
    return true;
}

void AudioMixer::checkStreamActivity() {
    auto now = std::chrono::steady_clock::now();
    
    // Check agent stream
    if (agent_silence_count > MAX_SILENCE_FRAMES ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_agent_frame).count() > 1000) {
        agent_stream_active = false;
    }
    
    // // Check user stream
    // if (user_silence_count > MAX_SILENCE_FRAMES ||
    //     std::chrono::duration_cast<std::chrono::milliseconds>(now - last_user_frame).count() > 1000) {
    //     user_stream_active = false;
    // }
}

void AudioMixer::addFrame(const pj::MediaFrame& frame, bool is_agent) {
    if (frame.buf.empty()) return;

    std::lock_guard<std::mutex> lock(mix_mutex);
    
    bool is_silent = isFrameSilent(frame.buf);
    
    // Update silence counters and timestamps
    if (is_agent) {
        agent_silence_count = is_silent ? agent_silence_count + 1 : 0;
        last_agent_frame = std::chrono::steady_clock::now();
        agent_stream_active = true;
    } else {
        user_silence_count = is_silent ? user_silence_count + 1 : 0;
        last_user_frame = std::chrono::steady_clock::now();
        user_stream_active = true;
    }
    
    // Convert input buffer to int16_t samples
    std::vector<int16_t> samples;
    samples.resize(frame.buf.size() / 2);
    memcpy(samples.data(), frame.buf.data(), frame.buf.size());

    AudioFrame audio_frame{
        std::move(samples),
        std::chrono::steady_clock::now(),
        is_silent
    };

    auto& buffer = is_agent ? agent_buffer : user_buffer;
    buffer.push_back(std::move(audio_frame));

    // Prevent buffer overflow
    while (buffer.size() > MAX_BUFFER_SIZE) {
        buffer.pop_front();
    }

    checkStreamActivity();
    processBuffers();
}

void AudioMixer::processBuffers() {
    // Process user frames for user-only recording as we consume them
    // This ensures each frame is only written once to user_only_buffer
    
    // If only one stream is active, process it directly
    if (agent_stream_active && !user_stream_active) {
        while (!agent_buffer.empty()) {
            writeSingleStream(agent_buffer.front().samples);
            agent_buffer.pop_front();
        }
        return;
    }
    
    if (!agent_stream_active && user_stream_active) {
        while (!user_buffer.empty()) {
            // Write to both buffers when only user is active
            writeUserOnlyStream(user_buffer.front().samples);
            writeSingleStream(user_buffer.front().samples);
            user_buffer.pop_front();
        }
        return;
    }

    // Both streams active - mix them for mixed recording
    while (!agent_buffer.empty() && !user_buffer.empty()) {
        auto& agent_frame = agent_buffer.front();
        auto& user_frame = user_buffer.front();

        // Always write user frame to user-only buffer
        writeUserOnlyStream(user_frame.samples);

        // Check timestamps to maintain synchronization
        auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            agent_frame.timestamp - user_frame.timestamp).count();

        // If frames are too far apart in time, process the older frame alone
        if (std::abs(time_diff) > FRAME_DURATION_MS * 2) {
            if (time_diff > 0) {
                writeSingleStream(user_frame.samples);
                user_buffer.pop_front();
            } else {
                writeSingleStream(agent_frame.samples);
                agent_buffer.pop_front();
            }
            continue;
        }

        // Mix the frames for mixed recording
        mixFrames(agent_frame.samples, user_frame.samples);
        agent_buffer.pop_front();
        user_buffer.pop_front();
    }
}

void AudioMixer::writeSingleStream(const std::vector<int16_t>& samples) {
    audio_buffer.insert(audio_buffer.end(), samples.begin(), samples.end());
}

void AudioMixer::writeUserOnlyStream(const std::vector<int16_t>& samples) {
    user_only_buffer.insert(user_only_buffer.end(), samples.begin(), samples.end());
}

void AudioMixer::clearBuffers() {
    std::lock_guard<std::mutex> lock(mix_mutex);
    agent_buffer.clear();
    user_buffer.clear();
    audio_buffer.clear();
    user_only_buffer.clear();
    agent_silence_count = 0;
    user_silence_count = 0;
    agent_stream_active = false;
    user_stream_active = false;
}

void AudioMixer::mixFrames(const std::vector<int16_t>& agent_samples, 
                          const std::vector<int16_t>& user_samples) {
    size_t num_samples = std::min(agent_samples.size(), user_samples.size());
    
    for (size_t i = 0; i < num_samples; i++) {
        // Apply gain and mix
        int32_t agent_sample = static_cast<int32_t>(agent_samples[i] * agent_gain);
        int32_t user_sample = static_cast<int32_t>(user_samples[i] * user_gain);
        
        // Mix samples with overflow protection
        int32_t mixed = agent_sample + user_sample;
        mixed = std::min(std::max(mixed, static_cast<int32_t>(INT16_MIN)), 
                       static_cast<int32_t>(INT16_MAX));
        
        mix_buffer[i] = static_cast<int16_t>(mixed);
    }

    // Add mixed buffer to audio buffer
    audio_buffer.insert(audio_buffer.end(), mix_buffer.begin(), mix_buffer.begin() + num_samples);
}

std::vector<uint8_t> R2Uploader::convertToWav(const std::vector<int16_t>& raw_data, size_t sample_rate) {
    // WAV header structure
    struct WavHeader {
        // RIFF chunk
        char riff_header[4] = {'R', 'I', 'F', 'F'};
        uint32_t wav_size;
        char wave_header[4] = {'W', 'A', 'V', 'E'};
        
        // fmt sub-chunk
        char fmt_header[4] = {'f', 'm', 't', ' '};
        uint32_t fmt_chunk_size = 16;
        uint16_t audio_format = 1; // PCM
        uint16_t num_channels = 1; // Mono
        uint32_t sample_rate;
        uint32_t byte_rate;        // sample_rate * num_channels * bytes_per_sample
        uint16_t block_align;      // num_channels * bytes_per_sample
        uint16_t bit_depth = 16;
        
        // data sub-chunk
        char data_header[4] = {'d', 'a', 't', 'a'};
        uint32_t data_bytes;
    } header;

    // Set sample rate dependent fields
    header.sample_rate = sample_rate;
    header.block_align = header.num_channels * (header.bit_depth / 8);
    header.byte_rate = header.sample_rate * header.block_align;

    // Calculate sizes
    header.data_bytes = raw_data.size() * sizeof(int16_t);
    header.wav_size = header.data_bytes + sizeof(WavHeader) - 8;

    // Create WAV data buffer
    std::vector<uint8_t> wav_data(sizeof(WavHeader) + header.data_bytes);
    
    // Copy header
    memcpy(wav_data.data(), &header, sizeof(WavHeader));
    
    // Copy audio data
    memcpy(wav_data.data() + sizeof(WavHeader), 
           raw_data.data(), 
           header.data_bytes);

    return wav_data;
}

// CURL callbacks
size_t R2Uploader::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

struct UploadContext {
    const uint8_t* data;
    size_t length;
    size_t offset;
};

static size_t ReadCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* context = static_cast<UploadContext*>(userdata);
    size_t bytes_to_copy = std::min(size * nitems, context->length - context->offset);
    
    if (bytes_to_copy > 0) {
        memcpy(buffer, context->data + context->offset, bytes_to_copy);
        context->offset += bytes_to_copy;
        return bytes_to_copy;
    }
    
    return 0;  // Signal end of data
}

// Helper functions for S3 authentication
std::string getAmzDate() {
    char buf[128];
    time_t now = time(nullptr);
    strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", gmtime(&now));
    return std::string(buf);
}

std::string getDateStamp() {
    char buf[128];
    time_t now = time(nullptr);
    strftime(buf, sizeof(buf), "%Y%m%d", gmtime(&now));
    return std::string(buf);
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char* digest = HMAC(EVP_sha256(),
                                key.c_str(), key.length(),
                                reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
                                nullptr, nullptr);
    
    std::stringstream ss;
    for(int i = 0; i < EVP_MD_size(EVP_sha256()); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return ss.str();
}

std::string getSigningKey(const std::string& secret_key, 
                         const std::string& date_stamp,
                         const std::string& region,
                         const std::string& service) {
    std::string k_secret = "AWS4" + secret_key;
    
    unsigned char k_date[EVP_MAX_MD_SIZE];
    unsigned int k_date_len;
    HMAC(EVP_sha256(), k_secret.c_str(), k_secret.length(),
         reinterpret_cast<const unsigned char*>(date_stamp.c_str()), date_stamp.length(),
         k_date, &k_date_len);
    
    unsigned char k_region[EVP_MAX_MD_SIZE];
    unsigned int k_region_len;
    HMAC(EVP_sha256(), k_date, k_date_len,
         reinterpret_cast<const unsigned char*>(region.c_str()), region.length(),
         k_region, &k_region_len);
    
    unsigned char k_service[EVP_MAX_MD_SIZE];
    unsigned int k_service_len;
    HMAC(EVP_sha256(), k_region, k_region_len,
         reinterpret_cast<const unsigned char*>(service.c_str()), service.length(),
         k_service, &k_service_len);
    
    unsigned char k_signing[EVP_MAX_MD_SIZE];
    unsigned int k_signing_len;
    std::string aws4_request = "aws4_request";
    HMAC(EVP_sha256(), k_service, k_service_len,
         reinterpret_cast<const unsigned char*>(aws4_request.c_str()), aws4_request.length(),
         k_signing, &k_signing_len);

    // Return the raw bytes
    return std::string(reinterpret_cast<char*>(k_signing), k_signing_len);
}

std::string sha256(const std::string& str) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, str.c_str(), str.length());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string base64_encode(const unsigned char* input, int length) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);  // No newlines
    BIO_write(bio, input, length);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);

    return result;
}

std::string md5_base64(const std::vector<uint8_t>& data) {
    unsigned char md[MD5_DIGEST_LENGTH];
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data.data(), data.size());
    MD5_Final(md, &ctx);
    
    return base64_encode(md, MD5_DIGEST_LENGTH);
}

std::string getCanonicalRequest(const std::string& method,
                              const std::string& canonical_uri,
                              const std::string& host,
                              const std::string& amz_date,
                              const std::string& content_md5) {
    // Each line must end with a single \n, no extra newlines
    std::string canonical_request = 
        method + "\n" +
        canonical_uri + "\n" +
        "" + "\n" +  // Empty query string
        "content-md5:" + content_md5 + "\n" +
        "host:" + host + "\n" +
        "x-amz-content-sha256:UNSIGNED-PAYLOAD\n" +
        "x-amz-date:" + amz_date + "\n" +
        "" + "\n" +  // Empty line after headers
        "content-md5;host;x-amz-content-sha256;x-amz-date\n" +
        "UNSIGNED-PAYLOAD";  // No newline at end of last line

    std::istringstream iss(canonical_request);

    return canonical_request;
}


std::string hexDump(const unsigned char* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

bool R2Uploader::uploadWithHandle(CURL* curl,
                                  const std::string& call_id,
                                  const std::vector<uint8_t>& wav_data,
                                  const std::string& folder,
                                  int max_retries) {
    if (!curl) return false;

    std::string host = "s3." + AWS_REGION + ".amazonaws.com";
    std::string canonical_uri = "/" + AWS_BUCKET_NAME + "/" + folder + "/" + call_id + ".wav";
    std::string url = "https://" + host + canonical_uri;

    // Calculate Content-MD5
    std::string content_md5 = md5_base64(wav_data);

    struct curl_slist* headers = nullptr;
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        if (headers) {
            curl_slist_free_all(headers);
            headers = nullptr;
        }
        // Reset request-level options between attempts; the underlying
        // TCP+TLS connection in the handle's cache stays warm.
        curl_easy_reset(curl);

        std::string amz_date = getAmzDate();
        std::string date_stamp = amz_date.substr(0, 8);
        
        std::string canonical_request = getCanonicalRequest(
            "PUT", canonical_uri, host, amz_date, content_md5);

        // Calculate request hash
        std::string canonical_request_hash = sha256(canonical_request);
        // Create string to sign
        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" +
            amz_date + "\n" +
            date_stamp + "/" + AWS_REGION + "/s3/aws4_request\n" +
            canonical_request_hash;

        std::string signing_key = getSigningKey(AWS_SECRET_KEY, date_stamp, AWS_REGION, "s3");

        // Calculate final signature with raw bytes
        unsigned char signature_bytes[EVP_MAX_MD_SIZE];
        unsigned int signature_len;
        HMAC(EVP_sha256(), signing_key.c_str(), signing_key.length(),
             reinterpret_cast<const unsigned char*>(string_to_sign.c_str()), string_to_sign.length(),
             signature_bytes, &signature_len);

        // Convert signature to hex
        std::stringstream signature_ss;
        for(unsigned int i = 0; i < signature_len; i++) {
            signature_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(signature_bytes[i]);
        }
        std::string signature = signature_ss.str();

        // Build authorization header
        std::string credential = AWS_ACCESS_KEY + "/" + date_stamp + "/" + AWS_REGION + "/s3/aws4_request";
        std::string auth_header = 
            "AWS4-HMAC-SHA256 " 
            "Credential=" + credential + ", "
            "SignedHeaders=content-md5;host;x-amz-content-sha256;x-amz-date, "
            "Signature=" + signature;

        // Add headers in same order as boto3
        headers = curl_slist_append(headers, ("Content-MD5: " + content_md5).c_str());
        headers = curl_slist_append(headers, ("Host: " + host).c_str());
        headers = curl_slist_append(headers, "X-Amz-Content-SHA256: UNSIGNED-PAYLOAD");
        headers = curl_slist_append(headers, ("X-Amz-Date: " + amz_date).c_str());
        headers = curl_slist_append(headers, ("Authorization: " + auth_header).c_str());
        headers = curl_slist_append(headers, ("Content-Length: " + std::to_string(wav_data.size())).c_str());
        headers = curl_slist_append(headers, "Expect: 100-continue");


        // Configure CURL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        // Setup upload context
        UploadContext context{wav_data.data(), wav_data.size(), 0};
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &context);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(wav_data.size()));

        std::string response_data;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

        CURLcode res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

            if (response_code != 200) {
                std::cerr << "Upload failed with response code: " << response_code << std::endl;
            }

            if (response_code == 200) {
                curl_slist_free_all(headers);
                return true;
            }
        }

        if (attempt < max_retries) {
            std::this_thread::sleep_for(std::chrono::seconds(2 << (attempt - 1)));
        }
    }

    if (headers) curl_slist_free_all(headers);
    return false;
}

std::future<bool> R2Uploader::uploadAsync(const std::string& call_id,
                                        const std::vector<int16_t>& raw_data,
                                        size_t sample_rate,
                                        const std::string& folder) {
    // WAV-encode on the caller thread (microseconds) and hand the bytes
    // to the upload pool. The pool owns a long-lived curl handle per
    // worker, so TLS to S3 is reused across uploads.
    std::vector<uint8_t> wav_data = convertToWav(raw_data, sample_rate);
    return UploadManager::instance().submit(call_id, std::move(wav_data), folder);
}

bool R2Uploader::writeWavToFile(const std::string& path, const std::vector<int16_t>& raw_data, size_t sample_rate) {
    try {
        std::vector<uint8_t> wav_data = convertToWav(raw_data, sample_rate);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "[AudioRecorder] Could not open local wav path: " << path << std::endl;
            return false;
        }
        out.write(reinterpret_cast<const char*>(wav_data.data()),
                  static_cast<std::streamsize>(wav_data.size()));
        std::cout << "[AudioRecorder] Wrote local recording: " << path
                  << " (" << wav_data.size() << " bytes)" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[AudioRecorder] Exception writing local wav: " << e.what() << std::endl;
        return false;
    }
}

AudioRecorder::~AudioRecorder() {
    stopRecording();
}

bool AudioRecorder::startRecording() {
    std::lock_guard<std::mutex> lock(mutex);
    if (is_recording) {
        return true;
    }

    mixer = std::make_unique<AudioMixer>(sample_rate);
    is_recording = true;
    return true;
}

void AudioRecorder::stopRecording() {
    std::lock_guard<std::mutex> lock(mutex);
    if (is_recording) {
        is_recording = false;
        
        if (!mixer) {
            std::cerr << "[AudioRecorder] Error: Mixer is null for call: " << call_id << std::endl;
            return;
        }
        
        const auto& mixed_buffer = mixer->getAudioBuffer();
        const auto& user_buffer = mixer->getUserOnlyBuffer();
        
        if (mixed_buffer.empty() && user_buffer.empty()) {
            std::cerr << "[AudioRecorder] Warning: Empty audio buffers for call: " << call_id << std::endl;
            mixer.reset();
            return;
        }
        
        try {
            // Upload mixed audio to sip_recordings folder
            if (!mixed_buffer.empty()) {
                auto mixed_future = R2Uploader::getInstance().uploadAsync(call_id, mixed_buffer, sample_rate, mixed_audio_folder);
                upload_future_mixed = std::move(mixed_future);
                std::cout << "[AudioRecorder] Started mixed audio upload for call: " << call_id << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[AudioRecorder] Exception during mixed audio upload initiation: " << e.what() << std::endl;
        }

        try {
            // Upload user-only audio to user_recordings folder
            if (!user_buffer.empty()) {
                auto user_future = R2Uploader::getInstance().uploadAsync(call_id, user_buffer, sample_rate, user_only_audio_folder);
                upload_future_user = std::move(user_future);
                std::cout << "[AudioRecorder] Started user-only audio upload for call: " << call_id << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[AudioRecorder] Exception during user audio upload initiation: " << e.what() << std::endl;
        }

        // Also persist local copies so recordings are inspectable without S3
        // (the ./cores volume is mounted to the host at SipTrunking/cores).
        try {
            const std::string local_dir = "/cores/recordings";
            std::error_code ec;
            std::filesystem::create_directories(local_dir, ec);
            if (!mixed_buffer.empty()) {
                R2Uploader::getInstance().writeWavToFile(local_dir + "/" + call_id + "_mixed.wav", mixed_buffer, sample_rate);
            }
            if (!user_buffer.empty()) {
                R2Uploader::getInstance().writeWavToFile(local_dir + "/" + call_id + "_user.wav", user_buffer, sample_rate);
            }
        } catch (const std::exception& e) {
            std::cerr << "[AudioRecorder] Exception writing local recordings: " << e.what() << std::endl;
        }

        mixer.reset();
    }
}

std::future<bool> AudioRecorder::uploadMixedToR2() {
    if (!mixer) {
        std::cerr << "[AudioRecorder] Error: Cannot upload mixed audio, mixer is null" << std::endl;
        return std::async(std::launch::async, []() { return false; });
    }
    
    const auto& buffer = mixer->getAudioBuffer();
    return R2Uploader::getInstance().uploadAsync(call_id, buffer, sample_rate, mixed_audio_folder);
}

std::future<bool> AudioRecorder::uploadUserOnlyToR2() {
    if (!mixer) {
        std::cerr << "[AudioRecorder] Error: Cannot upload user-only audio, mixer is null" << std::endl;
        return std::async(std::launch::async, []() { return false; });
    }
    
    const auto& buffer = mixer->getUserOnlyBuffer();
    return R2Uploader::getInstance().uploadAsync(call_id, buffer, sample_rate, user_only_audio_folder);
}

void AudioRecorder::writeFrame(const pj::MediaFrame& frame, bool is_agent) {
    std::lock_guard<std::mutex> lock(mutex);
    if (is_recording && mixer && !frame.buf.empty()) {
        mixer->addFrame(frame, is_agent);
    }
}

void AudioRecorder::discardBufferedAudio() {
    std::lock_guard<std::mutex> lock(mutex);
    if (is_recording && mixer) {
        mixer->clearBuffers();
        std::cout << "[AudioRecorder] Discarded pre-transfer audio; recording post-transfer only for call: " << call_id << std::endl;
    }
}
