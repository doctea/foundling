import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ARDUINO = r'''
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

extern uint32_t test_time;
inline uint32_t millis() { return test_time; }

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t byte) = 0;
    virtual size_t write(const uint8_t* data, size_t size) = 0;
    virtual void flush() {}
    size_t print(const char* text) { return write((const uint8_t*)text, std::strlen(text)); }
    size_t println(const char* text) { return print(text) + println(); }
    size_t println() { return print("\r\n"); }
    int printf(const char* format, ...) {
        char buffer[256];
        va_list args;
        va_start(args, format);
        const int size = std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        return (int)write((const uint8_t*)buffer, (size_t)size);
    }
    int getWriteError() const { return error_; }
protected:
    void setWriteError(int error = 1) { error_ = error; }
private:
    int error_ = 0;
};

struct FakeSerial {
    bool connected = true;
    int available = 512;
    size_t maximum_write = 512;
    size_t largest_request = 0;
    size_t flushes = 0;
    std::string data;
    std::function<void()> on_write;
    explicit operator bool() const { return connected; }
    bool dtr() const { return connected; }
    int availableForWrite() const { return available; }
    size_t write(const uint8_t* bytes, size_t size) {
        largest_request = std::max(largest_request, size);
        const size_t accepted = connected ? std::min(size, maximum_write) : 0;
        data.append((const char*)bytes, accepted);
        if (on_write) on_write();
        return accepted;
    }
    void flush() { ++flushes; }
};
extern FakeSerial Serial;
'''

