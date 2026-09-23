/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#pragma once

// =============================================================================
// DX11_V309_CAMERA_RESOLVER - STEP 1 of the camera replacement (SHADOW MODE).
//
// This header changes NO behaviour. It computes a second, independent opinion
// about which coordinate space a draw's vertices are in, so the two answers can
// be compared in the log before anything is switched over.
//
// WHY THIS EXISTS
// ---------------
// D3D11Rtx::ExtractTransforms is ~1900 lines containing 14 competing camera
// recovery strategies, and DrawCallTransforms::cameraRelativeView is written at
// 13 separate sites across both extraction and draw submission. The effective
// rule is therefore "last writer wins", independent of how good each writer's
// evidence was. Field logs show the failure directly: a frame logs
//   [D3D11Rtx] Camera-relative identity view CONFIRMED from coherent camera block
// while the draw that encloses the eye that same frame reports
//   [cam-obstruction] ... cameraRelative=0 objToWorldT=[0,0,0] worldToViewT=[-0,-0,-0]
// i.e. vertices that are in CAMERA space get submitted as if they were in WORLD
// space with no translation, which pins that geometry to the eye.
//
// Two design faults produce that:
//   1. A bool cannot express the four spaces a draw can actually be in, so
//      "not camera-relative" silently means three different things.
//   2. Every heuristic mutates shared state instead of proposing a candidate,
//      so the outcome depends on evaluation order rather than on evidence.
//
// This resolver fixes both: an explicit space enum, and a single pure function
// that ranks evidence and returns the best candidate plus the reason it won.
// It has no side effects and holds no state, so it is safe to call anywhere.
// =============================================================================

#include "../util/util_matrix.h"

#include <cstdint>

namespace dxvk {

  // The coordinate space a draw's vertex positions are already expressed in.
  // This is what the submitted geometry needs applied to it, and it is exactly
  // the distinction the cameraRelativeView bool cannot make:
  //
  //   Object -> needs objectToWorld, then worldToView, then viewToProjection
  //   World  -> needs               worldToView, then viewToProjection
  //   View   -> needs                            only viewToProjection
  //   Clip   -> needs nothing; already projected (screen-space / UI passes)
  enum class TransformSpace : uint32_t {
    Unknown = 0,
    Object,
    World,
    View,
    Clip,
  };

  inline const char* transformSpaceName(TransformSpace space) {
    switch (space) {
      case TransformSpace::Object:  return "object";
      case TransformSpace::World:   return "world";
      case TransformSpace::View:    return "view";
      case TransformSpace::Clip:    return "clip";
      default:                      return "unknown";
    }
  }

  // Everything the resolver is allowed to look at. Deliberately a plain value
  // struct: the resolver must not reach into D3D11Rtx state, or it becomes
  // order-dependent again - the very thing this replaces.
  struct CameraEvidence {
    Matrix4 objectToWorld;
    Matrix4 worldToView;
    Matrix4 viewToProjection;

    // The DX11 vertex-capture path recovered the rasterizer's own SV_Position
    // for this draw, so the vertices are post-transform rather than raw mesh
    // data. Which space they landed in then depends on worldAnchoredCapture.
    bool capturedPostTransform = false;

    // The post-transform capture was reconstructed back into a stable world
    // space (rather than being left in the camera's own space).
    bool worldAnchoredCapture = false;

    // A real view matrix has been positively confirmed for this frame.
    bool viewConfirmed = false;

    // The confirmed view is the camera-relative IDENTITY view: the engine has
    // already subtracted the camera origin from geometry before the vertex
    // shader, so "world space" for this engine IS camera space.
    bool viewIsCameraRelative = false;

    // The projection was synthesized from the viewport because no real game
    // projection was found; treat any conclusion drawn from it as weak.
    bool usedViewportFallbackProjection = false;

    // Draw writes no depth and blends - a composited overlay rather than
    // scene geometry. Used only to separate Clip from View.
    bool depthWriteDisabled = false;
    bool blendEnabled = false;
  };

  struct ResolvedTransform {
    TransformSpace space = TransformSpace::Unknown;

    // 0..100. Only meaningful for ranking candidates against each other; it is
    // not a probability. Anything under 50 means "guess", and once this drives
    // behaviour (step 2) a low score should fall back to the safest option
    // rather than acting on a coin flip.
    uint32_t confidence = 0;

