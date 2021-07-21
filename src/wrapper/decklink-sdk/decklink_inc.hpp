#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <unknwn.h>

#include <DeckLinkAPI_i.h>
#else
#include <DeckLinkAPI.h>
#endif