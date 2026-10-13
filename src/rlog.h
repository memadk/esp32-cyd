// Remote logging: mirrors every ESP-IDF/Arduino log line to a UDP syslog receiver (tools/logserver.py on the Mac).
#pragma once
#include <Arduino.h>

void rlog_begin(const char *deviceName);                 // install the log hook (call early in setup)
void rlog_set_target(const String &host, uint16_t port); // host: IP, "name.local" (mDNS) or DNS name
void rlog_poll();                                        // call regularly from the network task (resolves, sends the boot report)
void rlog_set_boot_report(const String &text);           // sent once as soon as the target is reachable
