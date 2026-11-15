# Shared serial output

`shared_debug.h` provides the existing `Serial_print`, `Serial_println`,
`Serial_printf`, `Debug_print*`, and `Serial_flush` macros. Define
`ENABLE_SERIAL_OUTPUT_QUEUE` consistently across a project's translation units
to route these macros through `foundling::serial_output()`. Without it, their
existing direct-Serial behavior is retained.

The shared output object derives from Arduino `Print`, so normal values, flash
strings, and the platform's `printf` support remain available. Initialize it
from setup before enabling interrupt producers, and call
`foundling::serial_output().drain()` each loop.

## Text and binary packets

Text uses a fixed 2 KiB FIFO. Writes outside a packet also attempt a bounded,
non-waiting drain. During a packet, text is only queued: even a wrapper flush
cannot write into the packet. Interrupt-state preservation and interrupt-context
detection are provided for single-core Cortex-M targets, including Teensy 4.x.
On other architectures, use these APIs only from main context until equivalent
interrupt guards are supplied. This is not a multicore synchronization primitive.

Use `foundling::SerialPacket` for binary framing:

```cpp
#include <serial_output.h>

foundling::SerialPacket packet;
if (packet) {
    packet.print("==START-FRAME==");
    packet.write(payload, payload_size);
    packet.print("==END-FRAME==");
    packet.finish();
}
```

Create and write packets from normal main context with interrupts enabled, not
inside an interrupt handler or an interrupt-disabled critical section.

Packet writes use chunks of at most 512 bytes, advance by the actual accepted
byte count, and retry positive partial writes. Bulk chunks are flushed separately
to avoid accumulating a multi-packet transfer in Teensy's 2 KiB transmit buffers.
Zero writes, disconnection, or a
250 ms packet deadline fail the packet; subsequent writes (including a trailer)
are suppressed. The underlying driver's own blocking call can overshoot that
deadline. Destruction releases the packet lock even if the sender exits early.
`finish()` records successful acceptance by the driver, not acknowledgement by
the host or proof of framebuffer consistency.

Queue overflow drops excess bytes, sets the output object's write-error flag,
and counts the loss. Short writes and failed packets are also counted.
`stats()` exposes these counters; the next available drain emits a bounded
`SERIAL-OUTPUT ...` diagnostic when they change. Draining preserves unwritten
text across short writes. `flush()` attempts a bounded drain and driver flush;
it does not wait indefinitely for a stalled host.

**Direct `Serial.print/write/printf/flush` calls bypass this protection.**
Projects should migrate relevant diagnostic producers to the shared macros;
binary protocol output must use the packet API, not the text macros.

## Tests

Run the host behavior test with Python:

```powershell
python .\test\serial_output_host_test.py
```

On Windows it uses Visual Studio's `vcvars64.bat` (override its path with
`VCVARS64`). Elsewhere it uses `CXX` or `g++`. All generated files are kept in
a temporary test directory and removed afterward.
