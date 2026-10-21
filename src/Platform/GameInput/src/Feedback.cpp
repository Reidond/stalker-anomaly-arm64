#include "GameInput/Feedback.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace gameinput
{
namespace
{
constexpr float kTwoPi = 6.28318530718f;

float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

Rgb Mix(Rgb a, Rgb b, float t)
{
    t = Clamp01(t);
    return {uint8_t(Lerp(a.r, b.r, t) + 0.5f), uint8_t(Lerp(a.g, b.g, t) + 0.5f), uint8_t(Lerp(a.b, b.b, t) + 0.5f)};
}

Rgb Scale(Rgb c, float k)
{
    k = Clamp01(k);
    return {uint8_t(c.r * k + 0.5f), uint8_t(c.g * k + 0.5f), uint8_t(c.b * k + 0.5f)};
}

// "ak74u" from "wpn_ak74u_something".
std::string WeaponToken(std::string_view section)
{
    std::string s(section);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (s.rfind("wpn_", 0) == 0)
        s.erase(0, 4);
    const size_t us = s.find('_');
    return us == std::string::npos ? s : s.substr(0, us);
}

// Names match as a prefix so variants ("toz34", "toz66", "ak74u2") share a bucket; a leading '='
// requires an exact match, for short names that would otherwise swallow others ("=pm" vs "pmm").
bool TokenIn(const std::string& token, std::initializer_list<const char*> names)
{
    for (const char* n : names)
    {
        const std::string name = n;
        if (name[0] == '=' ? token == name.substr(1) : token.rfind(name, 0) == 0)
            return true;
    }
    return false;
}
} // namespace

const char* WeaponClassName(WeaponClass cls)
{
    switch (cls)
    {
    case WeaponClass::None: return "none";
    case WeaponClass::Melee: return "melee";
    case WeaponClass::Pistol: return "pistol";
    case WeaponClass::Smg: return "smg";
    case WeaponClass::AssaultRifle: return "assault rifle";
    case WeaponClass::Lmg: return "lmg";
    case WeaponClass::Shotgun: return "shotgun";
    case WeaponClass::Sniper: return "sniper";
    case WeaponClass::Launcher: return "launcher";
    case WeaponClass::Throwable: return "throwable";
    }
    return "?";
}

WeaponClass ClassifyWeapon(std::string_view section)
{
    const std::string t = WeaponToken(section);
    if (t.empty())
        return WeaponClass::None;
    if (TokenIn(t, {"binoc", "bolt", "detector", "fake"}))
        return WeaponClass::None;
    if (TokenIn(t, {"knife", "axe", "machete"}))
        return WeaponClass::Melee;
    if (TokenIn(t, {"rpg", "rg-6", "gp25", "gp-25", "m203", "grenadelauncher"}))
        return WeaponClass::Launcher;
    if (TokenIn(t, {"grenade", "rgd5", "=f1"}))
        return WeaponClass::Throwable;
    if (TokenIn(t, {"svd", "svu", "sv98", "sv-98", "trg", "mosin", "kar98", "vintorez", "gauss", "m700", "l96", "awp",
            "barrett", "m82", "pgm", "dragunov", "vss"}))
        return WeaponClass::Sniper;
    if (TokenIn(t, {"toz", "spas", "mp133", "mp153", "protecta", "bm16", "saiga", "winchester", "wincheaster",
            "remington", "ithaca", "benelli", "ks23"}))
        return WeaponClass::Shotgun;
    if (TokenIn(t, {"pkm", "rpk", "m249", "rpd", "pkp", "pecheneg", "negev", "m60", "mg42"}))
        return WeaponClass::Lmg;
    if (TokenIn(t, {"mp5", "ak74u", "aks74u", "bizon", "uzi", "p90", "mac10", "mp7", "pp19", "kriss", "vector", "aps",
            "ppsh", "pp2000", "vityaz", "skorpion", "mp9", "ump", "tmp"}))
        return WeaponClass::Smg;
    if (TokenIn(t, {"=pm", "=pb", "=tt", "fort", "usp", "colt", "walther", "beretta", "hpsa", "sig220", "desert", "deagle",
            "mp443", "grach", "glock", "fn57", "revolver", "makarov", "browning", "cz75", "p220", "python", "=mr"}))
        return WeaponClass::Pistol;
    return WeaponClass::AssaultRifle;
}

// ---- TriggerEffect ----------------------------------------------------------------------------

bool TriggerEffect::operator==(const TriggerEffect& o) const
{
    return type == o.type && start == o.start && end == o.end && strength == o.strength &&
        end_strength == o.end_strength && amplitude == o.amplitude && frequency == o.frequency;
}

TriggerEffect TriggerEffect::MakeFeedback(float start, float strength)
{
    TriggerEffect e;
    e.type = Type::Feedback;
    e.start = start;
    e.strength = strength;
    return e;
}

TriggerEffect TriggerEffect::MakeWeapon(float start, float end, float strength)
{
    TriggerEffect e;
    e.type = Type::Weapon;
    e.start = start;
    e.end = end;
    e.strength = strength;
    return e;
}

TriggerEffect TriggerEffect::MakeVibration(float start, float amplitude, float frequency)
{
    TriggerEffect e;
    e.type = Type::Vibration;
    e.start = start;
    e.amplitude = amplitude;
    e.frequency = frequency;
    return e;
}

TriggerEffect TriggerEffect::MakeSlope(float start, float end, float strength, float end_strength)
{
    TriggerEffect e;
    e.type = Type::Slope;
    e.start = start;
    e.end = end;
    e.strength = strength;
    e.end_strength = end_strength;
    return e;
}

const char* TriggerEffectName(const TriggerEffect& e)
{
    switch (e.type)
    {
    case TriggerEffect::Type::Off: return "off";
    case TriggerEffect::Type::Feedback: return "feedback";
    case TriggerEffect::Type::Weapon: return "weapon";
    case TriggerEffect::Type::Vibration: return "vibration";
    case TriggerEffect::Type::Slope: return "slope";
    }
    return "?";
}

TriggerEffect RestingFireEffect(WeaponClass cls)
{
    using E = TriggerEffect;
    switch (cls)
    {
    case WeaponClass::Pistol: return E::MakeWeapon(0.50f, 0.65f, 0.65f);
    case WeaponClass::Smg: return E::MakeWeapon(0.42f, 0.58f, 0.45f);
    case WeaponClass::AssaultRifle: return E::MakeWeapon(0.40f, 0.62f, 0.55f);
    case WeaponClass::Lmg: return E::MakeWeapon(0.35f, 0.70f, 0.75f);
    case WeaponClass::Shotgun: return E::MakeWeapon(0.30f, 0.78f, 1.00f);
    case WeaponClass::Sniper: return E::MakeWeapon(0.55f, 0.90f, 0.90f);
    case WeaponClass::Launcher: return E::MakeWeapon(0.30f, 0.85f, 1.00f);
    case WeaponClass::Throwable: return E::MakeFeedback(0.40f, 0.30f);
    case WeaponClass::Melee: return E::MakeFeedback(0.35f, 0.20f);
    case WeaponClass::None: break;
    }
    return E::MakeOff();
}

TriggerPair SelectTriggerEffects(const WeaponState& s)
{
    using E = TriggerEffect;
    TriggerPair out;

    const bool gun = s.weapon != WeaponClass::None && s.weapon != WeaponClass::Melee &&
        s.weapon != WeaponClass::Throwable;

    // R2: the trigger.
    if (s.weapon == WeaponClass::None)
        out.right = E::MakeOff();
    else if (s.jammed)
        out.right = E::MakeFeedback(0.10f, 1.00f); // a wall: the weapon will not fire
    else if (s.empty || s.reloading)
        out.right = E::MakeOff();                   // limp: nothing happens when you pull
    else if (s.firing && s.automatic &&
        (s.weapon == WeaponClass::Smg || s.weapon == WeaponClass::AssaultRifle || s.weapon == WeaponClass::Lmg))
        out.right = E::MakeVibration(0.40f, s.weapon == WeaponClass::Lmg ? 0.80f : 0.55f, 0.55f);
    else
        out.right = RestingFireEffect(s.weapon);

    // L2: a light spring while a gun is up, firmer once aiming so the pad feels "locked in".
    if (!gun)
        out.left = E::MakeOff();
    else if (s.aiming)
        out.left = E::MakeFeedback(0.25f, 0.40f);
    else
        out.left = E::MakeFeedback(0.30f, 0.18f);
    return out;
}

// ---- RumbleMixer ------------------------------------------------------------------------------

void RumbleMixer::Add(const Effect& e)
{
    // Keep the list bounded so a long burst of hits cannot grow it without limit.
    if (effects_.size() >= 16)
        effects_.erase(effects_.begin());
    effects_.push_back(e);
}

void RumbleMixer::PlayHit(HitType type, float severity)
{
    const float s = 0.25f + 0.75f * Clamp01(severity);
    Effect e;
    switch (type)
    {
    case HitType::Bullet: e = {0.14f, 0, 0.55f * s, 0.85f * s, 0, 0, 0, 2.0f}; break;
    case HitType::Melee: e = {0.18f, 0, 0.75f * s, 0.35f * s, 0, 0, 0, 2.0f}; break;
    case HitType::Explosion: e = {0.55f, 0, 1.00f * s, 0.80f * s, 0.6f * s, 0.6f * s, 0, 1.2f}; break;
    case HitType::Fall: e = {0.22f, 0, 0.95f * s, 0.15f, 0, 0, 0, 2.0f}; break;
    case HitType::Fire: e = {0.50f, 0, 0.20f, 0.55f * s, 0, 0, 14.0f, 1.0f}; break;
    case HitType::Shock: e = {0.40f, 0, 0.20f, 1.00f * s, 0, 0, 22.0f, 1.0f}; break;
    case HitType::Chemical: e = {0.35f, 0, 0.35f, 0.35f, 0, 0, 6.0f, 1.0f}; break;
    case HitType::Radiation: e = {0.25f, 0, 0.20f * s, 0.20f * s, 0, 0, 0, 1.5f}; break;
    case HitType::Psy: e = {0.90f, 0, 0.60f * s, 0.10f, 0, 0, 3.5f, 0.8f}; break;
    }
    Add(e);
}

void RumbleMixer::PlayShot(WeaponClass cls)
{
    Effect e;
    switch (cls)
    {
    case WeaponClass::Pistol: e = {0.07f, 0, 0.25f, 0.55f, 0, 0.5f, 0, 2.0f}; break;
    case WeaponClass::Smg: e = {0.05f, 0, 0.15f, 0.40f, 0, 0.4f, 0, 2.0f}; break;
    case WeaponClass::AssaultRifle: e = {0.06f, 0, 0.30f, 0.50f, 0, 0.5f, 0, 2.0f}; break;
    case WeaponClass::Lmg: e = {0.06f, 0, 0.40f, 0.55f, 0, 0.6f, 0, 2.0f}; break;
    case WeaponClass::Shotgun: e = {0.16f, 0, 0.90f, 0.55f, 0, 0.8f, 0, 1.8f}; break;
    case WeaponClass::Sniper: e = {0.18f, 0, 0.85f, 0.50f, 0, 0.8f, 0, 1.8f}; break;
    case WeaponClass::Launcher: e = {0.30f, 0, 1.00f, 0.60f, 0, 1.0f, 0, 1.4f}; break;
    default: return;
    }
    Add(e);
}

void RumbleMixer::PlayDryFire()
{
    Add({0.04f, 0, 0.0f, 0.30f, 0, 0.2f, 0, 2.0f});
}

void RumbleMixer::PlayLand(float severity)
{
    Add({0.12f, 0, 0.60f * Clamp01(severity), 0.0f, 0, 0, 0, 2.0f});
}

void RumbleMixer::SetBlowout(float intensity)
{
    blowout_ = Clamp01(intensity);
}

void RumbleMixer::Clear()
{
    effects_.clear();
    blowout_ = 0.0f;
}

MotorState RumbleMixer::Update(float dt)
{
    MotorState m;
    for (Effect& e : effects_)
    {
        e.elapsed += dt;
        const float t = e.elapsed / e.duration;
        if (t >= 1.0f)
            continue;
        const float attack = std::min(1.0f, e.elapsed / 0.008f);
        float env = std::pow(1.0f - t, e.decay) * attack;
        if (e.pulse_hz > 0.0f)
            env *= (std::fmod(e.elapsed * e.pulse_hz, 1.0f) < 0.5f) ? 1.0f : 0.25f;
        m.low = std::max(m.low, e.low * env);
        m.high = std::max(m.high, e.high * env);
        m.left_trigger = std::max(m.left_trigger, e.left * env);
        m.right_trigger = std::max(m.right_trigger, e.right * env);
    }
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                       [](const Effect& e) { return e.elapsed >= e.duration; }),
        effects_.end());

    if (blowout_ > 0.0f)
    {
        blowout_phase_ += dt;
        const float swell = 0.5f + 0.5f * std::sin(kTwoPi * 0.9f * blowout_phase_);
        const float rattle = 0.5f + 0.5f * std::sin(kTwoPi * 7.0f * blowout_phase_);
        m.low = std::max(m.low, blowout_ * (0.30f + 0.35f * swell));
        m.high = std::max(m.high, blowout_ * 0.22f * rattle);
    }
    else
    {
        blowout_phase_ = 0.0f;
    }

    m.low = Clamp01(m.low);
    m.high = Clamp01(m.high);
    m.left_trigger = Clamp01(m.left_trigger);
    m.right_trigger = Clamp01(m.right_trigger);
    return m;
}

