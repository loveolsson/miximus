#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// Keep all Windows prerequisites behind the DeckLink wrapper. The generated
// MIDL header currently includes some of these transitively, but the wrapper
// also uses COM activation, UTF-8 conversion, BSTR ownership, and IID comparison.
#include <windows.h>
#include <combaseapi.h>
#include <oleauto.h>
// unknwn.h (IUnknown/COM) must be included before the generated DeckLinkAPI.h.
#include <unknwn.h>
#include <DeckLinkAPI.h>
// clang-format on
#else
#include <DeckLinkAPI.h>
#define BOOL bool
#endif
