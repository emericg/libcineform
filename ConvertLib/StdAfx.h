/*! @file StdAfx.h

*  @brief
*
*  @version 1.0.0
*
*  (C) Copyright 2017 GoPro Inc (http://gopro.com/).
*
*  Licensed under either:
*  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
*  - MIT license, http://opensource.org/licenses/MIT
*  at your option.
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
*/

#pragma once

#if _WIN32
#include <windows.h>
//#include <atlbase.h>
#include <tchar.h>
//#include <malloc.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <memory.h>
#include <limits.h>
#include <stdint.h>

#include <emmintrin.h> // SSE2 intrisics

#ifdef __APPLE__
#include "CoreFoundation/CoreFoundation.h"
#endif

#ifndef _WIN32

// The standard integer types should be available on most platforms
#define __STDC_LIMIT_MACROS
#include <stdint.h>

#endif
