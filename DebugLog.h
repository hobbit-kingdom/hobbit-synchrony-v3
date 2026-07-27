/*
	DebugLog.h
	Console output gate for release builds.

	The mod prints a lot while it works: every NPC it resolves, every stone throw,
	every chest/trigger/pickup it replicates, every nickname packet. That is what
	you want while developing and noise for everyone else, so all of it goes
	through dprintf()/dcout() and only reaches the console when debug logging is
	on.

	Turn it on with "debug=1" in synchrony_config.txt (see loadDebugLoggingFlag
	in client.cpp). Default is off.

	What deliberately does NOT go through here: the startup prompts and the
	connection status, which the player has to be able to read to use the mod at
	all, and the in-game chat overlay, which is not console output.

	Header-only: g_debugLogging is a C++17 inline variable, so every translation
	unit shares one copy without a matching .cpp in the project file.
*/

#pragma once

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <streambuf>

/// Set once at startup from the config file. Read from several threads, never
/// written after startup, so it needs no synchronisation.
inline bool g_debugLogging = false;

/// printf that goes nowhere unless debug logging is on.
inline void dprintf(const char* format, ...)
{
	if (!g_debugLogging)
		return;

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

namespace DebugLogDetail
{
	/// Swallows everything written to it, including the formatting work the
	/// stream operators do before it - those still run, but nothing is emitted.
	struct NullBuffer : std::streambuf
	{
		int overflow(int ch) override { return ch; }
	};

	inline std::ostream& nullStream()
	{
		static NullBuffer buffer;
		static std::ostream stream(&buffer);
		return stream;
	}
}

/// std::cout when debug logging is on, a discarding stream otherwise.
inline std::ostream& dcout()
{
	return g_debugLogging ? std::cout : DebugLogDetail::nullStream();
}
