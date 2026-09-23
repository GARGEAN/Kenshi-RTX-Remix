// V718 bounded pure-candidate oracle. Production arithmetic is checked against
// this block by the offline source-equivalence fixture. Never runs on normal launch.
namespace ogre_candidates {
  struct Candidates {
    std::array<Matrix4, 8> views, projections;
    uint32_t count = 0;
  };
  static Candidates collect(const Matrix4& combined, float viewportAspect, bool prune) {
    Candidates out;
    const float signs[] = { -1.0f, 1.0f };
              for (const float depthSign : signs) {
                Matrix4 view(0.0f);
                for (uint32_t column = 0; column < 4; ++column)
                  view[column][2] = combined[column][3] / depthSign;

                for (const float xSign : signs) {
                  if (prune && xSign < 0.0f) continue;
                  const float xOffCentre = combined[0][0] * view[0][2]
                                         + combined[1][0] * view[1][2]
                                         + combined[2][0] * view[2][2];
                  const float x0 = combined[0][0] - xOffCentre * view[0][2];
                  const float x1 = combined[1][0] - xOffCentre * view[1][2];
                  const float x2 = combined[2][0] - xOffCentre * view[2][2];
                  const float xScaleMagnitude = std::sqrt(x0 * x0 + x1 * x1 + x2 * x2);
                  if (!std::isfinite(xScaleMagnitude) || xScaleMagnitude < 1.0e-5f)
                    continue;
                  const float xScale = xSign * xScaleMagnitude;
                  for (uint32_t column = 0; column < 4; ++column)
                    view[column][0] = (combined[column][0]
                                     - xOffCentre * view[column][2]) / xScale;

                  for (const float ySign : signs) {
                    if (prune && ySign < 0.0f) continue;
                    const float yOffCentre = combined[0][1] * view[0][2]
                                           + combined[1][1] * view[1][2]
                                           + combined[2][1] * view[2][2];
                    const float y0 = combined[0][1] - yOffCentre * view[0][2];
                    const float y1 = combined[1][1] - yOffCentre * view[1][2];
                    const float y2 = combined[2][1] - yOffCentre * view[2][2];
                    const float yScaleMagnitude = std::sqrt(y0 * y0 + y1 * y1 + y2 * y2);
                    if (!std::isfinite(yScaleMagnitude) || yScaleMagnitude < 1.0e-5f)
                      continue;
                    const float yScale = ySign * yScaleMagnitude;
                    for (uint32_t column = 0; column < 4; ++column)
                      view[column][1] = (combined[column][1]
                                       - yOffCentre * view[column][2]) / yScale;
                    view[3][3] = 1.0f;

                    if (!isViewMatrix(view) || determinant(view) < 0.75)
                      continue;

                    const float depthScale = combined[0][2] * view[0][2]
                                           + combined[1][2] * view[1][2]
                                           + combined[2][2] * view[2][2];
                    const float depthOffset = combined[3][2] - depthScale * view[3][2];
                    Matrix4 projection(0.0f);
                    projection[0][0] = xScale;
                    projection[1][1] = yScale;
                    projection[2][0] = xOffCentre;
                    projection[2][1] = yOffCentre;
                    projection[2][2] = depthScale;
                    projection[2][3] = depthSign;
                    projection[3][2] = depthOffset;
                    if (classifyPerspective(projection) == 0)
                      continue;

                    if (!isSaneKenshiOgreProjection(projection, viewportAspect)) continue;

                    const Matrix4 recomposed = projection * view;
                    float maxReference = 1.0f;
                    float maxDifference = 0.0f;
                    for (uint32_t column = 0; column < 4; ++column) {
                      for (uint32_t row = 0; row < 4; ++row) {
                        maxReference = std::max(maxReference, std::abs(combined[column][row]));
                        maxDifference = std::max(maxDifference,
                          std::abs(recomposed[column][row] - combined[column][row]));
                      }
                    }
                    if (maxDifference > 0.002f * maxReference)
                      continue;

                    out.views[out.count] = view; out.projections[out.count] = projection; ++out.count;
                  }
                }
              }
    return out;
  }
  static bool equal(const Candidates& a, const Candidates& b) {
    if (a.count != b.count) return false;
    for (uint32_t i = 0; i < a.count; ++i)
      if (std::memcmp(&a.views[i], &b.views[i], sizeof(Matrix4)) != 0
        || std::memcmp(&a.projections[i], &b.projections[i], sizeof(Matrix4)) != 0) return false;
    return true;
  }
  static void verify(const Matrix4& combined, float aspect) {
    if (!kenshi_telemetry::enabled() || !transform_cpu::signRemaining) return;
    --transform_cpu::signRemaining; ++transform_cpu::signVerified;
    const auto original = collect(combined, aspect, false);
    const auto pruned = collect(combined, aspect, true);
    transform_cpu::signViable += original.count;
    if (!equal(original, pruned)) {
      ++transform_cpu::signMismatches; transform_cpu::valid = false;
      Logger::err("[TransformCPU V718] sign candidate mismatch; baseline retained");
    }
  }
  struct Accounting {
    bool viable = false;
    Accounting() { terrain_profile::count(terrain_profile::OgreCandidates); }
    ~Accounting() { if (!viable) terrain_profile::count(terrain_profile::OgreRejected); }
  };
}