    // Why this candidate won. Printed in the shadow log so a disagreement can
    // be traced to a specific rule instead of to "somewhere in 1900 lines".
    const char* reason = "none";
  };

  // Pure function. Rules are ordered strongest evidence first; the first match
  // wins, which makes the outcome depend on evidence quality rather than on the
  // order the caller happens to evaluate things in.
  inline ResolvedTransform resolveTransformSpace(const CameraEvidence& ev) {
    const bool identityObjectToWorld = isIdentityExact(ev.objectToWorld);
    const bool identityWorldToView   = isIdentityExact(ev.worldToView);

    // 1. Post-transform capture that was explicitly re-anchored into world
    //    space. The reconstruction is the strongest statement available: it
    //    says outright which space the vertices were put back into.
    if (ev.capturedPostTransform && ev.worldAnchoredCapture) {
      return { TransformSpace::World, 95, "post-transform capture, world-anchored" };
    }

    // 2. Post-transform capture that was NOT re-anchored. The vertices are
    //    still in the camera's space; only the projection remains to apply.
    if (ev.capturedPostTransform && !ev.worldAnchoredCapture) {
      return { TransformSpace::View, 90, "post-transform capture, not world-anchored" };
    }

    // 3. The engine renders camera-relative: world space and camera space are
    //    the same thing, so the vertices are already in view space.
    //
    //    DX11_V315_CAMERA_RELATIVE_NEEDS_IDENTITY: the view actually in use must
    //    BE identity. That is what "camera-relative identity view" means - the
    //    engine has already subtracted the camera origin before the vertex
    //    shader, so there is no view transform left to apply.
    //
    //    Requiring it is not pedantry, it is the discriminator. Field logs show
    //    Skyrim setting viewIsCameraRelative=1 (from a coherent camera block at
    //    VS slot 12) while the view genuinely in use comes from a ViewProj
    //    decomposition at VS slot 2 and is NOT identity - the two are logged in
    //    the same millisecond. Trusting the flag alone classified 628 world-space
    //    draws as view-space; the submit path then correctly refused to place
    //    them (unsafeCameraRelativeSkipped) because camera-relative geometry has
    //    to be position-captured, and the scene rendered empty.
    //
    //    So: flag AND identity => genuinely camera-relative. Flag WITHOUT
    //    identity => the flag is stale or was confirmed against the wrong
    //    constant buffer; fall through to the world-space rules below.
    if (ev.viewConfirmed && ev.viewIsCameraRelative && identityWorldToView) {
      return { TransformSpace::View, 85, "confirmed camera-relative identity view" };
    }

    // 4. Nothing to apply and nothing to place: an already-projected screen
    //    pass. Requires the compositing signature so opaque world geometry that
    //    merely lacks recovered matrices is not mistaken for UI.
    if (identityObjectToWorld && identityWorldToView
     && ev.depthWriteDisabled && ev.blendEnabled) {
      return { TransformSpace::Clip, 80, "identity transforms + no depth write + blending" };
    }

    // 5. A real, non-identity view was confirmed. The view does the placing, so
    //    the vertices it is applied to are world-space.
    if (ev.viewConfirmed && !identityWorldToView) {
      return { TransformSpace::World, 75, "confirmed non-identity view" };
    }

    // 6. A meaningful object-to-world exists, so the vertices are raw mesh data
    //    still waiting to be placed in the world.
    if (!identityObjectToWorld) {
      return { TransformSpace::Object, 65, "non-identity objectToWorld" };
    }

    // 7. Everything is identity and nothing was confirmed. A synthesized
    //    projection makes this weaker still. World is the least destructive
    //    assumption (it applies the view, which is identity here anyway), but
    //    the low score marks it as a guess.
    if (ev.usedViewportFallbackProjection) {
      return { TransformSpace::World, 20, "no evidence; viewport-fallback projection" };
    }

    return { TransformSpace::World, 35, "no evidence; defaulting to world" };
  }

  // What the CURRENT code's single bool amounts to, expressed in the new enum,
  // so the two can be compared. cameraRelativeView == true means "vertices are
  // in camera space"; false collapses Object and World together, so the object
  // matrix has to be consulted to recover which one it meant.
  inline TransformSpace legacySpaceFromCameraRelativeFlag(
      bool cameraRelativeView, const Matrix4& objectToWorld) {
    if (cameraRelativeView) {
      return TransformSpace::View;
    }
    return isIdentityExact(objectToWorld) ? TransformSpace::World : TransformSpace::Object;
  }

}
