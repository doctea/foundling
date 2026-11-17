#pragma once

#include <Arduino.h>
#include <cstring>

#if defined(PICO_RP2040) || defined(PICO_RP2350)
    #include <pico/critical_section.h>
    #include <pico/platform.h>
#elif !defined(ARDUINO)
    #include <mutex>
#endif

#ifndef FOUNDLING_SERIAL_QUEUE_CAPACITY
    #define FOUNDLING_SERIAL_QUEUE_CAPACITY 2048
#endif

namespace foundling {

class SerialOutputLock {
public:
    SerialOutputLock() {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            critical_section_init(&section_);
        #endif
    }
    ~SerialOutputLock() {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            critical_section_deinit(&section_);
        #endif
    }
    SerialOutputLock(const SerialOutputLock&) = delete;
    SerialOutputLock& operator=(const SerialOutputLock&) = delete;

private:
    friend class SerialOutputGuard;
    #if defined(PICO_RP2040) || defined(PICO_RP2350)
        critical_section_t section_{};
    #elif !defined(ARDUINO)
        std::mutex mutex_;
    #endif
};

class SerialOutputGuard {
public:
    explicit SerialOutputGuard(SerialOutputLock& lock) : lock_(lock) {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            critical_section_enter_blocking(&lock_.section_);
        #elif defined(__arm__) || defined(__thumb__)
            asm volatile("mrs %0, primask\ncpsid i" : "=r"(state_) :: "memory");
        #elif !defined(ARDUINO)
            lock_.mutex_.lock();
        #endif
    }
    ~SerialOutputGuard() {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            critical_section_exit(&lock_.section_);
        #elif defined(__arm__) || defined(__thumb__)
            asm volatile("msr primask, %0" :: "r"(state_) : "memory");
        #elif !defined(ARDUINO)
            lock_.mutex_.unlock();
        #endif
    }
    SerialOutputGuard(const SerialOutputGuard&) = delete;
    SerialOutputGuard& operator=(const SerialOutputGuard&) = delete;
private:
    SerialOutputLock& lock_;
    #if defined(__arm__) && !defined(PICO_RP2040) && !defined(PICO_RP2350)
        uint32_t state_;
    #elif defined(__thumb__) && !defined(PICO_RP2040) && !defined(PICO_RP2350)
        uint32_t state_;
    #endif
};

inline bool serial_in_interrupt() {
    #if defined(PICO_RP2040) || defined(PICO_RP2350)
    return __get_current_exception() != 0;
    #elif defined(__arm__) || defined(__thumb__)
    uint32_t state;
    asm volatile("mrs %0, ipsr" : "=r"(state));
    return state != 0;
    #else
    return false;
    #endif
}

struct SerialOutputStats {
    uint32_t dropped_bytes = 0;
    uint32_t short_writes = 0;
    uint32_t packets_completed = 0;
    uint32_t packets_failed = 0;
    size_t queued_bytes = 0;
};

class SerialPacket;

class SerialOutput : public Print {
public:
    static constexpr size_t QUEUE_CAPACITY = FOUNDLING_SERIAL_QUEUE_CAPACITY;
    static constexpr size_t WRITE_CHUNK_SIZE = 512;
    static_assert(QUEUE_CAPACITY > 0, "serial output queue capacity must be positive");
    using Print::write;

    SerialOutput() = default;
    SerialOutput(const SerialOutput&) = delete;
    SerialOutput& operator=(const SerialOutput&) = delete;

    // Call once from core 0 before starting producers on other cores.
    void begin() {
        if (serial_in_interrupt() || !on_serial_core()) return;
        const bool connected = (bool)Serial;
        SerialOutputGuard guard(lock_);
        connected_ = connected;
    }

    bool connected() const {
        SerialOutputGuard guard(lock_);
        return connected_;
    }

    size_t write(uint8_t byte) override { return write(&byte, 1); }

