/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Layouts (grounded on `A3DTrackMan.cpp`, `A3DTrackData.h/.cpp` of Angelica 2.2 and the 1.5.5
 * client):
 *
 * `.stck`: `uint32 'STCK'`, `uint32 version`, `A3DSKLTRACKFILEHEADER { int iNumBoneTrack,
 * iAnimStart, iAnimEnd, iAnimFPS }`, then per bone track `int id` + position track + rotation
 * track. Each track: `A3DTRACKSAVEDATA { int iNumKey, iNumSegment, iFrameRate }` then
 * - version 1: `T keys[iNumKey]`, `SEGMENT segments[iNumSegment]` (16 bytes each);
 * - version 2: [quaternions: `int format` (0 = raw `x,y,z,w`, 1 = `x,y,z` only)] keys,
 *   `int iTrackLengthMs`, `int compression`, [`uint16 key_ids[iNumKey]` when compressed].
 */

#include "angelica_anim.hh"
#include "angelica_skin.hh"

#include <algorithm>
#include <cmath>

namespace blender::io::angelica {

static bool anim_fail(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
  return false;
}

/** Fill #KeyTrack::key_frames for version 1 tracks from the segment table. */
static void build_v1_frames(KeyTrack &track, const std::vector<float> &raw_keys)
{
  const int comps = track.components;
  const int key_count = track.stored_key_count;
  const float fps = float(track.frame_rate);

  if (track.segments.empty() || fps <= 0.0f) {
    track.keys = raw_keys;
    track.key_frames.resize(size_t(key_count));
    for (int i = 0; i < key_count; i++) {
      track.key_frames[size_t(i)] = float(i);
    }
    return;
  }

  track.keys.clear();
  track.key_frames.clear();
  auto add_key = [&](int key, float frame) {
    if (key < 0 || key >= key_count) {
      return;
    }
    if (!track.key_frames.empty() && frame <= track.key_frames.back()) {
      return;
    }
    track.keys.insert(track.keys.end(),
                      raw_keys.begin() + size_t(key) * size_t(comps),
                      raw_keys.begin() + size_t(key + 1) * size_t(comps));
    track.key_frames.push_back(frame);
  };

  for (const TrackSegment &seg : track.segments) {
    const float start_frame = float(seg.start_time_ms) * fps * 0.001f;
    const float end_frame = float(seg.end_time_ms) * fps * 0.001f;
    if (seg.start_key == seg.end_key) {
      /* Held key: the engine returns it for the whole segment. */
      add_key(seg.start_key, start_frame);
      const float last = std::ceil(end_frame) - 1.0f;
      if (last > start_frame) {
        add_key(seg.start_key, last);
      }
    }
    else {
      const int first = std::max(seg.start_key, 0);
      const int last = std::min(seg.end_key, key_count - 1);
      for (int k = first; k <= last; k++) {
        add_key(k, start_frame + float(k - seg.start_key));
      }
    }
  }

  if (track.key_frames.empty()) {
    track.keys = raw_keys;
    track.key_frames.resize(size_t(key_count));
    for (int i = 0; i < key_count; i++) {
      track.key_frames[size_t(i)] = float(i);
    }
  }
}

bool read_key_track(BinaryReader &reader,
                    const uint32_t track_set_version,
                    const TrackValueType type,
                    KeyTrack &r_track,
                    std::string *r_error)
{
  r_track = KeyTrack();
  const int comps = int(type);
  r_track.components = comps;
  r_track.record_begin = reader.tell();

  const int32_t num_keys = reader.i32();
  const int32_t num_segments = reader.i32();
  const int32_t frame_rate = reader.i32();
  if (reader.failed()) {
    return anim_fail(r_error, "Truncated track header");
  }
  if (num_keys < 0 || num_segments < 0) {
    return anim_fail(r_error, "Invalid track key or segment count");
  }
  if (num_keys > 0 && frame_rate <= 0) {
    return anim_fail(r_error, "Invalid track frame rate");
  }
  /* Every key takes at least 12 bytes (compressed quaternion), floats 4. */
  if (size_t(num_keys) * size_t(comps == 1 ? 4 : 12) > reader.remaining()) {
    return anim_fail(r_error, "Track key count exceeds the file size");
  }
  r_track.frame_rate = frame_rate;
  r_track.stored_key_count = num_keys;

  std::vector<float> raw_keys(size_t(num_keys) * size_t(comps));

  if (track_set_version < 2) {
    for (float &v : raw_keys) {
      v = reader.f32();
    }
    if (size_t(num_segments) * 16 > reader.remaining()) {
      return anim_fail(r_error, "Track segment count exceeds the file size");
    }
    r_track.segments.resize(size_t(num_segments));
    for (TrackSegment &seg : r_track.segments) {
      seg.start_time_ms = reader.i32();
      seg.end_time_ms = reader.i32();
      seg.start_key = reader.i32();
      seg.end_key = reader.i32();
    }
    if (reader.failed()) {
      return anim_fail(r_error, "Truncated track data");
    }
    build_v1_frames(r_track, raw_keys);
    r_track.record_end = reader.tell();
    return true;
  }

  /* Version 2. */
  if (type == TrackValueType::Quaternion) {
    const int32_t format = reader.i32();
    if (format == 0) {
      for (float &v : raw_keys) {
        v = reader.f32();
      }
    }
    else if (format == 1) {
      r_track.quaternion_without_w = true;
      for (int i = 0; i < num_keys; i++) {
        float *q = &raw_keys[size_t(i) * 4];
        q[0] = reader.f32();
        q[1] = reader.f32();
        q[2] = reader.f32();
        const float s = 1.0f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
        q[3] = s > 0.0f ? std::sqrt(s) : 0.0f;
      }
    }
    else {
      return anim_fail(r_error, "Unknown quaternion compression format " + std::to_string(format));
    }
  }
  else {
    for (float &v : raw_keys) {
      v = reader.f32();
    }
  }

  r_track.track_length_ms = reader.i32();
  r_track.compression = reader.i32();
  if (reader.failed()) {
    return anim_fail(r_error, "Truncated track data");
  }
  if (r_track.compression < 0 || r_track.compression > 2) {
    return anim_fail(r_error, "Unknown track compression " + std::to_string(r_track.compression));
  }

  r_track.keys = std::move(raw_keys);
  r_track.key_frames.resize(size_t(num_keys));
  if (r_track.compression != int32_t(TrackCompression::Raw)) {
    if (size_t(num_keys) * 2 > reader.remaining()) {
      return anim_fail(r_error, "Truncated track key ids");
    }
    for (int i = 0; i < num_keys; i++) {
      r_track.key_frames[size_t(i)] = float(reader.u16());
    }
  }
  else {
    for (int i = 0; i < num_keys; i++) {
      r_track.key_frames[size_t(i)] = float(i);
    }
  }
  if (reader.failed()) {
    return anim_fail(r_error, "Truncated track data");
  }
  r_track.record_end = reader.tell();
  return true;
}

bool read_bone_track(BinaryReader &reader,
                     const uint32_t track_set_version,
                     const int id,
                     BoneTrack &r_track,
                     std::string *r_error)
{
  r_track = BoneTrack();
  r_track.id = id;
  std::string error;
  if (!read_key_track(
          reader, track_set_version, TrackValueType::Vector3, r_track.position, &error))
  {
    return anim_fail(r_error, "Position track " + std::to_string(id) + ": " + error);
  }
  if (!read_key_track(
          reader, track_set_version, TrackValueType::Quaternion, r_track.rotation, &error))
  {
    return anim_fail(r_error, "Rotation track " + std::to_string(id) + ": " + error);
  }
  return true;
}

bool parse_track_set(ByteSpan data, TrackSet &r_track_set, std::string *r_error)
{
  r_track_set = TrackSet();
  const AFileContent content = afile_open(data);
  BinaryReader reader(content.body);
  const size_t body_offset = size_t(content.body.data() - data.data());

  const uint32_t flag = reader.u32();
  const uint32_t version = reader.u32();
  if (reader.failed()) {
    return anim_fail(r_error, "File too small for a track set header");
  }
  if (flag != TRACK_SET_IDENTIFY) {
    return anim_fail(r_error, "Not a skeleton track set (.stck) file");
  }
  if (version < 1 || version > TRACK_SET_VERSION) {
    return anim_fail(r_error, "Unsupported track set version " + std::to_string(version));
  }
  r_track_set.version = version;

  const int32_t num_tracks = reader.i32();
  r_track_set.anim_start = reader.i32();
  r_track_set.anim_end = reader.i32();
  r_track_set.anim_fps = reader.i32();
  if (reader.failed()) {
    return anim_fail(r_error, "Truncated track set header");
  }
  /* Each bone track is at least an id and two 12 byte track headers. */
  if (num_tracks < 0 || size_t(num_tracks) * 28 > reader.remaining()) {
    return anim_fail(r_error, "Invalid bone track count");
  }

  r_track_set.tracks.resize(size_t(num_tracks));
  for (int i = 0; i < num_tracks; i++) {
    const int32_t id = reader.i32();
    for (int j = 0; j < i; j++) {
      if (r_track_set.tracks[size_t(j)].id == id) {
        return anim_fail(r_error, "Duplicate bone track id " + std::to_string(id));
      }
    }
    BoneTrack &track = r_track_set.tracks[size_t(i)];
    if (!read_bone_track(reader, version, id, track, r_error)) {
      return false;
    }
    for (KeyTrack *key_track : {&track.position, &track.rotation}) {
      key_track->record_begin += body_offset;
      key_track->record_end += body_offset;
    }
  }
  return true;
}

}  // namespace blender::io::angelica
