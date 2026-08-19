#include "audio/recorder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/mp3_encoder.hpp"

namespace sysrecord {
namespace {

constexpr ma_uint32 kCaptureChannels = 2;
constexpr ma_uint32 kRingBufferSeconds = 4;
constexpr ma_uint32 kEncoderChunkFrames = 9216;

[[nodiscard]] std::runtime_error miniaudioError(const char* operation, const ma_result result) {
    return std::runtime_error(std::string{operation} + ": " + ma_result_description(result));
}

}  // namespace

class Recorder::Impl final {
public:
    ~Impl() {
        try {
            stop();
        } catch (...) {
        }
    }

    void start(const std::filesystem::path& outputPath, const int bitrateKbps) {
        std::unique_lock stateLock{stateMutex_};
        if (state_ != RecorderState::Idle) {
            throw std::logic_error("A recording is already active.");
        }

        errorMessage_.clear();
        outputPath_ = outputPath;
        bytesWritten_.store(0, std::memory_order_relaxed);
        droppedFrames_.store(0, std::memory_order_relaxed);
        stopRequested_.store(false, std::memory_order_release);
        wakePending_.store(false, std::memory_order_release);

        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_loopback);
        deviceConfig.capture.format = ma_format_f32;
        deviceConfig.capture.channels = kCaptureChannels;
        deviceConfig.sampleRate = 0;
        deviceConfig.dataCallback = dataCallback;
        deviceConfig.pUserData = this;

        constexpr ma_backend backends[]{ma_backend_wasapi};
        const ma_result deviceResult =
            ma_device_init_ex(backends, static_cast<ma_uint32>(std::size(backends)), nullptr, &deviceConfig, &device_);
        if (deviceResult != MA_SUCCESS) {
            throw miniaudioError("Unable to initialize WASAPI loopback capture", deviceResult);
        }
        isDeviceInitialized_ = true;

        try {
            if (device_.sampleRate == 0 || device_.capture.channels == 0) {
                throw std::runtime_error("The default output device reported an invalid audio format.");
            }
            if (device_.sampleRate > std::numeric_limits<ma_uint32>::max() / kRingBufferSeconds) {
                throw std::runtime_error("The default output device sample rate is unsupported.");
            }

            const ma_uint32 bufferFrames = device_.sampleRate * kRingBufferSeconds;
            const ma_result ringResult =
                ma_pcm_rb_init(ma_format_f32, device_.capture.channels, bufferFrames, nullptr, nullptr, &ringBuffer_);
            if (ringResult != MA_SUCCESS) {
                throw miniaudioError("Unable to create the capture ring buffer", ringResult);
            }
            isRingBufferInitialized_ = true;

            encoder_ =
                std::make_unique<Mp3Encoder>(outputPath, device_.sampleRate, device_.capture.channels, bitrateKbps);
            encoderThread_ = std::thread{[this] { encodeLoop(); }};

            const ma_result startResult = ma_device_start(&device_);
            if (startResult != MA_SUCCESS) {
                throw miniaudioError("Unable to start WASAPI loopback capture", startResult);
            }
            isDeviceStarted_ = true;
            startedAt_ = std::chrono::steady_clock::now();
            pausedDuration_ = std::chrono::steady_clock::duration::zero();
            state_ = RecorderState::Recording;
        } catch (...) {
            stopRequested_.store(true, std::memory_order_release);
            signalEncoder();
            stateLock.unlock();
            if (encoderThread_.joinable()) {
                encoderThread_.join();
            }
            stateLock.lock();
            releaseResources();
            outputPath_.clear();
            throw;
        }
    }

