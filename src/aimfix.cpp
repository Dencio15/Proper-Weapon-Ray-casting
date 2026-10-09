// aimfix.asi - GTA V style weapon aiming for GTA SA 1.0 US (SA-MP safe).
//
// 1. Weapon-origin shots. CWeapon::FireInstantHit gets the camera position and the far point
//    under the crosshair (CCamera::Find3rdPersonCamTargetVector) and raycasts the bullet from
//    the CAMERA (0x74071C), so you can shoot over cover your gun is behind. We hook that call
//    (0x7403BE, the local player's aiming path): the target becomes what is really under the
//    crosshair and the ray starts at the weapon origin, so walls in front of the gun block it.
//
// 2. Obstruction marker. While aiming, the same shot is predicted every frame: weapon origin
//    (exactly as CTaskSimpleUseGun::FireGun computes it) toward the same aim point, raycast
//    with FireInstantHit's collision flags. If something between the gun and the crosshair
//    target blocks that ray, an X is drawn where the bullet will actually hit.
//
// Both use AimTarget() below, so the marker always shows the ray the game will fire.
// Raycasts use GTA's collision models (CWorld::ProcessLineOfSight), which is what bullets hit;
// the visible mesh can differ slightly from collision.
//
// Settings: aimfix.ini next to gta_sa.exe (created with defaults on first run).
// Marker texture: models\txd\aimfix.txd (make_aimfix_txd.py).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>

