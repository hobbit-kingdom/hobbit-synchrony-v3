#pragma once
#ifdef _WIN32
#include <winsock2.h>   // Ensure winsock2.h is included before Windows.h
#include <Windows.h>
#include <memoryapi.h>
#include <TlHelp32.h>
#endif

#include <iostream>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// In-process memory access
// ---------------------------------------------------------------------------
// This code runs as a DLL injected INTO the game, so "the target process" is our
// own address space. ReadProcessMemory/WriteProcessMemory each cost a kernel
// transition, and the VirtualProtectEx bracketing around them cost two more - so
// reading a single field used to be three syscalls plus a heap allocation. These
// helpers do it directly.
//
// They are SEH-guarded to preserve the old behaviour exactly: a stale or bogus
// address used to make ReadProcessMemory fail and leave the destination zeroed
// rather than crash, and callers do rely on getting 0 back from a dead object
// (isChestOpened, isNpcType, the object-stack scans...). __try cannot live in a
// function that needs C++ object unwinding, so these are standalone and POD-only.
inline bool ProcAnalyzerSafeCopy(void* dst, const void* src, size_t size) noexcept
{
	__try
	{
		memcpy(dst, src, size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// Write variant. The common case is a plain writable data page and costs nothing
// extra; only if that faults do we fall back to the old make-it-writable dance,
// so read-only targets still behave the way VirtualProtectEx used to allow.
inline bool ProcAnalyzerSafeWrite(void* dst, const void* src, size_t size) noexcept
{
	if (ProcAnalyzerSafeCopy(dst, src, size))
		return true;

	DWORD oldProtect = 0;
	if (!VirtualProtect(dst, size, PAGE_READWRITE, &oldProtect))
		return false;

	const bool ok = ProcAnalyzerSafeCopy(dst, src, size);

	DWORD restored = 0;
	VirtualProtect(dst, size, oldProtect, &restored);
	return ok;
}

class ProcessAnalyzer
{

public:
	ProcessAnalyzer()
	{

	}
	HANDLE getProcess(const char* processName)
	{
		HANDLE process;
		int pid = 0;

		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			printf("Error: %lu\n", GetLastError());
			return nullptr;
		}

		PROCESSENTRY32 pe32;
		pe32.dwSize = sizeof(PROCESSENTRY32); // you need this as windows API may evole and have different size for ProcessEntry32

		if (Process32First(snapshot, &pe32))
		{
			do
			{
				WCHAR wProcessName[MAX_PATH];
				MultiByteToWideChar(CP_ACP, 0, processName, -1, wProcessName, MAX_PATH);

				if (wcscmp(pe32.szExeFile, wProcessName) == 0)
				{
					pid = pe32.th32ProcessID;
					break;
				}
			} while (Process32Next(snapshot, &pe32));
		}

		if (snapshot != NULL)
			CloseHandle(snapshot);

		if (pid == 0) {
			printf("Warning: %s - Process Not Found\n", processName);
			return nullptr;
		}

		HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
		process = processHandle;
		return processHandle;
	}

	// Direct writes - see ProcAnalyzerSafeWrite above. `process` is kept only so the
	// existing call sites and overloads compile unchanged; we are always our own
	// process. No error spam on failure: a stale address is an expected outcome
	// here, and this runs on the game thread.
	void writeData(HANDLE process, LPVOID address, std::vector<uint8_t> data)
	{
		(void)process;
		if (address == nullptr || data.empty())
			return;

		ProcAnalyzerSafeWrite(address, data.data(), data.size());
	}

	// Direct reads. The vector is value-initialised, so a faulting address still
	// yields zeroes exactly as the old ReadProcessMemory failure path did.
	std::vector<uint8_t> readData(HANDLE process, LPVOID address, size_t bytesSize)
	{
		(void)process;
		std::vector<uint8_t> data(bytesSize);
		if (address != nullptr && bytesSize != 0)
			ProcAnalyzerSafeCopy(data.data(), address, bytesSize);

		return data;
	}



	std::vector<uint32_t> searchProcessMemory(HANDLE process, const std::vector<uint8_t>& pattern) {
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);

		MEMORY_BASIC_INFORMATION mbi;
		SIZE_T address = 0;
		std::vector<uint32_t> foundAddresses;

		SIZE_T patternSize = pattern.size();

		// Iterate through the memory pages
		while (address < (SIZE_T)sysInfo.lpMaximumApplicationAddress) {
			if (VirtualQueryEx(process, (LPCVOID)address, &mbi, sizeof(mbi))) {

				// Check if the memory region is readable
				if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READONLY || mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_EXECUTE_READ)) {
					std::vector<uint8_t> buffer(mbi.RegionSize);
					SIZE_T bytesRead;

					// Read the memory region
					if (ReadProcessMemory(process, (LPCVOID)address, buffer.data(), mbi.RegionSize, &bytesRead)) {

						// Search for the pattern in the read memory
						for (SIZE_T i = 0; i < bytesRead - patternSize; i++) {
							if (memcmp(buffer.data() + i, pattern.data(), patternSize) == 0) {

								// Store the address of the found pattern
								foundAddresses.push_back(static_cast<uint32_t>(address + i));
							}
						}
					}
				}
				address += mbi.RegionSize; // Move to the next memory region
			}
			else {
				break; // Exit if VirtualQueryEx fails
			}
		}

		return foundAddresses; // Return the vector of found addresses
	}
};

