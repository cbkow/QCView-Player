// AudioSyncServo — drift → consumption-ratio controller for the
// continuous A/V sync servo.
//
// Replaces the old bang-bang model (re-seek the audio decoder when
// |drift| > 150 ms) inside the servo band: the caller measures drift
// each update tick and this PI controller returns a consumption ratio
// in [1 - kMaxTrim, 1 + kMaxTrim] that the render-callback-side
// FractionalResampler applies when draining the decoder ring. A trim
// of ±0.2 % is ~3.5 cents of pitch — inaudible — and sample-continuous,
// so drift converges to ~0 with no clicks or gaps.
//
// The caller keeps the escape tiers (this class never seeks):
//   |drift| <= servo band (~100 ms) -> servo only (tiered authority,
//                                      see maxTrimFor)
//   band .. 1 s, sustained          -> soft re-seek (existing cooldown)
//   > 1 s                           -> immediate re-seek (discontinuity)
//
// update() runs on the UI thread at an irregular cadence (per video
// frame in video mode, ~30 Hz pump otherwise) — hence dt-aware terms.
// The returned ratio is published to the render callback through an
// atomic owned by the caller. Not a QObject; no locks.

#pragma once

namespace qcv {

class AudioSyncServo
{
public:
    // driftSeconds: master-clock target minus audio playout position
    // (positive = audio is behind, needs to consume faster).
    // dtSeconds: time since the previous update() call.
    double update(double driftSeconds, double dtSeconds)
    {
        if (dtSeconds <= 0.0 || dtSeconds > 0.5) dtSeconds = 1.0 / 30.0;

        // Integral with anti-windup: clamped to the trim range so a
        // long one-sided drift can't wind past what the output can
        // ever apply.
        m_integral += driftSeconds * dtSeconds * kI;
        m_integral  = clampTrim(m_integral, kMinTrim);

        // Tiered authority: inside ±20 ms the trim stays at the
        // inaudible ±0.2 %; beyond that the cap opens linearly to
        // ±1.5 % (~26 cents) at 100 ms so a mid-band error is pulled
        // back within a few seconds instead of being handed to the
        // re-seek tier (a hard cut). As the error shrinks the cap
        // closes again, so the correction tapers smoothly.
        const double absDrift = driftSeconds < 0.0 ? -driftSeconds
                                                   : driftSeconds;
        const double cap  = maxTrimFor(absDrift);
        const double trim = clampTrim(kP * driftSeconds + m_integral, cap);

        // Slew-limit the ratio so corrections ramp smoothly (no
        // zipper artifacts from step changes between callbacks).
        const double target = 1.0 + trim;
        double delta = target - m_ratio;
        if (delta >  kMaxSlewPerUpdate) delta =  kMaxSlewPerUpdate;
        if (delta < -kMaxSlewPerUpdate) delta = -kMaxSlewPerUpdate;
        m_ratio += delta;
        return m_ratio;
    }

    void reset()
    {
        m_integral = 0.0;
        m_ratio    = 1.0;
    }

    double ratio() const { return m_ratio; }

private:
    static double clampTrim(double t, double cap)
    {
        if (t >  cap) return  cap;
        if (t < -cap) return -cap;
        return t;
    }

    static double maxTrimFor(double absDrift)
    {
        if (absDrift <= kQuietBand) return kMinTrim;
        if (absDrift >= kFullBand)  return kMaxTrim;
        const double f = (absDrift - kQuietBand) / (kFullBand - kQuietBand);
        return kMinTrim + f * (kMaxTrim - kMinTrim);
    }

    // kP: 100 ms of drift maps to the full ±1.5 % trim; 20 ms already
    //     saturates the quiet ±0.2 % cap.
    // kI: soaks up steady-state clock skew (typical crystal mismatch
    //     is tens of ppm; the quiet cap is 2000 ppm — ample).
    static constexpr double kP                = 0.15;   // per second
    static constexpr double kI                = 0.01;   // per second^2
    static constexpr double kMinTrim          = 0.002;  // ±0.2 % (quiet)
    static constexpr double kMaxTrim          = 0.015;  // ±1.5 % (catch-up)
    static constexpr double kQuietBand        = 0.020;  // s
    static constexpr double kFullBand         = 0.100;  // s
    static constexpr double kMaxSlewPerUpdate = 0.001;

    double m_integral = 0.0;
    double m_ratio    = 1.0;
};

} // namespace qcv
