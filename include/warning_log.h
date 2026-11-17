#pragma once

#include <cstdarg>
#include <cstdio>
#include "serial_output.h"

#ifndef FOUNDLING_WARNING_LOG_CAPACITY
#define FOUNDLING_WARNING_LOG_CAPACITY 8
#endif

#ifndef FOUNDLING_WARNING_MESSAGE_SIZE
#define FOUNDLING_WARNING_MESSAGE_SIZE 96
#endif

namespace foundling {

class WarningLog {
public:
    static constexpr size_t CAPACITY = FOUNDLING_WARNING_LOG_CAPACITY;
    static constexpr size_t MESSAGE_SIZE = FOUNDLING_WARNING_MESSAGE_SIZE;
    static_assert(CAPACITY > 0 && MESSAGE_SIZE > 1, "invalid warning log size");

    void append(const char (&message)[MESSAGE_SIZE]) {
        SerialOutputGuard guard(lock_);
        std::memcpy(messages_[next_], message, MESSAGE_SIZE);
        next_ = (next_ + 1) % CAPACITY;
        if (count_ < CAPACITY) ++count_;
    }

    size_t size() const {
        SerialOutputGuard guard(lock_);
        return count_;
    }

    bool read(size_t index, char (&message)[MESSAGE_SIZE]) const {
        SerialOutputGuard guard(lock_);
        if (index >= count_) return false;
        std::memcpy(message, messages_[(next_ + CAPACITY - count_ + index) % CAPACITY], MESSAGE_SIZE);
        return true;
    }

private:
    mutable SerialOutputLock lock_;
    char messages_[CAPACITY][MESSAGE_SIZE] = {};
    size_t next_ = 0;
    size_t count_ = 0;
};

inline WarningLog& warning_log() {
    static WarningLog log;
    return log;
}

inline void format_warning(char (&message)[WarningLog::MESSAGE_SIZE], const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    warning_log().append(message);
}

} // namespace foundling