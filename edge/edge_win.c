/* gettimeofday for picotls on Windows. picotls's wincompat.h renames
 * gettimeofday to wintimeofday and declares it, but its own definition does not
 * compile under MinGW, so the program supplies it, as the Godot fork does. */
#ifdef _WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
struct timezone;
int wintimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    if (tv) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t now = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10 - 11644473600000000ULL;
        tv->tv_sec = (long)(now / 1000000);
        tv->tv_usec = (long)(now % 1000000);
    }
    return 0;
}
#endif