    size_t write(const uint8_t* data, size_t size) override {
        size_t accepted;
        {
            SerialOutputGuard guard(lock_);
            accepted = size <= QUEUE_CAPACITY - queued_ ? size : 0;
            if (accepted) {
                const size_t first = accepted < QUEUE_CAPACITY - tail_ ? accepted : QUEUE_CAPACITY - tail_;
                std::memcpy(queue_ + tail_, data, first);
                if (accepted > first) std::memcpy(queue_, data + first, accepted - first);
                tail_ = (tail_ + accepted) % QUEUE_CAPACITY;
                queued_ += accepted;
            }
            stats_.dropped_bytes += (uint32_t)(size - accepted);
            if (accepted != size) setWriteError();
        }
        if (!serial_in_interrupt() && on_serial_core()) drain();
        return accepted;
    }

    SerialOutputStats stats() const {
        SerialOutputGuard guard(lock_);
        SerialOutputStats result = stats_;
        result.queued_bytes = queued_ + pending_size_ - pending_offset_;
        return result;
    }

    // Only transmit bytes the USB driver can currently accept without waiting.
    size_t drain(size_t budget = WRITE_CHUNK_SIZE) {
        if (serial_in_interrupt() || !on_serial_core()) return 0;
        const bool connected = (bool)Serial;
        {
            SerialOutputGuard guard(lock_);
            connected_ = connected;
        }
        if (!connected || !begin_wire()) return 0;
        size_t sent = report_errors(budget);
        if (report_offset_ != report_size_) {
            end_wire();
            return sent;
        }
        while (sent < budget) {
            const int available = Serial.availableForWrite();
            if (available <= 0) break;
            size_t count;
            const uint8_t* data;
            {
                SerialOutputGuard guard(lock_);
                if (pending_offset_ == pending_size_) {
                    pending_size_ = queued_ < WRITE_CHUNK_SIZE ? queued_ : WRITE_CHUNK_SIZE;
                    pending_offset_ = 0;
                    for (size_t i = 0; i < pending_size_; ++i) {
                        pending_[i] = queue_[head_];
                        head_ = (head_ + 1) % QUEUE_CAPACITY;
                    }
                    queued_ -= pending_size_;
                }
                count = pending_size_ - pending_offset_;
                if (count > budget - sent) count = budget - sent;
                if (count > (size_t)available) count = (size_t)available;
                data = pending_ + pending_offset_;
            }
            if (!count) break;
            const size_t written = Serial.write(data, count);
            {
                SerialOutputGuard guard(lock_);
                if (written < count) {
                    ++stats_.short_writes;
                    setWriteError();
                }
                pending_offset_ += written;
            }
            sent += written;
            if (!written) break;
        }
        end_wire();
        return sent;
    }

    void flush() override {
        if (serial_in_interrupt() || !on_serial_core()) return;
        drain(QUEUE_CAPACITY);
        if (begin_wire()) {
            if (connected()) Serial.flush();
            end_wire();
        }
    }

private:
    static bool on_serial_core() {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            return get_core_num() == 0;
        #else
            return true;
        #endif
    }

    friend class SerialPacket;
    bool begin_wire() {
        SerialOutputGuard guard(lock_);
        if (wire_busy_) return false;
        wire_busy_ = true;
        return true;
    }
    void end_wire() {
        SerialOutputGuard guard(lock_);
        wire_busy_ = false;
    }
    void note_short_write() {
        SerialOutputGuard guard(lock_);
        ++stats_.short_writes;
        setWriteError();
    }
    void end_packet(bool completed) {
        SerialOutputGuard guard(lock_);
        if (completed) ++stats_.packets_completed;
        else {
            ++stats_.packets_failed;
            setWriteError();
        }
        wire_busy_ = false;
    }
    size_t report_errors(size_t budget) {
        const SerialOutputStats current = stats();
        if (report_offset_ == report_size_) {
            if (current.dropped_bytes == reported_dropped_ &&
                current.short_writes == reported_short_ &&
                current.packets_failed == reported_failed_) return 0;
            report_stats_ = current;
            report_size_ = (size_t)snprintf(report_, sizeof(report_),
                "SERIAL-OUTPUT dropped=%lu short_writes=%lu failed_packets=%lu\n",
                (unsigned long)current.dropped_bytes, (unsigned long)current.short_writes,
                (unsigned long)current.packets_failed);
            report_offset_ = 0;
        }
        const int available = Serial.availableForWrite();
        if (available <= 0 || !budget) return 0;
        size_t count = report_size_ - report_offset_;
        if (count > budget) count = budget;
        if (count > (size_t)available) count = (size_t)available;
        const size_t written = Serial.write((const uint8_t*)report_ + report_offset_, count);
        report_offset_ += written;
        if (report_offset_ == report_size_) {
            reported_dropped_ = report_stats_.dropped_bytes;
            reported_short_ = report_stats_.short_writes;
            reported_failed_ = report_stats_.packets_failed;
        }
        if (written < count) {
            SerialOutputGuard guard(lock_);
            setWriteError();
        }
        return written;
    }

