#pragma once

#include "Shalltear/Core/Export.h"

#ifdef SHALLTEAR_SHARED
#ifdef SHALLTEAR_CSHARP_BUILD
#define SHALLTEAR_CSHARP_API SLT_API_EXPORT
#else
#define SHALLTEAR_CSHARP_API SLT_API_IMPORT
#endif
#else
#define SHALLTEAR_CSHARP_API
#endif
