// Rolling Stock Road Transport - TesmioLoader plugin
// Version 1.0.0 public release.
// Target: Workers & Resources: Soviet Republic 1.1.1.7.
//
// Core behaviour:
// - normal ROAD vehicle carriers may carry allowed rail wagons/locomotives;
// - compatibility uses one shared payload + longitudinal deck-length rule for
//   both the route Vehicle type selection UI and real loading;
// - rolling-stock width/height cannot contradict a passed weight/length test;
// - railway depots are valid delivery stops for these carriers;
// - unloading is triggered immediately before the native railway-depot route
//   stop advances and uses the game's own depot-row insertion/storage path.
//
// Performance design:
// - no per-frame building-highlighter hook;
// - no continuous world/building scan;
// - no permanent railway-depot type mutation;
// - static patches are validated against expected bytes before writing.
//
// This is a conventional x64 Windows DLL with a normal DllMain entry point and
// TesmioLoader host-ABI exports.

#include "tesmio_api.h"

#ifndef __fastcall
#define __fastcall
#endif
#ifndef __stdcall
#define __stdcall
#endif

using byte = unsigned char;
using usize = size_t;

extern "C" int _fltused = 0;

typedef unsigned long WinDword;
typedef int WinBool;
typedef void* WinHandle;

extern "C" __declspec(dllimport) WinBool __stdcall VirtualProtect(
    void* address, size_t size, WinDword newProtect, WinDword* oldProtect);
extern "C" __declspec(dllimport) WinBool __stdcall FlushInstructionCache(
    WinHandle process, const void* address, size_t size);
extern "C" __declspec(dllimport) WinHandle __stdcall GetCurrentProcess(void);
extern "C" WinBool __stdcall DllMain(void* module, WinDword reason, void* reserved);

// Retain one image-base relocation so the PE carries a normal relocation table.
static void* volatile g_imageAnchor = reinterpret_cast<void*>(&DllMain);

static const TsmHost* g_host = nullptr;
static byte* g_exeBase = nullptr;
static usize g_exeSize = 0;

static int g_enabled = 1;
static int g_allowWagons = 1;
static int g_allowLocomotives = 1;
static int g_allowRailDepotDropoff = 1;
static int g_enablePickupBridge = 0; // legacy v0.2 setting; deliberately disabled
static int g_enableLoadingPathPatches = 1;
static int g_enableTrainDepotUnloadBridge = 0; // legacy v0.2.7 hook; not installed
static int g_enableTriggeredDepotUnload = 1;
static int g_enableActualLoadWagonScale = 1;
static int g_railFitLengthWeightOnly = 1;
static int g_pickerShowAllFittingRail = 1;
static int g_resetCarrierStateAfterUnload = 0; // v0.3.3: never rewrite native route/load state
static int g_triggerRetryTicks = 60;
static int g_debugLogging = 0;
static int g_debugLimit = 20;
static int g_debugCount = 0;
static int g_unloadTraceCount = 0;
static int g_fitTraceCount = 0;
static int g_pickerTraceCount = 0;
static int g_loadTraceCount = 0;
static int g_triggerUnloadDepth = 0;
static constexpr int UNLOAD_TRACE_LIMIT = 12;
static constexpr int FIT_TRACE_LIMIT = 12;
static constexpr int LOAD_TRACE_LIMIT = 12;
static constexpr int PICKER_TRACE_LIMIT = 12;
static constexpr int TRIGGER_STATE_SLOTS = 64;

// SOVIET64.exe v1.1.1.7.
static constexpr usize RVA_CAN_LOAD_VEHICLE      = 0x003E2350; // FUN_1403e2350
static constexpr usize RVA_BUILDING_COMPAT       = 0x003E2860; // FUN_1403e2860
static constexpr usize RVA_WAGON_FIT_SCALE       = 0x003E2540; // inside FUN_1403e2350
static constexpr usize RVA_CANDIDATE_ELIGIBILITY = 0x0067E680; // FUN_14067e680
static constexpr usize RVA_UNLOAD_CARRIED        = 0x0068A010; // FUN_14068a010 (diagnostic)
static constexpr usize RVA_ACTUAL_LOAD           = 0x0068A320; // FUN_14068a320
static constexpr usize RVA_ACTUAL_LOAD_WAGON_SCALE = 0x0068A68C; // scale branch inside FUN_14068a320
static constexpr usize RVA_RESET_TRANSFER_STATE   = 0x0068B1F0; // FUN_14068b1f0
static constexpr usize RVA_VEHICLE_PROCESS       = 0x0068B230; // FUN_14068b230 (legacy unload observer; no longer hooked)
static constexpr usize RVA_ROUTE_ADVANCE         = 0x0067DA00; // FUN_14067da00 - advances completed route stop
static constexpr usize RVA_VEHICLE_PICKER_LIST   = 0x003DE350; // legacy v0.3.9 hook; not installed
static constexpr usize RVA_RAIL_TOTAL_LENGTH      = 0x003E5960; // FUN_1403e5960 - exact rail length incl. linked parts
static constexpr usize RVA_POINTER_VECTOR_RESERVE = 0x000B1700; // FUN_1400b1700 - reserve pointer vector
static constexpr usize RVA_VEHICLE_TYPE_SELECTOR = 0x003DE6E0; // FUN_1403de6e0
static constexpr usize VEHICLE_TYPE_SELECTOR_SCAN_LENGTH = 0x610;
static constexpr usize RVA_ROUTE_STOP_SELECTION   = 0x002B50B0; // FUN_1402b50b0
static constexpr usize RVA_ROUTE_DEPOT_GATE        = 0x002B5806; // road route fallback inside FUN_1402b50b0
static constexpr usize RVA_BUILDING_HIGHLIGHT     = 0x0040BCC0; // legacy v0.2.4 hook; not installed

// Exact internal branch sites inside FUN_14068b230. These are byte-validated
// against SOVIET64.exe 1.1.1.7 before any write occurs.
static constexpr usize RVA_LOADING_PATH_A = 0x006969C6;
static constexpr usize RVA_LOADING_PATH_B = 0x006972DC;

static constexpr int VEHICLETYPE_ROAD = 1;
static constexpr int VEHICLETYPE_ROAD_SERVICE = 2;
static constexpr int VEHICLETYPE_RAIL_WAGON = 3;
static constexpr int VEHICLETYPE_RAIL_LOCOMOTIVE = 4;

static constexpr int BUILDINGTYPE_ROAD_DEPOT = 0x0E;
static constexpr int BUILDINGTYPE_RAIL_DEPOT = 0x0F;

static constexpr usize VEHICLE_TYPE_OFFSET = 0x294;
static constexpr usize VEHICLE_ASSET_OFFSET = 0x1708;
static constexpr usize VEHICLE_PARENT_OFFSET = 0x398;
static constexpr usize VEHICLE_ATTACHED_BEGIN_OFFSET = 0x3A0;
static constexpr usize VEHICLE_ATTACHED_END_OFFSET = 0x3A8;
static constexpr usize VEHICLE_CARRIED_BEGIN_OFFSET = 0x488;
static constexpr usize VEHICLE_CARRIED_END_OFFSET = 0x490;
static constexpr usize BUILDING_DESC_OFFSET = 0x318;
static constexpr usize BUILDING_TYPE_OFFSET = 0x360;
static constexpr usize BUILDING_SUBTYPE_OFFSET = 0x364;
static constexpr usize BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET = 0xEA8;
static constexpr usize DECK_BEGIN_OFFSET = 0x7C38;
static constexpr usize DECK_END_OFFSET = 0x7C40;
static constexpr usize VEHICLE_CAPACITY_OFFSET = 0x8604;
static constexpr usize VEHICLE_EMPTY_WEIGHT_OFFSET = 0x867C;
static constexpr usize VEHICLE_BBOX_MIN_X_OFFSET = 0x310;
static constexpr usize VEHICLE_BBOX_MIN_Y_OFFSET = 0x314;
static constexpr usize VEHICLE_BBOX_MAX_X_OFFSET = 0x31C;
static constexpr usize VEHICLE_BBOX_MAX_Y_OFFSET = 0x320;
static constexpr usize VEHICLE_RAIL_LENGTH_OFFSET = 0x7A78;
static constexpr usize VEHICLE_LINKED_PARTS_BEGIN_OFFSET = 0x7C08;
static constexpr usize VEHICLE_LINKED_PARTS_END_OFFSET = 0x7C10;
static constexpr usize VEHICLE_LINKED_PART_STRIDE = 0x110;
static constexpr usize VEHICLE_LINKED_PART_ASSET_OFFSET = 0x100;
static constexpr usize VEHICLE_TOPLEVEL_LINK_OFFSET = 0x7C00;
static constexpr usize VEHICLE_HIDDEN_FLAG_A_OFFSET = 0x288;
static constexpr usize VEHICLE_HIDDEN_FLAG_B_OFFSET = 0x289;
static constexpr usize WORLD_VEHICLE_ASSETS_BEGIN_OFFSET = 0x12840;
static constexpr usize WORLD_VEHICLE_ASSETS_END_OFFSET = 0x12848;
static constexpr usize WORLD_PICKER_BEGIN_OFFSET = 0x12858;
static constexpr usize WORLD_VEHICLE_AVAILABILITY_GATE_OFFSET = 0x5D8;
static constexpr usize WORLD_CURRENT_DAY_OFFSET = 0x590;
static constexpr usize WORLD_CURRENT_YEAR_OFFSET = 0x594;
static constexpr usize VEHICLE_AVAILABLE_PARENT_OFFSET = 0x9C68;
static constexpr usize VEHICLE_AVAILABLE_FROM_YEAR_OFFSET = 0x9B50;
static constexpr usize VEHICLE_AVAILABLE_FROM_DAY_OFFSET = 0x9B54;
static constexpr usize VEHICLE_AVAILABLE_TO_YEAR_OFFSET = 0x9B58;
static constexpr usize VEHICLE_AVAILABLE_TO_DAY_OFFSET = 0x9B5C;
static constexpr usize WORLD_PICKER_END_OFFSET = 0x12860;
static constexpr usize WORLD_PICKER_CAP_OFFSET = 0x12868;
static constexpr usize VEHICLE_ASSET_STRIDE = 0x9F08;
static constexpr float RAIL_TRANSVERSE_FIT_EPSILON = 0.10f;
static constexpr int RAIL_TRANSVERSE_SNAPSHOT_LIMIT = 64;
static constexpr int RAIL_FIT_OVERRIDE_ASSET_LIMIT = 128;
static constexpr usize WORLD_SELECTED_VEHICLE_OFFSET = 0xD298;
static constexpr usize VEHICLE_ROUTE_BUILDING_OFFSET = 0x4F0;
static constexpr usize VEHICLE_ROUTE_MATCH_BUILDING_OFFSET = 0x4F8;
static constexpr usize VEHICLE_ACTIVE_BUILDING_OFFSET = 0x528;
static constexpr usize VEHICLE_ALT_ACTIVE_BUILDING_OFFSET = 0x538;
static constexpr usize VEHICLE_RESERVED_BUILDING_OFFSET = 0xC80;
static constexpr usize VEHICLE_OPERATION_STATE_OFFSET = 0x50C;
static constexpr usize VEHICLE_LOADING_PHASE_OFFSET = 0x594;
static constexpr usize VEHICLE_OCCUPIED_BUILDING_OFFSET = 0x5E8;
static constexpr usize VEHICLE_PARENT_ROUTE_MODE_OFFSET = 0x1794;
static constexpr usize VEHICLE_ROUTE_STOPS_BEGIN_OFFSET = 0x680;
static constexpr usize VEHICLE_ROUTE_STOPS_END_OFFSET = 0x688;
static constexpr usize VEHICLE_ROUTE_INDEX_OFFSET = 0x698;

using u64 = unsigned long long;
using CanLoadVehicleFn = char (__fastcall*)(void* context, void* carrier, void* cargo);
using BuildingCompatFn = u64 (__fastcall*)(char* world, u64 building, long long vehicleAsset,
                                           char param4, unsigned int* outReason);
using CandidateEligibilityFn = u64 (__fastcall*)(void* vehicleInstance);
using VehicleProcessFn = u64 (__fastcall*)(void* vehicleInstance, void* param2, void* param3,
                                           char param4, char param5, float param6,
                                           char param7, char param8);
using RouteAdvanceFn = void (__fastcall*)(void* routeOwner, char param2);
using ActualLoadFn = char (__fastcall*)(void* carrierInstance, void* cargoInstance, char param3);
using UnloadCarriedFn = bool (__fastcall*)(void* carrierInstance, void* building,
                                           int* outIndex, void* param4);
using ResetTransferStateFn = void (__fastcall*)(void* carrierInstance);
using VehiclePickerListFn = void (__fastcall*)(void* world, int mode, void* vehicleInstance);
using VehicleTypeSelectorFn = void (__fastcall*)(void* world, char railOnly, void* vehicleInstance, void* group);
using RailTotalLengthFn = float (__fastcall*)(void* vehicleAsset);
using PointerVectorReserveFn = void (__fastcall*)(void* vectorObject, usize extraCount);
using RouteStopSelectionFn = u64 (__fastcall*)(u64 world, int vehicleType, int cargoType,
                                                float highlightPower, long long* routeStops,
                                                long long* selectedVehicles, u64* outBuilding,
                                                u64* outNode);
using BuildingHighlightFn = void (__fastcall*)(u64 world, long long building, int mode,
                                               float highlightPower, char flag);

static CanLoadVehicleFn g_originalCanLoadVehicle = nullptr;
static BuildingCompatFn g_originalBuildingCompat = nullptr;
static CandidateEligibilityFn g_originalCandidateEligibility = nullptr;
static VehicleProcessFn g_originalVehicleProcess = nullptr; // legacy, not installed in v0.3.6
static RouteAdvanceFn g_originalRouteAdvance = nullptr;
static ActualLoadFn g_originalActualLoad = nullptr;
static UnloadCarriedFn g_originalUnloadCarried = nullptr;
static ResetTransferStateFn g_resetTransferState = nullptr;
static VehiclePickerListFn g_originalVehiclePickerList = nullptr; // legacy picker hook; not installed
static VehicleTypeSelectorFn g_originalVehicleTypeSelector = nullptr;
static RailTotalLengthFn g_nativeRailTotalLength = nullptr;
static PointerVectorReserveFn g_pointerVectorReserve = nullptr;
static RouteStopSelectionFn g_originalRouteStopSelection = nullptr;
static BuildingHighlightFn g_originalBuildingHighlight = nullptr;


struct TriggerUnloadState
{
    void* carrier;
    // Exact railway depot this delivery has been armed to use.  This is only
    // learned while the carrier is loaded, after pickup has completed (or as
    // a save-load fallback for a carrier that was already loaded).
    void* railDepotTarget;
    void* lastTarget;
    int armed;
    int armedByPickup;
};

static TriggerUnloadState g_triggerUnloadStates[TRIGGER_STATE_SLOTS] = {};

struct RouteStopSnapshot
{
    void* owner;
    void* stopBuilding;
    int index;
    int valid;
};

// The game simulation path used here is single-threaded. This state lives only
// while a road vehicle carrier is being processed by FUN_14068b230.
static int g_pickupScopeDepth = 0;
static void* g_pendingCargoAsset = nullptr;
static void* g_pendingCargoInstance = nullptr;
static int g_pendingCargoType = 0;

// Rail rolling stock has a very different cross-section from road vehicles.
// The native vehicle-carrier packer checks width and height as well as length,
// which rejects some wagons even when their longitudinal length and weight are
// within the flatbed limits.  v0.3.7 keeps the native test first; only models
// rejected solely by the transverse dimensions are remembered here and use a
// temporary transverse-size override during the real attachment call.
static void* g_lengthWeightOverrideAssets[RAIL_FIT_OVERRIDE_ASSET_LIMIT] = {};
static int g_lengthWeightOverrideAssetCount = 0;

