/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Color blocks: `u16 c0, u16 c1` (RGB 5:6:5) and 2-bit indices, pixel 0 in the low bits. With
 * `c0 > c1` the palette is c0, c1, (2 c0 + c1) / 3, (c0 + 2 c1) / 3; with `c0 <= c1` (DXT1 only)
 * it is c0, c1, (c0 + c1) / 2 and transparent black.
 *
 * Endpoints are fitted along the principal axis of the block's colors, then refined by least
 * squares for the chosen indices, keeping whichever quantized pair has the lower error.
 *
 * DXT3 alpha: 4 bits per pixel. DXT5 alpha: `u8 a0, u8 a1` and 3-bit indices; `a0 > a1` gives 8
 * interpolated levels, `a0 <= a1` gives 6 levels plus 0 and 255; both are tried.
 */

#include "angelica_dds.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace blender::io::angelica {

namespace {

using Color = float[3];

uint16_t pack565(const float c[3])
{
  auto quantize = [](const float x, const int max) {
    return std::clamp(int(std::lround(std::clamp(x, 0.0f, 255.0f) * float(max) / 255.0f)), 0, max);
  };
  return uint16_t((quantize(c[0], 31) << 11) | (quantize(c[1], 63) << 5) | quantize(c[2], 31));
}

void unpack565(const uint16_t c, float r_color[3])
{
  const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  r_color[0] = float((r << 3) | (r >> 2));
  r_color[1] = float((g << 2) | (g >> 4));
  r_color[2] = float((b << 3) | (b >> 2));
}

float distance2(const float a[3], const float b[3])
{
  const float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
  return dr * dr + dg * dg + db * db;
}

/** Palette of a color block; `three` selects the 3 color + transparent mode. */
void make_palette(const uint16_t c0, const uint16_t c1, const bool three, float r_palette[4][3])
{
  unpack565(c0, r_palette[0]);
  unpack565(c1, r_palette[1]);
  for (int k = 0; k < 3; k++) {
    const float a = r_palette[0][k], b = r_palette[1][k];
    if (three) {
      r_palette[2][k] = (a + b) / 2.0f;
      r_palette[3][k] = 0.0f;
    }
    else {
      r_palette[2][k] = (2.0f * a + b) / 3.0f;
      r_palette[3][k] = (a + 2.0f * b) / 3.0f;
    }
  }
}

/** Nearest palette entry per pixel; transparent pixels get index 3. Returns the total error. */
float assign_indices(const Color pixels[16],
                     const bool opaque[16],
                     const float palette[4][3],
                     const int colors,
                     uint8_t r_indices[16])
{
  float total = 0.0f;
  for (int i = 0; i < 16; i++) {
    if (!opaque[i]) {
      r_indices[i] = 3;
      continue;
    }
    int best = 0;
    float best_error = distance2(pixels[i], palette[0]);
    for (int j = 1; j < colors; j++) {
      const float error = distance2(pixels[i], palette[j]);
      if (error < best_error) {
        best = j;
        best_error = error;
      }
    }
    r_indices[i] = uint8_t(best);
    total += best_error;
  }
  return total;
}

/** Weight of c0 for each index (c1 gets the rest). */
float index_weight(const int index, const bool three)
{
  static const float four_weights[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
  static const float three_weights[4] = {1.0f, 0.0f, 0.5f, 0.0f};
  return three ? three_weights[index] : four_weights[index];
}

struct ColorFit {
  uint16_t c0 = 0, c1 = 0;
  uint8_t indices[16] = {};
  float error = 0.0f;
};

void evaluate(const Color pixels[16],
              const bool opaque[16],
              const bool three,
              const float e0[3],
              const float e1[3],
              ColorFit &best)
{
  ColorFit fit;
  fit.c0 = pack565(e0);
  fit.c1 = pack565(e1);
  float palette[4][3];
  make_palette(fit.c0, fit.c1, three, palette);
  fit.error = assign_indices(pixels, opaque, palette, three ? 3 : 4, fit.indices);
  if (fit.error < best.error) {
    best = fit;
  }
}

/** Fit the endpoints of one color block (3 color mode when some pixels are transparent). */
ColorFit fit_colors(const Color pixels[16], const bool opaque[16], const bool three)
{
  ColorFit best;
  best.error = 1e30f;
  int count = 0;
  float mean[3] = {0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 16; i++) {
    if (opaque[i]) {
      for (int k = 0; k < 3; k++) {
        mean[k] += pixels[i][k];
      }
      count++;
    }
  }
  if (count == 0) {
    std::fill(std::begin(best.indices), std::end(best.indices), uint8_t(3));
    best.error = 0.0f;
    return best;
  }
  for (float &m : mean) {
    m /= float(count);
  }

  /* Principal axis of the colors (power iteration on the covariance). */
  float cov[6] = {};
  for (int i = 0; i < 16; i++) {
    if (!opaque[i]) {
      continue;
    }
    const float d[3] = {pixels[i][0] - mean[0], pixels[i][1] - mean[1], pixels[i][2] - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float axis[3] = {1.0f, 1.0f, 1.0f};
  for (int iteration = 0; iteration < 8; iteration++) {
    const float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
    const float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
    const float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
    const float len = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (len < 1e-6f) {
      break;
    }
    axis[0] = x / len;
    axis[1] = y / len;
    axis[2] = z / len;
  }
  /* Unit length, so projections are distances along the axis. */
  const float axis_len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  for (float &a : axis) {
    a /= axis_len;
  }
  float tmin = 1e30f, tmax = -1e30f;
  for (int i = 0; i < 16; i++) {
    if (opaque[i]) {
      const float t = (pixels[i][0] - mean[0]) * axis[0] + (pixels[i][1] - mean[1]) * axis[1] +
                      (pixels[i][2] - mean[2]) * axis[2];
      tmin = std::min(tmin, t);
      tmax = std::max(tmax, t);
    }
  }
  float e0[3], e1[3];
  for (int k = 0; k < 3; k++) {
    e0[k] = mean[k] + axis[k] * tmax;
    e1[k] = mean[k] + axis[k] * tmin;
  }
  evaluate(pixels, opaque, three, e0, e1, best);

  /* Least squares endpoints for the chosen indices, twice. */
  for (int iteration = 0; iteration < 2; iteration++) {
    float aa = 0.0f, ab = 0.0f, bb = 0.0f, ax[3] = {}, bx[3] = {};
    for (int i = 0; i < 16; i++) {
      if (!opaque[i]) {
        continue;
      }
      const float w = index_weight(best.indices[i], three), v = 1.0f - w;
      aa += w * w;
      ab += w * v;
      bb += v * v;
      for (int k = 0; k < 3; k++) {
        ax[k] += w * pixels[i][k];
        bx[k] += v * pixels[i][k];
      }
    }
    const float det = aa * bb - ab * ab;
    if (std::abs(det) < 1e-6f) {
      break;
    }
    for (int k = 0; k < 3; k++) {
      e0[k] = (ax[k] * bb - bx[k] * ab) / det;
      e1[k] = (bx[k] * aa - ax[k] * ab) / det;
    }
    const float before = best.error;
    evaluate(pixels, opaque, three, e0, e1, best);
    if (best.error >= before) {
      break;
    }
  }
  return best;
}

/**
 * The 8 bytes of a color block, endpoints ordered for the mode the decoder picks. `three`: some
 * pixels are transparent (3 color mode is required); `allow_three`: DXT1, where an opaque block may
 * also use the 3 color mode when its midpoint fits better.
 */
void encode_color_block(const Color pixels[16],
                        const bool opaque[16],
                        bool three,
                        const bool allow_three,
                        uint8_t out[8])
{
  ColorFit fit = fit_colors(pixels, opaque, three);
  if (!three && allow_three) {
    ColorFit fit3 = fit_colors(pixels, opaque, true);
    if (fit3.error < fit.error) {
      fit = fit3;
      three = true;
    }
  }
  uint16_t c0 = fit.c0, c1 = fit.c1;
  uint8_t *indices = fit.indices;
  if (three) {
    /* c0 <= c1 selects 3 colors + transparent. */
    if (c0 > c1) {
      std::swap(c0, c1);
      for (int i = 0; i < 16; i++) {
        indices[i] = indices[i] == 0 ? 1 : (indices[i] == 1 ? 0 : indices[i]);
      }
    }
  }
  else if (c0 < c1) {
    std::swap(c0, c1);
    for (int i = 0; i < 16; i++) {
      indices[i] ^= 1;
    }
  }
  else if (c0 == c1) {
    /* Equal endpoints switch the decoder to 3 colors: stay on the first one. */
    std::fill(indices, indices + 16, uint8_t(0));
  }
  uint32_t bits = 0;
  for (int i = 0; i < 16; i++) {
    bits |= uint32_t(indices[i]) << (2 * i);
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  out[4] = uint8_t(bits);
  out[5] = uint8_t(bits >> 8);
  out[6] = uint8_t(bits >> 16);
  out[7] = uint8_t(bits >> 24);
}

void encode_alpha_dxt3(const uint8_t alpha[16], uint8_t out[8])
{
  std::memset(out, 0, 8);
  for (int i = 0; i < 16; i++) {
    const int a4 = (int(alpha[i]) * 15 + 127) / 255;
    out[i / 2] |= uint8_t(a4 << (4 * (i & 1)));
  }
}

/** Indices and error of a DXT5 alpha palette. */
int fit_alpha(const uint8_t alpha[16], const int palette[8], uint8_t r_indices[16])
{
  int total = 0;
  for (int i = 0; i < 16; i++) {
    int best = 0, best_error = 1 << 30;
    for (int j = 0; j < 8; j++) {
      const int d = int(alpha[i]) - palette[j];
      if (d * d < best_error) {
        best = j;
        best_error = d * d;
      }
    }
    r_indices[i] = uint8_t(best);
    total += best_error;
  }
  return total;
}

void encode_alpha_dxt5(const uint8_t alpha[16], uint8_t out[8])
{
  int lo = 255, hi = 0, inner_lo = 255, inner_hi = 0;
  for (int i = 0; i < 16; i++) {
    lo = std::min(lo, int(alpha[i]));
    hi = std::max(hi, int(alpha[i]));
    if (alpha[i] != 0 && alpha[i] != 255) {
      inner_lo = std::min(inner_lo, int(alpha[i]));
      inner_hi = std::max(inner_hi, int(alpha[i]));
    }
  }
  /* 8 levels between the extremes (a0 > a1). */
  int a0 = hi, a1 = lo;
  int palette[8];
  uint8_t indices[16];
  if (a0 == a1) {
    std::fill(std::begin(indices), std::end(indices), uint8_t(0));
  }
  else {
    palette[0] = a0;
    palette[1] = a1;
    for (int i = 2; i < 8; i++) {
      palette[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    }
    const int error = fit_alpha(alpha, palette, indices);
    /* 6 levels between the inner values plus exact 0 and 255 (a0 <= a1). */
    if (inner_lo <= inner_hi && error > 0) {
      int palette6[8] = {inner_lo, inner_hi, 0, 0, 0, 0, 0, 255};
      for (int i = 2; i < 6; i++) {
        palette6[i] = ((6 - i) * inner_lo + (i - 1) * inner_hi) / 5;
      }
      uint8_t indices6[16];
      if (fit_alpha(alpha, palette6, indices6) < error) {
        a0 = inner_lo;
        a1 = inner_hi;
        std::copy(std::begin(indices6), std::end(indices6), std::begin(indices));
      }
    }
  }
  uint64_t bits = 0;
  for (int i = 0; i < 16; i++) {
    bits |= uint64_t(indices[i]) << (3 * i);
  }
  out[0] = uint8_t(a0);
  out[1] = uint8_t(a1);
  for (int i = 0; i < 6; i++) {
    out[2 + i] = uint8_t(bits >> (8 * i));
  }
}

}  // namespace

void compress_blocks(const BlockFormat format,
                     const int width,
                     const int height,
                     const uint8_t *rgba,
                     Bytes &r_blocks)
{
  const int blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  const size_t block_size = format == BlockFormat::DXT1 ? 8 : 16;
  const size_t start = r_blocks.size();
  r_blocks.resize(start + size_t(blocks_x) * size_t(blocks_y) * block_size);
  uint8_t *out = r_blocks.data() + start;

  for (int by = 0; by < blocks_y; by++) {
    for (int bx = 0; bx < blocks_x; bx++) {
      Color pixels[16];
      uint8_t alpha[16];
      bool opaque[16];
      bool has_transparent = false;
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
          const uint8_t *p = rgba + (size_t(sy) * size_t(width) + size_t(sx)) * 4;
          const int i = y * 4 + x;
          pixels[i][0] = p[0];
          pixels[i][1] = p[1];
          pixels[i][2] = p[2];
          alpha[i] = p[3];
          opaque[i] = format != BlockFormat::DXT1 || p[3] >= 128;
          has_transparent |= !opaque[i];
        }
      }
      if (format == BlockFormat::DXT3) {
        encode_alpha_dxt3(alpha, out);
        out += 8;
      }
      else if (format == BlockFormat::DXT5) {
        encode_alpha_dxt5(alpha, out);
        out += 8;
      }
      encode_color_block(pixels, opaque, has_transparent, format == BlockFormat::DXT1, out);
      out += 8;
    }
  }
}

bool decompress_blocks(const BlockFormat format,
                       const int width,
                       const int height,
                       const ByteSpan blocks,
                       Bytes &r_rgba)
{
  const int blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  const size_t block_size = format == BlockFormat::DXT1 ? 8 : 16;
  if (width <= 0 || height <= 0 ||
      blocks.size() < size_t(blocks_x) * size_t(blocks_y) * block_size)
  {
    return false;
  }
  r_rgba.assign(size_t(width) * size_t(height) * 4, 0);
  const uint8_t *in = blocks.data();
  for (int by = 0; by < blocks_y; by++) {
    for (int bx = 0; bx < blocks_x; bx++) {
      uint8_t alpha[16];
      std::fill(std::begin(alpha), std::end(alpha), uint8_t(255));
      if (format == BlockFormat::DXT3) {
        for (int i = 0; i < 16; i++) {
          const int a4 = (in[i / 2] >> (4 * (i & 1))) & 15;
          alpha[i] = uint8_t(a4 * 17);
        }
        in += 8;
      }
      else if (format == BlockFormat::DXT5) {
        const int a0 = in[0], a1 = in[1];
        int palette[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 2; i < 8; i++) {
            palette[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
          }
        }
        else {
          for (int i = 2; i < 6; i++) {
            palette[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
          }
          palette[6] = 0;
          palette[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) {
          bits |= uint64_t(in[2 + i]) << (8 * i);
        }
        for (int i = 0; i < 16; i++) {
          alpha[i] = uint8_t(palette[(bits >> (3 * i)) & 7]);
        }
        in += 8;
      }
      const uint16_t c0 = uint16_t(in[0] | (in[1] << 8)), c1 = uint16_t(in[2] | (in[3] << 8));
      const uint32_t indices = uint32_t(in[4]) | (uint32_t(in[5]) << 8) | (uint32_t(in[6]) << 16) |
                               (uint32_t(in[7]) << 24);
      in += 8;
      /* DXT3/DXT5 colors always use 4 colors; DXT1 picks by the endpoint order. */
      const bool three = format == BlockFormat::DXT1 && c0 <= c1;
      float palette[4][3];
      make_palette(c0, c1, three, palette);
      for (int i = 0; i < 16; i++) {
        const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= width || y >= height) {
          continue;
        }
        const int index = (indices >> (2 * i)) & 3;
        uint8_t *out = &r_rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
        for (int k = 0; k < 3; k++) {
          out[k] = uint8_t(std::lround(palette[index][k]));
        }
        out[3] = (three && index == 3) ? 0 : alpha[i];
        if (three && index == 3) {
          out[0] = out[1] = out[2] = 0;
        }
      }
    }
  }
  return true;
}

}  // namespace blender::io::angelica