namespace {

// --- math ---

struct CVector {
    float x, y, z;
};

CVector Add(const CVector& a, const CVector& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CVector Sub(const CVector& a, const CVector& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CVector Mul(const CVector& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const CVector& a, const CVector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const CVector& a) { return std::sqrt(Dot(a, a)); }

struct CColPoint {
    CVector point;
    float pad0;
    CVector normal;
    uint8_t rest[0x2C - 0x1C];
};

struct CRect {  // GTA layout: left, bottom, right, top
    float left, bottom, right, top;
};

struct CRGBA {
    uint8_t r, g, b, a;
};

struct CSprite2d {
    void* texture;
};

// --- GTA SA 1.0 US ---

constexpr uintptr_t kCamTargetCall = 0x7403BE;   // FireInstantHit: call CCamera::Find3rdPersonCamTargetVector
constexpr uintptr_t kHudDrawCall = 0x53E4FF;     // Render2dStuff: call CHud::Draw
constexpr uintptr_t kCrosshairCall = 0x58FBBF;   // CHud::Draw: call CHud::DrawCrossHairs (its only caller)
constexpr uintptr_t kTheCamera = 0xB6F028;
constexpr uintptr_t kIgnoreEntity = 0xB7CD68;    // CEntity* CWorld::pIgnoreEntity
constexpr uintptr_t kPlayerInFocus = 0xB7CD74;   // u8 CWorld::PlayerInFocus (read by FindPlayerPed(-1))
constexpr uintptr_t kFireZOffsetFallback = 0x858CB0;  // float, FireGun origin height when no skeleton

using CamTargetFn = void(__thiscall*)(void*, float, CVector, CVector*, CVector*);
using ProcessLineOfSightFn = bool(__cdecl*)(const CVector&, const CVector&, CColPoint&, void*&, bool buildings,
                                            bool vehicles, bool peds, bool objects, bool dummies, bool seeThrough,
                                            bool cameraIgnore, bool shootThrough);
const auto CamTargetOriginal = reinterpret_cast<CamTargetFn>(0x514970);
const auto ProcessLineOfSight = reinterpret_cast<ProcessLineOfSightFn>(0x56BA00);
const auto FindPlayerPed = reinterpret_cast<uint8_t*(__cdecl*)(int)>(0x56E210);
const auto GetWeaponInfo = reinterpret_cast<uint8_t*(__cdecl*)(int type, int skill)>(0x743C60);
const auto GetWeaponSkill = reinterpret_cast<int(__thiscall*)(void* ped, int type)>(0x5E3B60);
const auto GetBonePosition = reinterpret_cast<void(__thiscall*)(void* ped, CVector& out, int bone, bool updateSkin)>(0x5E4280);
const auto CalcScreenCoors = reinterpret_cast<bool(__cdecl*)(const CVector& in, CVector* out, float* w, float* h,
                                                             bool checkFar, bool checkNear)>(0x70CE30);
const auto FindTxdSlot = reinterpret_cast<int(__cdecl*)(const char*)>(0x731850);
const auto AddTxdSlot = reinterpret_cast<int(__cdecl*)(const char*)>(0x731C80);
const auto LoadTxd = reinterpret_cast<bool(__cdecl*)(int slot, const char* path)>(0x7320B0);
const auto AddTxdRef = reinterpret_cast<void(__cdecl*)(int slot)>(0x731A00);
const auto PushCurrentTxd = reinterpret_cast<void(__cdecl*)()>(0x7316A0);
const auto PopCurrentTxd = reinterpret_cast<void(__cdecl*)()>(0x7316B0);
const auto SetCurrentTxd = reinterpret_cast<void(__cdecl*)(int slot)>(0x7319C0);
const auto SpriteSetTexture = reinterpret_cast<void(__thiscall*)(CSprite2d*, const char*)>(0x727270);
const auto SpriteDraw = reinterpret_cast<void(__thiscall*)(CSprite2d*, const CRect&, const CRGBA&)>(0x728350);
const auto IsPlayerPed = reinterpret_cast<bool(__thiscall*)(void* ped)>(0x5DF8F0);  // CPed::IsPlayer
const auto GetTransformedBonePosition =
    reinterpret_cast<void(__thiscall*)(void* ped, CVector& inOut, int bone, bool updateSkin)>(0x5E01C0);
// CBulletTraces::AddTrace(start, end, radius, lifetime ms, alpha): GTA's tracer renderer
const auto AddBulletTrace = reinterpret_cast<void(__cdecl*)(CVector* start, CVector* end, float radius, unsigned ms,
                                                            uint8_t alpha)>(0x723750);
constexpr uintptr_t kShotLosCall = 0x740721;     // FireInstantHit: aimed bullet's ProcessLineOfSight (samp hooks it too)
constexpr uintptr_t kHipLosCall = 0x740B69;      // FireInstantHit: unaimed (hipfire) bullet's LOS (samp hooks it too)

// CPed / CWeapon / CWeaponInfo / CCam fields used by FireGun and FireInstantHit
constexpr unsigned kPedFlags = 0x474;         // bit 0x400: skeleton available -> bone based origin
constexpr unsigned kPedWeapons = 0x5A0;       // CWeapon[13], 0x1C each, eWeaponType first
constexpr unsigned kPedActiveSlot = 0x718;    // int8
constexpr unsigned kWeaponInfoFireType = 0x0; // eWeaponFire, 1 = instant hit
constexpr unsigned kWeaponInfoRange = 0x8;    // float
constexpr unsigned kWeaponInfoFireOffset = 0x24;
constexpr int kBoneRightWrist = 24;
constexpr unsigned kCamActive = 0x59, kCams = 0x174, kCamSize = 0x238, kCamMode = 0x0C;

// Collision flags FireInstantHit uses for the shot (0x7406FA)
bool ShotLineOfSight(const CVector& from, const CVector& to, CColPoint& col, void* ignore) {
    void*& ignoreEntity = *reinterpret_cast<void**>(kIgnoreEntity);
    void* const old = ignoreEntity;
    ignoreEntity = ignore;
    void* entity = nullptr;
    const bool hit = ProcessLineOfSight(from, to, col, entity, true, true, true, true, true, false, false, true);
    ignoreEntity = old;
    return hit;
}

// --- settings (aimfix.ini) ---

struct Settings {
    bool alignShots = true;
    bool fromMuzzle = true;
    bool marker = true;
    bool hideCrosshair = true;
    char txd[32] = "aimfix";
    char texture[32] = "x";
    float size = 0.18f;        // world metres
    float minPx = 10.0f, maxPx = 42.0f;
    CRGBA colour {255, 60, 60, 230};
    float maxDistance = 80.0f; // camera -> marker
    float tolerance = 0.3f;    // blocked if the bullet stops this much before the crosshair target
    float smoothing = 20.0f;   // 1/s; higher follows faster
    float fade = 12.0f;        // alpha change per second
    bool tracer = true;
    bool tracerOthers = true;
    float tracerRadius = 0.02f;
    unsigned tracerMs = 300;
    uint8_t tracerAlpha = 200;
} g_cfg;

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

float IniFloat(const char* path, const char* sec, const char* key, float def) {
    char buf[32], defBuf[32];
    snprintf(defBuf, sizeof defBuf, "%g", def);
    GetPrivateProfileStringA(sec, key, defBuf, buf, sizeof buf, path);
    return static_cast<float>(atof(buf));
}

void LoadSettings() {
    const char* path = ".\\aimfix.ini";
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") == 0 && f) {
            fputs("; aimfix.asi settings\n"
                  "[Aim]\n"
                  "; 1 = shots fire from the weapon toward what is under the crosshair (cover in\n"
                  ";     front of the gun blocks them); 0 = GTA default, shots fire from the camera\n"
                  "AlignShots=1\n"
                  "; where aimed shots start: Muzzle (the gun barrel) or Origin (GTA's point, above the gun)\n"
                  "ShotFrom=Muzzle\n\n"
                  "[Marker]\n"
                  "; X shown where the bullet will hit when something blocks the gun\n"
                  "Enabled=1\n"
                  "; 1 = hide the crosshair while the X is shown\n"
                  "HideCrosshair=1\n"
                  "; texture from models\\txd\\<Txd>.txd\n"
                  "Txd=aimfix\n"
                  "Texture=x\n"
                  "; size in world metres, clamped to MinPx..MaxPx on screen\n"
                  "Size=0.18\n"
                  "MinPx=10\n"
                  "MaxPx=42\n"
                  "; RRGGBBAA\n"
                  "Colour=FF3C3CE6\n"
                  "MaxDistance=80\n"
                  "; metres the bullet must stop short of the crosshair target to count as blocked\n"
                  "Tolerance=0.3\n"
                  "; follow speed (1/s) and fade speed\n"
                  "Smoothing=20\n"
                  "Fade=12\n\n"
                  "[Tracer]\n"
                  "; line from the gun to where each of your aimed shots really hit\n"
                  "Enabled=1\n"
                  "; 1 = also show other players' tracers (not NPCs)\n"
                  "Others=1\n"
                  "; thickness in metres, lifetime in ms, opacity 0-255\n"
                  "Radius=0.02\n"
                  "TimeMs=300\n"
                  "Alpha=200\n",
                  f);
            fclose(f);
        }
    }
    g_cfg.alignShots = GetPrivateProfileIntA("Aim", "AlignShots", 1, path) != 0;
    char from[16];
    GetPrivateProfileStringA("Aim", "ShotFrom", "Muzzle", from, sizeof from, path);
    g_cfg.fromMuzzle = _stricmp(from, "Origin") != 0;
    g_cfg.marker = GetPrivateProfileIntA("Marker", "Enabled", 1, path) != 0;
    g_cfg.hideCrosshair = GetPrivateProfileIntA("Marker", "HideCrosshair", 1, path) != 0;
    GetPrivateProfileStringA("Marker", "Txd", "aimfix", g_cfg.txd, sizeof g_cfg.txd, path);
    GetPrivateProfileStringA("Marker", "Texture", "x", g_cfg.texture, sizeof g_cfg.texture, path);
    g_cfg.size = IniFloat(path, "Marker", "Size", g_cfg.size);
    g_cfg.minPx = IniFloat(path, "Marker", "MinPx", g_cfg.minPx);
    g_cfg.maxPx = IniFloat(path, "Marker", "MaxPx", g_cfg.maxPx);
    g_cfg.maxDistance = IniFloat(path, "Marker", "MaxDistance", g_cfg.maxDistance);
    g_cfg.tolerance = IniFloat(path, "Marker", "Tolerance", g_cfg.tolerance);
    g_cfg.smoothing = IniFloat(path, "Marker", "Smoothing", g_cfg.smoothing);
    g_cfg.fade = IniFloat(path, "Marker", "Fade", g_cfg.fade);
    g_cfg.tracer = GetPrivateProfileIntA("Tracer", "Enabled", 1, path) != 0;
    g_cfg.tracerOthers = GetPrivateProfileIntA("Tracer", "Others", 1, path) != 0;
    g_cfg.tracerRadius = IniFloat(path, "Tracer", "Radius", g_cfg.tracerRadius);
    g_cfg.tracerMs = GetPrivateProfileIntA("Tracer", "TimeMs", g_cfg.tracerMs, path);
    g_cfg.tracerAlpha = static_cast<uint8_t>(GetPrivateProfileIntA("Tracer", "Alpha", g_cfg.tracerAlpha, path));
    char colour[16];
    GetPrivateProfileStringA("Marker", "Colour", "FF3C3CE6", colour, sizeof colour, path);
    const unsigned long rgba = strtoul(colour, nullptr, 16);
    g_cfg.colour = {static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(rgba >> 16), static_cast<uint8_t>(rgba >> 8),
                    static_cast<uint8_t>(rgba)};
    Log("settings: align=%d marker=%d txd=%s:%s size=%.2f px=%.0f..%.0f colour=%08lX", g_cfg.alignShots,
        g_cfg.marker, g_cfg.txd, g_cfg.texture, g_cfg.size, g_cfg.minPx, g_cfg.maxPx, rgba);
}

// --- shared aim logic ---

bool g_shotHookInstalled = false;

// Given the camera ray (camSource -> farTarget) and the weapon origin, returns the target the
// shot is fired at. With AlignShots the origin is aimed at the camera ray's hit point (same
// range), otherwise GTA's far point is kept. aimHit/aimPoint report the crosshair hit.
CVector AimTarget(const CVector& origin, const CVector& camSource, const CVector& farTarget, void* shooter,
                  bool& aimHit, CVector& aimPoint) {
    CColPoint col;
    aimHit = ShotLineOfSight(camSource, farTarget, col, shooter);
    aimPoint = aimHit ? col.point : farTarget;
    if (!aimHit || !g_cfg.alignShots) return farTarget;

    // Only hits in front of the gun; a wall between camera and player keeps GTA's ray
    const CVector camRay = Sub(farTarget, camSource);
    const float camLen = Length(camRay);
    const CVector toHit = Sub(col.point, origin);
    if (camLen < 0.001f || Dot(toHit, camRay) / camLen < 0.5f) return farTarget;

    const float reach = Length(Sub(farTarget, origin));
    return Add(origin, Mul(toHit, reach / Length(toHit)));
}

// Raycasts only register surfaces they enter, so a gun pushed into a wall would fire straight
// through it. If anything lies between the chest and the weapon origin, start the shot at the
// chest instead: the ray then enters that wall from outside and stops there.
constexpr int kBoneUpperTorso = 4;

bool MuzzlePosition(uint8_t* ped, CVector& muzzle);

// Where aimed shots start: the gun's muzzle (what you see, default) or GTA's origin (wrist +
// fireOffset.z + 0.15, which sits above the gun and clears low cover the barrel is behind)
CVector ShotStart(uint8_t* ped, const CVector& gtaOrigin) {
    CVector muzzle;
    if (g_cfg.fromMuzzle && MuzzlePosition(ped, muzzle)) return muzzle;
    return gtaOrigin;
}

CVector SafeShotStart(void* ped, const CVector& origin) {
    if (!ped || !(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ped) + kPedFlags) & 0x400)) return origin;
    CVector chest;
    GetBonePosition(ped, chest, kBoneUpperTorso, false);
    chest.z = origin.z;  // same height as the gun, so only the horizontal reach is tested
    CColPoint col;
    return ShotLineOfSight(chest, origin, col, ped) ? chest : origin;
}