// Route-stop bridge state. The route editor calls the building-highlighter at
// the end of each suitability pass. We cache that exact hovered building and,
// on the next frame, temporarily expose a railway depot as a road depot while
// the selected vehicle is a road flatbed. The game UI is frame-driven and
// single-threaded, so this avoids mutating unrelated simulation work.
static int g_routeSelectionScopeDepth = 0;
static u64 g_routeSelectionWorld = 0;
static u64 g_routeCurrentBuilding = 0;
static u64 g_routeCachedBuilding = 0;

static bool Readable(const void* p, usize n)
{
    return g_host && g_host->readablePtr && g_host->readablePtr(p, n) != 0;
}

static int ReadInt(const void* p)
{
    const volatile int* v = reinterpret_cast<const volatile int*>(p);
    return *v;
}

static void WriteInt(void* p, int value)
{
    volatile int* v = reinterpret_cast<volatile int*>(p);
    *v = value;
}

static byte ReadByte(const void* p)
{
    return *reinterpret_cast<const volatile byte*>(p);
}

static void WriteByte(void* p, byte value)
{
    *reinterpret_cast<volatile byte*>(p) = value;
}

static int CountDisp32(const byte* p, usize n, unsigned value)
{
    const byte b0 = static_cast<byte>(value & 0xffu);
    const byte b1 = static_cast<byte>((value >> 8) & 0xffu);
    const byte b2 = static_cast<byte>((value >> 16) & 0xffu);
    const byte b3 = static_cast<byte>((value >> 24) & 0xffu);
    int count = 0;
    if (n < 4) return 0;
    for (usize i = 0; i + 4 <= n; ++i)
    {
        if (p[i] == b0 && p[i + 1] == b1 && p[i + 2] == b2 && p[i + 3] == b3)
            ++count;
    }
    return count;
}

// A deliberately conservative x86-64 instruction-length decoder. It handles
// the normal MSVC prologue forms used by this game. Unknown forms return 0 and
// make the plugin refuse to patch rather than guessing an instruction boundary.
static usize ModRmLength(const byte* p, usize remaining, bool addressOverride)
{
    if (remaining < 1) return 0;
    const byte modrm = p[0];
    const unsigned mod = (modrm >> 6) & 3u;
    const unsigned rm = modrm & 7u;
    usize len = 1;

    if (mod == 3) return len;

    // In 64-bit mode, the address-size override selects 32-bit addressing. The
    // ModRM/SIB displacement lengths needed here are otherwise the same.
    (void)addressOverride;

    if (rm == 4)
    {
        if (remaining < len + 1) return 0;
        const byte sib = p[len++];
        const unsigned base = sib & 7u;
        if (mod == 0 && base == 5)
        {
            if (remaining < len + 4) return 0;
            len += 4;
        }
    }
    else if (mod == 0 && rm == 5)
    {
        if (remaining < len + 4) return 0;
        len += 4;
    }

    if (mod == 1)
    {
        if (remaining < len + 1) return 0;
        len += 1;
    }
    else if (mod == 2)
    {
        if (remaining < len + 4) return 0;
        len += 4;
    }
    return len;
}

static usize DecodeInstruction(const byte* p, usize remaining)
{
    if (!p || remaining == 0) return 0;

    usize i = 0;
    bool operandOverride = false;
    bool addressOverride = false;
    bool rexW = false;

    // Legacy prefixes.
    for (;;)
    {
        if (i >= remaining) return 0;
        const byte c = p[i];
        if (c == 0x66) { operandOverride = true; ++i; continue; }
        if (c == 0x67) { addressOverride = true; ++i; continue; }
        if (c == 0xF0 || c == 0xF2 || c == 0xF3 ||
            c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 ||
            c == 0x64 || c == 0x65)
        {
            ++i;
            continue;
        }
        break;
    }

    // REX prefix.
    if (i < remaining && (p[i] & 0xF0u) == 0x40u)
    {
        rexW = (p[i] & 0x08u) != 0;
        ++i;
    }
    if (i >= remaining) return 0;

    const byte op = p[i++];

    // Single-byte, no-ModRM instructions common in prologues.
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3 ||
        op == 0xC9 || op == 0xCC || op == 0x9C || op == 0x9D)
        return i;

    if (op >= 0x70 && op <= 0x7F)
        return (remaining >= i + 1) ? i + 1 : 0;

    if (op == 0x6A || op == 0xEB)
        return (remaining >= i + 1) ? i + 1 : 0;

    if (op == 0x68 || op == 0xE8 || op == 0xE9)
        return (remaining >= i + 4) ? i + 4 : 0;

    if (op >= 0xB8 && op <= 0xBF)
    {
        const usize imm = rexW ? 8 : (operandOverride ? 2 : 4);
        return (remaining >= i + imm) ? i + imm : 0;
    }

    if (op == 0xA1 || op == 0xA3)
    {
        const usize addr = addressOverride ? 4 : 8;
        return (remaining >= i + addr) ? i + addr : 0;
    }

    if (op == 0xC2)
        return (remaining >= i + 2) ? i + 2 : 0;

    bool hasModRm = false;
    usize immediate = 0;

    switch (op)
    {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x63:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8D: case 0x8F:
        case 0xD1: case 0xD3:
        case 0xF6: case 0xF7: case 0xFE: case 0xFF:
            hasModRm = true;
            break;
        case 0x69:
            hasModRm = true; immediate = operandOverride ? 2 : 4; break;
        case 0x6B:
            hasModRm = true; immediate = 1; break;
        case 0x80: case 0x82: case 0x83:
            hasModRm = true; immediate = 1; break;
        case 0x81:
            hasModRm = true; immediate = operandOverride ? 2 : 4; break;
        case 0xC0: case 0xC1: case 0xC6:
            hasModRm = true; immediate = 1; break;
        case 0xC7:
            hasModRm = true; immediate = operandOverride ? 2 : 4; break;
        default:
            break;
    }

    if (op == 0x0F)
    {
        if (i >= remaining) return 0;
        const byte op2 = p[i++];
        if (op2 >= 0x80 && op2 <= 0x8F)
            return (remaining >= i + 4) ? i + 4 : 0;

        switch (op2)
        {
            case 0x10: case 0x11: case 0x12: case 0x13:
            case 0x16: case 0x17:
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2E: case 0x2F:
            case 0x40: case 0x41: case 0x42: case 0x43:
            case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B:
            case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            case 0x54: case 0x55: case 0x56: case 0x57:
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            case 0x6E: case 0x6F: case 0x7E: case 0x7F:
            case 0xAF: case 0xB6: case 0xB7: case 0xBE: case 0xBF:
                hasModRm = true;
                break;
            case 0xBA:
                hasModRm = true; immediate = 1; break;
            default:
                return 0;
        }
    }

    if (!hasModRm) return 0;

    const usize mr = ModRmLength(p + i, remaining - i, addressOverride);
    if (!mr) return 0;
    i += mr;

    // TEST r/m, imm uses an immediate only for /0. Reading ModRM is safe here.
    if (op == 0xF6 || op == 0xF7)
    {
        const byte modrm = p[i - mr];
        const unsigned ext = (modrm >> 3) & 7u;
        if (ext == 0)
            immediate = (op == 0xF6) ? 1 : (operandOverride ? 2 : 4);
    }

    return (remaining >= i + immediate) ? i + immediate : 0;
}

static usize PrologueLength(const byte* p, usize maxBytes)
{
    usize total = 0;
    while (total < 14)
    {
        const usize len = DecodeInstruction(p + total, maxBytes - total);
        if (!len) return 0;
        total += len;
        if (total > 64) return 0;
    }
    return total;
}

static void* ReadPtr(const void* p)
{
    return *reinterpret_cast<void* const volatile*>(p);
}

static float ReadFloat(const void* p)
{
    return *reinterpret_cast<const volatile float*>(p);
}

static void WriteFloat(void* p, float value)
{
    *reinterpret_cast<volatile float*>(p) = value;
}

static bool IsAllowedRailType(int type)
{
    return (type == VEHICLETYPE_RAIL_WAGON && g_allowWagons) ||
           (type == VEHICLETYPE_RAIL_LOCOMOTIVE && g_allowLocomotives);
}

static bool IsDetachedCandidate(void* vehicleInstance)
{
    if (!vehicleInstance ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_PARENT_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET, sizeof(void*)))
        return false;

    if (ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_PARENT_OFFSET) != nullptr)
        return false;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET));
    return begin == end;
}

static bool HasVehicleDeck(void* asset)
{
    if (!asset ||
        !Readable(static_cast<byte*>(asset) + DECK_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(asset) + DECK_END_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(asset) + VEHICLE_CAPACITY_OFFSET, sizeof(float)))
        return false;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(asset) + DECK_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(asset) + DECK_END_OFFSET));
    if (!begin || end < begin || end - begin < 0x80 || end - begin > 0x100000)
        return false;

    return ReadFloat(static_cast<byte*>(asset) + VEHICLE_CAPACITY_OFFSET) > 0.0f;
}

static int CarriedCount(void* carrier)
{
    if (!carrier ||
        !Readable(static_cast<byte*>(carrier) + VEHICLE_CARRIED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(carrier) + VEHICLE_CARRIED_END_OFFSET, sizeof(void*)))
        return -1;
    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrier) + VEHICLE_CARRIED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrier) + VEHICLE_CARRIED_END_OFFSET));
    if (end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x100000)
        return -1;
    return static_cast<int>((end - begin) >> 3);
}

