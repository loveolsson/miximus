#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// clang-format off
// unknwn.h (IUnknown/COM) must be included before DeckLinkAPI_i.h
#include <unknwn.h>
#include <DeckLinkAPI_i.h>
// clang-format on
#else
#include <DeckLinkAPI.h>
#define BOOL bool
#endif