// --- 1. shot alignment hook ---

CamTargetFn g_chainedCamTarget = nullptr;

// FireInstantHit keeps the shooting ped in EBP (IsPlayer(ebp) at 0x74034A, vehicle/flags at
// 0x740AE1). samp replays remote players' shots through the same code without switching
// PlayerInFocus, so the shooter is captured by these stubs and compared with player 0.
uint8_t* g_shooter = nullptr;

bool ShooterIsLocalPlayer() { return g_shooter && g_shooter == FindPlayerPed(0); }

void __fastcall CamTargetHook(void* camera, void*, float range, CVector source, CVector* camSource, CVector* target) {
    g_chainedCamTarget(camera, range, source, camSource, target);
    if (!camSource || !target || !g_cfg.alignShots) return;
    if (!ShooterIsLocalPlayer()) return;  // remote players / NPCs: leave their shots alone
    bool aimHit;
    CVector aimPoint;
    // FireInstantHit raycasts camSource -> target (0x74071C), i.e. from the camera. Aim at the
    // crosshair hit, then start the ray at the weapon instead, so cover in front of the gun
    // stops the bullet (GTA V behaviour). Spread is added to target afterwards by the game.
    const CVector start = SafeShotStart(g_shooter, ShotStart(g_shooter, source));
    *target = AimTarget(start, *camSource, *target, g_shooter, aimHit, aimPoint);
    *camSource = start;
}