static int CountCarriedAllowedRail(void* carrier)
{
    if (!carrier ||
        !Readable(static_cast<byte*>(carrier) + VEHICLE_CARRIED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(carrier) + VEHICLE_CARRIED_END_OFFSET, sizeof(void*)))
        return 0;

    const usize begin = reinterpret_cast<usize>(
        ReadPtr(static_cast<byte*>(carrier) + VEHICLE_CARRIED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(
        ReadPtr(static_cast<byte*>(carrier) + VEHICLE_CARRIED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 ||
        end - begin > 0x100000 || !Readable(reinterpret_cast<void*>(begin), end - begin))
        return 0;

    int count = 0;
    for (usize p = begin; p < end; p += sizeof(void*))
    {
        void* carried = ReadPtr(reinterpret_cast<void*>(p));
        if (!carried ||
            !Readable(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
            continue;
        void* asset = ReadPtr(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET);
        if (!asset || !Readable(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET, sizeof(int)))
            continue;
        if (IsAllowedRailType(ReadInt(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET)))
            ++count;
    }
    return count;
}

static void Trace(const char* fmt, int a = 0, int b = 0, int c = 0, int d = 0)
{
    if (!g_debugLogging || !g_host || g_debugCount >= g_debugLimit)
        return;
    ++g_debugCount;
    g_host->log(fmt, a, b, c, d);
    if (g_debugCount == g_debugLimit)
        g_host->log("Rolling Stock Road Transport  debug trace limit reached; further decisions are suppressed");
}

static void RestorePendingCargo(const char* reason)
{
    if (!g_pendingCargoAsset)
        return;

    if (Readable(static_cast<byte*>(g_pendingCargoAsset) + VEHICLE_TYPE_OFFSET, sizeof(int)))
    {
        const int current = ReadInt(static_cast<byte*>(g_pendingCargoAsset) + VEHICLE_TYPE_OFFSET);
        if (current == VEHICLETYPE_ROAD && IsAllowedRailType(g_pendingCargoType))
            WriteInt(static_cast<byte*>(g_pendingCargoAsset) + VEHICLE_TYPE_OFFSET, g_pendingCargoType);
    }

    if (g_debugLogging && g_debugCount < g_debugLimit)
    {
        ++g_debugCount;
        g_host->log("Rolling Stock Road Transport  restored pending rail type %d (%s)",
                    g_pendingCargoType, reason ? reason : "unspecified");
        if (g_debugCount == g_debugLimit)
            g_host->log("Rolling Stock Road Transport  debug trace limit reached; further decisions are suppressed");
    }

    g_pendingCargoAsset = nullptr;
    g_pendingCargoInstance = nullptr;
    g_pendingCargoType = 0;
}

static bool ResolvePatchApis()
{
    return true;
}

static bool PatchBytes(usize rva, const byte* expected, const byte* replacement,
                       usize length, const char* label)
{
    if (!g_exeBase || rva + length > g_exeSize || !expected || !replacement || length == 0)
    {
        g_host->log("Rolling Stock Road Transport  %s patch range invalid", label);
        return false;
    }

    byte* target = g_exeBase + rva;
    if (!Readable(target, length))
    {
        g_host->log("Rolling Stock Road Transport  %s patch target is unreadable", label);
        return false;
    }

    for (usize i = 0; i < length; ++i)
    {
        if (target[i] != expected[i])
        {
            g_host->log("Rolling Stock Road Transport  %s byte validation failed at RVA 0x%X + 0x%X",
                        label, static_cast<unsigned>(rva), static_cast<unsigned>(i));
            return false;
        }
    }

    if (!ResolvePatchApis())
        return false;

    unsigned long oldProtect = 0;
    if (!VirtualProtect(target, length, 0x40u, &oldProtect)) // PAGE_EXECUTE_READWRITE
    {
        g_host->log("Rolling Stock Road Transport  %s VirtualProtect failed", label);
        return false;
    }

    for (usize i = 0; i < length; ++i)
        target[i] = replacement[i];

    FlushInstructionCache(GetCurrentProcess(), target, length);

    unsigned long ignored = 0;
    VirtualProtect(target, length, oldProtect, &ignored);

    for (usize i = 0; i < length; ++i)
    {
        if (target[i] != replacement[i])
        {
            g_host->log("Rolling Stock Road Transport  %s post-write verification failed", label);
            return false;
        }
    }

    g_host->log("Rolling Stock Road Transport  %s patch active at RVA 0x%X (%u bytes)",
                label, static_cast<unsigned>(rva), static_cast<unsigned>(length));
    return true;
}

static bool InstallVehicleTypeSelectorPatch()
{
    // FUN_1403de6e0 builds the popup shown after choosing Load/Unload vehicle
    // types. For rail types 3/4, vanilla reaches this exact comparison:
    //
    //     selected carrier asset type != SHIP (6) -> reject candidate
    //
    // Replace only the immediate operand with ROAD (1). Once admitted by this
    // category gate, FUN_1403de610 calls FUN_1403e2350, where our existing fit
    // detour preserves all native deck-size and carrying-capacity checks.
    static const byte expected[] = { 0x83,0xB8,0x94,0x02,0x00,0x00,0x06 };
    static const byte patched[]  = { 0x83,0xB8,0x94,0x02,0x00,0x00,0x01 };

    if (!g_exeBase || RVA_VEHICLE_TYPE_SELECTOR + VEHICLE_TYPE_SELECTOR_SCAN_LENGTH > g_exeSize)
    {
        g_host->log("Rolling Stock Road Transport  vehicle-type selector scan range invalid");
        return false;
    }

    byte* start = g_exeBase + RVA_VEHICLE_TYPE_SELECTOR;
    if (!Readable(start, VEHICLE_TYPE_SELECTOR_SCAN_LENGTH))
    {
        g_host->log("Rolling Stock Road Transport  vehicle-type selector scan range unreadable");
        return false;
    }

    usize matchOffset = 0;
    int matches = 0;
    for (usize i = 0; i + sizeof(expected) <= VEHICLE_TYPE_SELECTOR_SCAN_LENGTH; ++i)
    {
        bool same = true;
        for (usize j = 0; j < sizeof(expected); ++j)
        {
            if (start[i + j] != expected[j])
            {
                same = false;
                break;
            }
        }
        if (same)
        {
            matchOffset = i;
            ++matches;
        }
    }

    if (matches != 1)
    {
        g_host->log("Rolling Stock Road Transport  vehicle-type selector gate validation found %d matches (expected 1)", matches);
        return false;
    }

    const usize patchRva = RVA_VEHICLE_TYPE_SELECTOR + matchOffset;
    if (!PatchBytes(patchRva, expected, patched, sizeof(expected),
                    "vehicle-type selector road-carrier rail gate"))
        return false;

    g_host->log("Rolling Stock Road Transport  selector gate now admits rail types 3/4 for ROAD carriers before native fit checks");
    return true;
}

static bool InstallWagonFitScalePatch()
{
    // FUN_1403e2350 applies the native carried-vehicle geometry scale (0.66)
    // to ordinary road vehicles (type 1) and locomotives (type 4). Wagons
    // (type 3) were never intended for road carriers, so they are omitted and
    // are tested at full rail-world dimensions. Extend the same native scale
    // to the contiguous rail range 3..4 while preserving all weight, deck and
    // placement checks.
    static const byte expected[] = {
        0x83,0xFE,0x01,0x75,0x03,0x0F,0x28,0xF0,
        0x83,0xFE,0x04,0x75,0x03,0x0F,0x28,0xF0
    };
    static const byte patched[] = {
        0x83,0xFE,0x01,0x74,0x08,
        0x8D,0x46,0xFD,
        0x83,0xF8,0x01,
        0x77,0x03,
        0x0F,0x28,0xF0
    };
    return PatchBytes(RVA_WAGON_FIT_SCALE, expected, patched, sizeof(expected),
                      "rail-wagon carried geometry scale parity");
}

static bool InstallActualLoadWagonScalePatch()
{
    // FUN_14068a320 repeats the carried-vehicle placement calculation when the
    // vehicle is physically attached to the flatbed. Vanilla applies 0.66 to
    // road vehicles and locomotives, but not wagons. Match the already-patched
    // fit routine so a wagon accepted by the selector cannot fail a second,
    // full-size geometry test during the real load operation.
    static const byte expected[] = {
        0x41,0x3B,0xCA,0x75,0x04,0x44,0x0F,0x28,0xC0,
        0x83,0xF9,0x04,0x75,0x04,0x44,0x0F,0x28,0xC0
    };
    static const byte patched[] = {
        0x83,0xF9,0x01,0x74,0x08,
        0x8D,0x41,0xFD,
        0x83,0xF8,0x01,
        0x77,0x04,
        0x44,0x0F,0x28,0xC0,
        0x90
    };
    return PatchBytes(RVA_ACTUAL_LOAD_WAGON_SCALE, expected, patched, sizeof(expected),
                      "rail-wagon actual-load geometry scale parity");
}

static bool InstallLoadingPathPatches()
{
    // Path A scans vehicles available at the current building. Vanilla admits
    // rail types 3/4 only when the carrier is a ship (type 6), then falls back
    // to a narrow tram exception. Change only that carrier comparison to ROAD.
    // The next call is FUN_14068a310 -> FUN_1403e2350, so native fit still wins.
    static const byte expectedA[] = { 0x83,0xB8,0x94,0x02,0x00,0x00,0x06 };
    static const byte patchedA[]  = { 0x83,0xB8,0x94,0x02,0x00,0x00,0x01 };

    // Path B handles a second stored-vehicle loading route. Preserve vanilla
    // road vehicle (1) and container/cabin (7) branches, add only rail wagon
    // (3) and locomotive (4), reject all other types, and jump directly to the
    // existing FUN_14068a310 fit call. The full 32-byte instruction region is
    // replaced; unlike v0.2.1, no instruction is split or partially overwritten.
    static const byte expectedB[] = {
        0x83,0xF8,0x01,0x74,0x59,
        0x83,0xF8,0x07,0x74,0x54,
        0x83,0xF8,0x04,0x0F,0x85,0x55,0x01,0x00,0x00,
        0x80,0xBE,0x7D,0x06,0x00,0x00,0x00,
        0x0F,0x84,0x48,0x01,0x00,0x00
    };
    static const byte patchedB[] = {
        0x83,0xF8,0x01,0x74,0x59,
        0x83,0xF8,0x07,0x74,0x54,
        0x83,0xE8,0x03,
        0x83,0xF8,0x01,
        0x0F,0x87,0x52,0x01,0x00,0x00,
        0xE9,0x43,0x00,0x00,0x00,
        0x0F,0x1F,0x44,0x00,0x00
    };

    if (!PatchBytes(RVA_LOADING_PATH_A, expectedA, patchedA, sizeof(expectedA),
                    "road-carrier rail pickup gate A"))
        return false;
    if (!PatchBytes(RVA_LOADING_PATH_B, expectedB, patchedB, sizeof(expectedB),
                    "road-carrier rail pickup gate B"))
        return false;
    return true;
}

static bool InstallRouteDepotPatch()
{
    // FUN_1402b50b0 has a fallback for depot-class buildings. Vanilla permits
    // a ROAD vehicle only when the hovered building type is exactly road depot
    // 0x0E. At this point EAX already contains the root building type and the
    // surrounding bitset has limited it to depot/terminal classes. Accept the
    // two-value range 0x0E..0x0F and leave every other route rule unchanged.
    // This replaces the v0.2.4 high-frequency highlighter/route detours.
    static const byte expected[] = {
        0x48,0x8B,0x83,0x18,0x03,0x00,0x00,
        0x83,0xB8,0x60,0x03,0x00,0x00,0x0E,
        0x0F,0x85,0x9E,0x0E,0x00,0x00
    };
    static const byte patched[] = {
        0x83,0xF8,0x0E,
        0x74,0x0F,
        0x83,0xF8,0x0F,
        0x0F,0x85,0xA4,0x0E,0x00,0x00,
        0x66,0x0F,0x1F,0x44,0x00,0x00
    };
    return PatchBytes(RVA_ROUTE_DEPOT_GATE, expected, patched, sizeof(expected),
                      "road flatbed road/rail depot route gate");
}

static bool ValidateCanLoadTarget(const byte* target)
{
    if (!target || !Readable(target, 0x500)) return false;
    return CountDisp32(target, 0x500, 0x00000294u) >= 3 &&
           CountDisp32(target, 0x500, 0x00007C38u) >= 2 &&
           CountDisp32(target, 0x500, 0x00007C40u) >= 2 &&
           CountDisp32(target, 0x500, 0x00008600u) >= 1 &&
           CountDisp32(target, 0x500, 0x00008604u) >= 1 &&
           CountDisp32(target, 0x500, 0x0000867Cu) >= 1;
}

static bool ValidateBuildingCompatTarget(const byte* target)
{
    if (!target || !Readable(target, 0x1400)) return false;
    return CountDisp32(target, 0x1400, 0x00000318u) >= 2 &&
           CountDisp32(target, 0x1400, 0x00000360u) >= 1 &&
           CountDisp32(target, 0x1400, 0x00000364u) >= 8 &&
           CountDisp32(target, 0x1400, 0x00000294u) >= 3;
}

static bool ValidateEligibilityTarget(const byte* target)
{
    if (!target || !Readable(target, 0x60)) return false;
    return CountDisp32(target, 0x60, 0x00001794u) >= 1 &&
           CountDisp32(target, 0x60, 0x00001708u) >= 1 &&
           CountDisp32(target, 0x60, 0x00000294u) >= 1;
}

static bool ValidateVehicleProcessTarget(const byte* target)
{
    if (!target || !Readable(target, 0xC500)) return false;
    return CountDisp32(target, 0xC500, 0x00001708u) >= 5 &&
           CountDisp32(target, 0xC500, 0x00000294u) >= 12 &&
           CountDisp32(target, 0xC500, 0x0000179Cu) >= 5 &&
           CountDisp32(target, 0xC500, 0x000003A0u) >= 1 &&
           CountDisp32(target, 0xC500, 0x000003A8u) >= 1;
}

static bool ValidateRouteAdvanceTarget(const byte* target)
{
    // FUN_14067da00 starts by incrementing the current route index.  The hook
    // must run before that write so the still-current stop can be inspected.
    static const byte expected[] = {
        0x40,0x57,
        0x48,0x83,0xEC,0x20,
        0xFF,0x81,0x98,0x06,0x00,0x00,
        0x33,0xFF
    };
    if (!target || !Readable(target, sizeof(expected))) return false;
    for (usize i = 0; i < sizeof(expected); ++i)
        if (target[i] != expected[i]) return false;
    return true;
}

static bool ValidateActualLoadTarget(const byte* target)
{
    if (!target || !Readable(target, 0x900)) return false;
    return CountDisp32(target, 0x900, 0x00001708u) >= 8 &&
           CountDisp32(target, 0x900, 0x00007C38u) >= 2 &&
           CountDisp32(target, 0x900, 0x00008604u) >= 1 &&
           CountDisp32(target, 0x900, 0x0000867Cu) >= 2;
}

static bool ValidateUnloadTarget(const byte* target)
{
    if (!target || !Readable(target, 0x300)) return false;
    return CountDisp32(target, 0x300, 0x00000488u) >= 2 &&
           CountDisp32(target, 0x300, 0x00000490u) >= 1 &&
           CountDisp32(target, 0x300, 0x00001708u) >= 1;
}

static bool ResolveTransferStateReset()
{
    if (!g_exeBase || RVA_RESET_TRANSFER_STATE + 0x33 > g_exeSize)
        return false;
    byte* target = g_exeBase + RVA_RESET_TRANSFER_STATE;
    static const byte expected[] = {
        0x33,0xC0,
        0xC7,0x81,0x68,0x05,0x00,0x00,0x00,0x00,0x80,0x3F,
        0x48,0x89,0x81,0x5C,0x05,0x00,0x00,
        0x48,0x89,0x81,0x94,0x05,0x00,0x00
    };
    if (!Readable(target, sizeof(expected)))
        return false;
    for (usize i = 0; i < sizeof(expected); ++i)
        if (target[i] != expected[i])
            return false;
    g_resetTransferState = reinterpret_cast<ResetTransferStateFn>(target);
    return true;
}

static bool InstallHook(usize rva, void* detour, void** original,
                        bool (*validator)(const byte*), const char* label, bool required)
{
    if (rva + 0x100 > g_exeSize)
    {
        g_host->log("Rolling Stock Road Transport  %s RVA outside executable", label);
        return !required;
    }

    byte* target = g_exeBase + rva;
    if (validator && !validator(target))
    {
        g_host->log("Rolling Stock Road Transport  %s validation failed at RVA 0x%X",
                    label, static_cast<unsigned>(rva));
        return !required;
    }

    const usize stolen = PrologueLength(target, 64);
    if (stolen < 14 || stolen > 64)
    {
        g_host->log("Rolling Stock Road Transport  %s has no safe hook boundary at RVA 0x%X",
                    label, static_cast<unsigned>(rva));
        return !required;
    }

    byte expected[64];
    for (usize i = 0; i < stolen; ++i) expected[i] = target[i];

    void* trampoline = nullptr;
    if (!g_host->installInlineHook(target, detour, &trampoline, expected, stolen, label))
    {
        g_host->log("Rolling Stock Road Transport  %s hook installation failed", label);
        return !required;
    }

    *original = trampoline;
    g_host->log("Rolling Stock Road Transport  %s active at RVA 0x%X (%u bytes)",
                label, static_cast<unsigned>(rva), static_cast<unsigned>(stolen));
    return true;
}

struct RailTransverseSnapshot
{
    byte* asset;
    float minX;
    float minY;
    float maxX;
    float maxY;
};

static bool IsLengthWeightOverrideAsset(void* asset)
{
    if (!asset) return false;
    for (int i = 0; i < g_lengthWeightOverrideAssetCount; ++i)
    {
        if (g_lengthWeightOverrideAssets[i] == asset)
            return true;
    }
    return false;
}

static void RememberLengthWeightOverrideAsset(void* asset)
{
    if (!asset || IsLengthWeightOverrideAsset(asset))
        return;
    if (g_lengthWeightOverrideAssetCount >= RAIL_FIT_OVERRIDE_ASSET_LIMIT)
        return;
    g_lengthWeightOverrideAssets[g_lengthWeightOverrideAssetCount++] = asset;
}

static bool SnapshotContainsAsset(const RailTransverseSnapshot* snapshots, int count, const byte* asset)
{
    for (int i = 0; i < count; ++i)
    {
        if (snapshots[i].asset == asset)
            return true;
    }
    return false;
}

static bool NarrowOneAssetTransverse(byte* asset, RailTransverseSnapshot* snapshots, int* count)
{
    if (!asset || !snapshots || !count)
        return false;
    if (SnapshotContainsAsset(snapshots, *count, asset))
        return true;
    if (*count >= RAIL_TRANSVERSE_SNAPSHOT_LIMIT)
        return false;
    if (!Readable(asset + VEHICLE_BBOX_MIN_X_OFFSET, sizeof(float)) ||
        !Readable(asset + VEHICLE_BBOX_MIN_Y_OFFSET, sizeof(float)) ||
        !Readable(asset + VEHICLE_BBOX_MAX_X_OFFSET, sizeof(float)) ||
        !Readable(asset + VEHICLE_BBOX_MAX_Y_OFFSET, sizeof(float)))
        return false;

    RailTransverseSnapshot& snap = snapshots[*count];
    snap.asset = asset;
    snap.minX = ReadFloat(asset + VEHICLE_BBOX_MIN_X_OFFSET);
    snap.minY = ReadFloat(asset + VEHICLE_BBOX_MIN_Y_OFFSET);
    snap.maxX = ReadFloat(asset + VEHICLE_BBOX_MAX_X_OFFSET);
    snap.maxY = ReadFloat(asset + VEHICLE_BBOX_MAX_Y_OFFSET);
    ++(*count);

    // Preserve the minimum/centre reference used by the asset; only the two
    // transverse extents seen by the native packer are collapsed.  The rail
    // longitudinal length at +0x7A78 (including linked articulated sections)
    // is never touched, and neither is empty weight.
    WriteFloat(asset + VEHICLE_BBOX_MAX_X_OFFSET, snap.minX + RAIL_TRANSVERSE_FIT_EPSILON);
    WriteFloat(asset + VEHICLE_BBOX_MAX_Y_OFFSET, snap.minY + RAIL_TRANSVERSE_FIT_EPSILON);
    return true;
}

static bool NarrowAssetAndLinkedParts(byte* asset, RailTransverseSnapshot* snapshots, int* count)
{
    if (!NarrowOneAssetTransverse(asset, snapshots, count))
        return false;

    if (!Readable(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(asset + VEHICLE_LINKED_PARTS_END_OFFSET, sizeof(void*)))
        return true;

    const usize begin = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_END_OFFSET));
    if (!begin || end < begin || (end - begin) % VEHICLE_LINKED_PART_STRIDE != 0 ||
        end - begin > VEHICLE_LINKED_PART_STRIDE * 32u)
        return true;

    const usize bytes = end - begin;
    if (bytes && !Readable(reinterpret_cast<void*>(begin), bytes))
        return true;

    const usize linkedCount = bytes / VEHICLE_LINKED_PART_STRIDE;
    for (usize i = 0; i < linkedCount; ++i)
    {
        byte* record = reinterpret_cast<byte*>(begin + i * VEHICLE_LINKED_PART_STRIDE);
        if (!Readable(record + VEHICLE_LINKED_PART_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* linkedAsset = static_cast<byte*>(ReadPtr(record + VEHICLE_LINKED_PART_ASSET_OFFSET));
        if (!linkedAsset)
            continue;
        if (!NarrowOneAssetTransverse(linkedAsset, snapshots, count))
            return false;
    }
    return true;
}

static void RestoreTransverseSnapshots(RailTransverseSnapshot* snapshots, int count)
{
    if (!snapshots) return;
    for (int i = count - 1; i >= 0; --i)
    {
        byte* asset = snapshots[i].asset;
        if (!asset) continue;
        WriteFloat(asset + VEHICLE_BBOX_MIN_X_OFFSET, snapshots[i].minX);
        WriteFloat(asset + VEHICLE_BBOX_MIN_Y_OFFSET, snapshots[i].minY);
        WriteFloat(asset + VEHICLE_BBOX_MAX_X_OFFSET, snapshots[i].maxX);
        WriteFloat(asset + VEHICLE_BBOX_MAX_Y_OFFSET, snapshots[i].maxY);
    }
}

static float EffectiveRailLength(byte* asset)
{
    if (!asset || !Readable(asset + VEHICLE_RAIL_LENGTH_OFFSET, sizeof(float)))
        return -1.0f;
    float length = ReadFloat(asset + VEHICLE_RAIL_LENGTH_OFFSET);
    if (!Readable(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(asset + VEHICLE_LINKED_PARTS_END_OFFSET, sizeof(void*)))
        return length;

    const usize begin = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_END_OFFSET));
    if (!begin || end < begin || (end - begin) % VEHICLE_LINKED_PART_STRIDE != 0 ||
        end - begin > VEHICLE_LINKED_PART_STRIDE * 32u)
        return length;
    if (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin))
        return length;

    const usize linkedCount = (end - begin) / VEHICLE_LINKED_PART_STRIDE;
    for (usize i = 0; i < linkedCount; ++i)
    {
        byte* record = reinterpret_cast<byte*>(begin + i * VEHICLE_LINKED_PART_STRIDE);
        if (!Readable(record + VEHICLE_LINKED_PART_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* linkedAsset = static_cast<byte*>(ReadPtr(record + VEHICLE_LINKED_PART_ASSET_OFFSET));
        if (linkedAsset && Readable(linkedAsset + VEHICLE_RAIL_LENGTH_OFFSET, sizeof(float)))
            length += ReadFloat(linkedAsset + VEHICLE_RAIL_LENGTH_OFFSET);
    }
    return length;
}

static float MaxCarrierDeckSpan(byte* carrierAsset)
{
    if (!carrierAsset ||
        !Readable(carrierAsset + DECK_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(carrierAsset + DECK_END_OFFSET, sizeof(void*)))
        return -1.0f;
    const usize begin = reinterpret_cast<usize>(ReadPtr(carrierAsset + DECK_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(carrierAsset + DECK_END_OFFSET));
    if (!begin || end < begin || (end - begin) % 0x80u != 0 || end - begin > 0x8000u)
        return -1.0f;
    if (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin))
        return -1.0f;

    float maxSpan = 0.0f;
    const usize count = (end - begin) / 0x80u;
    for (usize i = 0; i < count; ++i)
    {
        byte* region = reinterpret_cast<byte*>(begin + i * 0x80u);
        const float dx = ReadFloat(region + 0x18) - ReadFloat(region + 0x0C);
        const float dy = ReadFloat(region + 0x1C) - ReadFloat(region + 0x10);
        const float dz = ReadFloat(region + 0x20) - ReadFloat(region + 0x14);
        const float adx = dx < 0.0f ? -dx : dx;
        const float ady = dy < 0.0f ? -dy : dy;
        const float adz = dz < 0.0f ? -dz : dz;
        if (maxSpan < adx) maxSpan = adx;
        if (maxSpan < ady) maxSpan = ady;
        if (maxSpan < adz) maxSpan = adz;
    }
    return maxSpan;
}


static float NativeRailTotalLength(byte* asset)
{
    if (!asset)
        return -1.0f;
    if (g_nativeRailTotalLength)
    {
        const float value = g_nativeRailTotalLength(asset);
        if (value > 0.0f && value < 1000.0f)
            return value;
    }
    return EffectiveRailLength(asset);
}

static float RailCarriedLength(byte* asset)
{
    const float raw = NativeRailTotalLength(asset);
    if (raw <= 0.0f)
        return -1.0f;
    // Match the game's carried-vehicle visual/placement scale already used for
    // locomotives and patched for wagons. This is the longitudinal value users
    // actually get when the rail vehicle is on a road flatbed.
    return raw * 0.66f;
}

static float RailTotalEmptyWeight(byte* asset)
{
    if (!asset || !Readable(asset + VEHICLE_EMPTY_WEIGHT_OFFSET, sizeof(float)))
        return -1.0f;

    float total = ReadFloat(asset + VEHICLE_EMPTY_WEIGHT_OFFSET);
    if (!Readable(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(asset + VEHICLE_LINKED_PARTS_END_OFFSET, sizeof(void*)))
        return total;

    const usize begin = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(asset + VEHICLE_LINKED_PARTS_END_OFFSET));
    if (!begin || end < begin || (end - begin) % VEHICLE_LINKED_PART_STRIDE != 0 ||
        end - begin > VEHICLE_LINKED_PART_STRIDE * 32u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return total;

    const usize count = (end - begin) / VEHICLE_LINKED_PART_STRIDE;
    for (usize i = 0; i < count; ++i)
    {
        byte* record = reinterpret_cast<byte*>(begin + i * VEHICLE_LINKED_PART_STRIDE);
        if (!Readable(record + VEHICLE_LINKED_PART_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* linked = static_cast<byte*>(ReadPtr(record + VEHICLE_LINKED_PART_ASSET_OFFSET));
        if (linked && Readable(linked + VEHICLE_EMPTY_WEIGHT_OFFSET, sizeof(float)))
            total += ReadFloat(linked + VEHICLE_EMPTY_WEIGHT_OFFSET);
    }
    return total;
}

struct DeckInterval
{
    float begin;
    float end;
};

static float CarrierDeckLongitudinalSpan(byte* carrierAsset)
{
    if (!carrierAsset ||
        !Readable(carrierAsset + DECK_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(carrierAsset + DECK_END_OFFSET, sizeof(void*)))
        return -1.0f;

    const usize begin = reinterpret_cast<usize>(ReadPtr(carrierAsset + DECK_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(carrierAsset + DECK_END_OFFSET));
    if (!begin || end < begin || (end - begin) % 0x80u != 0 || end - begin > 0x8000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return -1.0f;

    DeckInterval intervals[64];
    int intervalCount = 0;
    const usize regionCount = (end - begin) / 0x80u;
    for (usize i = 0; i < regionCount && intervalCount < 64; ++i)
    {
        byte* region = reinterpret_cast<byte*>(begin + i * 0x80u);
        // FUN_1403e2350 passes the deck box to FUN_140002890 as (dx, dz, dy).
        // Candidate rail length is its second packed dimension, therefore deck
        // longitudinal capacity is the local Z interval (+0x14 .. +0x20).
        float z0 = ReadFloat(region + 0x14);
        float z1 = ReadFloat(region + 0x20);
        if (z1 < z0)
        {
            const float t = z0;
            z0 = z1;
            z1 = t;
        }
        if (z1 <= z0)
            continue;
        intervals[intervalCount].begin = z0;
        intervals[intervalCount].end = z1;
        ++intervalCount;
    }
    if (intervalCount == 0)
        return -1.0f;

    // Sort and merge touching/overlapping longitudinal deck regions. This
    // handles carriers whose usable flatbed is authored as multiple adjacent
    // deck boxes instead of one large rectangle.
    for (int i = 0; i < intervalCount - 1; ++i)
    {
        int best = i;
        for (int j = i + 1; j < intervalCount; ++j)
            if (intervals[j].begin < intervals[best].begin)
                best = j;
        if (best != i)
        {
            const DeckInterval t = intervals[i];
            intervals[i] = intervals[best];
            intervals[best] = t;
        }
    }

    float maxSpan = 0.0f;
    float currentBegin = intervals[0].begin;
    float currentEnd = intervals[0].end;
    for (int i = 1; i < intervalCount; ++i)
    {
        if (intervals[i].begin <= currentEnd + 0.05f)
        {
            if (intervals[i].end > currentEnd)
                currentEnd = intervals[i].end;
        }
        else
        {
            const float span = currentEnd - currentBegin;
            if (span > maxSpan) maxSpan = span;
            currentBegin = intervals[i].begin;
            currentEnd = intervals[i].end;
        }
    }
    const float finalSpan = currentEnd - currentBegin;
    if (finalSpan > maxSpan) maxSpan = finalSpan;
    return maxSpan;
}

static bool IsCurrentRailAssetAvailable(void* world, byte* asset)
{
    if (!world || !asset)
        return true;

    // Respect the game's global "all vehicles"/availability setting.
    if (Readable(static_cast<byte*>(world) + WORLD_VEHICLE_AVAILABILITY_GATE_OFFSET, sizeof(int)) &&
        ReadInt(static_cast<byte*>(world) + WORLD_VEHICLE_AVAILABILITY_GATE_OFFSET) == 0)
        return true;

    if (!Readable(static_cast<byte*>(world) + WORLD_CURRENT_YEAR_OFFSET, sizeof(float)) ||
        !Readable(static_cast<byte*>(world) + WORLD_CURRENT_DAY_OFFSET, sizeof(int)))
        return true;

    const int currentYear = static_cast<int>(ReadFloat(static_cast<byte*>(world) + WORLD_CURRENT_YEAR_OFFSET));
    const int currentDay = ReadInt(static_cast<byte*>(world) + WORLD_CURRENT_DAY_OFFSET);

    // Native vehicle lists stop applying historical production gates after
    // 1995; preserve that behaviour.
    if (currentYear > 1995)
        return true;

    byte* dateAsset = asset;
    if (Readable(asset + VEHICLE_AVAILABLE_PARENT_OFFSET, sizeof(void*)))
    {
        byte* parent = static_cast<byte*>(ReadPtr(asset + VEHICLE_AVAILABLE_PARENT_OFFSET));
        if (parent && Readable(parent + VEHICLE_AVAILABLE_FROM_YEAR_OFFSET, sizeof(float)))
            dateAsset = parent;
    }

    if (!Readable(dateAsset + VEHICLE_AVAILABLE_FROM_YEAR_OFFSET, sizeof(float)) ||
        !Readable(dateAsset + VEHICLE_AVAILABLE_FROM_DAY_OFFSET, sizeof(int)) ||
        !Readable(dateAsset + VEHICLE_AVAILABLE_TO_YEAR_OFFSET, sizeof(float)) ||
        !Readable(dateAsset + VEHICLE_AVAILABLE_TO_DAY_OFFSET, sizeof(int)))
        return true;

    const int fromYear = static_cast<int>(ReadFloat(dateAsset + VEHICLE_AVAILABLE_FROM_YEAR_OFFSET));
    const int fromDay = ReadInt(dateAsset + VEHICLE_AVAILABLE_FROM_DAY_OFFSET);
    const int toYear = static_cast<int>(ReadFloat(dateAsset + VEHICLE_AVAILABLE_TO_YEAR_OFFSET));
    const int toDay = ReadInt(dateAsset + VEHICLE_AVAILABLE_TO_DAY_OFFSET);

    if (fromYear > 0)
    {
        if (currentYear < fromYear) return false;
        if (currentYear == fromYear && currentDay < fromDay) return false;
    }

    // A sane production-end date is enforced so the selector corresponds to
    // what can be bought at Customs now. Invalid/sentinel end dates are left
    // unrestricted, matching the game's permissive handling of modded assets.
    if (toYear >= fromYear && toYear > 0 && toYear < 3000)
    {
        if (currentYear > toYear) return false;
        if (currentYear == toYear && currentDay > toDay) return false;
    }
    return true;
}

struct RailFitLimits
{
    byte* carrierAsset;
    float payloadLimit;
    float deckLengthLimit;
    float cargoWeight;
    float cargoLength;
};

static bool RailFitsCarrierAssetByWeightLength(byte* carrierAsset, byte* cargoAsset,
                                                float existingWeight, float existingLength,
                                                RailFitLimits* outLimits)
{
    if (outLimits)
    {
        outLimits->carrierAsset = carrierAsset;
        outLimits->payloadLimit = -1.0f;
        outLimits->deckLengthLimit = -1.0f;
        outLimits->cargoWeight = -1.0f;
        outLimits->cargoLength = -1.0f;
    }

    if (!carrierAsset || !cargoAsset ||
        !Readable(carrierAsset + VEHICLE_TYPE_OFFSET, sizeof(int)) ||
        !Readable(cargoAsset + VEHICLE_TYPE_OFFSET, sizeof(int)) ||
        !Readable(carrierAsset + VEHICLE_CAPACITY_OFFSET, sizeof(float)) ||
        !HasVehicleDeck(carrierAsset))
        return false;

    if (ReadInt(carrierAsset + VEHICLE_TYPE_OFFSET) != VEHICLETYPE_ROAD ||
        !IsAllowedRailType(ReadInt(cargoAsset + VEHICLE_TYPE_OFFSET)))
        return false;

    const float capacity = ReadFloat(carrierAsset + VEHICLE_CAPACITY_OFFSET);
    const float deckLength = CarrierDeckLongitudinalSpan(carrierAsset);
    const float cargoWeight = RailTotalEmptyWeight(cargoAsset);
    const float cargoLength = RailCarriedLength(cargoAsset);

    if (outLimits)
    {
        outLimits->payloadLimit = capacity;
        outLimits->deckLengthLimit = deckLength;
        outLimits->cargoWeight = cargoWeight;
        outLimits->cargoLength = cargoLength;
    }

    if (capacity <= 0.0f || deckLength <= 0.0f || cargoWeight < 0.0f || cargoLength <= 0.0f)
        return false;

    // The mod's eligibility contract is intentionally one-dimensional:
    // payload mass + longitudinal carried length only. No width/height/3D box
    // packing is allowed to veto a rail vehicle that satisfies these limits.
    return existingWeight + cargoWeight <= capacity + 0.001f &&
           existingLength + cargoLength <= deckLength + 0.001f;
}

static bool VehicleInstanceFitsRailByWeightLength(void* vehicleInstance, byte* cargoAsset,
                                                   RailFitLimits* outLimits)
{
    if (!vehicleInstance || !cargoAsset ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        return false;

    byte* ownAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET));
    RailFitLimits limits = {};
    if (RailFitsCarrierAssetByWeightLength(ownAsset, cargoAsset, 0.0f, 0.0f, &limits))
    {
        if (outLimits) *outLimits = limits;
        return true;
    }

    if (!Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET, sizeof(void*)))
        return false;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x4000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return false;

    for (usize p = begin; p < end; p += sizeof(void*))
    {
        void* child = ReadPtr(reinterpret_cast<void*>(p));
        if (!child || !Readable(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* childAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET));
        if (RailFitsCarrierAssetByWeightLength(childAsset, cargoAsset, 0.0f, 0.0f, &limits))
        {
            if (outLimits) *outLimits = limits;
            return true;
        }
    }
    return false;
}

static char __fastcall DetourCanLoadVehicle(void* context, void* carrier, void* cargo)
{
    if (!g_originalCanLoadVehicle)
        return 0;

    // Legacy candidate bridge cleanup. The static pickup gates used by current
    // builds do not normally use this, but a stale pending type must never leak
    // into the shared vehicle asset table.
    if (g_pendingCargoAsset)
    {
        if (cargo == g_pendingCargoAsset)
            RestorePendingCargo("weight/length fit reached");
        else
            RestorePendingCargo("different fit candidate reached");
    }

    if (!carrier || !cargo ||
        !Readable(static_cast<byte*>(carrier) + VEHICLE_TYPE_OFFSET, sizeof(int)) ||
        !Readable(static_cast<byte*>(cargo) + VEHICLE_TYPE_OFFSET, sizeof(int)))
        return g_originalCanLoadVehicle(context, carrier, cargo);

    byte* carrierAsset = static_cast<byte*>(carrier);
    byte* cargoAsset = static_cast<byte*>(cargo);
    const int carrierType = ReadInt(carrierAsset + VEHICLE_TYPE_OFFSET);
    const int cargoType = ReadInt(cargoAsset + VEHICLE_TYPE_OFFSET);

    if (carrierType != VEHICLETYPE_ROAD || !IsAllowedRailType(cargoType))
        return g_originalCanLoadVehicle(context, carrier, cargo);

    RailFitLimits limits = {};
    const bool fits = RailFitsCarrierAssetByWeightLength(
        carrierAsset, cargoAsset, 0.0f, 0.0f, &limits);

    if (fits)
        RememberLengthWeightOverrideAsset(cargoAsset);

    Trace("Rolling Stock Road Transport  unified fit: rail type %d result %d", cargoType, fits ? 1 : 0);
    if (g_host && g_fitTraceCount < FIT_TRACE_LIMIT)
    {
        ++g_fitTraceCount;
        const int weight10 = limits.cargoWeight < 0.0f ? -1 : static_cast<int>(limits.cargoWeight * 10.0f + 0.5f);
        const int capacity10 = limits.payloadLimit < 0.0f ? -1 : static_cast<int>(limits.payloadLimit * 10.0f + 0.5f);
        const int lengthCm = limits.cargoLength < 0.0f ? -1 : static_cast<int>(limits.cargoLength * 100.0f + 0.5f);
        const int deckCm = limits.deckLengthLimit < 0.0f ? -1 : static_cast<int>(limits.deckLengthLimit * 100.0f + 0.5f);
        g_host->log(
            "Rolling Stock Road Transport  weight/length fit: carrier_asset=%p cargo_asset=%p rail_type=%d result=%d weight_x10=%d capacity_x10=%d carried_length_cm=%d deck_length_cm=%d",
            carrier, cargo, cargoType, fits ? 1 : 0, weight10, capacity10, lengthCm, deckCm);
        if (g_fitTraceCount == FIT_TRACE_LIMIT)
            g_host->log("Rolling Stock Road Transport  fit trace limit reached");
    }
    return fits ? 1 : 0;
}

static byte* ResolveAssetPointer(long long value)
{
    if (!value) return nullptr;
    byte* direct = reinterpret_cast<byte*>(static_cast<usize>(value));
    if (Readable(direct + VEHICLE_TYPE_OFFSET, sizeof(int)))
    {
        const int t = ReadInt(direct + VEHICLE_TYPE_OFFSET);
        if (0 <= t && t <= 16) return direct;
    }
    if (Readable(direct + VEHICLE_ASSET_OFFSET, sizeof(void*)))
    {
        void* nested = ReadPtr(direct + VEHICLE_ASSET_OFFSET);
        if (nested && Readable(static_cast<byte*>(nested) + VEHICLE_TYPE_OFFSET, sizeof(int)))
            return static_cast<byte*>(nested);
    }
    return nullptr;
}

static bool ResolveBuildingKind(u64 building, int* outType, int* outSubtype)
{
    if (outType) *outType = -1;
    if (outSubtype) *outSubtype = -1;
    if (!building) return false;
    byte* buildingPtr = reinterpret_cast<byte*>(static_cast<usize>(building));
    if (!Readable(buildingPtr + BUILDING_DESC_OFFSET, sizeof(void*))) return false;
    void* desc = ReadPtr(buildingPtr + BUILDING_DESC_OFFSET);
    if (!desc || !Readable(static_cast<byte*>(desc) + BUILDING_TYPE_OFFSET, sizeof(int)) ||
        !Readable(static_cast<byte*>(desc) + BUILDING_SUBTYPE_OFFSET, sizeof(int))) return false;
    if (outType) *outType = ReadInt(static_cast<byte*>(desc) + BUILDING_TYPE_OFFSET);
    if (outSubtype) *outSubtype = ReadInt(static_cast<byte*>(desc) + BUILDING_SUBTYPE_OFFSET);
    return true;
}

static byte* ResolveBuildingDesc(u64 building)
{
    if (!building) return nullptr;
    byte* buildingPtr = reinterpret_cast<byte*>(static_cast<usize>(building));
    if (!Readable(buildingPtr + BUILDING_DESC_OFFSET, sizeof(void*))) return nullptr;
    void* desc = ReadPtr(buildingPtr + BUILDING_DESC_OFFSET);
    if (!desc || !Readable(static_cast<byte*>(desc) + BUILDING_TYPE_OFFSET, sizeof(int)) ||
        !Readable(static_cast<byte*>(desc) + BUILDING_SUBTYPE_OFFSET, sizeof(int)))
        return nullptr;
    return static_cast<byte*>(desc);
}

static bool IsVehicleCarrierInstance(void* vehicleInstance)
{
    if (!vehicleInstance ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        return false;
    void* asset = ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET);
    if (!asset || !Readable(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET, sizeof(int)))
        return false;
    return ReadInt(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET) == VEHICLETYPE_ROAD &&
           HasVehicleDeck(asset);
}

static bool SelectedRouteVehicleIsCarrier(u64 world, long long* selectedVehicles)
{
    if (world)
    {
        byte* worldPtr = reinterpret_cast<byte*>(static_cast<usize>(world));
        if (Readable(worldPtr + WORLD_SELECTED_VEHICLE_OFFSET, sizeof(void*)))
        {
            void* selected = ReadPtr(worldPtr + WORLD_SELECTED_VEHICLE_OFFSET);
            if (IsVehicleCarrierInstance(selected))
                return true;
        }
    }

    if (selectedVehicles && Readable(selectedVehicles, sizeof(long long) * 2))
    {
        const usize begin = static_cast<usize>(selectedVehicles[0]);
        const usize end = static_cast<usize>(selectedVehicles[1]);
        if (begin && end >= begin && ((end - begin) & 7u) == 0 && end - begin <= 0x4000 &&
            Readable(reinterpret_cast<void*>(begin), end - begin))
        {
            const usize count = (end - begin) >> 3;
            for (usize i = 0; i < count; ++i)
            {
                void* vehicle = ReadPtr(reinterpret_cast<byte*>(begin) + i * sizeof(void*));
                if (IsVehicleCarrierInstance(vehicle))
                    return true;
            }
        }
    }
    return false;
}

static bool IsRailDepotKind(int type, int subtype)
{
    return type == BUILDINGTYPE_RAIL_DEPOT || (type == 0x28 && subtype == 0x1A);
}

static u64 __fastcall DetourBuildingCompat(char* world, u64 building, long long vehicleAsset,
                                            char param4, unsigned int* outReason)
{
    if (!g_originalBuildingCompat)
        return 0;

    int buildingType = -1;
    int buildingSubtype = -1;
    ResolveBuildingKind(building, &buildingType, &buildingSubtype);
    byte* asset = ResolveAssetPointer(vehicleAsset);
    int vehicleType = -1;
    bool deck = false;
    if (asset)
    {
        vehicleType = ReadInt(asset + VEHICLE_TYPE_OFFSET);
        deck = HasVehicleDeck(asset);
    }

    // Ask the native function the exact question it already understands for a
    // railway depot: "can this rail vehicle use this building?"  The carrier's
    // real ROAD type is restored before this call returns.
    if (g_allowRailDepotDropoff && asset && vehicleType == VEHICLETYPE_ROAD && deck &&
        IsRailDepotKind(buildingType, buildingSubtype))
    {
        WriteInt(asset + VEHICLE_TYPE_OFFSET, VEHICLETYPE_RAIL_LOCOMOTIVE);
        const u64 bridged = g_originalBuildingCompat(world, building,
                                                      reinterpret_cast<long long>(asset),
                                                      param4, outReason);
        WriteInt(asset + VEHICLE_TYPE_OFFSET, vehicleType);
        Trace("Rolling Stock Road Transport  depot bridge: building %d/%d native-as-rail %d deck %d",
              buildingType, buildingSubtype, static_cast<int>(bridged & 0xffu), deck ? 1 : 0);
        if ((bridged & 0xffu) != 0)
            return bridged;
    }

    const u64 nativeResult = g_originalBuildingCompat(world, building, vehicleAsset, param4, outReason);
    if (vehicleType == VEHICLETYPE_ROAD && deck &&
        (buildingType == BUILDINGTYPE_RAIL_DEPOT || buildingType == 0x28))
    {
        const int reason = outReason ? static_cast<int>(*outReason) : -1;
        Trace("Rolling Stock Road Transport  depot check: building %d/%d native %d reason %d",
              buildingType, buildingSubtype, static_cast<int>(nativeResult & 0xffu), reason);
    }
    return nativeResult;
}

static u64 __fastcall DetourCandidateEligibility(void* vehicleInstance)
{
    if (!g_originalCandidateEligibility)
        return 0;

    if (g_pickupScopeDepth > 0 && g_enablePickupBridge)
    {
        RestorePendingCargo("next candidate scan");

        if (vehicleInstance &&
            Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        {
            void* asset = ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET);
            if (asset && Readable(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET, sizeof(int)))
            {
                const int type = ReadInt(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET);
                if (IsAllowedRailType(type) && IsDetachedCandidate(vehicleInstance))
                {
                    // Upstream station and route candidate filters already know
                    // how to select ordinary road vehicles. Present the rail
                    // candidate as ROAD only until the real fit function runs.
                    g_pendingCargoAsset = asset;
                    g_pendingCargoInstance = vehicleInstance;
                    g_pendingCargoType = type;
                    WriteInt(static_cast<byte*>(asset) + VEHICLE_TYPE_OFFSET, VEHICLETYPE_ROAD);
                    Trace("Rolling Stock Road Transport  pickup bridge: rail type %d presented as road candidate",
                          type);
                }
            }
        }
    }

    return g_originalCandidateEligibility(vehicleInstance);
}

static TriggerUnloadState* GetTriggerUnloadState(void* carrier)
{
    TriggerUnloadState* empty = nullptr;
    for (int i = 0; i < TRIGGER_STATE_SLOTS; ++i)
    {
        TriggerUnloadState* state = &g_triggerUnloadStates[i];
        if (state->carrier == carrier)
            return state;
        if (!state->carrier && !empty)
            empty = state;
    }

    // Fixed-size fallback: replace the first inactive slot. This path is only
    // reached when more than 64 loaded rail transporters are active at once.
    TriggerUnloadState* state = empty ? empty : &g_triggerUnloadStates[0];
    state->carrier = carrier;
    state->railDepotTarget = nullptr;
    state->lastTarget = nullptr;
    state->armed = 0;
    state->armedByPickup = 0;
    return state;
}

static bool IsRailDepotPointer(void* building)
{
    if (!building)
        return false;
    int type = -1;
    int subtype = -1;
    return ResolveBuildingKind(reinterpret_cast<u64>(building), &type, &subtype) &&
           IsRailDepotKind(type, subtype);
}

static int AttemptNativeRailDepotUnload(void* carrier, void* building,
                                        const char* triggerName);

static void* ReadLiveRouteTarget(void* carrier);

static void ClearArmedDelivery(TriggerUnloadState* state)
{
    if (!state) return;
    state->railDepotTarget = nullptr;
    state->lastTarget = nullptr;
    state->armed = 0;
    state->armedByPickup = 0;
}

static void ArmRailDelivery(void* carrier, bool fromPickup)
{
    if (!carrier) return;
    TriggerUnloadState* state = GetTriggerUnloadState(carrier);
    if (!state) return;

    state->armed = 1;
    state->armedByPickup = fromPickup ? 1 : 0;
    state->railDepotTarget = nullptr;
    state->lastTarget = ReadLiveRouteTarget(carrier);

    // If the game has already promoted the next route stop to the live target
    // in the same update as pickup/save load, capture it immediately.
    if (state->lastTarget && IsRailDepotPointer(state->lastTarget))
        state->railDepotTarget = state->lastTarget;

    if (g_host && g_unloadTraceCount < UNLOAD_TRACE_LIMIT)
    {
        int type = -1;
        int subtype = -1;
        if (state->railDepotTarget)
            ResolveBuildingKind(reinterpret_cast<u64>(state->railDepotTarget), &type, &subtype);
        ++g_unloadTraceCount;
        g_host->log(
            "Rolling Stock Road Transport  delivery armed (%s): depot=%d/%d carried=%d rail=%d",
            fromPickup ? "pickup confirmed" : "loaded-save fallback",
            type, subtype, CarriedCount(carrier), CountCarriedAllowedRail(carrier));
    }
}

static int AttemptNativeRailDepotUnload(void* carrier, void* building,
                                        const char* triggerName)
{
    if (!carrier || !building || !g_originalUnloadCarried ||
        g_triggerUnloadDepth != 0 || !IsRailDepotPointer(building))
        return 0;

    const int carriedBefore = CarriedCount(carrier);
    const int railBefore = CountCarriedAllowedRail(carrier);
    if (carriedBefore <= 0 || railBefore <= 0)
        return 0;

    byte originalGate = 0;
    bool gateReadable =
        Readable(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET,
                 sizeof(byte));
    if (gateReadable)
    {
        originalGate =
            ReadByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET);
        if (originalGate != 0)
            WriteByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET, 0);
    }

    ++g_triggerUnloadDepth;
    int previousCount = carriedBefore;
    int successfulCalls = 0;
    int nativeAccepted = 0;
    const int maxAttempts = carriedBefore > 32 ? 32 : carriedBefore;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const bool nativeResult =
            g_originalUnloadCarried(carrier, building, nullptr, nullptr);
        if (nativeResult)
            nativeAccepted = 1;
        const int currentCount = CarriedCount(carrier);
        if (currentCount >= previousCount)
            break;
        ++successfulCalls;
        previousCount = currentCount;
        if (CountCarriedAllowedRail(carrier) <= 0)
            break;
    }
    --g_triggerUnloadDepth;

    if (gateReadable && originalGate != 0)
        WriteByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET,
                  originalGate);

    const int carriedAfter = CarriedCount(carrier);
    const int railAfter = CountCarriedAllowedRail(carrier);
    // Do not reset or rewrite the road carrier's native route/load state here.
    // The transfer is an external carried-vector -> depot-row insertion; the
    // route processor must be allowed to complete the stop naturally so the
    // following customs pickup starts with untouched vanilla state.
    if (g_host && g_unloadTraceCount < UNLOAD_TRACE_LIMIT)
    {
        int buildingType = -1;
        int buildingSubtype = -1;
        ResolveBuildingKind(reinterpret_cast<u64>(building), &buildingType, &buildingSubtype);
        ++g_unloadTraceCount;
        g_host->log(
            "Rolling Stock Road Transport  depot row transfer (%s): building=%d/%d native=%d transferred=%d carried=%d->%d rail=%d->%d",
            triggerName ? triggerName : "unknown", buildingType, buildingSubtype,
            nativeAccepted, successfulCalls, carriedBefore, carriedAfter,
            railBefore, railAfter);
        if (g_unloadTraceCount == UNLOAD_TRACE_LIMIT)
            g_host->log("Rolling Stock Road Transport  depot unload trace limit reached");
    }

    return successfulCalls;
}

static void* ReadLiveRouteTarget(void* carrier)
{
    if (!carrier)
        return nullptr;

    byte* vehicle = static_cast<byte*>(carrier);
    if (!Readable(vehicle + VEHICLE_ACTIVE_BUILDING_OFFSET, sizeof(void*)) ||
        !Readable(vehicle + VEHICLE_ALT_ACTIVE_BUILDING_OFFSET, sizeof(void*)))
        return nullptr;

    void* primaryTarget = ReadPtr(vehicle + VEHICLE_ACTIVE_BUILDING_OFFSET);
    void* alternateTarget = ReadPtr(vehicle + VEHICLE_ALT_ACTIVE_BUILDING_OFFSET);
    return alternateTarget ? alternateTarget : primaryTarget;
}

static void* ResolveRouteOwner(void* carrier)
{
    if (!carrier)
        return nullptr;

    byte* vehicle = static_cast<byte*>(carrier);
    if (Readable(vehicle + VEHICLE_PARENT_OFFSET, sizeof(void*)))
    {
        void* parent = ReadPtr(vehicle + VEHICLE_PARENT_OFFSET);
        if (parent &&
            Readable(static_cast<byte*>(parent) + VEHICLE_ROUTE_STOPS_BEGIN_OFFSET, sizeof(void*)) &&
            Readable(static_cast<byte*>(parent) + VEHICLE_ROUTE_STOPS_END_OFFSET, sizeof(void*)) &&
            Readable(static_cast<byte*>(parent) + VEHICLE_ROUTE_INDEX_OFFSET, sizeof(int)))
            return parent;
    }
    return carrier;
}

static RouteStopSnapshot CaptureRouteStopSnapshot(void* carrier)
{
    RouteStopSnapshot snapshot = {};
    snapshot.index = -1;

    void* owner = ResolveRouteOwner(carrier);
    if (!owner)
        return snapshot;

    byte* routeOwner = static_cast<byte*>(owner);
    if (!Readable(routeOwner + VEHICLE_ROUTE_STOPS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(routeOwner + VEHICLE_ROUTE_STOPS_END_OFFSET, sizeof(void*)) ||
        !Readable(routeOwner + VEHICLE_ROUTE_INDEX_OFFSET, sizeof(int)))
        return snapshot;

    byte* begin = static_cast<byte*>(ReadPtr(routeOwner + VEHICLE_ROUTE_STOPS_BEGIN_OFFSET));
    byte* end = static_cast<byte*>(ReadPtr(routeOwner + VEHICLE_ROUTE_STOPS_END_OFFSET));
    const int index = ReadInt(routeOwner + VEHICLE_ROUTE_INDEX_OFFSET);
    if (!begin || !end || end < begin || ((end - begin) & 7) != 0 || index < 0)
        return snapshot;

    const usize count = static_cast<usize>(end - begin) / sizeof(void*);
    if (static_cast<usize>(index) >= count || count > 4096)
        return snapshot;

    byte* entry = begin + static_cast<usize>(index) * sizeof(void*);
    if (!Readable(entry, sizeof(void*)))
        return snapshot;

    snapshot.owner = owner;
    snapshot.stopBuilding = ReadPtr(entry);
    snapshot.index = index;
    snapshot.valid = snapshot.stopBuilding != nullptr ? 1 : 0;
    return snapshot;
}

static bool RouteStopAdvanced(const RouteStopSnapshot& before,
                              const RouteStopSnapshot& after)
{
    if (!before.valid)
        return false;
    if (!after.valid)
        return true;
    return before.owner != after.owner ||
           before.index != after.index ||
           before.stopBuilding != after.stopBuilding;
}

static void* FindLoadedRailCarrierInConsist(void* routeOwner)
{
    if (!routeOwner)
        return nullptr;

    // Most vehicle carriers (including the tested Mark E) keep both the
    // route state and the carried-vehicle vector on the same instance.
    if (IsVehicleCarrierInstance(routeOwner) &&
        CountCarriedAllowedRail(routeOwner) > 0)
        return routeOwner;

    // Articulated combinations may keep the route on the tractor and the
    // vehicle deck on an attached child.  Check only that native attached
    // vector; this is bounded and runs only when a route stop completes.
    byte* owner = static_cast<byte*>(routeOwner);
    if (!Readable(owner + VEHICLE_ATTACHED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(owner + VEHICLE_ATTACHED_END_OFFSET, sizeof(void*)))
        return nullptr;

    const usize begin = reinterpret_cast<usize>(ReadPtr(owner + VEHICLE_ATTACHED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(owner + VEHICLE_ATTACHED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 ||
        end - begin > 0x4000 || !Readable(reinterpret_cast<void*>(begin), end - begin))
        return nullptr;

    for (usize p = begin; p < end; p += sizeof(void*))
    {
        void* child = ReadPtr(reinterpret_cast<void*>(p));
        if (IsVehicleCarrierInstance(child) && CountCarriedAllowedRail(child) > 0)
            return child;
    }
    return nullptr;
}

static void* ReadCurrentIndexedRouteStop(void* routeOwner)
{
    if (!routeOwner)
        return nullptr;
    byte* owner = static_cast<byte*>(routeOwner);
    if (!Readable(owner + VEHICLE_ROUTE_STOPS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(owner + VEHICLE_ROUTE_STOPS_END_OFFSET, sizeof(void*)) ||
        !Readable(owner + VEHICLE_ROUTE_INDEX_OFFSET, sizeof(int)))
        return nullptr;

    const usize begin = reinterpret_cast<usize>(ReadPtr(owner + VEHICLE_ROUTE_STOPS_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(owner + VEHICLE_ROUTE_STOPS_END_OFFSET));
    const int index = ReadInt(owner + VEHICLE_ROUTE_INDEX_OFFSET);
    if (!begin || end < begin || ((end - begin) & 7u) != 0 ||
        end - begin > 0x20000 || index < 0)
        return nullptr;
    const usize count = (end - begin) >> 3;
    if (static_cast<usize>(index) >= count ||
        !Readable(reinterpret_cast<void*>(begin + static_cast<usize>(index) * sizeof(void*)), sizeof(void*)))
        return nullptr;
    return ReadPtr(reinterpret_cast<void*>(begin + static_cast<usize>(index) * sizeof(void*)));
}

static int g_routeAdvanceUnloadDepth = 0;

static void __fastcall DetourRouteAdvance(void* routeOwner, char param2)
{
    if (!g_originalRouteAdvance)
        return;

    // FUN_14067da00 is the game's authoritative "current route stop is
    // complete; advance to the next one" operation.  Inspect the still-current
    // stop BEFORE calling the original function.  This avoids all guessed
    // target/occupied-building timing and cannot fire later at customs.
    if (g_enableTriggeredDepotUnload && g_originalUnloadCarried &&
        g_routeAdvanceUnloadDepth == 0 && routeOwner)
    {
        void* currentStop = ReadCurrentIndexedRouteStop(routeOwner);
        if (currentStop && IsRailDepotPointer(currentStop))
        {
            void* carrier = FindLoadedRailCarrierInConsist(routeOwner);
            if (carrier && CountCarriedAllowedRail(carrier) > 0)
            {
                ++g_routeAdvanceUnloadDepth;
                const int transferred = AttemptNativeRailDepotUnload(
                    carrier, currentStop, "native route-stop advance");
                --g_routeAdvanceUnloadDepth;

                if (g_host && g_unloadTraceCount < UNLOAD_TRACE_LIMIT)
                {
                    int type = -1;
                    int subtype = -1;
                    ResolveBuildingKind(reinterpret_cast<u64>(currentStop), &type, &subtype);
                    ++g_unloadTraceCount;
                    g_host->log(
                        "Rolling Stock Road Transport  route-advance guard: depot=%d/%d transferred=%d remaining=%d",
                        type, subtype, transferred, CountCarriedAllowedRail(carrier));
                }
            }
        }
    }

    g_originalRouteAdvance(routeOwner, param2);
}

static void ObserveArmedRailDelivery(void* carrier)
{
    if (!g_enableTriggeredDepotUnload || !g_originalUnloadCarried ||
        g_triggerUnloadDepth != 0 || !carrier)
        return;

    byte* vehicle = static_cast<byte*>(carrier);
    if (!Readable(vehicle + VEHICLE_CARRIED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(vehicle + VEHICLE_CARRIED_END_OFFSET, sizeof(void*)))
        return;

    const int railCount = CountCarriedAllowedRail(carrier);
    TriggerUnloadState* state = nullptr;

    if (railCount <= 0)
    {
        // Clear any completed/aborted delivery state without creating a slot
        // for every ordinary road vehicle.
        for (int i = 0; i < TRIGGER_STATE_SLOTS; ++i)
        {
            if (g_triggerUnloadStates[i].carrier == carrier)
            {
                ClearArmedDelivery(&g_triggerUnloadStates[i]);
                break;
            }
        }
        return;
    }

    if (!IsVehicleCarrierInstance(carrier))
        return;

    state = GetTriggerUnloadState(carrier);
    if (!state)
        return;

    // Save-load recovery: a carrier that enters the plugin already holding
    // rolling stock did not pass through DetourActualLoad in this process.
    if (!state->armed)
        ArmRailDelivery(carrier, false);

    if (!state->armed)
        return;

    void* target = ReadLiveRouteTarget(carrier);

    // While en route, remember the exact railway depot promoted by the native
    // route processor.  Nothing is transferred merely because the depot has
    // become the target.
    if (target && IsRailDepotPointer(target))
    {
        if (state->railDepotTarget != target)
        {
            state->railDepotTarget = target;
            if (g_host && g_unloadTraceCount < UNLOAD_TRACE_LIMIT)
            {
                int type = -1;
                int subtype = -1;
                ResolveBuildingKind(reinterpret_cast<u64>(target), &type, &subtype);
                ++g_unloadTraceCount;
                g_host->log(
                    "Rolling Stock Road Transport  armed delivery target acquired: building=%d/%d carried=%d rail=%d",
                    type, subtype, CarriedCount(carrier), railCount);
            }
        }
        state->lastTarget = target;
        return;
    }

    // The route processor's live target advances only after the current stop
    // has completed.  v0.3.0 proved that this is the reliable event around a
    // railway depot.  The crucial protection here is that the carrier must
    // first have been armed by a confirmed pickup (or loaded-save fallback)
    // AND must have explicitly held this exact railway depot as its target.
    // Thus a later customs stop can never invent a depot transfer on its own.
    if (state->railDepotTarget && target != state->railDepotTarget)
    {
        void* completedDepot = state->railDepotTarget;
        const int transferred = AttemptNativeRailDepotUnload(
            carrier, completedDepot, "armed planned-depot stop completed");

        if (transferred > 0 || CountCarriedAllowedRail(carrier) <= 0)
        {
            ClearArmedDelivery(state);
            return;
        }

        // No row space (or a native rejection): do not unload later at an
        // unrelated stop.  Keep the delivery armed, but require the railway
        // depot to become the live target again on a future visit before any
        // retry can occur.
        state->railDepotTarget = nullptr;
    }

    state->lastTarget = target;
}

static u64 __fastcall DetourVehicleProcess(void* vehicleInstance, void* param2, void* param3,
                                            char param4, char param5, float param6,
                                            char param7, char param8)
{
    if (!g_originalVehicleProcess)
        return 0;

    // Observe on both sides of the native update.  The pre-call pass sees a
    // depot target while approaching; the post-call pass catches the exact
    // update in which the native route advances after completing that stop.
    ObserveArmedRailDelivery(vehicleInstance);

    const u64 result = g_originalVehicleProcess(vehicleInstance, param2, param3,
                                                 param4, param5, param6, param7, param8);

    ObserveArmedRailDelivery(vehicleInstance);
    return result;
}


static void CarrierExistingUsage(void* carrierInstance, float* outWeight, float* outLength)
{
    if (outWeight) *outWeight = 0.0f;
    if (outLength) *outLength = 0.0f;
    if (!carrierInstance ||
        !Readable(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_END_OFFSET, sizeof(void*)))
        return;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x4000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return;

    float weight = 0.0f;
    float length = 0.0f;
    for (usize p = begin; p < end; p += sizeof(void*))
    {
        void* carried = ReadPtr(reinterpret_cast<void*>(p));
        if (!carried || !Readable(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* asset = static_cast<byte*>(ReadPtr(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET));
        if (!asset || !Readable(asset + VEHICLE_TYPE_OFFSET, sizeof(int)))
            continue;

        if (Readable(asset + VEHICLE_EMPTY_WEIGHT_OFFSET, sizeof(float)))
            weight += ReadFloat(asset + VEHICLE_EMPTY_WEIGHT_OFFSET);

        // Linked children are represented separately in the carried vector;
        // count only top-level carried vehicles for longitudinal occupancy.
        if (Readable(static_cast<byte*>(carried) + VEHICLE_PARENT_OFFSET, sizeof(void*)) &&
            ReadPtr(static_cast<byte*>(carried) + VEHICLE_PARENT_OFFSET) != nullptr)
            continue;

        const int type = ReadInt(asset + VEHICLE_TYPE_OFFSET);
        float rawLength = -1.0f;
        if (IsAllowedRailType(type))
            rawLength = NativeRailTotalLength(asset);
        else if (Readable(asset + VEHICLE_RAIL_LENGTH_OFFSET, sizeof(float)))
            rawLength = ReadFloat(asset + VEHICLE_RAIL_LENGTH_OFFSET);

        if (rawLength > 0.0f)
        {
            float scale = 1.0f;
            if (type == VEHICLETYPE_ROAD || IsAllowedRailType(type)) scale = 0.66f;
            if (type == 9) scale = 0.9f;
            length += rawLength * scale;
        }
    }

    if (outWeight) *outWeight = weight;
    if (outLength) *outLength = length;
}

static char __fastcall DetourActualLoad(void* carrierInstance, void* cargoInstance, char param3)
{
    if (!g_originalActualLoad)
        return 0;

    byte* cargoAsset = nullptr;
    byte* carrierAsset = nullptr;
    int cargoType = -1;
    if (cargoInstance && Readable(static_cast<byte*>(cargoInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
    {
        cargoAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(cargoInstance) + VEHICLE_ASSET_OFFSET));
        if (cargoAsset && Readable(cargoAsset + VEHICLE_TYPE_OFFSET, sizeof(int)))
            cargoType = ReadInt(cargoAsset + VEHICLE_TYPE_OFFSET);
    }
    if (carrierInstance && Readable(static_cast<byte*>(carrierInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        carrierAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(carrierInstance) + VEHICLE_ASSET_OFFSET));

    const int before = CarriedCount(carrierInstance);
    bool explicitFit = false;
    RailFitLimits limits = {};
    float existingWeight = 0.0f;
    float existingLength = 0.0f;

    if (IsAllowedRailType(cargoType) && carrierAsset && cargoAsset)
    {
        CarrierExistingUsage(carrierInstance, &existingWeight, &existingLength);
        explicitFit = RailFitsCarrierAssetByWeightLength(
            carrierAsset, cargoAsset, existingWeight, existingLength, &limits);

        // The selector and actual pickup share this exact gate. If the rail
        // vehicle is over payload or longitudinal deck length, do not let a
        // looser native 3D placement result contradict the UI.
        if (!explicitFit)
        {
            if (g_host && g_loadTraceCount < LOAD_TRACE_LIMIT)
            {
                ++g_loadTraceCount;
                const int cargoWeight10 = limits.cargoWeight < 0.0f ? -1 : static_cast<int>(limits.cargoWeight * 10.0f + 0.5f);
                const int payload10 = limits.payloadLimit < 0.0f ? -1 : static_cast<int>(limits.payloadLimit * 10.0f + 0.5f);
                const int cargoLengthCm = limits.cargoLength < 0.0f ? -1 : static_cast<int>(limits.cargoLength * 100.0f + 0.5f);
                const int deckLengthCm = limits.deckLengthLimit < 0.0f ? -1 : static_cast<int>(limits.deckLengthLimit * 100.0f + 0.5f);
                g_host->log(
                    "Rolling Stock Road Transport  actual pickup blocked by weight/length: carrier=%p rail_type=%d existing_weight_x10=%d cargo_weight_x10=%d payload_x10=%d existing_length_cm=%d cargo_length_cm=%d deck_length_cm=%d",
                    carrierInstance, cargoType,
                    static_cast<int>(existingWeight * 10.0f + 0.5f), cargoWeight10, payload10,
                    static_cast<int>(existingLength * 100.0f + 0.5f), cargoLengthCm, deckLengthCm);
            }
            return 0;
        }
    }

    RailTransverseSnapshot snapshots[RAIL_TRANSVERSE_SNAPSHOT_LIMIT];
    int snapshotCount = 0;
    bool transverseOverrideActive = false;
    if (explicitFit && g_railFitLengthWeightOnly && cargoAsset)
    {
        // Once mass and longitudinal length have passed, width and height are
        // deliberately removed from the native attachment packer. The packer
        // still places the vehicle and updates all native carried-vehicle state.
        transverseOverrideActive = NarrowAssetAndLinkedParts(cargoAsset, snapshots, &snapshotCount);

        // Keep already-carried rail vehicles narrow during repacking as well,
        // otherwise an older carried asset can reintroduce a transverse veto.
        if (carrierInstance &&
            Readable(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_BEGIN_OFFSET, sizeof(void*)) &&
            Readable(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_END_OFFSET, sizeof(void*)))
        {
            const usize carriedBegin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_BEGIN_OFFSET));
            const usize carriedEnd = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(carrierInstance) + VEHICLE_CARRIED_END_OFFSET));
            if (carriedBegin && carriedEnd >= carriedBegin && ((carriedEnd - carriedBegin) & 7u) == 0 &&
                carriedEnd - carriedBegin <= 0x4000u &&
                (carriedEnd == carriedBegin || Readable(reinterpret_cast<void*>(carriedBegin), carriedEnd - carriedBegin)))
            {
                for (usize p = carriedBegin; p < carriedEnd; p += sizeof(void*))
                {
                    void* carried = ReadPtr(reinterpret_cast<void*>(p));
                    if (!carried || !Readable(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
                        continue;
                    byte* carriedAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(carried) + VEHICLE_ASSET_OFFSET));
                    if (!carriedAsset || !Readable(carriedAsset + VEHICLE_TYPE_OFFSET, sizeof(int)))
                        continue;
                    if (IsAllowedRailType(ReadInt(carriedAsset + VEHICLE_TYPE_OFFSET)))
                        NarrowAssetAndLinkedParts(carriedAsset, snapshots, &snapshotCount);
                }
            }
        }
    }

    const char result = g_originalActualLoad(carrierInstance, cargoInstance, param3);
    RestoreTransverseSnapshots(snapshots, snapshotCount);
    const int after = CarriedCount(carrierInstance);

    if (IsAllowedRailType(cargoType) && g_host && g_loadTraceCount < LOAD_TRACE_LIMIT)
    {
        ++g_loadTraceCount;
        const int cargoWeight10 = limits.cargoWeight < 0.0f ? -1 : static_cast<int>(limits.cargoWeight * 10.0f + 0.5f);
        const int payload10 = limits.payloadLimit < 0.0f ? -1 : static_cast<int>(limits.payloadLimit * 10.0f + 0.5f);
        const int cargoLengthCm = limits.cargoLength < 0.0f ? -1 : static_cast<int>(limits.cargoLength * 100.0f + 0.5f);
        const int deckLengthCm = limits.deckLengthLimit < 0.0f ? -1 : static_cast<int>(limits.deckLengthLimit * 100.0f + 0.5f);
        g_host->log(
            "Rolling Stock Road Transport  actual pickup: carrier=%p rail_type=%d result=%d carried=%d->%d weight_length_fit=%d cargo_weight_x10=%d payload_x10=%d cargo_length_cm=%d deck_length_cm=%d",
            carrierInstance, cargoType, static_cast<int>(result), before, after,
            explicitFit ? 1 : 0, cargoWeight10, payload10, cargoLengthCm, deckLengthCm);
        if (g_loadTraceCount == LOAD_TRACE_LIMIT)
            g_host->log("Rolling Stock Road Transport  pickup trace limit reached");
    }
    return result;
}

static bool __fastcall DetourUnloadCarried(void* carrierInstance, void* building,
                                           int* outIndex, void* param4)
{
    if (!g_originalUnloadCarried)
        return false;

    const int before = CarriedCount(carrierInstance);
    const int railBefore = CountCarriedAllowedRail(carrierInstance);
    int buildingType = -1;
    int buildingSubtype = -1;
    const bool kindResolved =
        ResolveBuildingKind(reinterpret_cast<u64>(building), &buildingType, &buildingSubtype);
    const bool eligible =
        g_enableTrainDepotUnloadBridge &&
        before > 0 && railBefore > 0 &&
        IsVehicleCarrierInstance(carrierInstance) &&
        kindResolved && IsRailDepotKind(buildingType, buildingSubtype);

    byte originalGate = 0;
    bool gateReadable = false;
    if (eligible &&
        Readable(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET,
                 sizeof(byte)))
    {
        gateReadable = true;
        originalGate =
            ReadByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET);
        if (originalGate != 0)
            WriteByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET, 0);
    }

    const bool result = g_originalUnloadCarried(carrierInstance, building, outIndex, param4);

    if (gateReadable && originalGate != 0)
        WriteByte(static_cast<byte*>(building) + BUILDING_GENERIC_UNLOAD_BLOCK_OFFSET,
                  originalGate);

    const int after = CarriedCount(carrierInstance);
    const int railAfter = CountCarriedAllowedRail(carrierInstance);

    if (railBefore > 0 && g_host && g_unloadTraceCount < UNLOAD_TRACE_LIMIT)
    {
        ++g_unloadTraceCount;
        g_host->log(
            "Rolling Stock Road Transport  depot unload: eligible=%d building=%d/%d gate=%d result=%d carried=%d->%d rail=%d->%d",
            eligible ? 1 : 0, buildingType, buildingSubtype,
            gateReadable ? static_cast<int>(originalGate) : -1,
            result ? 1 : 0, before, after, railBefore, railAfter);
        if (g_unloadTraceCount == UNLOAD_TRACE_LIMIT)
            g_host->log("Rolling Stock Road Transport  depot unload trace limit reached");
    }

    return result;
}


static bool ValidateRouteStopSelectionTarget(const byte* target)
{
    static const byte expected[] = {
        0x4C,0x89,0x4C,0x24,0x20,
        0x44,0x89,0x44,0x24,0x18,
        0x89,0x54,0x24,0x10
    };
    if (!target || !Readable(target, sizeof(expected))) return false;
    for (usize i = 0; i < sizeof(expected); ++i)
        if (target[i] != expected[i]) return false;
    return true;
}

static bool ValidateBuildingHighlightTarget(const byte* target)
{
    static const byte expected[] = {
        0x40,0x55,0x53,0x56,0x57,
        0x48,0x8D,0xAC,0x24,0xF8,0xE5,0xFF,0xFF,
        0xB8,0x08,0x1B,0x00,0x00
    };
    if (!target || !Readable(target, sizeof(expected))) return false;
    for (usize i = 0; i < sizeof(expected); ++i)
        if (target[i] != expected[i]) return false;
    return true;
}

static void __fastcall DetourBuildingHighlight(u64 world, long long building, int mode,
                                                float highlightPower, char flag)
{
    if (g_routeSelectionScopeDepth > 0 && world == g_routeSelectionWorld && building)
        g_routeCurrentBuilding = static_cast<u64>(building);

    if (g_originalBuildingHighlight)
        g_originalBuildingHighlight(world, building, mode, highlightPower, flag);
}

static u64 __fastcall DetourRouteStopSelection(u64 world, int vehicleType, int cargoType,
                                               float highlightPower, long long* routeStops,
                                               long long* selectedVehicles, u64* outBuilding,
                                               u64* outNode)
{
    if (!g_originalRouteStopSelection)
        return 0;

    const bool eligible = g_allowRailDepotDropoff &&
                          vehicleType == VEHICLETYPE_ROAD &&
                          SelectedRouteVehicleIsCarrier(world, selectedVehicles);

    const u64 patchedBuilding = g_routeCachedBuilding;
    byte* patchedDesc = nullptr;
    int originalBuildingType = -1;
    int cachedType = -1;
    int cachedSubtype = -1;

    if (eligible && patchedBuilding &&
        ResolveBuildingKind(patchedBuilding, &cachedType, &cachedSubtype) &&
        IsRailDepotKind(cachedType, cachedSubtype))
    {
        patchedDesc = ResolveBuildingDesc(patchedBuilding);
        if (patchedDesc)
        {
            originalBuildingType = ReadInt(patchedDesc + BUILDING_TYPE_OFFSET);
            WriteInt(patchedDesc + BUILDING_TYPE_OFFSET, BUILDINGTYPE_ROAD_DEPOT);
        }
    }

    const int previousDepth = g_routeSelectionScopeDepth;
    const u64 previousWorld = g_routeSelectionWorld;
    const u64 previousCurrent = g_routeCurrentBuilding;
    g_routeSelectionScopeDepth = previousDepth + 1;
    g_routeSelectionWorld = world;
    g_routeCurrentBuilding = 0;

    const u64 result = g_originalRouteStopSelection(world, vehicleType, cargoType,
                                                     highlightPower, routeStops,
                                                     selectedVehicles, outBuilding, outNode);
    const u64 hoveredBuilding = g_routeCurrentBuilding;

    g_routeSelectionScopeDepth = previousDepth;
    g_routeSelectionWorld = previousWorld;
    g_routeCurrentBuilding = previousCurrent;

    if (patchedDesc)
        WriteInt(patchedDesc + BUILDING_TYPE_OFFSET, originalBuildingType);

    g_routeCachedBuilding = hoveredBuilding;

    int hoveredType = -1;
    int hoveredSubtype = -1;
    if (hoveredBuilding &&
        ResolveBuildingKind(hoveredBuilding, &hoveredType, &hoveredSubtype) &&
        IsRailDepotKind(hoveredType, hoveredSubtype))
    {
        if (eligible && patchedDesc && patchedBuilding == hoveredBuilding)
            Trace("Rolling Stock Road Transport  route depot bridge: result %d building %d/%d",
                  static_cast<int>(result & 0xffu), hoveredType, hoveredSubtype);
        else
            Trace("Rolling Stock Road Transport  route depot primed: eligible %d building %d/%d",
                  eligible ? 1 : 0, hoveredType, hoveredSubtype);
    }

    return result;
}


static bool VehicleInstanceHasRoadCarrierDeck(void* vehicleInstance)
{
    if (!vehicleInstance ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        return false;

    byte* ownAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET));
    if (ownAsset && Readable(ownAsset + VEHICLE_TYPE_OFFSET, sizeof(int)) &&
        ReadInt(ownAsset + VEHICLE_TYPE_OFFSET) == VEHICLETYPE_ROAD && HasVehicleDeck(ownAsset))
        return true;

    if (!Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET, sizeof(void*)))
        return false;
    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x4000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return false;
    for (usize p = begin; p < end; p += sizeof(void*))
    {
        void* child = ReadPtr(reinterpret_cast<void*>(p));
        if (!child || !Readable(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* asset = static_cast<byte*>(ReadPtr(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET));
        if (asset && Readable(asset + VEHICLE_TYPE_OFFSET, sizeof(int)) &&
            ReadInt(asset + VEHICLE_TYPE_OFFSET) == VEHICLETYPE_ROAD && HasVehicleDeck(asset))
            return true;
    }
    return false;
}

static bool ValidateVehicleTypeSelectorTarget(const byte* target)
{
    if (!target || !Readable(target, 0x220))
        return false;
    static const byte prefix[] = {
        0x4C,0x89,0x4C,0x24,0x20,
        0x4C,0x89,0x44,0x24,0x18,
        0x88,0x54,0x24,0x10
    };
    for (usize i = 0; i < sizeof(prefix); ++i)
        if (target[i] != prefix[i]) return false;
    return CountDisp32(target, 0x220, 0x00012858u) >= 1 &&
           CountDisp32(target, 0x220, 0x00012840u) >= 1;
}

static bool ValidateVehiclePickerTarget(const byte* target)
{
    if (!target || !Readable(target, 0x500)) return false;
    return CountDisp32(target, 0x500, 0x00012858u) >= 1 &&
           CountDisp32(target, 0x500, 0x00001708u) >= 1;
}

static bool IsTopLevelPickerRailAsset(byte* asset)
{
    if (!asset || !Readable(asset + VEHICLE_TYPE_OFFSET, sizeof(int)) ||
        !Readable(asset + VEHICLE_HIDDEN_FLAG_A_OFFSET, sizeof(byte)) ||
        !Readable(asset + VEHICLE_HIDDEN_FLAG_B_OFFSET, sizeof(byte)) ||
        !Readable(asset + VEHICLE_TOPLEVEL_LINK_OFFSET, sizeof(void*)))
        return false;

    const int type = ReadInt(asset + VEHICLE_TYPE_OFFSET);
    if (!IsAllowedRailType(type))
        return false;

    // Keep internal/hidden linked-part assets out of the user-facing list.
    // These are the same structural guards used by the game's general vehicle
    // categorisation paths; the change below is about physical-fit visibility,
    // not exposing tender/child definitions as standalone vehicles.
    if (ReadByte(asset + VEHICLE_HIDDEN_FLAG_A_OFFSET) != 0 ||
        ReadByte(asset + VEHICLE_HIDDEN_FLAG_B_OFFSET) != 0 ||
        ReadPtr(asset + VEHICLE_TOPLEVEL_LINK_OFFSET) != nullptr)
        return false;

    return true;
}

static bool CarrierAssetFitsRailPicker(byte* carrierAsset, byte* cargoAsset)
{
    if (!g_originalCanLoadVehicle || !carrierAsset || !cargoAsset ||
        !Readable(carrierAsset + VEHICLE_TYPE_OFFSET, sizeof(int)) ||
        !HasVehicleDeck(carrierAsset))
        return false;

    const int carrierType = ReadInt(carrierAsset + VEHICLE_TYPE_OFFSET);
    if (carrierType != VEHICLETYPE_ROAD)
        return false;

    // Reuse the exact physical rule used by real pickup. ROAD_SERVICE bypasses
    // the vanilla road/rail category rejection only for the duration of this
    // fit query; payload, longitudinal packing and deck geometry stay native.
    WriteInt(carrierAsset + VEHICLE_TYPE_OFFSET, VEHICLETYPE_ROAD_SERVICE);
    char result = g_originalCanLoadVehicle(nullptr, carrierAsset, cargoAsset);

    if (result == 0 && g_railFitLengthWeightOnly)
    {
        RailTransverseSnapshot snapshots[RAIL_TRANSVERSE_SNAPSHOT_LIMIT];
        int snapshotCount = 0;
        if (NarrowAssetAndLinkedParts(cargoAsset, snapshots, &snapshotCount))
            result = g_originalCanLoadVehicle(nullptr, carrierAsset, cargoAsset);
        RestoreTransverseSnapshots(snapshots, snapshotCount);
    }

    WriteInt(carrierAsset + VEHICLE_TYPE_OFFSET, carrierType);
    return result != 0;
}

static bool VehicleInstanceFitsRailPicker(void* vehicleInstance, byte* cargoAsset)
{
    if (!vehicleInstance || !cargoAsset ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
        return false;

    byte* ownAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ASSET_OFFSET));
    if (CarrierAssetFitsRailPicker(ownAsset, cargoAsset))
        return true;

    if (!Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET, sizeof(void*)))
        return false;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(vehicleInstance) + VEHICLE_ATTACHED_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x4000u)
        return false;
    if (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin))
        return false;

    const usize count = (end - begin) >> 3;
    for (usize i = 0; i < count; ++i)
    {
        void* child = ReadPtr(reinterpret_cast<byte*>(begin) + i * sizeof(void*));
        if (!child || !Readable(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET, sizeof(void*)))
            continue;
        byte* childAsset = static_cast<byte*>(ReadPtr(static_cast<byte*>(child) + VEHICLE_ASSET_OFFSET));
        if (CarrierAssetFitsRailPicker(childAsset, cargoAsset))
            return true;
    }
    return false;
}

static bool PickerContains(void* world, void* candidate)
{
    if (!world || !candidate ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET, sizeof(void*)))
        return false;
    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x100000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return false;
    const usize count = (end - begin) >> 3;
    for (usize i = 0; i < count; ++i)
        if (ReadPtr(reinterpret_cast<byte*>(begin) + i * sizeof(void*)) == candidate)
            return true;
    return false;
}

static bool AppendPickerCandidate(void* world, void* candidate)
{
    if (!world || !candidate || !g_pointerVectorReserve ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_CAP_OFFSET, sizeof(void*)))
        return false;

    byte* vectorObject = static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET;
    usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
    usize cap = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_CAP_OFFSET));
    if (end > cap)
        return false;
    if (end == cap)
    {
        g_pointerVectorReserve(vectorObject, 1);
        end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
        cap = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_CAP_OFFSET));
        if (end >= cap)
            return false;
    }
    if (!Readable(reinterpret_cast<void*>(end), sizeof(void*)))
        return false;

    *reinterpret_cast<void* volatile*>(end) = candidate;
    *reinterpret_cast<void* volatile*>(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET) =
        reinterpret_cast<void*>(end + sizeof(void*));
    return true;
}

static int PickerCount(void* world)
{
    if (!world ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET, sizeof(void*)))
        return -1;
    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
    if (end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x100000u)
        return -1;
    return static_cast<int>((end - begin) >> 3);
}

static void CountPickerRailTypes(void* world, int* outWagons, int* outLocomotives)
{
    if (outWagons) *outWagons = 0;
    if (outLocomotives) *outLocomotives = 0;
    if (!world ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET, sizeof(void*)))
        return;

    const usize begin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET));
    const usize end = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
    if (!begin || end < begin || ((end - begin) & 7u) != 0 || end - begin > 0x100000u ||
        (end > begin && !Readable(reinterpret_cast<void*>(begin), end - begin)))
        return;

    for (usize p = begin; p < end; p += sizeof(void*))
    {
        byte* asset = static_cast<byte*>(ReadPtr(reinterpret_cast<void*>(p)));
        if (!asset || !Readable(asset + VEHICLE_TYPE_OFFSET, sizeof(int)))
            continue;
        const int type = ReadInt(asset + VEHICLE_TYPE_OFFSET);
        if (type == VEHICLETYPE_RAIL_WAGON && outWagons) ++(*outWagons);
        if (type == VEHICLETYPE_RAIL_LOCOMOTIVE && outLocomotives) ++(*outLocomotives);
    }
}

