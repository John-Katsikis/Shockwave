#pragma once

// CPU profiling with Tracy (https://github.com/wolfpld/tracy). Build with
// -DSHOCKWAVE_PROFILE=ON, start the Tracy profiler, and it connects to the running engine
// or editor. With the option off, these macros compile to nothing.
//
//   void Physics::step() {
//       SW_PROFILE_SCOPE("Physics step");
//       ...
//   }

#if defined(SHOCKWAVE_PROFILE) && SHOCKWAVE_PROFILE
#include <tracy/Tracy.hpp>
#define SW_PROFILE_SCOPE(name) ZoneScopedN(name)
// A zone named at runtime (a system's or render pass's name). Costs a string copy.
#define SW_PROFILE_SCOPE_DYNAMIC(text, size)                                                                           \
    ZoneScoped;                                                                                                        \
    ZoneName(text, size)
#define SW_PROFILE_FRAME() FrameMark
#define SW_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define SW_PROFILE_ENABLED 1
#else
#define SW_PROFILE_SCOPE(name)
#define SW_PROFILE_SCOPE_DYNAMIC(text, size)
#define SW_PROFILE_FRAME()
#define SW_PROFILE_THREAD(name)
#define SW_PROFILE_ENABLED 0
#endif
