#include <shlwapi.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "Memory.h"
#include "MinHook.h"
#include "ini.h"
#include "types.h"

HMODULE GameModule = nullptr;
uintptr_t GameBase = (uintptr_t)GameModule;
uintptr_t* CamoIndexData = NULL;
uintptr_t ActSquatStillOffset = 0;
MovementWork* plWorkGlobal = NULL;
MotionControl* mCtrlGlobal = NULL;
mINI::INIStructure Config;
bool CrouchWalkEnabled = false;
bool CrouchMoving = false;
bool CrouchMovingSlow = false;
bool IgnoreButtonHold = false;
bool HijackSequence = false;
double* actorWaitValue = nullptr;

// config values
float CamoIndexModifier = 1.0f;
float CrouchWalkSpeed = 6.0f;
float CrouchStalkSpeed = 3.0f;
int CamoIndexValue = 0;

InitializeCamoIndexDelegate* InitializeCamoIndex;
CalculateCamoIndexDelegate* CalculateCamoIndex;
ActionSquatStillDelegate* ActionSquatStill;
PlayerSetMotionDelegate* PlayerSetMotionInternal;
SetMotionDataDelegate* SetMotionData;
PlayerStatusCheckDelegate* PlayerStatusCheck;
PlayerStatusSetDelegate* PlayerStatusSet;
ActMovementDelegate* ActMovement;
GetButtonHoldingStateDelegate* GetButtonHoldingState;
MotionPlaySequenceDelegate* MotionPlaySequence;

namespace
{
    std::mutex gLogMutex;

    void Log(const char* format, ...)
    {
        std::lock_guard<std::mutex> lock(gLogMutex);

        char buffer[1024] = {};
        va_list args;
        va_start(args, format);
        vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
        va_end(args);

        OutputDebugStringA("[MGS3CrouchWalk] ");
        OutputDebugStringA(buffer);
        OutputDebugStringA("\n");

        char path[MAX_PATH] = {};
        DWORD length = GetTempPathA(MAX_PATH, path);
        if (length == 0 || length >= MAX_PATH)
            return;

        FILE* file = nullptr;
        char logPath[MAX_PATH] = {};
        sprintf_s(logPath, "%sMGS3CrouchWalk.log", path);
        if (fopen_s(&file, logPath, "a") != 0 || file == nullptr)
            return;

        SYSTEMTIME time = {};
        GetLocalTime(&time);
        fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u] %s\n",
            time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond, buffer);
        fclose(file);
    }
}

uintptr_t PlayerSetMotion(int64_t work, PlayerMotion motion)
{
    int motionIndex = (int)motion;

    if (motionIndex > 0)
        motionIndex--;

    return PlayerSetMotionInternal(work, (PlayerMotion)motionIndex);
}

int64_t __fastcall ActMovementHook(MovementWork* plWork, int64_t work, int flag)
{
    if (plWork == nullptr || ActMovement == nullptr)
        return 0;

    if (plWorkGlobal != NULL && plWorkGlobal->action != ActSquatStillOffset)
    {
        CrouchWalkEnabled = false;
        CrouchMoving = false;
    }

    return ActMovement(plWork, work, flag);
}

void __fastcall SetMotionDataHook(MotionControl* motionControl, int layer, PlayerMotion motion, int time, int64_t mask)
{
    if (motionControl == nullptr || motionControl->mtcmControl == nullptr || SetMotionData == nullptr)
        return;

    if (motionControl->mtcmControl->mtarName == 0x6891CC)
        mCtrlGlobal = motionControl;

    if (motionControl == mCtrlGlobal && motion == PlayerMotion::StandMoveStalk && CrouchWalkEnabled)
    {
        float* currentTime = (float*)((uintptr_t)motionControl + 0x128);

        time = (int)*currentTime;
        motion = PlayerMotion::SquatMove;

        HijackSequence = true;
    }

    SetMotionData(motionControl, layer, motion, time, mask);
    HijackSequence = false;
}

int64_t __fastcall GetButtonHoldingStateHook(int64_t work, MovementWork* plWork)
{
    if (GetButtonHoldingState == nullptr)
        return 0;

    if (IgnoreButtonHold)
        return 0;

    return GetButtonHoldingState(work, plWork);
}

int64_t __fastcall MotionPlaySequenceHook(__int64 mtsq_ctrl, int layer, int num, int flag, int loop_time)
{
    if (MotionPlaySequence == nullptr)
        return 0;

    if (HijackSequence)
        num = CrouchMovingSlow ? PlayerMotion::StandMoveSlow : PlayerMotion::StandMoveStalk;

    return MotionPlaySequence(mtsq_ctrl, layer, num, flag, loop_time);
}