    void stop() {
        std::unique_lock stateLock{stateMutex_};
        if (state_ == RecorderState::Idle && !isDeviceInitialized_) {
            return;
        }

        std::string stopError;
        if (isDeviceStarted_) {
            const ma_result stopResult = ma_device_stop(&device_);
            isDeviceStarted_ = false;
            if (stopResult != MA_SUCCESS) {
                stopError = miniaudioError("Unable to stop WASAPI loopback capture", stopResult).what();
            }
        }

        stopRequested_.store(true, std::memory_order_release);
        signalEncoder();
        stateLock.unlock();
        if (encoderThread_.joinable()) {
            encoderThread_.join();
        }
        stateLock.lock();

        if (encoder_ != nullptr) {
            bytesWritten_.store(encoder_->bytesWritten(), std::memory_order_relaxed);
        }
        if (stopError.empty()) {
            stopError = errorMessage_;
        }
        releaseResources();
        state_ = RecorderState::Idle;
        if (!stopError.empty()) {
            errorMessage_ = stopError;
            throw std::runtime_error(stopError);
        }
    }

    void pause() {
        std::scoped_lock stateLock{stateMutex_};
        if (state_ != RecorderState::Recording) {
            throw std::logic_error("Recording is not active.");
        }

        const ma_result result = ma_device_stop(&device_);
        if (result != MA_SUCCESS) {
            throw miniaudioError("Unable to pause WASAPI loopback capture", result);
        }
        isDeviceStarted_ = false;
        pausedAt_ = std::chrono::steady_clock::now();
        state_ = RecorderState::Paused;
    }

    void resume() {
        std::scoped_lock stateLock{stateMutex_};
        if (state_ != RecorderState::Paused) {
            throw std::logic_error("Recording is not paused.");
        }

        const ma_result result = ma_device_start(&device_);
        if (result != MA_SUCCESS) {
            throw miniaudioError("Unable to resume WASAPI loopback capture", result);
        }
        isDeviceStarted_ = true;
        pausedDuration_ += std::chrono::steady_clock::now() - pausedAt_;
        state_ = RecorderState::Recording;
    }

    [[nodiscard]] RecorderStatus status() const {
        std::scoped_lock stateLock{stateMutex_};
        RecorderStatus current;
        current.state = state_;
        current.outputPath = outputPath_;
        current.bytesWritten = bytesWritten_.load(std::memory_order_relaxed);
        current.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        current.errorMessage = errorMessage_;

        if (state_ == RecorderState::Recording) {
            current.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt_ - pausedDuration_);
        } else if (state_ == RecorderState::Paused) {
            current.elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(pausedAt_ - startedAt_ - pausedDuration_);
        }
        return current;
    }

