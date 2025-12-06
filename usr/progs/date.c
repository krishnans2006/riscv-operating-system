#include "shell.h"
#include "string.h"
#include "syscall.h"
#include <stdint.h>

// A program that prints the current date and time to STDOUT. You will need to use the rtc device,
// which will return the time in nanoseconds since January 1st, 1970 GMT. You will need to convert
// this value into a human-readable date and time format (for example, 05 Dec 2025 18:00:00). For
// simplicity, you may ignore leap seconds and output the time in GMT. However, you should account
// for leap years.

int is_leap_year(int year) {
    if (year % 100 == 0) {
        return (year % 400 == 0);
    }
    return (year % 4 == 0);
}

uint64_t seconds_in_year(int year) {
    return (is_leap_year(year) ? 366 : 365) * 24 * 60 * 60;
}

uint64_t seconds_in_month(int year, int month) {
    uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (is_leap_year(year)) {
        days_per_month[1] = 29;
    }
    return days_per_month[month] * 24 * 60 * 60;
}

void main(int argc, char* argv[]) {
    // Read time

    int fd = _open(-1, "dev/rtc0");
    if (fd < 0) {
        dprintf(CONSOLEOUT, "Error: Unable to open RTC device.\n");
        return;
    }

    // 8 bytes to store the value
    uint64_t time_ns;

    long bytes_read = _read(fd, &time_ns, sizeof(uint64_t));
    if (bytes_read != sizeof(uint64_t)) {
        dprintf(CONSOLEOUT, "Error: Unable to read time from RTC device.\n");
        _close(fd);
        return;
    }

    _close(fd);

    // Convert from "nanoseconds since 1/1/1970" to "DD Mon YYYY HH:MM:SS"

    uint64_t total_seconds = time_ns / 1000000000;

    uint16_t year = 1970;
    while (total_seconds >= seconds_in_year(year)) {
        total_seconds -= seconds_in_year(year);
        year++;
    }

    uint8_t month = 0;
    while (total_seconds >= seconds_in_month(year, month)) {
        total_seconds -= seconds_in_month(year, month);
        month++;
    }

    uint8_t day = total_seconds / (24 * 60 * 60) + 1;
    total_seconds %= (24 * 60 * 60);

    uint8_t hour = total_seconds / (60 * 60);
    total_seconds %= (60 * 60);

    uint8_t minute = total_seconds / 60;

    uint8_t second = total_seconds % 60;

    // Print to stdout

    char* month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char buffer[64];
    size_t len = snprintf(
        buffer,
        sizeof(buffer),
        "%02d %s %04d %02d:%02d:%02d\n",
        day, month_names[month], year, hour, minute, second
    );
    if (len <= 0) {
        dprintf(CONSOLEOUT, "Error: Unable to format string.\n");
        return;
    }

    _write(STDOUT, buffer, (size_t)len);

    return;
}