// ---- Light bar --------------------------------------------------------------------------------

Rgb HealthColor(float h)
{
    h = Clamp01(h);
    const Rgb green = {20, 210, 70};
    const Rgb yellow = {235, 200, 0};
    const Rgb red = {255, 25, 15};
    if (h >= 0.5f)
        return Mix(yellow, green, (h - 0.5f) / 0.5f);
    return Mix(red, yellow, h / 0.5f);
}

void LightbarModel::FlashDamage(float severity01)
{
    flash_ = std::max(flash_, 0.35f + 0.65f * Clamp01(severity01));
}

Rgb LightbarModel::Update(float dt)
{
    time_ += dt;
    flash_ = std::max(0.0f, flash_ - dt / 0.28f);

    if (menu_)
        return {70, 110, 255};

    Rgb c = HealthColor(health_);

    if (health_ < 0.25f)
    {
        // Heartbeat: two quick thumps per beat, faster the lower the health.
        const float rate = 1.0f + (0.25f - health_) * 4.0f;
        const float phase = std::fmod(time_ * rate, 1.0f);
        const float beat = std::max(std::pow(std::max(0.0f, std::sin(phase * kTwoPi * 2.0f)), 6.0f), 0.0f) *
            (phase < 0.5f ? 1.0f : 0.0f);
        c = Scale(c, 0.30f + 0.70f * beat);
    }
    if (bleeding_)
        c = Mix(c, {255, 0, 0}, 0.55f * (0.5f + 0.5f * std::sin(kTwoPi * 1.2f * time_)));
    if (radiation_ > 0.0f)
        c = Mix(c, {190, 255, 0}, Clamp01(radiation_) * (0.35f + 0.35f * std::sin(kTwoPi * 8.0f * time_)));
    if (blowout_ > 0.0f)
        c = Mix(c, {255, 120, 10}, Clamp01(blowout_) * (0.55f + 0.45f * std::sin(kTwoPi * 2.5f * time_)));
    if (flash_ > 0.0f)
        c = Mix(c, {255, 255, 255}, flash_);
    return c;
}