SOURCE = r'''
#include <cassert>
#include <atomic>
#include <thread>
#define ENABLE_SERIAL_OUTPUT_QUEUE
#define ENABLE_WARNING_LOG
#define SERIAL_FLUSH_REALLY
#include "shared_debug.h"

uint32_t test_time = 0;
FakeSerial Serial;
foundling::SerialOutput* peer_output();
void peer_log();

static void reset_serial() {
    Serial = FakeSerial();
    test_time = 0;
}

static void queued_logs_do_not_enter_packets() {
    reset_serial();
    foundling::SerialOutput output;
    {
        foundling::SerialPacket packet(output);
        packet.print("START");
        output.print("debug");
        output.flush();
        assert(Serial.data == "START");
        packet.print("pixels");
        packet.print("END");
        assert(packet.finish());
    }
    output.drain();
    assert(Serial.data == "STARTpixelsENDdebug");
}

static void bulk_packets_are_complete_and_flushed_in_usb_sized_chunks() {
    reset_serial();
    foundling::SerialOutput output;
    std::vector<uint8_t> pixels(153600);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (uint8_t)i;
    foundling::SerialPacket packet(output);
    packet.print("START");
    assert(packet.write(pixels.data(), pixels.size()) == pixels.size());
    packet.print("END");
    assert(packet.finish());
    assert(Serial.largest_request <= 512);
    assert(Serial.flushes >= 300);
    assert(Serial.data == "START" + std::string((const char*)pixels.data(), pixels.size()) + "END");
    assert(output.stats().packets_completed == 1);
}

static void partial_writes_advance_by_actual_counts() {
    reset_serial();
    Serial.maximum_write = 3;
    foundling::SerialOutput output;
    foundling::SerialPacket packet(output);
    packet.print("0123456789abcdef");
    assert(packet.finish());
    assert(Serial.data == "0123456789abcdef");
    assert(output.stats().short_writes > 0);
    assert(output.stats().packets_failed == 0);
}

static void zero_write_suppresses_trailer_and_reports_failure() {
    reset_serial();
    Serial.maximum_write = 0;
    foundling::SerialOutput output;
    foundling::SerialPacket packet(output);
    packet.print("START");
    packet.print("pixels");
    packet.print("END");
    assert(!packet.finish());
    assert(Serial.data.empty());
    assert(output.stats().packets_failed == 1);
    assert(output.stats().short_writes == 1);
    Serial.maximum_write = 512;
    output.drain();
    assert(Serial.data.find("failed_packets=1") != std::string::npos);
}

static void deadline_and_disconnect_fail_without_trailers() {
    reset_serial();
    foundling::SerialOutput output;
    Serial.on_write = [] { test_time += 300; };
    std::vector<uint8_t> pixels(1024, 'p');
    {
        foundling::SerialPacket packet(output);
        assert(packet.write(pixels.data(), pixels.size()) == 512);
        packet.print("END");
        assert(!packet.finish());
    }
    assert(Serial.data == std::string(512, 'p'));
    assert(output.stats().packets_failed == 1);
    reset_serial();
    {
        foundling::SerialPacket packet(output);
        Serial.connected = false;
        assert(!packet.finish());
    }
    assert(output.stats().packets_failed == 2);
}

static void overflow_is_bounded_and_visible() {
    reset_serial();
    foundling::SerialOutput output;
    std::vector<uint8_t> text(foundling::SerialOutput::QUEUE_CAPACITY - 2, 'a');
    {
        foundling::SerialPacket packet(output);
        assert(output.write(text.data(), text.size()) == text.size());
        assert(output.write((const uint8_t*)"xyz", 3) == 0);
        assert(output.stats().dropped_bytes == 3);
        assert(output.getWriteError());
        packet.print("frame");
        assert(packet.finish());
    }
    for (int i = 0; i < 10; ++i) output.drain();
    assert(output.stats().queued_bytes == 0);
    assert(Serial.data.find("dropped=3") != std::string::npos);
    assert(Serial.data.substr(Serial.data.size() - text.size()) == std::string(text.size(), 'a'));
}

static void stalled_text_and_partial_diagnostics_are_preserved() {
    reset_serial();
    foundling::SerialOutput output;
    Serial.maximum_write = 0;
    output.print("queued");
    assert(output.stats().queued_bytes == 6);
    Serial.maximum_write = 3;
    for (int i = 0; i < 100; ++i) output.drain();
    assert(output.stats().queued_bytes == 0);
    assert(Serial.data.find("queued") != std::string::npos);
    assert(Serial.data.find("SERIAL-OUTPUT dropped=0 short_writes=") != std::string::npos);
    reset_serial();
    foundling::SerialOutput stalled;
    Serial.available = 0;
    stalled.print("wait");
    assert(Serial.data.empty());
    assert(stalled.stats().queued_bytes == 4);
    Serial.available = 512;
    assert(stalled.drain(2) == 2);
    assert(Serial.data == "wa");
    stalled.drain();
    assert(Serial.data == "wait");
}

static void cached_connection_state_tracks_the_usb_driver() {
    reset_serial();
    Serial.connected = false;
    foundling::SerialOutput& output = foundling::serial_output();
    output.begin();
    assert(!output.connected());
    Serial_printf("hidden");
    assert(Serial.data.empty());
    Serial.connected = true;
    assert(!output.connected());
    output.drain();
    assert(output.connected());
    Serial_printf("visible");
    assert(Serial.data == "visible");
    Serial.connected = false;
    output.drain();
    assert(!output.connected());
}

static void concurrent_writers_preserve_each_write() {
    reset_serial();
    foundling::SerialOutput output;
    const size_t thread_count = 4;
    const size_t record_count = 200;
    std::atomic<bool> start(false);
    std::atomic<size_t> failures(0);
    std::vector<std::thread> writers;
    writers.reserve(thread_count);
    for (size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
        writers.emplace_back([&, thread_id] {
            while (!start.load()) std::this_thread::yield();
            for (size_t record_id = 0; record_id < record_count; ++record_id) {
                char record[16];
                const int length = std::snprintf(record, sizeof(record), "%u:%04u\n",
                    (unsigned)thread_id, (unsigned)record_id);
                if (length <= 0 ||
                    output.write((const uint8_t*)record, (size_t)length) != (size_t)length) {
                    ++failures;
                }
            }
        });
    }
    start.store(true);
    for (auto& writer : writers) writer.join();
    for (int i = 0; i < 1000 && output.stats().queued_bytes; ++i) output.drain();

    assert(failures.load() == 0);
    assert(output.stats().queued_bytes == 0);
    assert(Serial.data.size() == thread_count * record_count * 7);
    bool seen[4][200] = {};
    for (size_t offset = 0; offset < Serial.data.size(); offset += 7) {
        const char* record = Serial.data.data() + offset;
        assert(record[0] >= '0' && record[0] <= '3');
        assert(record[1] == ':');
        assert(record[6] == '\n');
        unsigned value = 0;
        for (size_t digit = 2; digit < 6; ++digit) {
            assert(record[digit] >= '0' && record[digit] <= '9');
            value = value * 10 + (unsigned)(record[digit] - '0');
        }
        const size_t thread_id = (size_t)(record[0] - '0');
        assert(value < record_count);
        assert(!seen[thread_id][value]);
        seen[thread_id][value] = true;
    }
    for (size_t thread_id = 0; thread_id < thread_count; ++thread_id) {
        for (size_t record_id = 0; record_id < record_count; ++record_id) {
            assert(seen[thread_id][record_id]);
        }
    }
}

static void nested_or_abandoned_packets_release_only_their_own_lock() {
    reset_serial();
    foundling::SerialOutput output;
    {
        foundling::SerialPacket outer(output);
        {
            foundling::SerialPacket inner(output);
            assert(!inner);
        }
        output.print("queued");
        assert(Serial.data.empty());
        outer.print("frame");
        assert(outer.finish());
    }
    output.drain();
    assert(Serial.data.find("queued") > Serial.data.find("frame"));
    assert(output.stats().packets_failed == 1);
    {
        foundling::SerialPacket abandoned(output);
        abandoned.print("partial");
    }
    foundling::SerialPacket next(output);
    assert(next);
    assert(next.finish());
    assert(output.stats().packets_failed == 2);
}

static void wrappers_share_one_queue_across_translation_units() {
    reset_serial();
    foundling::serial_output().begin();
    assert(peer_output() == &foundling::serial_output());
    {
        foundling::SerialPacket packet;
        packet.print("START");
        peer_log();
        Serial_flush();
        assert(Serial.data == "START");
        packet.print("END");
        assert(packet.finish());
    }
    foundling::serial_output().drain();
    assert(Serial.data == "STARTENDpeer=7");
}

static void warnings_are_retained_offline_in_ring_order() {
    reset_serial();
    Serial.connected = false;
    foundling::serial_output().begin();
    auto& log = foundling::warning_log();
    Warn_println("first offline warning");
    char message[foundling::WarningLog::MESSAGE_SIZE];
    assert(log.read(0, message));
    assert(std::string(message) == "first offline warning\r\n");
    for (size_t index = 0; index < log.CAPACITY; ++index) {
        Warn_printf("offline-%u", (unsigned)index);
    }
    assert(log.size() == log.CAPACITY);
    assert(log.read(0, message));
    assert(std::string(message) == "offline-0");
    assert(!log.read(log.CAPACITY, message));
    assert(Serial.data.empty());
}

int main() {
    queued_logs_do_not_enter_packets();
    bulk_packets_are_complete_and_flushed_in_usb_sized_chunks();
    partial_writes_advance_by_actual_counts();
    zero_write_suppresses_trailer_and_reports_failure();
    deadline_and_disconnect_fail_without_trailers();
    overflow_is_bounded_and_visible();
    stalled_text_and_partial_diagnostics_are_preserved();
    cached_connection_state_tracks_the_usb_driver();
    concurrent_writers_preserve_each_write();
    nested_or_abandoned_packets_release_only_their_own_lock();
    wrappers_share_one_queue_across_translation_units();
    warnings_are_retained_offline_in_ring_order();
    std::puts("12 serial output and warning behavior cases passed");
}
'''