int* __fastcall CalculateCamoIndexHook(int* a1, int a2)
{
    if (CalculateCamoIndex == nullptr)
        return nullptr;

    int* result = CalculateCamoIndex(a1, a2);

    if (CamoIndexData == NULL || !CrouchWalkEnabled || !CrouchMoving)
        return result;

    int index = a2 << 7;
    auto camoIndex = (int*)((char*)&CamoIndexData[4] + index + 4);

    if (*camoIndex >= 1000) // ignore if stealth is equipped (todo: properly check item for ezgun and spider camo)
        return result;

    *camoIndex = *camoIndex < 0 ? *camoIndex / CamoIndexModifier : *camoIndex * CamoIndexModifier;
    *camoIndex += CamoIndexValue;

    if (*camoIndex > 950) *camoIndex = 950;
    if (*camoIndex < -1000) *camoIndex = -1000;

    return result;
}

int* __fastcall ActionSquatStillHook(int64_t work, MovementWork* plWork, int64_t a3, int64_t a4)
{
    if (ActionSquatStill == nullptr || plWork == nullptr || PlayerStatusCheck == nullptr || GetButtonHoldingState == nullptr || PlayerStatusSet == nullptr || PlayerSetMotionInternal == nullptr)
        return nullptr;

    // we store this here so we don't have to hardcode another address that
    // needs to be updated with each new game patch
    plWorkGlobal = plWork;

    // process the default squatting logic while ignoring the button hold state check
    IgnoreButtonHold = true;
    int* result = ActionSquatStill(work, plWork, a3, a4);
    IgnoreButtonHold = false;

    // detect holding X while crouched to go into prone
    if (!PlayerStatusCheck(0xE0u))
    {
        auto buttonState = GetButtonHoldingState(work, plWork);

        if (buttonState == 1)
            plWork->flag |= MovementFlag::FlagStand;
        else if (buttonState == 2)
            plWork->flag |= MovementFlag::FlagSquatToGround;
    }

    // check that the pad is being held down
    int16_t padForce = *(int16_t*)(work + 2016);
    bool wasMoving = CrouchMoving;
    bool wasSlow = CrouchMovingSlow;
    CrouchMoving = padForce > plWork->padForceLimit;
    CrouchMovingSlow = padForce < 180;

    if (CrouchMoving && !PlayerStatusCheck(0xDE)) // 0xDE seems to make sure we aren't in first person mode 
    {
        plWork->motion = PlayerSetMotion(work, CrouchWalkEnabled ? PlayerMotion::StandMoveStalk : PlayerMotion::RunUpwards);

        if (mCtrlGlobal != NULL)
        {
            if (actorWaitValue != nullptr) 
            {
                mCtrlGlobal->mtcmControl->motionTimeBase = (CrouchMovingSlow ? CrouchStalkSpeed : CrouchWalkSpeed) * (*actorWaitValue / (1.0 / 60));
            }
            else 
            {
                mCtrlGlobal->mtcmControl->motionTimeBase = CrouchMovingSlow ? CrouchStalkSpeed : CrouchWalkSpeed;
            }

            auto mtsqCntrl = *((uintptr_t*)mCtrlGlobal + 15);

            if (wasMoving && mtsqCntrl != 0 && wasSlow != CrouchMovingSlow)
            {
                MotionPlaySequence(mtsqCntrl, 0, CrouchMovingSlow ? PlayerMotion::StandMoveSlow : PlayerMotion::StandMoveStalk, 5, 0x1b0);
            }
        }

        PlayerStatusSet(11, 14, 0x10C, 0xFFFFFFFF); // enables grass movement sounds

        CrouchWalkEnabled = true;
    }

    if (CrouchWalkEnabled && (plWork->flag & MovementFlag::FlagStand) != 0)
        CrouchWalkEnabled = false;

    return result;
}

uintptr_t GetRelativeOffset(uint8_t* addr)
{
    if (addr == nullptr)
        return 0;

    return reinterpret_cast<uintptr_t>(addr) + 4 + *reinterpret_cast<int32_t*>(addr);
}

static bool IsFunctionPointerValid(uintptr_t address)
{
    return address != 0 && Memory::IsAddressInModule(GameModule, address);
}

static bool IsHookTargetValid(uintptr_t address)
{
    return address != 0 && Memory::IsAddressInModule(GameModule, address, 16);
}

