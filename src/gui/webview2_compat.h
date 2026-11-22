#pragma once

// 兼容层：让 WebView2.h 在 MSVC / MinGW 下都能直接包含
#include <windows.h>
#include <objbase.h>

#ifndef interface
#define interface struct
#endif

#ifndef MIDL_INTERFACE
#define MIDL_INTERFACE(x) struct __declspec(uuid(x)) __declspec(novtable)
#endif

#ifndef COREWVIEW2_EXPORT
#define COREWVIEW2_EXPORT
#endif

#include <WebView2.h>