    mutable SerialOutputLock lock_;
    bool wire_busy_ = false;
    bool connected_ = false;
    uint8_t queue_[QUEUE_CAPACITY] = {};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t queued_ = 0;
    uint8_t pending_[WRITE_CHUNK_SIZE] = {};
    size_t pending_size_ = 0;
    size_t pending_offset_ = 0;
    SerialOutputStats stats_;
    uint32_t reported_dropped_ = 0;
    uint32_t reported_short_ = 0;
    uint32_t reported_failed_ = 0;
    char report_[128] = {};
    size_t report_size_ = 0;
    size_t report_offset_ = 0;
    SerialOutputStats report_stats_;
};

inline SerialOutput& serial_output() {
    static SerialOutput output;
    return output;
}

class SerialPacket : public Print {
public:
    explicit SerialPacket(SerialOutput& output = serial_output())
        : output_(output), started_at_(millis()) {
        if (!serial_in_interrupt() && on_serial_core()) {
            output_.drain();
            active_ = output_.begin_wire();
        }
        if (!active_) {
            SerialOutputGuard guard(output_.lock_);
            ++output_.stats_.packets_failed;
            setWriteError();
        }
    }
    ~SerialPacket() {
        if (active_) output_.end_packet(false);
    }
    SerialPacket(const SerialPacket&) = delete;
    SerialPacket& operator=(const SerialPacket&) = delete;
    using Print::write;
    explicit operator bool() const { return active_ && good_; }

    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* data, size_t size) override {
        if (!active_ || !good_) return 0;
        if (size >= SerialOutput::WRITE_CHUNK_SIZE) Serial.flush();
        size_t sent = 0;
        while (sent < size) {
            if (!Serial || (uint32_t)(millis() - started_at_) >= 250) {
                good_ = false;
                setWriteError();
                break;
            }
            const size_t remaining = size - sent;
            const size_t count = remaining < SerialOutput::WRITE_CHUNK_SIZE
                ? remaining : SerialOutput::WRITE_CHUNK_SIZE;
            const size_t written = Serial.write(data + sent, count);
            if (written < count) output_.note_short_write();
            sent += written;
            // Teensy batches writes in 2 KiB DMA buffers. Flush bulk chunks so
            // each transfer stays within one high-speed USB packet.
            if (written && count == SerialOutput::WRITE_CHUNK_SIZE) Serial.flush();
            if (!written) {
                good_ = false;
                setWriteError();
                break;
            }
        }
        return sent;
    }

    bool finish() {
        if (!active_) return false;
        if (!Serial) good_ = false;
        if (good_ && Serial) Serial.flush();
        output_.end_packet(good_);
        active_ = false;
        return good_;
    }

private:
    static bool on_serial_core() {
        #if defined(PICO_RP2040) || defined(PICO_RP2350)
            return get_core_num() == 0;
        #else
            return true;
        #endif
    }

    SerialOutput& output_;
    uint32_t started_at_;
    bool active_ = false;
    bool good_ = true;
};

} // namespace foundling