static void RebuildSelectorRailCandidates(void* world, void* vehicleInstance)
{
    if (!world || !vehicleInstance || !VehicleInstanceHasRoadCarrierDeck(vehicleInstance) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_VEHICLE_ASSETS_BEGIN_OFFSET, sizeof(void*)) ||
        !Readable(static_cast<byte*>(world) + WORLD_VEHICLE_ASSETS_END_OFFSET, sizeof(void*)))
        return;

    const usize pickerBegin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_BEGIN_OFFSET));
    usize pickerEnd = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET));
    if (!pickerBegin || pickerEnd < pickerBegin || ((pickerEnd - pickerBegin) & 7u) != 0 ||
        pickerEnd - pickerBegin > 0x100000u ||
        (pickerEnd > pickerBegin && !Readable(reinterpret_cast<void*>(pickerBegin), pickerEnd - pickerBegin)))
        return;

    // Preserve every normal non-rail candidate that vanilla added, but replace
    // the rail portion with the authoritative truck -> payload/length result.
    usize write = pickerBegin;
    for (usize p = pickerBegin; p < pickerEnd; p += sizeof(void*))
    {
        void* candidate = ReadPtr(reinterpret_cast<void*>(p));
        bool isRail = false;
        if (candidate && Readable(static_cast<byte*>(candidate) + VEHICLE_TYPE_OFFSET, sizeof(int)))
            isRail = IsAllowedRailType(ReadInt(static_cast<byte*>(candidate) + VEHICLE_TYPE_OFFSET));
        if (!isRail)
        {
            *reinterpret_cast<void* volatile*>(write) = candidate;
            write += sizeof(void*);
        }
    }
    *reinterpret_cast<void* volatile*>(static_cast<byte*>(world) + WORLD_PICKER_END_OFFSET) =
        reinterpret_cast<void*>(write);

    const usize assetsBegin = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_VEHICLE_ASSETS_BEGIN_OFFSET));
    const usize assetsEnd = reinterpret_cast<usize>(ReadPtr(static_cast<byte*>(world) + WORLD_VEHICLE_ASSETS_END_OFFSET));
    if (!assetsBegin || assetsEnd < assetsBegin || (assetsEnd - assetsBegin) % VEHICLE_ASSET_STRIDE != 0 ||
        assetsEnd - assetsBegin > VEHICLE_ASSET_STRIDE * 10000u ||
        (assetsEnd > assetsBegin && !Readable(reinterpret_cast<void*>(assetsBegin), assetsEnd - assetsBegin)))
        return;

    int importedRail = 0;
    int currentAvailableRail = 0;
    int fittingRail = 0;
    int shownWagons = 0;
    int shownLocomotives = 0;
    RailFitLimits firstLimits = {};
    bool haveLimits = false;

    const usize assetCount = (assetsEnd - assetsBegin) / VEHICLE_ASSET_STRIDE;
    for (usize i = 0; i < assetCount; ++i)
    {
        byte* asset = reinterpret_cast<byte*>(assetsBegin + i * VEHICLE_ASSET_STRIDE);
        if (!IsTopLevelPickerRailAsset(asset))
            continue;
        ++importedRail;

        if (!IsCurrentRailAssetAvailable(world, asset))
            continue;
        ++currentAvailableRail;

        RailFitLimits limits = {};
        if (!VehicleInstanceFitsRailByWeightLength(vehicleInstance, asset, &limits))
            continue;
        ++fittingRail;
        if (!haveLimits)
        {
            firstLimits = limits;
            haveLimits = true;
        }

        if (!PickerContains(world, asset) && !AppendPickerCandidate(world, asset))
            continue;

        const int type = ReadInt(asset + VEHICLE_TYPE_OFFSET);
        if (type == VEHICLETYPE_RAIL_WAGON) ++shownWagons;
        if (type == VEHICLETYPE_RAIL_LOCOMOTIVE) ++shownLocomotives;
    }

    if (g_host && g_pickerTraceCount < PICKER_TRACE_LIMIT)
    {
        ++g_pickerTraceCount;
        const int payload10 = haveLimits ? static_cast<int>(firstLimits.payloadLimit * 10.0f + 0.5f) : -1;
        const int deckCm = haveLimits ? static_cast<int>(firstLimits.deckLengthLimit * 100.0f + 0.5f) : -1;
        int year = -1;
        int day = -1;
        if (Readable(static_cast<byte*>(world) + WORLD_CURRENT_YEAR_OFFSET, sizeof(float)))
            year = static_cast<int>(ReadFloat(static_cast<byte*>(world) + WORLD_CURRENT_YEAR_OFFSET));
        if (Readable(static_cast<byte*>(world) + WORLD_CURRENT_DAY_OFFSET, sizeof(int)))
            day = ReadInt(static_cast<byte*>(world) + WORLD_CURRENT_DAY_OFFSET);
        g_host->log(
            "Rolling Stock Road Transport  dynamic selector: vehicle=%p current_date=%d/%d imported_rail=%d current_available=%d fitting=%d shown_wagons=%d shown_locomotives=%d carrier_payload_x10=%d carrier_deck_length_cm=%d",
            vehicleInstance, year, day, importedRail, currentAvailableRail, fittingRail,
            shownWagons, shownLocomotives, payload10, deckCm);
    }
}

