#include "Auth/UbstreamAuthClock.h"
#include "CoreMinimal.h"

#if PLATFORM_MAC
#include <mach/mach_time.h>
#elif PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_LINUX
#include <time.h>
#endif

double UbstreamAuthClock::Seconds()
{
#if PLATFORM_MAC
	static const double Scale = [] { mach_timebase_info_data_t Info; mach_timebase_info(&Info); return double(Info.numer) / double(Info.denom) * 1e-9; }();
	return double(mach_continuous_time()) * Scale;
#elif PLATFORM_WINDOWS
	return double(GetTickCount64()) / 1000.0;
#elif PLATFORM_LINUX
	timespec Time;
	if (clock_gettime(CLOCK_BOOTTIME, &Time) == 0) return double(Time.tv_sec) + double(Time.tv_nsec) * 1e-9;
	return TNumericLimits<double>::Max(); // Clock failure must expire credentials.
#else
	return TNumericLimits<double>::Max(); // No unsupported clock can prolong access.
#endif
}
