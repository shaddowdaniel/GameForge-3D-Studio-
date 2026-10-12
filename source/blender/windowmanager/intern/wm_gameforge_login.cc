/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * GameForge account login screen (`WM_OT_gameforge_login`).
 *
 * Shown on startup instead of the splash until the user signs in with the atomgameforge.com
 * account and their plan includes the GameForge Studio. The popup shell lives here, drawn like the
 * splash screen (the splash image as its banner); its contents come from the `WM_MT_gameforge_login`
 * menu and the session from the `gameforge_account` Python module, which talks to the portal.
 *
 * The popup is the gate: `scripts/startup/bl_operators/gameforge_login.py` reopens it whenever it
 * is dismissed while the session is not authorized, and closes it once it is.
 */

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_datafiles.h"
#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

#include "wm.hh"

namespace blender {

static constexpr const char *LOGIN_BLOCK_NAME = "gameforge_login";

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/* The live popup. Valid only while #wm_gameforge_login_is_open is true: every (re)build of the
 * popup updates them, and the block name is unique, so an open popup is always the latest build. */
static ui::Block *g_login_block = nullptr;
static ARegion *g_login_region = nullptr;

static bool wm_gameforge_login_is_open(bContext *C)
{
  wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    return false;
  }
  const bScreen *screen = WM_window_get_active_screen(win);
  return screen && ui::popup_block_name_exists(screen, LOGIN_BLOCK_NAME);
}

static ImBuf *wm_gameforge_login_banner(int width, int *r_height)
{
  *r_height = 0;
#ifndef WITH_HEADLESS
  /* The splash image carries the GameForge branding, so the login and the splash look alike. */
  ImBuf *ibuf = IMB_load_image_from_memory(reinterpret_cast<const uchar *>(datatoc_splash_png),
                                           datatoc_splash_png_size,
                                           ImBufFlags::ByteData,
                                           "<gameforge login>");
  if (ibuf == nullptr) {
    return nullptr;
  }
  ibuf->color_mode = ImColorMode::RGBA;
  const int height = (width * ibuf->y) / ibuf->x;
  if (width != ibuf->x || height != ibuf->y) {
    IMB_scale(ibuf, width, height, IMBScaleFilter::Box, false);
  }
  IMB_premultiply_alpha(ibuf);
  *r_height = height;
  return ibuf;
#else
  UNUSED_VARS(width);
  return nullptr;
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Login Popup
 * \{ */

static ui::Block *wm_block_gameforge_login_create(bContext *C, ARegion *region, void * /*arg*/)
{
  const uiStyle *style = ui::style_get_dpi();

  ui::Block *block = block_begin(C, region, LOGIN_BLOCK_NAME, ui::EmbossType::Emboss);
  g_login_block = block;
  g_login_region = region;

  /* Same flags as the splash: stays open while interacting with its buttons, never clipped by the
   * window (the OS window size may still be settling on startup). */
  block_flag_enable(block, ui::BLOCK_LOOP | ui::BLOCK_KEEP_OPEN | ui::BLOCK_NO_WIN_CLIP);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);

  int width = style->widget.points * 32 * UI_SCALE_FAC;
  CLAMP_MAX(width, WM_window_native_pixel_x(CTX_wm_window(C)) * 0.7f);

  int banner_height = 0;
  if (ImBuf *ibuf = wm_gameforge_login_banner(width, &banner_height)) {
    uiDefButImage(block, ibuf, 0, 0.5f * U.widget_unit, width, banner_height, nullptr);
  }

  const int margin_x = UI_SCALE_FAC * 26;
  ui::Layout &layout = ui::block_layout(block,
                                        ui::LayoutDirection::Vertical,
                                        ui::LayoutType::Panel,
                                        margin_x,
                                        0,
                                        width - (margin_x * 2),
                                        UI_SCALE_FAC * 110,
                                        0,
                                        style);

  if (MenuType *mt = WM_menutype_find("WM_MT_gameforge_login", true)) {
    ui::menutype_draw(C, mt, &layout);
  }

  block_bounds_set_centered(block, 0);
  return block;
}

static wmOperatorStatus wm_gameforge_login_invoke(bContext *C,
                                                  wmOperator * /*op*/,
                                                  const wmEvent * /*event*/)
{
  /* Refreshable: the contents follow the session (code shown, waiting, plan not included...). */
  ui::popup_block_invoke_ex(C, wm_block_gameforge_login_create, nullptr, nullptr, true);
  return OPERATOR_FINISHED;
}

static bool wm_gameforge_login_poll(bContext *C)
{
  return WM_operator_winactive(C) && !wm_gameforge_login_is_open(C);
}

void WM_OT_gameforge_login(wmOperatorType *ot)
{
  ot->name = "GameForge Login";
  ot->idname = "WM_OT_gameforge_login";
  ot->description = "Sign in with your atomgameforge.com account";

  ot->invoke = wm_gameforge_login_invoke;
  ot->poll = wm_gameforge_login_poll;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Refresh & Close
 *
 * Used by the session timer: redraw the popup when the session changes, close it once signed in.
 * \{ */

static bool wm_gameforge_login_open_poll(bContext *C)
{
  return wm_gameforge_login_is_open(C);
}

static wmOperatorStatus wm_gameforge_login_refresh_exec(bContext *C, wmOperator * /*op*/)
{
  if (wm_gameforge_login_is_open(C) && g_login_region) {
    ED_region_tag_refresh_ui(g_login_region);
    ED_region_tag_redraw(g_login_region);
  }
  return OPERATOR_FINISHED;
}

void WM_OT_gameforge_login_refresh(wmOperatorType *ot)
{
  ot->name = "Refresh GameForge Login";
  ot->idname = "WM_OT_gameforge_login_refresh";
  ot->description = "Redraw the login screen with the current account state";

  ot->exec = wm_gameforge_login_refresh_exec;
  ot->poll = wm_gameforge_login_open_poll;
  ot->flag = OPTYPE_INTERNAL;
}

static wmOperatorStatus wm_gameforge_login_close_exec(bContext *C, wmOperator * /*op*/)
{
  if (wm_gameforge_login_is_open(C) && g_login_block) {
    ui::popup_block_close(C, CTX_wm_window(C), g_login_block);
  }
  g_login_block = nullptr;
  g_login_region = nullptr;
  return OPERATOR_FINISHED;
}

void WM_OT_gameforge_login_close(wmOperatorType *ot)
{
  ot->name = "Close GameForge Login";
  ot->idname = "WM_OT_gameforge_login_close";
  ot->description = "Close the login screen";

  ot->exec = wm_gameforge_login_close_exec;
  ot->poll = wm_gameforge_login_open_poll;
  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Startup
 * \{ */

bool wm_gameforge_login_required(bContext *C)
{
#ifdef WITH_PYTHON
  const char *imports[] = {"gameforge_account", nullptr};
  intptr_t value = 0;
  if (BPY_run_string_as_intptr(C,
                               imports,
                               "gameforge_account.is_required() and "
                               "not gameforge_account.get().state.authorized",
                               nullptr,
                               &value))
  {
    return value != 0;
  }
  /* The account module failed to load: fail closed, the login screen explains what happened. */
  return true;
#else
  UNUSED_VARS(C);
  return false;
#endif
}

/** \} */

}  // namespace blender
