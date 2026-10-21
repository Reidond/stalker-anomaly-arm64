// Controller feedback model: rumble events, light bar colour, adaptive trigger profiles.
//
// This part is pure logic. The engine adapter tells it what happened (hit, shot, blowout, health
// changed, weapon equipped) and it computes what the pad should do right now. An IFeedbackSink
// (SDL for rumble and light bar, native GameController for adaptive triggers) carries it out.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "GameInput/Config.h"
#include "GameInput/Types.h"

namespace gameinput
{
struct Rgb
{
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
};

// ---- Weapons and adaptive triggers ------------------------------------------------------------

enum class WeaponClass : uint8_t
{
    None,        // hands empty or an item
    Melee,
    Pistol,
    Smg,
    AssaultRifle,
    Lmg,
    Shotgun,
    Sniper,
    Launcher,
    Throwable,
};
const char* WeaponClassName(WeaponClass cls);

// Guesses the class from an Anomaly weapon section name ("wpn_ak74", "wpn_toz34"). Unknown
// sections fall back to AssaultRifle; callers with better data (the section's ini fields) can
// pass the class directly instead.
WeaponClass ClassifyWeapon(std::string_view section);

// One adaptive trigger setting, mirroring GCDualSenseAdaptiveTrigger's modes. Positions and
// strengths are 0..1 (0 = trigger at rest / no force).
struct TriggerEffect
{
    enum class Type : uint8_t
    {
        Off,
        Feedback,  // resistance from `start` to the end of travel
        Weapon,    // resistance between `start` and `end`, then a sharp release (gun-trigger click)
        Vibration, // buzzing from `start`
        Slope,     // resistance ramping from `strength` at `start` to `end_strength` at `end`
    };
    Type type = Type::Off;
    float start = 0.0f;
    float end = 0.0f;
    float strength = 0.0f;
    float end_strength = 0.0f;
    float amplitude = 0.0f;
    float frequency = 0.0f;

    bool operator==(const TriggerEffect& o) const;
    bool operator!=(const TriggerEffect& o) const { return !(*this == o); }

    static TriggerEffect MakeOff() { return {}; }
    static TriggerEffect MakeFeedback(float start, float strength);
    static TriggerEffect MakeWeapon(float start, float end, float strength);
    static TriggerEffect MakeVibration(float start, float amplitude, float frequency);
    static TriggerEffect MakeSlope(float start, float end, float strength, float end_strength);
};
const char* TriggerEffectName(const TriggerEffect& effect);

struct TriggerPair
{
    TriggerEffect left;  // L2: aim
    TriggerEffect right; // R2: fire
    bool operator==(const TriggerPair& o) const { return left == o.left && right == o.right; }
    bool operator!=(const TriggerPair& o) const { return !(*this == o); }
};

// Everything the trigger choice depends on; the adapter updates it as the game state changes.
struct WeaponState
{
    WeaponClass weapon = WeaponClass::None;
    bool automatic = false; // current fire mode is full-auto (or burst)
    bool empty = false;     // magazine empty: the trigger goes limp
    bool jammed = false;
    bool reloading = false;
    bool aiming = false;    // L2 aim active
    bool firing = false;    // R2 held and the weapon is actually shooting
};

// The static "at rest" profile for a weapon class: the R2 pull feel with no shot in progress.
TriggerEffect RestingFireEffect(WeaponClass cls);
TriggerPair SelectTriggerEffects(const WeaponState& state);

// ---- Rumble -----------------------------------------------------------------------------------

enum class HitType : uint8_t { Bullet, Melee, Explosion, Fall, Fire, Shock, Chemical, Radiation, Psy };

struct MotorState
{
    float low = 0.0f;           // heavy motor, 0..1
    float high = 0.0f;          // light motor
    float left_trigger = 0.0f;  // trigger motors (Xbox-style impulse triggers; DualSense via GC haptics)
    float right_trigger = 0.0f;
    bool operator==(const MotorState& o) const
    {
        return low == o.low && high == o.high && left_trigger == o.left_trigger && right_trigger == o.right_trigger;
    }
};

// Overlapping short rumble effects plus a continuous blowout rumble. Effects combine by taking
// the per-motor maximum, so a hit during a blowout is felt on top of it instead of being added up
// into a constant buzz.
class RumbleMixer
{
public:
    void PlayHit(HitType type, float severity);   // severity 0..1: fraction of max health lost
    void PlayShot(WeaponClass cls);
    void PlayDryFire();
    void PlayLand(float severity);
    void SetBlowout(float intensity);             // 0 = off; the long rolling rumble of an emission
    void Clear();
    MotorState Update(float dt);
    bool Active() const { return !effects_.empty() || blowout_ > 0.0f; }

private:
    struct Effect
    {
        float duration = 0.1f;
        float elapsed = 0.0f;
        float low = 0.0f, high = 0.0f, left = 0.0f, right = 0.0f;
        float pulse_hz = 0.0f; // >0: amplitude is gated by a square-ish wave (shock, burn)
        float decay = 1.5f;    // envelope exponent: higher = quicker fade
    };
    void Add(const Effect& e);

