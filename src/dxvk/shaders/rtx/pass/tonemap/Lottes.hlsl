// Lottes 2016 Global Tonemapping Operator
//
// Based on the glTF-Compressonator implementation:
// https://github.com/KhronosGroup/glTF-Compressonator/blob/master/Compressonator/Applications/_Plugins/C3DModel_viewers/glTF_DX12_EX/DX12Util/shaders/Tonemapping.hlsl
//
// Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2004-2006 ATI Technologies Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef LOTTES_HLSL
#define LOTTES_HLSL

float lottesColToneB(float hdrMax, float contrast, float shoulder, float midIn, float midOut)
{
  return
    -((-pow(midIn, contrast) + (midOut * (pow(hdrMax, contrast * shoulder) * pow(midIn, contrast) -
      pow(hdrMax, contrast) * pow(midIn, contrast * shoulder) * midOut)) /
      (pow(hdrMax, contrast * shoulder) * midOut - pow(midIn, contrast * shoulder) * midOut)) /
      (pow(midIn, contrast * shoulder) * midOut));
}

float lottesColToneC(float hdrMax, float contrast, float shoulder, float midIn, float midOut)
{
  return (pow(hdrMax, contrast * shoulder) * pow(midIn, contrast) -
    pow(hdrMax, contrast) * pow(midIn, contrast * shoulder) * midOut) /
    (pow(hdrMax, contrast * shoulder) * midOut - pow(midIn, contrast * shoulder) * midOut);
}

float lottesColTone(float x, float4 p)
{
  float z = pow(x, p.x);
  return z / (pow(z, p.y) * p.z + p.w);
}

float3 LottesToneMapping(float3 color, float hdrMax, float contrast, float shoulder, float midIn, float midOut)
{
  float b = lottesColToneB(hdrMax, contrast, shoulder, midIn, midOut);
  float c = lottesColToneC(hdrMax, contrast, shoulder, midIn, midOut);

  float peak = max(color.x, max(color.y, color.z));
  if (peak <= 0.0)
    return float3(0.0, 0.0, 0.0);

  float3 ratio = color / peak;
  peak = lottesColTone(peak, float4(contrast, shoulder, b, c));

  float crosstalk = 4.0;
  float saturation = contrast;
  float crossSaturation = contrast * 16.0;

  float white = 1.0;

  float saturationCrossSaturation = saturation / crossSaturation;
  ratio = pow(abs(ratio), float3(saturationCrossSaturation, saturationCrossSaturation, saturationCrossSaturation));
  ratio = lerp(ratio, float3(white, white, white), pow(peak, crosstalk));
  ratio = pow(abs(ratio), float3(crossSaturation, crossSaturation, crossSaturation));

  color = peak * ratio;
  return color;
}

#endif // LOTTES_HLSL