static void __fastcall DetourVehicleTypeSelector(void* world, char railOnly,
                                                  void* vehicleInstance, void* group)
{
    if (!g_originalVehicleTypeSelector)
        return;

    g_originalVehicleTypeSelector(world, railOnly, vehicleInstance, group);

    if (g_pickerShowAllFittingRail && vehicleInstance)
        RebuildSelectorRailCandidates(world, vehicleInstance);
}

extern "C" WinBool __stdcall DllMain(void* module, WinDword reason, void* reserved)
{
    (void)module;
    (void)reason;
    (void)reserved;
    return g_imageAnchor != nullptr ? 1 : 0;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    g_host = host;
    if (!g_host || !info || g_host->apiVersion != TSM_API_VERSION)
        return 1;

    g_exeBase = g_host->exeBase;
    g_exeSize = g_host->exeSize;
    info->name = "Rolling Stock Road Transport";
    info->version = "1.0.0";

    const char* ini = "plugins\\RollingStockRoadTransport.ini";
    g_enabled = g_host->configInt(ini, "RollingStockRoadTransport", "enabled", g_enabled);
    g_allowWagons = g_host->configInt(ini, "RollingStockRoadTransport", "allow_wagons", g_allowWagons);
    g_allowLocomotives = g_host->configInt(ini, "RollingStockRoadTransport", "allow_locomotives", g_allowLocomotives);
    g_allowRailDepotDropoff = g_host->configInt(ini, "RollingStockRoadTransport", "allow_train_depot_dropoff", g_allowRailDepotDropoff);
    g_enablePickupBridge = 0; // v0.2 bridge was unsafe and semantically incorrect
    g_enableLoadingPathPatches = g_host->configInt(ini, "RollingStockRoadTransport", "enable_loading_path_patches", g_enableLoadingPathPatches);
    g_enableTrainDepotUnloadBridge = 0; // v0.2.7 hook never receives the road-route event
    g_enableTriggeredDepotUnload = g_host->configInt(
        ini, "RollingStockRoadTransport", "enable_triggered_train_depot_unload",
        g_enableTriggeredDepotUnload);
    g_enableActualLoadWagonScale = g_host->configInt(
        ini, "RollingStockRoadTransport", "enable_actual_load_wagon_scale",
        g_enableActualLoadWagonScale);
    g_railFitLengthWeightOnly = g_host->configInt(
        ini, "RollingStockRoadTransport", "rail_fit_length_weight_only",
        g_railFitLengthWeightOnly);
    g_pickerShowAllFittingRail = g_host->configInt(
        ini, "RollingStockRoadTransport", "picker_show_all_fitting_rolling_stock",
        g_pickerShowAllFittingRail);
    // Deprecated v0.3.1 option. Forced state reset caused later route cycles
    // to attach/detach at the wrong stop, so v0.3.4 always leaves native
    // carrier route and loading state untouched.
    g_resetCarrierStateAfterUnload = 0;
    g_triggerRetryTicks = g_host->configInt(
        ini, "RollingStockRoadTransport", "triggered_unload_retry_ticks",
        g_triggerRetryTicks);
    if (g_triggerRetryTicks < 1) g_triggerRetryTicks = 1;
    if (g_triggerRetryTicks > 3600) g_triggerRetryTicks = 3600;
    g_debugLogging = g_host->configInt(ini, "RollingStockRoadTransport", "debug_logging", g_debugLogging);
    g_debugLimit = g_host->configInt(ini, "RollingStockRoadTransport", "debug_trace_limit", g_debugLimit);
    if (g_debugLimit < 0) g_debugLimit = 0;
    if (g_debugLimit > 1000) g_debugLimit = 1000;

    if (!g_enabled)
    {
        g_host->log("Rolling Stock Road Transport  disabled in RollingStockRoadTransport.ini");
        return 1;
    }

    g_host->log("Rolling Stock Road Transport  v1.0.0 initialised (wagons=%d locomotives=%d depot_dropoff=%d pickup_patches=%d actual_scale=%d unified_weight_length=%d dynamic_selector=%d reset_after_unload=%d triggered_unload=%d retry_ticks=%d debug=%d)",
                g_allowWagons != 0, g_allowLocomotives != 0,
                g_allowRailDepotDropoff != 0, g_enableLoadingPathPatches != 0,
                g_enableActualLoadWagonScale != 0,
                g_railFitLengthWeightOnly != 0,
                g_pickerShowAllFittingRail != 0,
                g_resetCarrierStateAfterUnload != 0,
                g_enableTriggeredDepotUnload != 0, g_triggerRetryTicks,
                g_debugLogging != 0);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_exeBase || !g_host || !g_host->installInlineHook)
        return 1;

    if (!Readable(g_exeBase + RVA_RAIL_TOTAL_LENGTH, 0xC0))
    {
        g_host->log("Rolling Stock Road Transport  native rail-length helper unavailable at RVA 0x%X",
                    static_cast<unsigned>(RVA_RAIL_TOTAL_LENGTH));
        return 1;
    }
    g_nativeRailTotalLength = reinterpret_cast<RailTotalLengthFn>(g_exeBase + RVA_RAIL_TOTAL_LENGTH);

    if (!InstallHook(RVA_CAN_LOAD_VEHICLE, reinterpret_cast<void*>(&DetourCanLoadVehicle),
                     reinterpret_cast<void**>(&g_originalCanLoadVehicle),
                     &ValidateCanLoadTarget,
                     "Rolling Stock Road Transport: unified weight/length vehicle fit", true))
        return 1;

    if (!InstallHook(RVA_BUILDING_COMPAT, reinterpret_cast<void*>(&DetourBuildingCompat),
                     reinterpret_cast<void**>(&g_originalBuildingCompat),
                     &ValidateBuildingCompatTarget,
                     "Rolling Stock Road Transport: rail depot destination", true))
        return 1;

    if (!Readable(g_exeBase + RVA_POINTER_VECTOR_RESERVE, 16))
    {
        g_host->log("Rolling Stock Road Transport  pointer-vector reserve target unavailable");
        return 1;
    }
    g_pointerVectorReserve = reinterpret_cast<PointerVectorReserveFn>(g_exeBase + RVA_POINTER_VECTOR_RESERVE);

    if (g_allowRailDepotDropoff && !InstallRouteDepotPatch())
        return 1;

    if (!InstallVehicleTypeSelectorPatch())
        return 1;

    // Hook the exact "Vehicle type selection" list builder that the route UI
    // invokes (FUN_1403de6e0), not the unrelated model picker used by v0.3.9.
    // The interior SHIP->ROAD category patch above remains active inside the
    // original/trampoline path; after native list construction we replace only
    // its rail entries with the dynamic current-availability + weight/length list.
    if (g_pickerShowAllFittingRail &&
        !InstallHook(RVA_VEHICLE_TYPE_SELECTOR, reinterpret_cast<void*>(&DetourVehicleTypeSelector),
                     reinterpret_cast<void**>(&g_originalVehicleTypeSelector),
                     &ValidateVehicleTypeSelectorTarget,
                     "Rolling Stock Road Transport: dynamic weight/length vehicle selector", true))
        return 1;

    if (!InstallWagonFitScalePatch())
        return 1;

    if (g_enableActualLoadWagonScale && !InstallActualLoadWagonScalePatch())
        return 1;

    if (g_enableLoadingPathPatches && !InstallLoadingPathPatches())
        return 1;

    if (!InstallHook(RVA_ACTUAL_LOAD, reinterpret_cast<void*>(&DetourActualLoad),
                     reinterpret_cast<void**>(&g_originalActualLoad),
                     &ValidateActualLoadTarget,
                     "Rolling Stock Road Transport: actual pickup diagnostics", true))
        return 1;

    if (g_enableTriggeredDepotUnload)
    {
        byte* unloadTarget = g_exeBase + RVA_UNLOAD_CARRIED;
        if (!ValidateUnloadTarget(unloadTarget))
        {
            g_host->log("Rolling Stock Road Transport  native carried-vehicle unload routine validation failed at RVA 0x%X",
                        static_cast<unsigned>(RVA_UNLOAD_CARRIED));
            return 1;
        }
        g_originalUnloadCarried = reinterpret_cast<UnloadCarriedFn>(unloadTarget);

        if (!InstallHook(RVA_ROUTE_ADVANCE, reinterpret_cast<void*>(&DetourRouteAdvance),
                         reinterpret_cast<void**>(&g_originalRouteAdvance),
                         &ValidateRouteAdvanceTarget,
                         "Rolling Stock Road Transport: railway-depot route advance", true))
            return 1;
    }

    g_host->log("Rolling Stock Road Transport  v1.0.0 active: one weight/length rule for selector + customs pickup, with native route-advance depot transfer unchanged");
    g_host->log("Rolling Stock Road Transport  selector rule: current-available top-level wagons/locomotives are shown when total empty mass <= truck payload and carried rail length <= longitudinal deck span");
    g_host->log("Rolling Stock Road Transport  pickup rule: the same mass/length predicate is authoritative; width and height cannot reject a rail vehicle after that predicate passes");
    g_host->log("Rolling Stock Road Transport  current-year availability remains enabled; v0.3.9 availability bypass is removed");
    g_host->log("Rolling Stock Road Transport  wagon geometry parity remains active in the native attachment routine");
    g_host->log("Rolling Stock Road Transport  railway-depot insertion now runs immediately before the game advances the completed depot route stop");
    g_host->log("Rolling Stock Road Transport  customs and other stops cannot trigger depot insertion because the still-current indexed stop must itself be a railway depot");
    g_host->log("Rolling Stock Road Transport  carrier route/load state is left untouched after a successful depot-row transfer");
    g_host->log("Rolling Stock Road Transport  no train-depot descriptor is left modified outside the transfer call");
    g_host->log("Rolling Stock Road Transport  rail length uses the game native total-length helper (including linked parts) at the carried 0.66 scale; deck length uses the carrier longitudinal Z span");
    g_host->log("Rolling Stock Road Transport  debug logging defaults OFF to avoid candidate-scan log spam");
    return 0;
}
