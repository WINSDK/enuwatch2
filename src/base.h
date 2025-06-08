#pragma once

#if defined __linux__

#define ENU_LINUX

#elif defined __APPLE__

#define ENU_MACOS

#else
#error "Unsupported OS: only Windows, Linux, and macOS are supported."
#endif
