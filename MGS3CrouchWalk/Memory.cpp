#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "Memory.h"
#include "MinHook.h"

namespace
{
    std::vector<int> PatternToBytes(const char* pattern)
    {
        std::vector<int> bytes;
        if (pattern == nullptr)
            return bytes;

        const char* current = pattern;
        const char* end = pattern + std::strlen(pattern);

        while (current < end)
        {
            if (*current == ' ' || *current == '\t')
            {
                ++current;
                continue;
            }

            if (*current == '?')
            {
                ++current;
                if (current < end && *current == '?')
                    ++current;
                bytes.push_back(-1);
                continue;
            }

            char* next = nullptr;
            unsigned long value = std::strtoul(current, &next, 16);
            if (next == current || value > 0xFF)
            {
                bytes.clear();
                return bytes;
            }

            bytes.push_back(static_cast<int>(value));
            current = next;
        }

        return bytes;
    }
}

bool Memory::DetourFunction(uintptr_t target, LPVOID detour, LPVOID* ppOriginal)
{
    if (target == 0 || detour == nullptr || ppOriginal == nullptr)
        return false;

    MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, ppOriginal);
    if (status != MH_OK)
        return false;

    status = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (status != MH_OK)
    {
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        *ppOriginal = nullptr;
        return false;
    }

    return true;
}

uintptr_t Memory::PatternScanBasic(uintptr_t beg, uintptr_t end, const uint8_t* str, uintptr_t len)
{
    if (beg == 0 || end <= beg || str == nullptr || len == 0 || end - beg < len)
        return 0;

    for (uintptr_t ptr = beg; ptr <= end - len; ++ptr)
    {
        if (std::memcmp(reinterpret_cast<const void*>(ptr), str, len) == 0)
            return ptr;
    }

    return 0;
}

uint8_t* Memory::PatternScan(void* module, const char* signature)
{
    if (module == nullptr || signature == nullptr)
        return nullptr;

    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<std::uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    auto patternBytes = PatternToBytes(signature);
    if (patternBytes.empty())
        return nullptr;

    const auto* sections = IMAGE_FIRST_SECTION(ntHeaders);
    const WORD sectionCount = ntHeaders->FileHeader.NumberOfSections;

    for (WORD sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex)
    {
        const IMAGE_SECTION_HEADER& section = sections[sectionIndex];

        // Only scan executable sections. The original scanner searched the
        // entire image, which can become ambiguous after patches/optimisation.
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;

        uintptr_t begin = reinterpret_cast<uintptr_t>(module) + section.VirtualAddress;
        uintptr_t size = section.Misc.VirtualSize;
        if (size < patternBytes.size())
            continue;

        uintptr_t end = begin + size;

        for (uintptr_t address = begin; address <= end - patternBytes.size(); ++address)
        {
            bool found = true;
            for (size_t j = 0; j < patternBytes.size(); ++j)
            {
                const int expected = patternBytes[j];
                if (expected != -1 && *reinterpret_cast<const uint8_t*>(address + j) != expected)
                {
                    found = false;
                    break;
                }
            }

            if (found)
                return reinterpret_cast<uint8_t*>(address);
        }
    }

    return nullptr;
}

bool Memory::IsAddressInModule(void* module, uintptr_t address, size_t size)
{
    if (module == nullptr || address == 0 || size == 0)
        return false;

    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<std::uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uintptr_t end = base + ntHeaders->OptionalHeader.SizeOfImage;

    if (address < base || address >= end)
        return false;

    return size <= (end - address);
}
