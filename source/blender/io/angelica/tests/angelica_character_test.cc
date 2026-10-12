/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests for assembled characters: reading the PWOps character description, choosing weapon
 * hooks and placing a child model's hook on the parent's hook.
 */

#if __has_include("testing/testing.h")
#  include "testing/testing.h"
#else
#  include <gtest/gtest.h>
#endif

#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"

#include "../importer/angelica_import_character.hh"

namespace blender::io::angelica::tests {

/* What `buildCharacter` returns (trimmed), wrapped like `/api/studio/appearance`. */
static const char *PWOPS_CHARACTER = R"({
  "ok": true,
  "character": {
    "smdPath": "models\\players\\形象\\武侠男\\躯干\\武侠男.smd",
    "parts": [
      {"ski": "models\\players\\装备\\男\\幽冥\\男通用幽冥一级.ski", "slot": "upper"},
      {"ski": "models\\players\\装备\\男\\幽冥\\男通用幽冥一级下.ski", "slot": "lower", "fashion": true},
      "models\\players\\装备\\男\\鞋\\鞋.ski"
    ],
    "attachments": [
      {"bonPath": "models\\weapons\\刀\\刀01.bon", "parts": ["models\\weapons\\刀\\刀01.ski"], "weapon": "right"},
      {"bonPath": "models\\players\\通用装备\\翅膀\\火凤凰\\火凤凰.bon",
       "parts": ["models\\players\\通用装备\\翅膀\\火凤凰\\火凤凰.ski"],
       "stckPath": "models\\players\\通用装备\\翅膀\\火凤凰\\火凤凰.stck",
       "hh": "HH_chibang", "cc": "CC_chibang", "flymount": true},
      {"smdPath": "models\\players\\通用装备\\飞剑\\剑\\剑.smd", "parts": [], "hh": "HH_feijian", "cc": "CC_feijian", "flymount": true},
      {"parts": ["orphan.ski"]}
    ],
    "face": {
      "bonPath": "models\\face\\武侠男标准脸\\武侠男标准脸.bon",
      "parts": [{"ski": "models\\face\\武侠男标准脸\\武侠男标准脸.ski", "tint": 4294967295}],
      "shapes": []
    },
    "suiteGfx": {"gfx": "策划联入\\套装\\套装光效.gfx", "hook": "HH_Spine"}
  }
})";

TEST(angelica_character, spec_from_pwops_json)
{
  CharacterSpec spec;
  std::string error;
  ASSERT_TRUE(character_spec_from_json(PWOPS_CHARACTER, spec, &error)) << error;

  EXPECT_FALSE(spec.smd_path.empty());
  EXPECT_NE(spec.smd_path.find(".smd"), std::string::npos);
  ASSERT_EQ(spec.skins.size(), 3);

  /* The entry without a model is skipped. */
  ASSERT_EQ(spec.attachments.size(), 3);
  EXPECT_EQ(spec.attachments[0].weapon_side, "right");
  EXPECT_EQ(spec.attachments[0].skins.size(), 1);
  EXPECT_FALSE(spec.attachments[0].flymount);
  EXPECT_EQ(spec.attachments[1].hanger_hook, "HH_chibang");
  EXPECT_EQ(spec.attachments[1].child_hook, "CC_chibang");
  EXPECT_FALSE(spec.attachments[1].stck_path.empty());
  EXPECT_TRUE(spec.attachments[1].flymount);
  EXPECT_TRUE(spec.attachments[2].bon_path.empty());
  EXPECT_FALSE(spec.attachments[2].smd_path.empty());

  ASSERT_TRUE(spec.face.has_value());
  EXPECT_EQ(spec.face->skins.size(), 1);
  EXPECT_FALSE(spec.suite_gfx.empty());
}

TEST(angelica_character, spec_rejects_non_characters)
{
  CharacterSpec spec;
  std::string error;
  EXPECT_FALSE(character_spec_from_json("not json", spec, &error));
  EXPECT_FALSE(character_spec_from_json("[1, 2]", spec, &error));
  EXPECT_FALSE(character_spec_from_json(R"({"parts": []})", spec, &error));
  EXPECT_FALSE(error.empty());
}

TEST(angelica_character, weapon_hooks)
{
  std::string hh, cc;
  weapon_hooks("right", true, false, hh, cc);
  EXPECT_EQ(hh, "HH_righthandweapon");
  EXPECT_EQ(cc, "CC_weapon");
  weapon_hooks("left", true, false, hh, cc);
  EXPECT_EQ(hh, "HH_lefthandweapon");
  /* Orbiting magic items only have CC_qiu. */
  weapon_hooks("right", false, true, hh, cc);
  EXPECT_EQ(hh, "HH_qiu");
  EXPECT_EQ(cc, "CC_qiu");
  /* A left hand item is never an orbiting one. */
  weapon_hooks("left", false, true, hh, cc);
  EXPECT_EQ(hh, "HH_lefthandweapon");
}

TEST(angelica_character, attachment_root_puts_child_hook_on_parent_hook)
{
  /* Parent hook: a hand at (0.4, 0, 1.2), turned 90 degrees around Z. */
  const float4x4 parent_point = math::from_loc_rot<float4x4>(
      float3(0.4f, 0.0f, 1.2f), math::EulerXYZ(0.0f, 0.0f, float(M_PI_2)));
  /* Child hook: the grip, 0.3 above the weapon root. */
  const float4x4 child_point = math::from_location<float4x4>(float3(0.0f, 0.0f, 0.3f));

  const float4x4 root = attachment_root_matrix(parent_point, child_point);

  /* The grip ends up exactly on the hand. */
  const float4x4 grip = root * child_point;
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      EXPECT_NEAR(grip[c][r], parent_point[c][r], 1e-5f);
    }
  }
  /* And the root sits 0.3 below the hand. */
  EXPECT_NEAR(root.location().z, 0.9f, 1e-5f);
}

}  // namespace blender::io::angelica::tests
