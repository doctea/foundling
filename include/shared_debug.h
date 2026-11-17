#pragma once

#ifndef CORE_TEENSY
    // if no FLASHMEM then we're probably not running on Teensy platform, so define it empty
    #ifndef FLASHMEM
        #define FLASHMEM
    #endif
    //#define F(x) { x }
#endif

int freeRam();
#ifdef ARDUINO_TEENSY41
    int freeExtRam(bool recount = false);
#endif

#ifdef ENABLE_SERIAL_OUTPUT_QUEUE
    #include "serial_output.h"
    #define FOUNDLING_SERIAL_OUTPUT foundling::serial_output()
    #define FOUNDLING_SERIAL_CONNECTED FOUNDLING_SERIAL_OUTPUT.connected()
#else
    #define FOUNDLING_SERIAL_OUTPUT Serial
    #define FOUNDLING_SERIAL_CONNECTED Serial
#endif

#ifndef Serial_println
    #define Serial_println(...) do { if(FOUNDLING_SERIAL_CONNECTED) FOUNDLING_SERIAL_OUTPUT.println(__VA_ARGS__); } while (0)
    #define Serial_printf(...) do { if(FOUNDLING_SERIAL_CONNECTED) FOUNDLING_SERIAL_OUTPUT.printf(__VA_ARGS__); } while (0)
    #define Serial_print(...)   do { if(FOUNDLING_SERIAL_CONNECTED) FOUNDLING_SERIAL_OUTPUT.print(__VA_ARGS__); } while (0)
#endif

#ifndef Serial_flush
    #ifdef SERIAL_FLUSH_REALLY
        #define Serial_flush() do { if(FOUNDLING_SERIAL_CONNECTED) FOUNDLING_SERIAL_OUTPUT.flush(); } while (0)
    #else
        #define Serial_flush() do {} while (0)
    #endif
#endif
#ifndef Debug_printf
    #ifdef ENABLE_DEBUG_SERIAL
        #define Debug_println(...)  Serial_println(__VA_ARGS__)
        #define Debug_printf(...)   Serial_printf(__VA_ARGS__)
        #define Debug_print(...)    Serial_print(__VA_ARGS__)
    #else
        #define Debug_println(...)  do {} while (0)
        #define Debug_printf(...)   do {} while (0)
        #define Debug_print(...)    do {} while (0)
    #endif
#endif

#ifdef ENABLE_WARNING_LOG
    #include "warning_log.h"
    #define Warn_printf(...) do { \
        if (!foundling::serial_in_interrupt()) { \
            char message[foundling::WarningLog::MESSAGE_SIZE] = {}; \
            foundling::format_warning(message, __VA_ARGS__); \
            Serial_printf("%s", message); \
        } \
    } while (0)
#else
    #define Warn_printf(...) Serial_printf(__VA_ARGS__)
#endif
#define Warn_print(text) Warn_printf("%s", text)
#define Warn_println(text) Warn_printf("%s\r\n", text)

extern bool debug_flag;