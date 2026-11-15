#pragma once

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
#else
    #define FOUNDLING_SERIAL_OUTPUT Serial
#endif

#ifndef Serial_println
    #define Serial_println(X)   do { if(Serial) FOUNDLING_SERIAL_OUTPUT.println(X); } while (0)
    #define Serial_printf(...) do { if(Serial) FOUNDLING_SERIAL_OUTPUT.printf(__VA_ARGS__); } while (0)
    #define Serial_print(X)     do { if(Serial) FOUNDLING_SERIAL_OUTPUT.print(X); } while (0)
#endif

#ifndef Serial_flush
    #ifdef SERIAL_FLUSH_REALLY
        #define Serial_flush() do { if(Serial) FOUNDLING_SERIAL_OUTPUT.flush(); } while (0)
    #else
        #define Serial_flush() {}
    #endif
#endif
#ifndef Debug_printf
    #ifdef ENABLE_DEBUG_SERIAL
        #define Debug_println(X)    Serial_println(X)
        #define Debug_printf(...)   Serial_printf(__VA_ARGS__)
        #define Debug_print(X)      Serial_print(X)
    #else
        #define Debug_println(X)    {}
        #define Debug_printf(...)     {}
        #define Debug_print(X)      {}
    #endif
#endif

extern bool debug_flag;