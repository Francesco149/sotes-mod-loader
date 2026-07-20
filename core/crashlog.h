// core/crashlog.h — top-level crash tracer.  See crashlog.c.
#ifndef OSS_CRASHLOG_H
#define OSS_CRASHLOG_H

// Install a vectored exception handler that appends a crash report (exception code, faulting
// module+offset, registers, stack) to `logpath` on a crash-class exception.  Call once, early.
void crashlog_init(const char *logpath);

#endif
