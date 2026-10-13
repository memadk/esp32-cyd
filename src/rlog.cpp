#include "rlog.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Delivery model: log lines are queued in a small RAM backlog until the receiver has answered our
// RLOG-HELLO with RLOG-ACK (the LAN path to the Mac is often not usable for the first ~30 s after boot).
// Once acknowledged, the backlog is flushed and lines go out directly.

static int s_sock = -1, s_beacon = -1;
static sockaddr_in s_dest{};
static bool s_destValid = false, s_acked = false, s_fromBeacon = false;   // beacon-found targets need no name lookups
static String s_host;
static uint16_t s_port = 5514;
static uint32_t s_lastResolve = 0, s_lastHello = 0;
static char s_name[32] = "esp32";
static volatile bool s_inHook = false;
static String s_backlog;                         // '\n'-separated lines waiting for the ACK
static constexpr size_t BACKLOG_MAX = 6 * 1024;
static constexpr uint16_t BEACON_PORT = 5515;
static SemaphoreHandle_t s_lock = nullptr;   // guards the backlog (heap ops inside: never use a spinlock here)

static void rawSend(const char *msg, int n) {
    if (s_sock < 0 || n <= 0) return;
    char pkt[440];
    int m = snprintf(pkt, sizeof pkt, "<134>%s %.*s", s_name, n, msg);
    if (m > (int)sizeof pkt - 1) m = sizeof pkt - 1;
    if (m > 0) sendto(s_sock, pkt, m, MSG_DONTWAIT, (sockaddr *)&s_dest, sizeof s_dest);
}

static void queueLine(const char *msg, int n) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;   // drop the line rather than block a caller
    while (s_backlog.length() + n + 1 > BACKLOG_MAX) {           // drop the oldest line
        int nl = s_backlog.indexOf('\n');
        if (nl < 0) { s_backlog = ""; break; }
        s_backlog.remove(0, nl + 1);
    }
    s_backlog.concat(msg, n);
    s_backlog += '\n';
    xSemaphoreGive(s_lock);
}

static void sendLine(const char *msg, int n) {
    if (n <= 0) return;
    if (s_acked && s_destValid) rawSend(msg, n);
    else queueLine(msg, n);
}

static void flushBacklog() {
    String lines;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    lines = s_backlog; s_backlog = "";
    xSemaphoreGive(s_lock);
    int pos = 0;
    while (pos < (int)lines.length()) {
        int nl = lines.indexOf('\n', pos);
        if (nl < 0) nl = lines.length();
        rawSend(lines.c_str() + pos, nl - pos);
        pos = nl + 1;
        delay(3);
    }
}

static int logHook(const char *fmt, va_list args) {
    char buf[320];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);
    int written = vprintf(fmt, args);                         // UART as before
    if (n > 0 && !s_inHook && !xPortInIsrContext()) {
        s_inHook = true;
        if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        sendLine(buf, n);
        s_inHook = false;
    }
    return written;
}

static void setTarget(uint32_t addr, uint16_t port, const char *how) {
    bool changed = !s_destValid || s_dest.sin_addr.s_addr != addr || s_dest.sin_port != htons(port);
    s_dest.sin_family = AF_INET; s_dest.sin_port = htons(port); s_dest.sin_addr.s_addr = addr;
    s_destValid = true;
    if (changed) { s_acked = false; s_lastHello = 0; printf("rlog: target %s:%u via %s\n", inet_ntoa(s_dest.sin_addr), port, how); }
}

static void pollBeacon() {
    if (s_beacon < 0) {
        s_beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_beacon < 0) return;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(BEACON_PORT); a.sin_addr.s_addr = INADDR_ANY;
        fcntl(s_beacon, F_SETFL, fcntl(s_beacon, F_GETFL, 0) | O_NONBLOCK);
        if (bind(s_beacon, (sockaddr *)&a, sizeof a) < 0) { close(s_beacon); s_beacon = -1; return; }
    }
    char buf[64]; sockaddr_in from{}; socklen_t fl = sizeof from;
    int n = recvfrom(s_beacon, buf, sizeof buf - 1, MSG_DONTWAIT, (sockaddr *)&from, &fl);
    if (n <= 0) return;
    buf[n] = 0;
    int port = 0;
    if (sscanf(buf, "SONOSLOG %d", &port) == 1 && port > 0) { setTarget(from.sin_addr.s_addr, port, "beacon"); s_fromBeacon = true; }
}

static void pollAck() {
    if (s_sock < 0) return;
    char buf[32]; sockaddr_in from{}; socklen_t fl = sizeof from;
    int n = recvfrom(s_sock, buf, sizeof buf - 1, MSG_DONTWAIT, (sockaddr *)&from, &fl);
    if (n <= 0) return;
    buf[n] = 0;
    if (strncmp(buf, "RLOG-ACK", 8) == 0 && !s_acked) {
        s_acked = true;
        printf("rlog: receiver acknowledged, flushing backlog\n");
        flushBacklog();
        log_i("remote log connected -> %s:%u", inet_ntoa(s_dest.sin_addr), ntohs(s_dest.sin_port));
    }
}

void rlog_begin(const char *deviceName) {
    strlcpy(s_name, deviceName, sizeof s_name);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_backlog.reserve(BACKLOG_MAX);
    esp_log_set_vprintf(logHook);
    esp_log_level_set("*", ESP_LOG_INFO);
}

void rlog_set_target(const String &host, uint16_t port) {
    s_host = host; s_port = port ? port : 5514;
    s_lastResolve = 0;
    s_fromBeacon = false;   // a saved config gets one lookup; the next beacon takes over again
}

void rlog_set_boot_report(const String &text) { sendLine(text.c_str(), text.length()); }   // queued until ACK

void rlog_poll() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (s_sock < 0) {
        s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_sock >= 0) fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);
    }
    pollBeacon();
    // Name lookup is only a fallback for networks without the receiver's beacon: once a beacon has set the
    // target, skip it entirely (each mDNS miss cost a 2 s block plus an ESPmDNS "Host was not found!" warning,
    // and a resolved name that differs from the beacon made the target flap every 10 min).
    if (s_host.length() && !s_fromBeacon) {
        uint32_t interval = s_destValid ? 10 * 60 * 1000 : 20 * 1000;
        if (s_lastResolve == 0 || millis() - s_lastResolve > interval) {
            s_lastResolve = millis();
            IPAddress ip;
            if (!ip.fromString(s_host)) {
                if (s_host.endsWith(".local")) ip = MDNS.queryHost(s_host.substring(0, s_host.length() - 6), 2000);
                else WiFi.hostByName(s_host.c_str(), ip);
            }
            if ((uint32_t)ip != 0) setTarget((uint32_t)ip, s_port, "name lookup");
            else printf("rlog: could not resolve %s\n", s_host.c_str());
        }
    }
    if (s_destValid && !s_acked && millis() - s_lastHello > 2000) {   // handshake until the receiver answers
        s_lastHello = millis();
        rawSend("RLOG-HELLO", 10);
    }
    pollAck();
}
