# Shared serial output

`shared_debug.h` provides the existing `Serial_print`, `Serial_println`,
`Serial_printf`, `Debug_print*`, and `Serial_flush` macros. Define
`ENABLE_SERIAL_OUTPUT_QUEUE` consistently across a project's translation units
to route these macros through `foundling::serial_output()`. Without it, their
existing direct-Serial behavior is retained.

The shared output object derives from Arduino `Print`, so normal values, flash
strings, and the platform's `printf` support remain available. Call
`foundling::serial_output().begin()` from core 0 before enabling producers on
other cores, and call `foundling::serial_output().drain()` from core 0's loop.
The wrappers refresh the connection state in foreground code on core 0, so a
host connecting during setup receives subsequent startup diagnostics even
before the first loop drain. Interrupts and secondary cores use the cached
state from `begin()`, `drain()`, or a foreground wrapper call; they never query
the USB driver. Text emitted while disconnected is not retained by the wrappers.

## Text and binary packets

Text uses a fixed FIFO, 2 KiB by default. Set
`FOUNDLING_SERIAL_QUEUE_CAPACITY` consistently across translation units to
change its size. Each write is accepted in full or dropped in full when the
queue lacks room. On RP2040/RP2350, all producers only enqueue and the core 0
loop drains. With TinyUSB, the drain uses DTR to avoid a yielding connection
check and kicks transfers for short writes. Other platforms retain their
existing foreground drain-on-write behavior and depend on the serial driver's
backpressure contract.

On RP2040/RP2350, the queue uses a Pico SDK critical section, which combines a
hardware spinlock with local interrupt masking. Host builds use a mutex, and
single-core Cortex-M targets retain interrupt masking. Call `begin()` before
starting producers on other cores so the queue and its lock are initialized
before concurrent use. This protects the queue; it does not make arbitrary
application state or the USB driver safe to access from multiple cores.

During a packet, text is only queued: even a wrapper flush cannot write into
the packet. `SerialPacket` must be created and written from core 0 in normal
main context with interrupts enabled.

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
Zero writes, disconnection, or a 250 ms packet deadline fail the packet;
subsequent writes (including a trailer) are suppressed. On RP2040/RP2350 a
packet also fails as soon as the TX FIFO is full rather than waiting for the
host. A large viewer frame may therefore arrive incomplete. On Teensy, the
driver's own blocking call can overshoot the packet deadline. Destruction
releases the packet lock even if the sender exits early.
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

## Retained warnings

`Warn_printf`, `Warn_print`, and `Warn_println` emit text through the same
wrappers. Define `ENABLE_WARNING_LOG` project-wide to also retain recent
warnings in RAM, including when no host is connected. On multicore targets,
call `foundling::warning_log()` before starting other producers. Use warnings
for infrequent problems, not from interrupt or audio sample code. With retention
enabled, interrupt warnings are ignored.

The default is eight entries of 96 bytes each, including the terminator. Set
`FOUNDLING_WARNING_LOG_CAPACITY` and `FOUNDLING_WARNING_MESSAGE_SIZE` to tune that
cost. Messages that don't fit are truncated and new entries overwrite the oldest.
Use `foundling::warning_log().size()` and `read(index, buffer)` (oldest first,
with a `char[foundling::WarningLog::MESSAGE_SIZE]` buffer) to retrieve copies.
The log is volatile; it isn't replayed on connection or written to flash.

## Tests

Run the host behavior test with Python:

```powershell
python .\test\serial_output_host_test.py
```

On Windows it uses Visual Studio's `vcvars64.bat` (override its path with
`VCVARS64`). Elsewhere it uses `CXX` or `g++`. All generated files are kept in
a temporary test directory and removed afterward.
