#pragma once

#define LUNAUT_NAME "lunaut"
#define LUNAUT_DISPLAY_NAME "Lunaut - Luau Language Server"

#ifndef LUNAUT_DEBUG
#define LUNAUT_DEBUG 0
#endif

#if defined(_WIN32)
#define LUNAUT_PLATFORM "Windows"
#define LUNAUT_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#define LUNAUT_PLATFORM "macOS"
#define LUNAUT_PLATFORM_WINDOWS 0
#else
#define LUNAUT_PLATFORM "Linux"
#define LUNAUT_PLATFORM_WINDOWS 0
#endif