// --- 3. bullet tracer: hook on the bullet's LOS, so the end point includes the game's spread ---

ProcessLineOfSightFn g_chainedShotLos = nullptr;

// Muzzle as FireGun places it: the weapon's fire offset carried by the right wrist bone
bool MuzzlePosition(uint8_t* ped, CVector& muzzle) {
    const int8_t slot = *reinterpret_cast<int8_t*>(ped + kPedActiveSlot);
    if (slot < 0 || slot > 12 || !(*reinterpret_cast<uint32_t*>(ped + kPedFlags) & 0x400)) return false;
    const int type = *reinterpret_cast<int*>(ped + kPedWeapons + slot * 0x1C);
    const uint8_t* info = GetWeaponInfo(type, GetWeaponSkill(ped, type));
    if (!info) return false;
    muzzle = *reinterpret_cast<const CVector*>(info + kWeaponInfoFireOffset);
    GetTransformedBonePosition(ped, muzzle, kBoneRightWrist, false);
    return true;
}

// Tracer for our shots and, with [Tracer] Others=1, other players' (samp replays remote shots
// through FireInstantHit and its own hook on these LOS calls puts in the synced hit, so the
// result we see is where their bullet really went). NPC shots are left alone.
bool ShouldTrace(uint8_t* shooter) {
    if (!g_cfg.tracer || !shooter) return false;
    if (shooter == FindPlayerPed(0)) return true;
    return g_cfg.tracerOthers && IsPlayerPed(shooter);
}