// ---- Feedback ---------------------------------------------------------------------------------

void Feedback::SetBlowout(float intensity01)
{
    rumble_.SetBlowout(intensity01);
    lightbar_.SetBlowout(intensity01);
}

void Feedback::OnHit(HitType type, float severity01)
{
    rumble_.PlayHit(type, severity01);
    lightbar_.FlashDamage(severity01);
}

void Feedback::OnShot()
{
    rumble_.PlayShot(weapon_.weapon);
}

void Feedback::OnDryFire()
{
    rumble_.PlayDryFire();
}

void Feedback::SetWeapon(WeaponClass cls, bool automatic)
{
    weapon_.weapon = cls;
    weapon_.automatic = automatic;
    if (cls == WeaponClass::None)
        weapon_.aiming = weapon_.firing = weapon_.empty = weapon_.jammed = weapon_.reloading = false;
}

void Feedback::SetWeaponStatus(bool empty, bool jammed, bool reloading)
{
    weapon_.empty = empty;
    weapon_.jammed = jammed;
    weapon_.reloading = reloading;
}

void Feedback::Update(float dt, const FeedbackConfig& cfg)
{
    MotorState m = rumble_.Update(dt);
    const float k = Clamp01(cfg.rumble_scale);
    m.low *= k;
    m.high *= k;
    m.left_trigger *= k;
    m.right_trigger *= k;
    out_.motors = m;

    out_.led = Scale(lightbar_.Update(dt), cfg.lightbar_brightness);
    // No weapon in your hands while a menu is open.
    out_.triggers = (cfg.trigger_effects > 0.5f && !menu_) ? SelectTriggerEffects(weapon_) : TriggerPair{};
}

