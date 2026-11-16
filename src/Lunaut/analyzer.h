#pragma once

#include <stdint.h>

#define LUNAUT_ANALYZER_ABI_VERSION 1
#define LUNAUT_ANALYZER_FRONTEND_PRE_0743 1
#define LUNAUT_ANALYZER_FRONTEND_POST_0743 2

#ifdef __cplusplus
extern "C"
{
#endif

	struct lunaut_analyzer_info
	{
		uint32_t size;
		uint32_t abi;
		const char* name;
		const char* luau_version;
		uint32_t luau_version_code;
		uint32_t frontend_api;
	};

	struct lunaut_analyzer_request
	{
		uint32_t size;
		const char* module;
		const char* path;
		const char* source;
		const char* settings;
	};

	struct lunaut_analyzer_response
	{
		uint32_t size;
		const char* diagnostics;
		const char* error;
	};

	typedef const struct lunaut_analyzer_info* (*lunaut_analyzer_info_function)(void);
	typedef int (*lunaut_analyzer_check_function)(const struct lunaut_analyzer_request*, struct lunaut_analyzer_response*);
	typedef void (*lunaut_analyzer_free_function)(const char*);

#ifdef __cplusplus
}
#endif