void AddTracer(uint8_t* shooter, const CVector& target, bool hit, const CColPoint& col) {
    CVector start, end = hit ? col.point : target;
    if (MuzzlePosition(shooter, start)) {
        AddBulletTrace(&start, &end, g_cfg.tracerRadius, g_cfg.tracerMs, g_cfg.tracerAlpha);
    }
}

ProcessLineOfSightFn g_chainedHipLos = nullptr;

// Aimed (0x740721) and hipfire (0x740B69) bullet LOS: same handling, different chained target
template <ProcessLineOfSightFn* Chained>
bool __cdecl BulletLosHook(const CVector& origin, const CVector& target, CColPoint& col, void*& entity, bool buildings,
                           bool vehicles, bool peds, bool objects, bool dummies, bool seeThrough, bool cameraIgnore,
                           bool shootThrough) {
    uint8_t* const shooter = g_shooter;  // captured by the stub right before this call
    const bool hit = (*Chained)(origin, target, col, entity, buildings, vehicles, peds, objects, dummies, seeThrough,
                                cameraIgnore, shootThrough);
    if (ShouldTrace(shooter)) AddTracer(shooter, target, hit, col);
    return hit;
}

// Entry stubs at the hooked call sites: record the shooter (EBP) and continue into the hook
// with registers and stack untouched.
__declspec(naked) void CamTargetStub() {
    __asm {
        mov dword ptr [g_shooter], ebp
        jmp CamTargetHook
    }
}