bool InstallHooks()
{
    if (GameModule == nullptr)
    {
        Log("Game module not found.");
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MH_Initialize failed: %d", status);
        return false;
    }

    // Resolve every target first. The old version installed hooks as it went,
    // so one missing signature could leave a half-installed plugin and later
    // cause a null-function call. We now fail atomically instead.
    const uintptr_t actMovementMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "40 53 56 57 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? 00 48 33 C4 48 89 84 24 B0 00 00 00 48 8B F9"));
    const uintptr_t setMotionDataMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "48 85 C9 0F 84 42 03 00 00 4C 8B DC 55 53 56 41"));
    const uintptr_t calculateCamoMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "48 83 EC 30 0F 29 74 24 20 48 8B F9 48 63 F2 E8"));
    const uintptr_t getButtonMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "44 0F B7 8A 8E 00 00 00 4C 8B C2 66 45 85 C9 78"));
    const uintptr_t motionPlayMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "4D 63 D8 48 85 C9 74 6B 48 63 C2 48 8D 14 40 48"));
    uint8_t* disableCrouchProneOffset = Memory::PatternScan(
        GameModule, "00 00 7E 19 83 4F 68 10");
    uint8_t* actorWaitValueOffset = Memory::PatternScan(
        GameModule, "83 3D ?? ?? ?? ?? 00 ?? ?? F2 0F 10 0D");

    const uintptr_t actSquatMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "4C 8B DC 55 57 41 56 49 8D 6B A1 48 81 EC 00 01"));
    const uintptr_t initializeCamoMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "85 D2 75 33 0F 57 C0 48 63 C2 48 C1 E0 07 48 8D"));
    const uintptr_t playerSetMotionMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "B9 0F 01 00 00 E8 ?? 36 FF FF 85 C0 74 2A BA FF"));
    const uintptr_t playerStatusCheckMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "8B D1 B8 01 00 00 00 83 E1 1F D3 E0 8B CA 48 C1"));
    const uintptr_t playerStatusSetMatch = reinterpret_cast<uintptr_t>(Memory::PatternScan(
        GameModule, "04 89 0F AB D0 41 89 04"));

    const uintptr_t calculateCamoIndexOffset = calculateCamoMatch >= 0x10 ? calculateCamoMatch - 0x10 : 0;
    const uintptr_t playerSetMotionOffset = playerSetMotionMatch >= 0x10 ? playerSetMotionMatch - 0x10 : 0;
    const uintptr_t playerStatusSetOffset = playerStatusSetMatch >= 0x46 ? playerStatusSetMatch - 0x46 : 0;

    const uintptr_t actorWaitAddress = actorWaitValueOffset != nullptr
        ? GetRelativeOffset(actorWaitValueOffset + 13)
        : 0;

    if (!IsHookTargetValid(actMovementMatch) ||
        !IsHookTargetValid(setMotionDataMatch) ||
        !IsHookTargetValid(calculateCamoIndexOffset) ||
        !IsHookTargetValid(getButtonMatch) ||
        !IsHookTargetValid(motionPlayMatch) ||
        !IsHookTargetValid(actSquatMatch) ||
        !IsFunctionPointerValid(initializeCamoMatch) ||
        !IsFunctionPointerValid(playerSetMotionOffset) ||
        !IsFunctionPointerValid(playerStatusCheckMatch) ||
        !IsFunctionPointerValid(playerStatusSetOffset) ||
        disableCrouchProneOffset == nullptr ||
        !Memory::IsAddressInModule(GameModule, reinterpret_cast<uintptr_t>(disableCrouchProneOffset), 8))
    {
        Log("Signature validation failed: act=%p setMotion=%p camo=%p button=%p sequence=%p squat=%p initCamo=%p setMotionInternal=%p statusCheck=%p statusSet=%p prone=%p",
            reinterpret_cast<void*>(actMovementMatch),
            reinterpret_cast<void*>(setMotionDataMatch),
            reinterpret_cast<void*>(calculateCamoIndexOffset),
            reinterpret_cast<void*>(getButtonMatch),
            reinterpret_cast<void*>(motionPlayMatch),
            reinterpret_cast<void*>(actSquatMatch),
            reinterpret_cast<void*>(initializeCamoMatch),
            reinterpret_cast<void*>(playerSetMotionOffset),
            reinterpret_cast<void*>(playerStatusCheckMatch),
            reinterpret_cast<void*>(playerStatusSetOffset),
            disableCrouchProneOffset);
        return false;
    }

    Log("All signatures resolved. Installing hooks.");

    if (actorWaitAddress != 0 && Memory::IsAddressInModule(GameModule, actorWaitAddress, sizeof(double)))
        actorWaitValue = reinterpret_cast<double*>(actorWaitAddress);
    else
        actorWaitValue = nullptr;

    ActSquatStillOffset = actSquatMatch;
    InitializeCamoIndex = reinterpret_cast<InitializeCamoIndexDelegate*>(initializeCamoMatch);
    PlayerSetMotionInternal = reinterpret_cast<PlayerSetMotionDelegate*>(playerSetMotionOffset);
    PlayerStatusCheck = reinterpret_cast<PlayerStatusCheckDelegate*>(playerStatusCheckMatch);
    PlayerStatusSet = reinterpret_cast<PlayerStatusSetDelegate*>(playerStatusSetOffset);

    struct HookEntry
    {
        uintptr_t target;
        LPVOID detour;
        LPVOID* original;
    };

    const HookEntry hooks[] = {
        { actMovementMatch, reinterpret_cast<LPVOID>(ActMovementHook), reinterpret_cast<LPVOID*>(&ActMovement) },
        { setMotionDataMatch, reinterpret_cast<LPVOID>(SetMotionDataHook), reinterpret_cast<LPVOID*>(&SetMotionData) },
        { calculateCamoIndexOffset, reinterpret_cast<LPVOID>(CalculateCamoIndexHook), reinterpret_cast<LPVOID*>(&CalculateCamoIndex) },
        { getButtonMatch, reinterpret_cast<LPVOID>(GetButtonHoldingStateHook), reinterpret_cast<LPVOID*>(&GetButtonHoldingState) },
        { motionPlayMatch, reinterpret_cast<LPVOID>(MotionPlaySequenceHook), reinterpret_cast<LPVOID*>(&MotionPlaySequence) },
        { actSquatMatch, reinterpret_cast<LPVOID>(ActionSquatStillHook), reinterpret_cast<LPVOID*>(&ActionSquatStill) },
    };

    size_t installed = 0;
    for (const auto& hook : hooks)
    {
        if (!Memory::DetourFunction(hook.target, hook.detour, hook.original))
        {
            Log("MinHook failed for target %p", reinterpret_cast<void*>(hook.target));
            for (size_t i = 0; i < installed; ++i)
                MH_RemoveHook(reinterpret_cast<LPVOID>(hooks[i].target));
            return false;
        }
        ++installed;
    }

    CamoIndexData = InitializeCamoIndex(0, 0);
    if (CamoIndexData == nullptr)
    {
        Log("InitializeCamoIndex returned null; removing hooks.");
        for (const auto& hook : hooks)
            MH_RemoveHook(reinterpret_cast<LPVOID>(hook.target));
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(disableCrouchProneOffset, 8, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("VirtualProtect failed for crouch/prone patch. Error=%lu", GetLastError());
        for (const auto& hook : hooks)
            MH_RemoveHook(reinterpret_cast<LPVOID>(hook.target));
        return false;
    }

    // Preserve the original bytes except for the final conditional byte.
    disableCrouchProneOffset[7] = 0x00;

    DWORD ignored = 0;
    VirtualProtect(disableCrouchProneOffset, 8, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), disableCrouchProneOffset, 8);
    Log("Crouch Walk hooks installed successfully. actorWait=%p", actorWaitValue);

    return true;
}