void Feedback::Reset()
{
    rumble_.Clear();
    weapon_.firing = false;
    out_.motors = {};
    out_.triggers = {};
}

// ---- FeedbackDriver ---------------------------------------------------------------------------

void FeedbackDriver::Invalidate()
{
    sent_led_ = false;
    sent_triggers_ = false;
    rumble_on_ = false;
    last_motors_ = {};
}

void FeedbackDriver::Apply(IFeedbackSink& sink, const FeedbackOutput& out, float dt, const FeedbackConfig& cfg)
{
    since_rumble_ += dt;
    since_led_ += dt;

    const MotorState& m = out.motors;
    const bool nonzero = m.low > 0.01f || m.high > 0.01f || m.left_trigger > 0.01f || m.right_trigger > 0.01f;
    if (nonzero || rumble_on_)
    {
        auto moved = [](float a, float b) { return std::fabs(a - b) > 0.04f; };
        const bool changed = moved(m.low, last_motors_.low) || moved(m.high, last_motors_.high) ||
            moved(m.left_trigger, last_motors_.left_trigger) || moved(m.right_trigger, last_motors_.right_trigger);
        // The sink's rumble has a finite duration, so refresh it while something is playing.
        if ((changed && since_rumble_ >= 1.0f / 30.0f) || (nonzero && since_rumble_ >= 0.15f) || (!nonzero && rumble_on_))
        {
            sink.SetRumble(nonzero ? m : MotorState{});
            last_motors_ = nonzero ? m : MotorState{};
            rumble_on_ = nonzero;
            since_rumble_ = 0.0f;
        }
    }

    if (cfg.lightbar > 0.5f)
    {
        auto delta = [](uint8_t a, uint8_t b) { return std::abs(int(a) - int(b)); };
        const bool visible = !sent_led_ || delta(out.led.r, last_led_.r) >= 3 || delta(out.led.g, last_led_.g) >= 3 ||
            delta(out.led.b, last_led_.b) >= 3;
        if (visible && (since_led_ >= 0.05f || !sent_led_))
        {
            sink.SetLed(out.led);
            last_led_ = out.led;
            sent_led_ = true;
            since_led_ = 0.0f;
        }
    }

    if (!sent_triggers_ || out.triggers != last_triggers_)
    {
        sink.SetTriggers(out.triggers);
        last_triggers_ = out.triggers;
        sent_triggers_ = true;
    }
}
} // namespace gameinput
