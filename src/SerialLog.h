// SerialLog.h
// ---------------------------------------------------------------------------
// One serialized path for ALL console output on the host, so the two
// core-pinned radio tasks + the UART-link service task + loop() can't
// interleave their bytes mid-line on the USB-CDC console (which garbles the
// log and shreds the machine-parseable `evt=` format).
//
// Everything that prints from a task context routes through here:
//   - logf()          : emit one whole line atomically.
//   - lock()/unlock() : bracket a multi-line block (e.g. the per-packet
//                       MeshDecoderDebug dump) so it prints as one unit.
// The mutex is RECURSIVE, so a task already holding lock() may call logf()
// (or nest another lock()) without deadlocking.
//
// Boot/setup prints that run before the tasks are spawned are single-threaded
// and need not use this (they cannot interleave).
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>

namespace SerialLog {

// Create the recursive mutex. Call once, early in setup(), before any logging
// task is spawned. Idempotent. Until it is called, logf()/lock() still work
// (they just don't serialize — fine for the single-threaded boot phase).
void begin();

// Emit one printf-formatted line atomically (whole line under the lock).
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Bracket a multi-line block so no other task's output splits it. Recursive:
// logf() between lock()/unlock() from the same task is safe.
void lock();
void unlock();

}  // namespace SerialLog