const void* const g_shotLosHook = reinterpret_cast<const void*>(&BulletLosHook<&g_chainedShotLos>);
const void* const g_hipLosHook = reinterpret_cast<const void*>(&BulletLosHook<&g_chainedHipLos>);

__declspec(naked) void ShotLosStub() {
    __asm {
        mov dword ptr [g_shooter], ebp
        jmp dword ptr [g_shotLosHook]
    }
}

__declspec(naked) void HipLosStub() {
    __asm {
        mov dword ptr [g_shooter], ebp
        jmp dword ptr [g_hipLosHook]
    }
}

// --- 2. shot prediction ---

struct ShotPrediction {
    bool blocked;
    CVector impact;
    CVector normal;
};

int CameraMode() {
    const auto* camera = reinterpret_cast<const uint8_t*>(kTheCamera);
    unsigned active = camera[kCamActive];
    if (active > 2) active = 0;
    return *reinterpret_cast<const uint16_t*>(camera + kCams + active * kCamSize + kCamMode);
}

// Mirrors CTaskSimpleUseGun::FireGun (origin) + CWeapon::FireInstantHit (target, raycast).
// Returns false when the player is not aiming an instant hit weapon.
bool PredictShot(ShotPrediction& out) {
    out.blocked = false;
    // FireInstantHit uses the camera target in modes 53/55/65/49; 55 (drive-by) and 49 fire
    // through other tasks, where this origin estimate would be wrong, so only 53 and 65.
    const int mode = CameraMode();
    if (mode != 53 && mode != 65) return false;
    if (*reinterpret_cast<const uint8_t*>(kPlayerInFocus) != 0) return false;

    uint8_t* ped = FindPlayerPed(0);
    if (!ped) return false;
    const int8_t slot = *reinterpret_cast<int8_t*>(ped + kPedActiveSlot);
    if (slot < 0 || slot > 12) return false;
    const int type = *reinterpret_cast<int*>(ped + kPedWeapons + slot * 0x1C);
    const uint8_t* info = GetWeaponInfo(type, GetWeaponSkill(ped, type));
    if (!info || *reinterpret_cast<const int*>(info + kWeaponInfoFireType) != 1) return false;  // instant hit only

    // Weapon origin, as FireGun passes it to Fire(): wrist bone raised by fireOffset.z + 0.15
    const CVector fireOffset = *reinterpret_cast<const CVector*>(info + kWeaponInfoFireOffset);
    CVector origin;
    if (*reinterpret_cast<uint32_t*>(ped + kPedFlags) & 0x400) {
        GetBonePosition(ped, origin, kBoneRightWrist, false);
        origin.z += fireOffset.z + 0.15f;
    } else {
        const auto* matrix = *reinterpret_cast<uint8_t**>(ped + 0x14);
        origin = matrix ? *reinterpret_cast<const CVector*>(matrix + 0x30) : *reinterpret_cast<const CVector*>(ped + 4);
        origin.z += *reinterpret_cast<const float*>(kFireZOffsetFallback);
    }

    // Same camera target and range FireInstantHit uses (range * 3.0 at 0x740390)
    const float range = *reinterpret_cast<const float*>(info + kWeaponInfoRange) * 3.0f;
    CVector camSource, farTarget;
    CamTargetOriginal(reinterpret_cast<void*>(kTheCamera), range, origin, &camSource, &farTarget);

    bool aimHit;
    CVector aimPoint;
    // Without the hook (or with AlignShots=0) the game shoots from the camera: nothing to show
    if (!g_shotHookInstalled || !g_cfg.alignShots) return false;
    const CVector start = SafeShotStart(ped, ShotStart(ped, origin));  // same start the shot hook will use
    const CVector target = AimTarget(start, camSource, farTarget, ped, aimHit, aimPoint);

    CColPoint col;
    if (!ShotLineOfSight(start, target, col, ped)) return true;  // bullet flies clear

    // Blocked if the bullet stops before what the crosshair is on (or anywhere, if the
    // crosshair is on nothing)
    const float impactDist = Length(Sub(col.point, start));
    const float aimDist = aimHit ? Length(Sub(aimPoint, start)) : 1e9f;
    out.blocked = impactDist < aimDist - g_cfg.tolerance;
    out.impact = col.point;
    out.normal = col.normal;
    return true;
}

