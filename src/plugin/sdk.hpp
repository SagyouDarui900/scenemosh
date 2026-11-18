// sdk.hpp — single entry point for the AviUtl2 SDK headers.
//
// The SDK headers use int64_t without including <cstdint> (MSVC's
// windows.h happens to provide it; MinGW's does not), so <cstdint>
// comes first. windows.h comes before the SDK headers, as the SDK
// samples do. Include this header instead of the SDK headers
// directly.

#pragma once

#include <cstdint>

#include <windows.h>

#include "plugin2.h"
#include "filter2.h"
#include "cache2.h"
#include "logger2.h"
#include "config2.h"
