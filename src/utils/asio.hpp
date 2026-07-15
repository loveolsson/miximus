#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <SDKDDKVer.h>
#endif

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
