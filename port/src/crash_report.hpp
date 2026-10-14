#pragma once

// Windows diagnostics written to stderr (run\crash.log under JugarConLog.bat). Every call is a no-op elsewhere.
// Frames print as module+offset; symbolize them with llvm-symbolizer against the executable of the same build.

// Logs unhandled exceptions with their stack, and starts a watchdog that logs the game thread's stack when it
// stops reaching the host pump.
void CrashReportInstall();

// Called from each host pump: marks the game thread alive for the watchdog.
void CrashReportHeartbeat();

// Logs why the process is leaving through a closed window, with the stack the game was on at the time, so a
// frozen game the player closed can be told apart from a clean quit.
void CrashReportWindowClosed();
