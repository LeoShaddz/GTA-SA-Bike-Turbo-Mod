// BikeTurbo.asi - GTA San Andreas PC v1.0 (US)
//
// Type BIKETURBO during gameplay to toggle the "bicycle turbo" on/off. It does not use the
// game's cheat system (custom input buffer; it does not count as a cheat or disable achievements/saves).
// When enabled, holding W (or the controller's accelerate button, e.g., A) while riding a BICYCLE
// makes the game register repeated button presses (actual state changes in CPad).
//
// Everything below has been verified in gta_sa.exe v1.0 US:
//  - CHud::SetHelpMessage = 0x588BE0 (cdecl: text, quick, permanent, addToBrief). This is the same
//    function the game uses to display the cheat activation message (called with 1,0,0). The help
//    box plays the original sound (frontend event 0x20) when the message appears.
//  - CHud::DrawHelpMessage = 0x58B6E0, called once per frame at 0x58FCFA (CHud::Draw).
//    The plugin intercepts this call to display the message on the game's main thread.
//  - CGame::Process = 0x53BEE0. Its first call (0x53BEE6) is CPad::UpdatePads (0x541DD0),
//    which reads the keyboard/controller (including GInput) and updates the CPad state every frame.
//    The plugin intercepts this call: immediately after the game updates CPad, and BEFORE any
//    character/vehicle is processed, the accelerate button (ButtonCross, CPad+0x20,
//    which corresponds to W on the keyboard) is "released" on alternating frames. This way, the
//    game registers actual button presses (pressed now / was not pressed before), rather than
//    just a continuous value.
//  - CTimer::m_FrameCounter = 0xB7CB4C (game frame counter; it does not advance while paused).
//  - Player CPad (pad 0) = 0xB73458. Bicycle = vehicle+0x594 == 10 (BMX).
//    Player vehicle: FindPlayerVehicle 0x56E0D0.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const uintptr_t VERSION_ADDR  = 0x82457C;
static const uint32_t  VERSION_OK    = 0x94BF;
static const uintptr_t FN_SETHELP    = 0x588BE0;
static const uintptr_t FN_DRAWHELP   = 0x58B6E0;
static const uintptr_t SITE_DRAWHELP = 0x58FCFA;
static const uintptr_t FN_UPDATEPADS = 0x541DD0;
static const uintptr_t SITE_UPDPADS  = 0x53BEE6;
static const uintptr_t PAD0          = 0xB73458;
static const int       OFF_CROSS     = 0x20;
static const uintptr_t FRAME_COUNTER = 0xB7CB4C;
static const uintptr_t FN_FINDVEH    = 0x56E0D0;
static const int       OFF_SUBCLASS  = 0x594;
static const int       SUBCLASS_BMX  = 10;

typedef void  (__cdecl   *SetHelp_t)(const char* text, int quick, int permanent, int brief);
typedef void* (__cdecl   *FindVeh_t)(int player, int remote);
typedef void  (__cdecl   *Void_t)();

static HMODULE hSelf = NULL;
static char    gIni[MAX_PATH];
static bool    gLog = false;

static char    gCode[24]   = "BIKETURBO";
static char    gTextOn[64] = "Bike Turbo On";
static char    gTextOff[64] = "Bike Turbo Off";
static unsigned gHalfFrames = 1;       // Number of "pressed" / "released" frames per tap

static volatile bool gOn      = false;
static volatile int  gPending = 0;     // 0 = none, 1 = display "On", 2 = display "Off"

