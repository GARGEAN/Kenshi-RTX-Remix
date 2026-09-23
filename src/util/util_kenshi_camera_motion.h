#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace dxvk::kenshi_camera_motion {
using Vec = std::array<double, 3>;
inline double dot(const Vec& a, const Vec& b) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
struct OrbitResult {
  bool accepted = false;
  double radius = 0, residual = 0, directionDot = 1;
};

// For an orbit about P: C = P - r*forward. Thus camera travel is
// r*(previousForward-currentForward), even at a large zoom distance.
// Only excuse predominantly orbital motion with a forward-facing, visible
// pivot. Translation/zoom left after that fit keeps the original distance cap.
inline OrbitResult classifyOrbit(const Vec& travel, Vec previousForward,
    Vec currentForward, double distanceLimit, double farPlane) {
  OrbitResult result;
  const double travelSqr = dot(travel, travel);
  const double prevSqr = dot(previousForward, previousForward);
  const double currSqr = dot(currentForward, currentForward);
  if (!std::isfinite(travelSqr) || !std::isfinite(prevSqr) || !std::isfinite(currSqr)
      || prevSqr < 1e-12 || currSqr < 1e-12 || !std::isfinite(distanceLimit)
      || distanceLimit <= 0 || !std::isfinite(farPlane) || farPlane <= 0)
    return result;
  Vec delta;
  for (unsigned i = 0; i < 3; ++i) {
    previousForward[i] /= std::sqrt(prevSqr);
    currentForward[i] /= std::sqrt(currSqr);
    delta[i] = previousForward[i] - currentForward[i];
  }
  result.directionDot = std::clamp(dot(previousForward, currentForward), -1.0, 1.0);
  const double deltaSqr = dot(delta, delta);
  // Abrupt turns over 45 degrees and effectively unchanged directions do not
  // qualify. A pure pan must never acquire a huge artificial orbit radius.
  if (result.directionDot < 0.7071067811865476 || deltaSqr < 1e-8) return result;
  result.radius = dot(travel, delta) / deltaSqr;
  if (result.radius <= 0 || result.radius > farPlane) return result;
  Vec residual;
  for (unsigned i = 0; i < 3; ++i) residual[i] = travel[i] - result.radius*delta[i];
  result.residual = std::sqrt(dot(residual, residual));
  result.accepted = result.residual <= distanceLimit
    && result.residual <= 0.1*std::sqrt(travelSqr);
  return result;
}
}