PEER = r'''
#define ENABLE_SERIAL_OUTPUT_QUEUE
#define ENABLE_WARNING_LOG
#include "shared_debug.h"
foundling::SerialOutput* peer_output() { return &foundling::serial_output(); }
void peer_log() { Serial_printf("peer=%d", 7); }
'''

PICO_SOURCE = r'''
#include <cassert>
#define PICO_RP2040 1
#define USE_TINYUSB
#include "serial_output.h"

FakeSerial Serial;
uint32_t test_time = 0;
unsigned test_core = 0;
unsigned test_exception = 0;

int main() {
    foundling::SerialOutput output;
    output.begin();
    output.print("short");
    assert(Serial.data.empty());
    test_core = 1;
    output.print(" core1");
    assert(output.drain() == 0);
    test_core = 0;
    test_exception = 16;
    output.print(" irq");
    assert(output.drain() == 0);
    test_exception = 0;
    Serial.available = 0;
    assert(output.drain() == 0);
    assert(Serial.flushes == 0);
    assert(Serial.largest_request == 0);
    {
        foundling::SerialPacket packet(output);
        assert(packet.print("frame") == 0);
        assert(!packet.finish());
    }
    assert(Serial.largest_request == 0);
    Serial.available = 512;
    output.drain();
    assert(Serial.data.find("failed_packets=1") != std::string::npos);
    assert(Serial.data.find("short core1 irq") != std::string::npos);
    assert(Serial.flushes == 1);
    std::puts("RP2 CDC ownership, full FIFO and short transfer kick passed");
}
'''