// --- marker rendering ---

struct Marker {
    bool textureTried = false;
    CSprite2d sprite {nullptr};
    CVector pos {};
    float alpha = 0.0f;  // 0..1
    bool hasPos = false;
    LARGE_INTEGER last {};
} g_marker;

void LoadMarkerTexture() {
    g_marker.textureTried = true;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "models\\txd\\%s.txd", g_cfg.txd);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log("marker: %s not found, marker disabled", path);
        return;
    }
    int slot = FindTxdSlot(g_cfg.txd);
    if (slot == -1) slot = AddTxdSlot(g_cfg.txd);
    LoadTxd(slot, path);
    AddTxdRef(slot);
    PushCurrentTxd();
    SetCurrentTxd(slot);
    SpriteSetTexture(&g_marker.sprite, g_cfg.texture);
    PopCurrentTxd();
    Log(g_marker.sprite.texture ? "marker: loaded %s:%s" : "marker: texture %s:%s not found", g_cfg.txd, g_cfg.texture);
}

float FrameSeconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g_marker.last.QuadPart
                         ? static_cast<float>(now.QuadPart - g_marker.last.QuadPart) / static_cast<float>(freq.QuadPart)
                         : 0.0f;
    g_marker.last = now;
    return dt > 0.1f ? 0.1f : dt;
}

bool g_markerShown = false;  // X drawn this frame (the crosshair is skipped next frame)

void UpdateAndDrawMarker() {
    g_markerShown = false;
    if (!g_cfg.marker) return;
    if (!g_marker.textureTried) LoadMarkerTexture();
    if (!g_marker.sprite.texture) return;

    const float dt = FrameSeconds();
    ShotPrediction shot;
    const bool aiming = PredictShot(shot);
    const bool show = aiming && shot.blocked;

    if (show) {
        // Lift off the surface so it never sinks into it; smooth small moves, snap big jumps
        const CVector target = Add(shot.impact, Mul(shot.normal, 0.03f));
        if (!g_marker.hasPos || g_marker.alpha <= 0.0f || Length(Sub(target, g_marker.pos)) > 1.5f) {
            g_marker.pos = target;
        } else {
            const float k = 1.0f - std::exp(-g_cfg.smoothing * dt);
            g_marker.pos = Add(g_marker.pos, Mul(Sub(target, g_marker.pos), k));
        }
        g_marker.hasPos = true;
    }
    const float step = g_cfg.fade * dt;
    g_marker.alpha = show ? (g_marker.alpha + step > 1.0f ? 1.0f : g_marker.alpha + step)
                          : (g_marker.alpha - step < 0.0f ? 0.0f : g_marker.alpha - step);
    // Hide at once when no longer aiming; fade only between blocked/clear while aiming
    if (!aiming) g_marker.alpha = 0.0f;
    if (g_marker.alpha <= 0.0f || !g_marker.hasPos) return;

    // Screen position; w/h are pixels per world unit at that depth (scales with distance)
    CVector screen;
    float w, h;
    if (!CalcScreenCoors(g_marker.pos, &screen, &w, &h, true, true)) return;
    if (screen.z > g_cfg.maxDistance) return;
    float half = g_cfg.size * w * 0.5f;
    if (half < g_cfg.minPx * 0.5f) half = g_cfg.minPx * 0.5f;
    if (half > g_cfg.maxPx * 0.5f) half = g_cfg.maxPx * 0.5f;

    CRGBA colour = g_cfg.colour;
    colour.a = static_cast<uint8_t>(colour.a * g_marker.alpha);
    const CRect rect {screen.x - half, screen.y + half, screen.x + half, screen.y - half};
    SpriteDraw(&g_marker.sprite, rect, colour);
    g_markerShown = show;  // not while fading out, so the crosshair returns at once
}

