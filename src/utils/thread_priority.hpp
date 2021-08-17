#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace miximus::utils {

static void set_max_thread_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), HIGH_PRIORITY_CLASS);
#else
    int         policy = 0;
    sched_param param  = {};

    pthread_getschedparam(pthread_self(), &policy, &param);
    param.sched_priority = sched_get_priority_max(policy);
    pthread_setschedparam(pthread_self(), policy, &param);
#endif
}

} // namespace miximus::utils