static void Log(const char* fmt, ...)
{
    if (!gLog) return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hSelf, path, MAX_PATH)) return;
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log"); else strcat(path, ".log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static bool GameFocused()
{
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------- Typed code detection (separate thread) ----------------
static DWORD WINAPI KeyThread(LPVOID)
{
    const int len = (int)strlen(gCode);
    char  buf[32] = { 0 };
    int   n = 0;
    bool  prev[26] = { false };
    DWORD last = 0;

    for (;;)
    {
        Sleep(4);
        if (!GameFocused()) { for (int i = 0; i < 26; i++) prev[i] = false; continue; }

        DWORD now = GetTickCount();
        if (n > 0 && now - last > 4000) n = 0;                // Long pause: reset input buffer

        for (int i = 0; i < 26; i++)
        {
            bool down = (GetAsyncKeyState('A' + i) & 0x8000) != 0;
            if (down && !prev[i])
            {
                if (n == len) { memmove(buf, buf + 1, len - 1); n = len - 1; }
                buf[n++] = (char)('A' + i);
                buf[n] = 0;
                last = now;
                if (n == len && memcmp(buf, gCode, len) == 0)
                {
                    gOn = !gOn;
                    gPending = gOn ? 1 : 2;
                    n = 0; buf[0] = 0;
                    Log("Turbo %s", gOn ? "ENABLED" : "DISABLED");
                }
            }
            prev[i] = down;
        }
    }
    return 0;
}

// ---------------- Notification (on the game thread) ----------------
// Replaces "call CHud::DrawHelpMessage": displays the pending message and then draws normally.
static void __cdecl HookDrawHelp()
{
    int p = gPending;
    if (p)
    {
        gPending = 0;
        ((SetHelp_t)FN_SETHELP)(p == 1 ? gTextOn : gTextOff, 1, 0, 0);   // Same call used by cheats
    }
    ((Void_t)FN_DRAWHELP)();
}

// ---------------- Turbo ----------------
static int gLogPulse = 0;

// Replaces "call CPad::UpdatePads" at the beginning of CGame::Process
static void __cdecl HookUpdatePads()
{
    ((Void_t)FN_UPDATEPADS)();                     // Let the game read the keyboard/controller normally

    if (!gOn) return;
    volatile short* cross = (volatile short*)(PAD0 + OFF_CROSS);
    if (*cross <= 0) return;                        // W / accelerate is not being held

    void* veh = ((FindVeh_t)FN_FINDVEH)(-1, 0);
    if (!veh) return;
    if (*(volatile int*)((uint8_t*)veh + OFF_SUBCLASS) != SUBCLASS_BMX) return;     // Bicycles only

    unsigned frame = *(volatile unsigned*)FRAME_COUNTER;
    if (((frame / gHalfFrames) & 1) != 0)           // Alternating frames: release the button
    {
        *cross = 0;
        if (gLogPulse < 6) { ++gLogPulse; Log("Turbo active on bicycle (alternating accelerate button state)"); }
    }
}

// ---------------- Installation ----------------
static uintptr_t CallTarget(uintptr_t site)
{
    uint8_t* c = (uint8_t*)site;
    if (c[0] != 0xE8) return 0;
    return site + 5 + (uintptr_t)(intptr_t)(*(int32_t*)(c + 1));
}

static bool WriteCall(uintptr_t at, void* target)
{
    DWORD old;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uint8_t*)at = 0xE8;
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)target - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

static void ReadText(const char* key, const char* def, char* out, size_t cap)
{
    GetPrivateProfileStringA("BikeTurbo", key, def, out, (DWORD)cap, gIni);
}

BOOL APIENTRY DllMain(HMODULE hm, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hm);
    hSelf = hm;

    GetModuleFileNameA(hm, gIni, MAX_PATH);
    char* dot = strrchr(gIni, '.');
    if (dot) strcpy(dot, ".ini"); else strcat(gIni, ".ini");

    gLog = GetPrivateProfileIntA("BikeTurbo", "Log", 0, gIni) != 0;
    if (!GetPrivateProfileIntA("BikeTurbo", "Enable", 1, gIni)) { Log("Disabled in .ini"); return TRUE; }

    if (*(uint32_t*)VERSION_ADDR != VERSION_OK) { Log("Unrecognized executable version (expected 1.0 US). Mod disabled."); return TRUE; }

    // Read the cheat code: letters A-Z only, 4 to 20 characters
    char code[32];
    ReadText("Code", "BIKETURBO", code, sizeof code);
    int k = 0;
    for (const char* p = code; *p && k < 20; ++p)
    {
        char c = *p; if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') gCode[k++] = c;
    }
    gCode[k] = 0;
    if (k < 4) strcpy(gCode, "BIKETURBO");

    ReadText("TextOn",  "Bike Turbo On",  gTextOn,  sizeof gTextOn);
    ReadText("TextOff", "Bike Turbo Off", gTextOff, sizeof gTextOff);

    int frames = GetPrivateProfileIntA("BikeTurbo", "TapFrames", 1, gIni);
    if (frames < 1) frames = 1;
    if (frames > 30) frames = 30;
    gHalfFrames = (unsigned)frames;
    gOn = GetPrivateProfileIntA("BikeTurbo", "StartEnabled", 0, gIni) != 0;

    // Verify the game's code before modifying anything
    bool ok = true;
    if (CallTarget(SITE_UPDPADS) != FN_UPDATEPADS) ok = false;
    if (CallTarget(SITE_DRAWHELP) != FN_DRAWHELP) ok = false;
    static const uint8_t setHelp[6] = { 0xA0, 0xC0, 0xAE, 0xBA, 0x00, 0x84 };
    if (memcmp((void*)FN_SETHELP, setHelp, sizeof setHelp) != 0) ok = false;
    if (!ok) { Log("Game code differs from expected. Another mod may have modified these locations. Mod disabled."); return TRUE; }

    bool a = WriteCall(SITE_UPDPADS, (void*)&HookUpdatePads);
    bool b = WriteCall(SITE_DRAWHELP, (void*)&HookDrawHelp);
    CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
    Log("Enabled: code=%s, %u frame(s) per half-tap, initially enabled=%d. Patch: pad=%s, message=%s",
        gCode, gHalfFrames, (int)gOn, a ? "OK" : "FAILED", b ? "OK" : "FAILED");
    return TRUE;
}