// CHud::Draw -> DrawCrossHairs: skipped while the X is up (state from the previous frame,
// since the HUD, crosshair included, is drawn before the marker is updated)
uintptr_t g_drawCrosshairs = 0;

void __cdecl CrosshairHook() {
    if (g_cfg.hideCrosshair && g_markerShown) return;
    reinterpret_cast<void(__cdecl*)()>(g_drawCrosshairs)();
}

// Render2dStuff -> CHud::Draw; the marker is drawn right after the HUD
uintptr_t g_hudDraw = 0;
void EnsureShotLosHook();

void __cdecl HudDrawHook() {
    reinterpret_cast<void(__cdecl*)()>(g_hudDraw)();
    EnsureShotLosHook();
    UpdateAndDrawMarker();
}

// --- install ---

// Redirects an E8 call to hook and returns whatever it called before (to chain), 0 on failure
uintptr_t RedirectCall(uintptr_t site, void* hook, const char* name) {
    auto* at = reinterpret_cast<uint8_t*>(site);
    if (at[0] != 0xE8) {
        Log("%s: call site 0x%X is not a call (%02X), not GTA SA 1.0 US or patched", name, static_cast<unsigned>(site), at[0]);
        return 0;
    }
    int32_t rel;
    memcpy(&rel, at + 1, 4);
    const uintptr_t previous = site + 5 + rel;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
    DWORD old;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    Log("%s: hooked 0x%X (chained to 0x%08X)", name, static_cast<unsigned>(site), static_cast<unsigned>(previous));
    return previous;
}

// samp redirects the bullet LOS call for bullet sync, possibly after we started. Checked once
// per frame on the game thread: if the call no longer points at us, chain whatever it points
// at now (samp's hook or the original) and take it back.
template <typename Fn>
void KeepCallHook(uintptr_t site, void* hook, Fn& chained, const char* name) {
    const auto* at = reinterpret_cast<const uint8_t*>(site);
    int32_t rel;
    memcpy(&rel, at + 1, 4);
    if (at[0] != 0xE8 || site + 5 + rel == reinterpret_cast<uintptr_t>(hook)) return;
    if (uintptr_t prev = RedirectCall(site, hook, name)) chained = (Fn)prev;  // function pointer or uintptr_t
}

// samp (0.3.DL at least) also redirects the crosshair call for its own crosshair offset, after
// we started; keep ours in front of it the same way.
void EnsureShotLosHook() {
    if (g_cfg.hideCrosshair && g_drawCrosshairs) {
        KeepCallHook(kCrosshairCall, reinterpret_cast<void*>(&CrosshairHook), g_drawCrosshairs, "crosshair");
    }
    if (!g_cfg.tracer) return;
    KeepCallHook(kShotLosCall, reinterpret_cast<void*>(&ShotLosStub), g_chainedShotLos, "tracer (aimed)");
    KeepCallHook(kHipLosCall, reinterpret_cast<void*>(&HipLosStub), g_chainedHipLos, "tracer (hipfire)");
}

DWORD WINAPI InitThread(LPVOID) {
    // Let samp.dll (if any) apply its patches first so redirects of these calls get chained.
    for (int i = 0; i < 300 && !GetModuleHandleA("samp.dll"); ++i) Sleep(100);
    Sleep(2000);
    if (uintptr_t prev = RedirectCall(kCamTargetCall, reinterpret_cast<void*>(&CamTargetStub), "shot alignment")) {
        g_chainedCamTarget = reinterpret_cast<CamTargetFn>(prev);
        g_shotHookInstalled = true;
    }
    if (uintptr_t prev = RedirectCall(kHudDrawCall, reinterpret_cast<void*>(&HudDrawHook), "marker")) {
        g_hudDraw = prev;
    }
    if (uintptr_t prev = RedirectCall(kCrosshairCall, reinterpret_cast<void*>(&CrosshairHook), "crosshair")) {
        g_drawCrosshairs = prev;
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_log = _fsopen("aimfix.log", "w", _SH_DENYNO);  // readable while the game runs
        Log("aimfix.asi loaded");
        LoadSettings();
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
