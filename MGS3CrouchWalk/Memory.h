#pragma once

#include <Windows.h>
#include <cstdint>

namespace Memory
{
    // Returns true only when MinHook successfully creates AND enables the hook.
    bool DetourFunction(uintptr_t target, LPVOID detour, LPVOID* ppOriginal);

    uintptr_t PatternScanBasic(uintptr_t beg, uintptr_t end, const uint8_t* str, uintptr_t len);

    // Scans executable PE sections only. This avoids accidentally matching byte
    // sequences in .rdata/.data after a game update.
    uint8_t* PatternScan(void* module, const char* signature);

    bool IsAddressInModule(void* module, uintptr_t address, size_t size = 1);
}