private:
    static void dataCallback(ma_device* device, void*, const void* inputFrames, const ma_uint32 frameCount) {
        auto* recorder = static_cast<Impl*>(device->pUserData);
        recorder->writeCapturedFrames(inputFrames, frameCount);
    }

    void writeCapturedFrames(const void* inputFrames, const ma_uint32 frameCount) {
        ma_uint32 remainingFrames = frameCount;
        const auto* source = static_cast<const float*>(inputFrames);
        while (remainingFrames > 0) {
            ma_uint32 framesToWrite = remainingFrames;
            void* destination = nullptr;
            const ma_result acquireResult = ma_pcm_rb_acquire_write(&ringBuffer_, &framesToWrite, &destination);
            if (acquireResult != MA_SUCCESS || framesToWrite == 0) {
                droppedFrames_.fetch_add(remainingFrames, std::memory_order_relaxed);
                break;
            }

            const std::size_t sampleCount = static_cast<std::size_t>(framesToWrite) * device_.capture.channels;
            if (source != nullptr) {
                std::memcpy(destination, source, sampleCount * sizeof(float));
                source += sampleCount;
            } else {
                std::memset(destination, 0, sampleCount * sizeof(float));
            }
            if (ma_pcm_rb_commit_write(&ringBuffer_, framesToWrite) != MA_SUCCESS) {
                droppedFrames_.fetch_add(remainingFrames, std::memory_order_relaxed);
                break;
            }
            remainingFrames -= framesToWrite;
        }

        if (remainingFrames != frameCount) {
            signalEncoder();
        }
    }

    void signalEncoder() {
        if (!wakePending_.exchange(true, std::memory_order_acq_rel)) {
            encoderWake_.release();
        }
    }

    void encodeLoop() {
        try {
            for (;;) {
                encoderWake_.acquire();
                wakePending_.store(false, std::memory_order_release);

                drainRingBuffer();
                if (stopRequested_.load(std::memory_order_acquire) && ma_pcm_rb_available_read(&ringBuffer_) == 0) {
                    break;
                }
            }
            encoder_->finalize();
            bytesWritten_.store(encoder_->bytesWritten(), std::memory_order_relaxed);
        } catch (const std::exception& error) {
            std::scoped_lock stateLock{stateMutex_};
            errorMessage_ = error.what();
            state_ = RecorderState::Error;
            stopRequested_.store(true, std::memory_order_release);
        }
    }

    void drainRingBuffer() {
        for (;;) {
            ma_uint32 framesToRead = std::min(ma_pcm_rb_available_read(&ringBuffer_), kEncoderChunkFrames);
            if (framesToRead == 0) {
                return;
            }

            void* source = nullptr;
            const ma_result acquireResult = ma_pcm_rb_acquire_read(&ringBuffer_, &framesToRead, &source);
            if (acquireResult != MA_SUCCESS) {
                throw miniaudioError("Unable to read captured audio", acquireResult);
            }
            if (framesToRead == 0) {
                return;
            }

            const std::size_t sampleCount = static_cast<std::size_t>(framesToRead) * device_.capture.channels;
            encoder_->encode(std::span{static_cast<const float*>(source), sampleCount}, framesToRead);
            const ma_result commitResult = ma_pcm_rb_commit_read(&ringBuffer_, framesToRead);
            if (commitResult != MA_SUCCESS) {
                throw miniaudioError("Unable to release captured audio", commitResult);
            }
            bytesWritten_.store(encoder_->bytesWritten(), std::memory_order_relaxed);
        }
    }

    void releaseResources() {
        encoder_.reset();
        if (isRingBufferInitialized_) {
            ma_pcm_rb_uninit(&ringBuffer_);
            isRingBufferInitialized_ = false;
        }
        if (isDeviceInitialized_) {
            ma_device_uninit(&device_);
            isDeviceInitialized_ = false;
        }
        isDeviceStarted_ = false;
        stopRequested_.store(false, std::memory_order_release);
        wakePending_.store(false, std::memory_order_release);
    }

    mutable std::mutex stateMutex_;
    ma_device device_{};
    ma_pcm_rb ringBuffer_{};
    std::unique_ptr<Mp3Encoder> encoder_;
    std::thread encoderThread_;
    std::binary_semaphore encoderWake_{0};
    std::atomic<bool> wakePending_{};
    std::atomic<bool> stopRequested_{};
    std::atomic<std::uint64_t> bytesWritten_{};
    std::atomic<std::uint64_t> droppedFrames_{};
    std::filesystem::path outputPath_;
    std::string errorMessage_;
    std::chrono::steady_clock::time_point startedAt_{};
    std::chrono::steady_clock::time_point pausedAt_{};
    std::chrono::steady_clock::duration pausedDuration_{};
    RecorderState state_{RecorderState::Idle};
    bool isDeviceInitialized_{};
    bool isRingBufferInitialized_{};
    bool isDeviceStarted_{};
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}

Recorder::~Recorder() = default;

void Recorder::start(const std::filesystem::path& outputPath, const int bitrateKbps) {
    impl_->start(outputPath, bitrateKbps);
}

void Recorder::stop() {
    impl_->stop();
}

void Recorder::pause() {
    impl_->pause();
}

void Recorder::resume() {
    impl_->resume();
}

RecorderStatus Recorder::status() const {
    return impl_->status();
}

}  // namespace sysrecord