void ReadConfig()
{
    mINI::INIFile file("MGS3CrouchWalk.ini");
    file.read(Config);

    // Keep the defaults when an old/missing INI is encountered. A malformed
    // config should never terminate the loader thread before hooks install.
    try
    {
        if (!Config["Settings"]["CamoIndexModifier"].empty())
            CamoIndexModifier = std::stof(Config["Settings"]["CamoIndexModifier"]);
        if (!Config["Settings"]["CamoIndexValue"].empty())
            CamoIndexValue = std::stoi(Config["Settings"]["CamoIndexValue"]) * 10;
        if (!Config["Settings"]["CrouchWalkSpeed"].empty())
            CrouchWalkSpeed = std::stof(Config["Settings"]["CrouchWalkSpeed"]);
        if (!Config["Settings"]["CrouchStalkSpeed"].empty())
            CrouchStalkSpeed = std::stof(Config["Settings"]["CrouchStalkSpeed"]);
    }
    catch (...)
    {
        CamoIndexModifier = 1.0f;
        CamoIndexValue = 0;
        CrouchWalkSpeed = 6.0f;
        CrouchStalkSpeed = 3.0f;
    }
}

DWORD WINAPI MainThread(LPVOID lpParam)
{
    GameModule = GetModuleHandleA("METAL GEAR SOLID3.exe");
    if (GameModule == nullptr)
    {
        Log("METAL GEAR SOLID3.exe is not loaded.");
        return 1;
    }

    GameBase = (uintptr_t)GameModule;

    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileName(GameModule, exePath, MAX_PATH);
    WCHAR* filename = PathFindFileName(exePath);

    if (wcsncmp(filename, L"launcher.exe", 13) == 0)
        return true;

    Sleep(3000); // allow the game to finish initialising its code/data
    ReadConfig();
    Log("Starting initialization for %ls", filename);
    return InstallHooks() ? 0 : 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, NULL, MainThread, NULL, NULL, NULL);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

