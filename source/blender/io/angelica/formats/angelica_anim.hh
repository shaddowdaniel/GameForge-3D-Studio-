/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Parser for Angelica Engine skeleton track sets (`.stck`, `A3DSklTrackSet`) and the key track
 * format (`A3DTrackData<T>`) they share with old `.bon` files and morph meshes.
 *
 * Keys are bone local: the position is the bone's translation in its parent space and the
 * rotation its orientation in parent space (`x, y, z, w`, see `angelica_skin.hh` for the
 * matrix / quaternion conventions). A bone uses the track whose id is its `first_joint`.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

constexpr uint32_t TRACK_SET_IDENTIFY = fourcc('S', 'T', 'C', 'K');
/** Version 1: keys + time segments. Version 2: key frame ids and quaternion compression. */
constexpr uint32_t TRACK_SET_VERSION = 2;

/** `A3DTrackData::SEGMENT` (version 1 tracks). */
struct TrackSegment {
  int start_time_ms = 0;
  int end_time_ms = 0;
  int start_key = 0;
  /** `start_key == end_key` means the key is held for the whole segment. */
  int end_key = 0;
};

enum class TrackValueType : uint8_t {
  Float = 1,
  Vector3 = 3,
  Quaternion = 4,
};

/** `ATrackDataCompressionAlgorithm` (version 2 tracks). */
enum class TrackCompression : int32_t {
  Raw = 0,
  RemoveIdenticalKeys = 1,
  RemoveLinearKeys = 2,
};

/**
 * One animation curve with `components` floats per key (1, 3 or 4 = quaternion `x, y, z, w`).
 *
 * #keys and #key_frames are the decoded data ready for import: key `i` has the value
 * `keys[i * components ...]` at frame `key_frames[i]` (frames at #frame_rate, 0 = time 0).
 * Interpolate linearly (slerp for rotations). Version 1 hold segments get an extra copy of the
 * held key on the segment's last frame so that linear interpolation reproduces the engine.
 */
struct KeyTrack {
  int components = 0;
  int frame_rate = 0;
  std::vector<float> keys;
  std::vector<float> key_frames;

  /* Raw data as stored. */
  /** Number of keys stored in the file. */
  int stored_key_count = 0;
  /** Version 1 segments. */
  std::vector<TrackSegment> segments;
  /** Version 2: track length in milliseconds (0 = derived from the key count). */
  int track_length_ms = 0;
  /** Version 2: compression; anything but #TrackCompression::Raw stores key frame ids. */
  int32_t compression = 0;
  /** Version 2 quaternion tracks: stored without `w` (`w = sqrt(1 - x^2 - y^2 - z^2)`). */
  bool quaternion_without_w = false;

  int key_count() const
  {
    return components > 0 ? int(keys.size()) / components : 0;
  }
};

/** Position and rotation track of one joint (`A3DSklTrackSet::BONETRACK`). */
struct BoneTrack {
  /** Joint index (`A3DBone::GetFirstJoint()` of the animated bone). */
  int id = -1;
  KeyTrack position;
  KeyTrack rotation;
};

struct TrackSet {
  uint32_t version = 0;
  int anim_start = 0;
  int anim_end = 0;
  int anim_fps = 0;
  std::vector<BoneTrack> tracks;
};

/** Parse a `.stck` file (versions 1 and 2). */
bool parse_track_set(ByteSpan data, TrackSet &r_track_set, std::string *r_error);

/**
 * Read one `A3DTRACKSAVEDATA` header followed by `A3DTrackData<T>::Load` data for the given track
 * set version (version < 2 is the old segment format). Used by `.stck`, old `.bon` and `.ski`
 * morph channels.
 */
bool read_key_track(BinaryReader &reader,
                    uint32_t track_set_version,
                    TrackValueType type,
                    KeyTrack &r_track,
                    std::string *r_error);

/** `A3DSklTrackSet::LoadBoneTrack`: position track then rotation track. */
bool read_bone_track(BinaryReader &reader,
                     uint32_t track_set_version,
                     int id,
                     BoneTrack &r_track,
                     std::string *r_error);

}  // namespace blender::io::angelica