    std::vector<Effect> effects_;
    float blowout_ = 0.0f;
    float blowout_phase_ = 0.0f;
};

// ---- Light bar --------------------------------------------------------------------------------

// Colour for a health fraction: green -> yellow -> red.
Rgb HealthColor(float health01);

class LightbarModel
{
public:
    void SetHealth(float health01) { health_ = health01 < 0 ? 0 : (health01 > 1 ? 1 : health01); }
    void SetBleeding(bool b) { bleeding_ = b; }
    void SetRadiation(float level01) { radiation_ = level01; }
    void SetBlowout(float intensity01) { blowout_ = intensity01; }
    void SetMenu(bool in_menu) { menu_ = in_menu; }
    void FlashDamage(float severity01);
    Rgb Update(float dt);

private:
    float health_ = 1.0f;
    bool bleeding_ = false;
    float radiation_ = 0.0f;
    float blowout_ = 0.0f;
    bool menu_ = false;
    float flash_ = 0.0f;
    float time_ = 0.0f;
};

// ---- Orchestration ----------------------------------------------------------------------------

struct FeedbackOutput
{
    MotorState motors;
    Rgb led;
    TriggerPair triggers;
};

class Feedback
{
public:
    // Engine -> feedback events.
    void SetHealth(float health01) { lightbar_.SetHealth(health01); health_ = health01; }
    void SetBleeding(bool bleeding) { lightbar_.SetBleeding(bleeding); }
    void SetRadiation(float level01) { lightbar_.SetRadiation(level01); }
    void SetBlowout(float intensity01);
    void SetMenu(bool in_menu) { lightbar_.SetMenu(in_menu); }
    void OnHit(HitType type, float severity01);
    void OnShot();
    void OnDryFire();
    void SetWeapon(WeaponClass cls, bool automatic);
    void SetWeaponStatus(bool empty, bool jammed, bool reloading);
    void SetAiming(bool aiming) { weapon_.aiming = aiming; }
    // Set by the input layer: R2 is down and the weapon can shoot.
    void SetFiring(bool firing) { weapon_.firing = firing; }

    void Update(float dt, const FeedbackConfig& cfg);
    // Silence everything (pause, disconnect, quit).
    void Reset();

    const FeedbackOutput& Current() const { return out_; }
    const WeaponState& Weapon() const { return weapon_; }
    // Direct triggers for diagnostics (lab "test" buttons).
    RumbleMixer& Rumble() { return rumble_; }

private:
    RumbleMixer rumble_;
    LightbarModel lightbar_;
    WeaponState weapon_;
    float health_ = 1.0f;
    FeedbackOutput out_;
};

// ---- Output ------------------------------------------------------------------------------------

class IFeedbackSink
{
public:
    virtual ~IFeedbackSink() = default;
    virtual void SetRumble(const MotorState& motors) = 0;
    virtual void SetLed(Rgb color) = 0;
    virtual void SetTriggers(const TriggerPair& triggers) = 0;
    // Stop all rumble and trigger effects.
    virtual void Silence() = 0;
};

// Rate-limits and de-duplicates what reaches the sink: rumble at <= ~30 Hz and refreshed while
// non-zero, the light bar only on visible change, triggers only on change.
class FeedbackDriver
{
public:
    void Apply(IFeedbackSink& sink, const FeedbackOutput& out, float dt, const FeedbackConfig& cfg);
    // Forget what was sent, e.g. after the pad reconnects.
    void Invalidate();

private:
    float since_rumble_ = 1.0f;
    float since_led_ = 1.0f;
    MotorState last_motors_;
    Rgb last_led_;
    TriggerPair last_triggers_;
    bool sent_led_ = false;
    bool sent_triggers_ = false;
    bool rumble_on_ = false;
};
} // namespace gameinput