def main():
    include = Path(__file__).resolve().parents[1] / "include"
    with tempfile.TemporaryDirectory(prefix="foundling-serial-") as directory:
        root = Path(directory)
        (root / "Arduino.h").write_text(ARDUINO, encoding="utf-8")
        (root / "serial_test.cpp").write_text(SOURCE, encoding="utf-8")
        (root / "peer.cpp").write_text(PEER, encoding="utf-8")
        (root / "rp2_test.cpp").write_text(PICO_SOURCE, encoding="utf-8")
        (root / "pico").mkdir()
        (root / "pico/critical_section.h").write_text(
            "struct critical_section_t {};\n"
            "inline void critical_section_init(critical_section_t*) {}\n"
            "inline void critical_section_deinit(critical_section_t*) {}\n"
            "inline void critical_section_enter_blocking(critical_section_t*) {}\n"
            "inline void critical_section_exit(critical_section_t*) {}\n",
            encoding="utf-8",
        )
        (root / "pico/platform.h").write_text(
            "extern unsigned test_core, test_exception;\n"
            "inline unsigned get_core_num() { return test_core; }\n"
            "inline unsigned __get_current_exception() { return test_exception; }\n",
            encoding="utf-8",
        )
        if os.name == "nt":
            vcvars = os.environ.get(
                "VCVARS64",
                r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
            )
            if not Path(vcvars).is_file():
                raise RuntimeError("Host C++ tools missing; set VCVARS64 to vcvars64.bat")
            command = (
                f'call "{vcvars}" >nul && cl /nologo /std:c++14 /EHsc /W4 /WX '
                f'/I"{root}" /I"{include}" serial_test.cpp peer.cpp /Fe:serial_test.exe '
                f'&& serial_test.exe && cl /nologo /std:c++14 /EHsc /W4 /WX '
                f'/I"{root}" /I"{include}" rp2_test.cpp /Fe:rp2_test.exe && rp2_test.exe'
            )
            subprocess.run(
                f'"{os.environ["COMSPEC"]}" /d /s /c "{command}"', cwd=root, check=True
            )
        else:
            compiler = shlex.split(os.environ.get("CXX", "g++"))
            binary = root / "serial_test"
            subprocess.run(compiler + [
                "-std=c++11", "-Wall", "-Wextra", "-Werror",
                "-pthread", f"-I{root}", f"-I{include}", str(root / "serial_test.cpp"),
                str(root / "peer.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
            rp2 = root / "rp2_test"
            subprocess.run(compiler + [
                "-std=c++11", "-Wall", "-Wextra", "-Werror",
                f"-I{root}", f"-I{include}", str(root / "rp2_test.cpp"), "-o", str(rp2),
            ], check=True)
            subprocess.run([str(rp2)], check=True)


if __name__ == "__main__":
    main()
