#pragma once

// PID controller producing a 0-100 % heater duty.
//  - derivative on measurement (no kick on setpoint steps), low-pass filtered
//  - conditional integration anti-windup
class Pid {
 public:
  void setGains(float kp, float ki, float kd) {
    kp_ = kp;
    ki_ = ki;
    kd_ = kd;
  }

  void reset(float pv) {
    i_ = 0;
    lastPv_ = pv;
    dFilt_ = 0;
    p = i = d = 0;
  }

  float update(float sp, float pv, float dt) {
    if (dt <= 0) dt = 0.001f;
    float e = sp - pv;

    float dRaw = (pv - lastPv_) / dt;
    lastPv_ = pv;
    const float alpha = dt / (dt + kDerivFilterS);
    dFilt_ += alpha * (dRaw - dFilt_);

    float pTerm = kp_ * e;
    float dTerm = -kd_ * dFilt_;
    float iCandidate = i_ + ki_ * e * dt;
    float out = pTerm + iCandidate + dTerm;
    // Only integrate if it doesn't push further into saturation.
    bool windingUp = (out > 100.0f && e > 0) || (out < 0.0f && e < 0);
    if (!windingUp) i_ = iCandidate;
    if (i_ < 0) i_ = 0;
    if (i_ > 100) i_ = 100;

    out = pTerm + i_ + dTerm;
    if (out < 0) out = 0;
    if (out > 100) out = 100;
    p = pTerm;
    i = i_;
    d = dTerm;
    return out;
  }

  // Last computed terms, for display.
  float p = 0, i = 0, d = 0;

 private:
  static constexpr float kDerivFilterS = 2.0f;
  float kp_ = 0, ki_ = 0, kd_ = 0;
  float i_ = 0;
  float lastPv_ = 0;
  float dFilt_ = 0;
};
