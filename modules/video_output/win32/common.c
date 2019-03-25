/*****************************************************************************
 * common.c: Windows video output common code
 *****************************************************************************
 * Copyright (C) 2001-2009 VLC authors and VideoLAN
 *
 * Authors: Gildas Bazin <gbazin@videolan.org>
 *          Martell Malone <martellmalone@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble: This file contains the functions related to the init of the vout
 *           structure, the common display code, the screensaver, but not the
 *           events and the Window Creation (events.c)
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_vout_display.h>

#include <windows.h>
#include <assert.h>

#define COBJMACROS
#include <shobjidl.h>

#include "common.h"
#include "../video_chroma/copy.h"

#if !VLC_WINSTORE_APP
static void CommonChangeThumbnailClip(vlc_object_t *, vout_display_sys_win32_t *, bool show);

static bool GetRect(const vout_display_sys_win32_t *sys, RECT *out)
{
    if (sys->b_windowless)
        return false;
    return GetClientRect(sys->hwnd, out);
}
#else /* VLC_WINSTORE_APP */
static inline BOOL EqualRect(const RECT *r1, const RECT *r2)
{
    return r1->left == r2->left && r1->right == r2->right &&
            r1->top == r2->top && r1->bottom == r2->bottom;
}
#endif /* VLC_WINSTORE_APP */

/* */
int CommonInit(vout_display_t *vd, vout_display_sys_win32_t *sys, bool b_windowless, const vout_display_cfg_t *vdcfg)
{
    sys->hwnd      = NULL;
    sys->hvideownd = NULL;
    sys->hparent   = NULL;
    sys->hfswnd    = NULL;
    sys->rect_dest_changed = false;
    sys->b_windowless = b_windowless;
    sys->is_first_placement = true;
    sys->is_on_top        = false;

#if !defined(NDEBUG) && defined(HAVE_DXGIDEBUG_H)
    sys->dxgidebug_dll = LoadLibrary(TEXT("DXGIDEBUG.DLL"));
#endif
#if VLC_WINSTORE_APP
    memset(&sys->rect_display, 0, sizeof(sys->rect_display));
#else /* !VLC_WINSTORE_APP */
    sys->pf_GetRect = GetRect;
    SetRectEmpty(&sys->rect_parent);

    var_Create(vd, "disable-screensaver", VLC_VAR_BOOL | VLC_VAR_DOINHERIT);

    sys->vdcfg = *vdcfg;

    if (b_windowless)
        return VLC_SUCCESS;

    var_Create(vd, "video-deco", VLC_VAR_BOOL | VLC_VAR_DOINHERIT);

    /* */
    sys->event = EventThreadCreate(vd, vdcfg);
    if (!sys->event)
        return VLC_EGENERIC;

    event_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
#ifdef MODULE_NAME_IS_direct3d9
    cfg.use_desktop = sys->use_desktop;
#endif
    cfg.x      = var_InheritInteger(vd, "video-x");
    cfg.y      = var_InheritInteger(vd, "video-y");
    cfg.width  = vdcfg->display.width;
    cfg.height = vdcfg->display.height;

    event_hwnd_t hwnd;
    if (EventThreadStart(sys->event, &hwnd, &cfg))
        return VLC_EGENERIC;

    sys->parent_window = hwnd.parent_window;
    sys->hparent       = hwnd.hparent;
    sys->hwnd          = hwnd.hwnd;
    sys->hvideownd     = hwnd.hvideownd;
    sys->hfswnd        = hwnd.hfswnd;

#endif /* !VLC_WINSTORE_APP */

    return VLC_SUCCESS;
}

/*****************************************************************************
* UpdateRects: update clipping rectangles
*****************************************************************************
* This function is called when the window position or size are changed, and
* its job is to update the source and destination RECTs used to display the
* picture.
*****************************************************************************/
void UpdateRects(vout_display_t *vd, vout_display_sys_win32_t *sys, bool is_forced)
{
    const video_format_t *source = &vd->source;

    RECT  rect;
    POINT point = { 0 };

    /* */
    const vout_display_cfg_t *cfg = &sys->vdcfg;

    /* Retrieve the window size */
    if (sys->b_windowless)
    {
        rect.left   = 0;
        rect.top    = 0;
        rect.right  = vd->source.i_visible_width;
        rect.bottom = vd->source.i_visible_height;
    }
    else
    {
        if (!sys->pf_GetRect(sys, &rect))
            return;
    }

    /* If nothing changed, we can return */
    bool moved_or_resized;
#if VLC_WINSTORE_APP
    moved_or_resized = RECTWidth(rect)  != RECTWidth(sys->rect_display) ||
                       RECTHeight(rect) != RECTHeight(sys->rect_display);
    sys->rect_display = rect;
#else
    if (sys->b_windowless)
    {
        moved_or_resized = false;
    }
    else
    {
        /* Retrieve the window position */
        ClientToScreen(sys->hwnd, &point);

        OffsetRect(&rect, point.x, point.y);

        moved_or_resized = EventThreadUpdateWindowPosition(sys->event, &rect);
    }
#endif
    if (!is_forced && !moved_or_resized)
        return;

    /* Update the window position and size */
    vout_display_cfg_t place_cfg = *cfg;
    place_cfg.display.width = RECTWidth(rect);
    place_cfg.display.height = RECTHeight(rect);

#if (defined(MODULE_NAME_IS_glwin32))
    /* Reverse vertical alignment as the GL tex are Y inverted */
    if (place_cfg.align.vertical == VLC_VIDEO_ALIGN_TOP)
        place_cfg.align.vertical = VLC_VIDEO_ALIGN_BOTTOM;
    else if (place_cfg.align.vertical == VLC_VIDEO_ALIGN_BOTTOM)
        place_cfg.align.vertical = VLC_VIDEO_ALIGN_TOP;
#endif

    vout_display_place_t place;
    vout_display_PlacePicture(&place, source, &place_cfg);

#if !VLC_WINSTORE_APP
    if (!sys->b_windowless)
    {
        EventThreadUpdateSourceAndPlace(sys->event, source, &place);

        UINT swpFlags = SWP_NOCOPYBITS | SWP_NOZORDER | SWP_ASYNCWINDOWPOS;
        if (sys->is_first_placement)
        {
            swpFlags |= SWP_SHOWWINDOW;
            sys->is_first_placement = false;
        }
        SetWindowPos(sys->hvideownd, 0,
            place.x, place.y, place.width, place.height,
            swpFlags);
    }
#endif

#define rect_dest           sys->rect_dest
    RECT before_rect_dest = rect_dest;
    /* Destination image position and dimensions */
#if defined(MODULE_NAME_IS_direct3d11) && !VLC_WINSTORE_APP
    rect_dest.left = 0;
    rect_dest.right = place.width;
    rect_dest.top = 0;
    rect_dest.bottom = place.height;
#else
    rect_dest.left = point.x + place.x;
    rect_dest.right = rect_dest.left + place.width;
    rect_dest.top = point.y + place.y;
    rect_dest.bottom = rect_dest.top + place.height;
#endif

    /* Signal the change in size/position */
    if (!EqualRect(&before_rect_dest, &rect_dest))
        sys->rect_dest_changed |= true;

#ifndef NDEBUG
    msg_Dbg(vd, "DirectXUpdateRects source"
        " offset: %i,%i visible: %ix%i decoded: %ix%i",
        source->i_x_offset, source->i_y_offset,
        source->i_visible_width, source->i_visible_height,
        source->i_width, source->i_height);
    msg_Dbg(vd, "DirectXUpdateRects image_dst"
        " coords: %li,%li,%li,%li",
        rect_dest.left, rect_dest.top,
        rect_dest.right, rect_dest.bottom);
#endif
#undef rect_dest

#if !VLC_WINSTORE_APP
    CommonChangeThumbnailClip(VLC_OBJECT(vd), sys, true);
#endif
}

#if !VLC_WINSTORE_APP
/* */
void CommonClean(vlc_object_t *obj, vout_display_sys_win32_t *sys)
{
    if (sys->event) {
        CommonChangeThumbnailClip(obj, sys, false);
        EventThreadStop(sys->event);
        EventThreadDestroy(sys->event);
    }
}

void CommonManage(vout_display_t *vd, vout_display_sys_win32_t *sys)
{
    if (sys->b_windowless)
        return;

    /* We used to call the Win32 PeekMessage function here to read the window
     * messages. But since window can stay blocked into this function for a
     * long time (for example when you move your window on the screen), I
     * decided to isolate PeekMessage in another thread. */
    /* If we do not control our window, we check for geometry changes
     * ourselves because the parent might not send us its events. */
    if (sys->hparent) {
        RECT rect_parent;
        POINT point;

        /* Check if the parent window has resized or moved */
        GetClientRect(sys->hparent, &rect_parent);
        point.x = point.y = 0;
        ClientToScreen(sys->hparent, &point);
        OffsetRect(&rect_parent, point.x, point.y);

        if (!EqualRect(&rect_parent, &sys->rect_parent)) {
            sys->rect_parent = rect_parent;

            /* This code deals with both resize and move
             *
             * For most drivers(direct3d9, gdi, opengl), move is never
             * an issue. The surface automatically gets moved together
             * with the associated window (hvideownd)
             */
            SetWindowPos(sys->hwnd, 0, 0, 0,
                         RECTWidth(rect_parent),
                         RECTHeight(rect_parent),
                         SWP_NOZORDER);

            UpdateRects(vd, sys, true);
        }
    }

    /* HasMoved means here resize or move */
    if (EventThreadGetAndResetHasMoved(sys->event))
        UpdateRects(vd, sys, false);
}

/* */
static void CommonChangeThumbnailClip(vlc_object_t *obj, vout_display_sys_win32_t *sys, bool show)
{
    /* Windows 7 taskbar thumbnail code */
    OSVERSIONINFO winVer;
    winVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    if (!GetVersionEx(&winVer) || winVer.dwMajorVersion <= 5)
        return;

    if( FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) )
        vlc_assert_unreachable();

    void *ptr;
    if (S_OK == CoCreateInstance(&CLSID_TaskbarList,
                                 NULL, CLSCTX_INPROC_SERVER,
                                 &IID_ITaskbarList3,
                                 &ptr)) {
        ITaskbarList3 *taskbl = ptr;
        taskbl->lpVtbl->HrInit(taskbl);

        HWND hroot = GetAncestor(sys->hwnd,GA_ROOT);
        RECT video;
        if (show) {
            GetWindowRect(sys->hparent, &video);
            POINT client = {video.left, video.top};
            if (ScreenToClient(hroot, &client))
            {
                unsigned int width = RECTWidth(video);
                unsigned int height = RECTHeight(video);
                video.left = client.x;
                video.top = client.y;
                video.right = video.left + width;
                video.bottom = video.top + height;
            }
        }
        HRESULT hr;
        hr = taskbl->lpVtbl->SetThumbnailClip(taskbl, hroot,
                                                 show ? &video : NULL);
        if ( hr != S_OK )
            msg_Err(obj, "SetThumbNailClip failed: 0x%0lx", hr);

        taskbl->lpVtbl->Release(taskbl);
    }
    CoUninitialize();
}

static int CommonControlSetFullscreen(vlc_object_t *obj, vout_display_sys_win32_t *sys, bool is_fullscreen)
{
#ifdef MODULE_NAME_IS_direct3d9
    if (sys->use_desktop && is_fullscreen)
        return VLC_EGENERIC;
#endif

    /* */
    if (sys->parent_window)
        return VLC_EGENERIC;

    if(sys->b_windowless)
        return VLC_SUCCESS;

    /* */
    HWND hwnd = sys->hparent && sys->hfswnd ? sys->hfswnd : sys->hwnd;

    /* Save the current windows placement/placement to restore
       when fullscreen is over */
    WINDOWPLACEMENT window_placement;
    window_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &window_placement);

    if (is_fullscreen) {
        msg_Dbg(obj, "entering fullscreen mode");

        /* Change window style, no borders and no title bar */
        SetWindowLong(hwnd, GWL_STYLE, WS_CLIPCHILDREN | WS_VISIBLE);

        if (sys->hparent) {
            /* Retrieve current window position so fullscreen will happen
            *on the right screen */
            HMONITOR hmon = MonitorFromWindow(sys->hparent,
                                              MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(MONITORINFO);
            if (GetMonitorInfo(hmon, &mi))
                SetWindowPos(hwnd, 0,
                             mi.rcMonitor.left,
                             mi.rcMonitor.top,
                             RECTWidth(mi.rcMonitor),
                             RECTHeight(mi.rcMonitor),
                             SWP_NOZORDER|SWP_FRAMECHANGED);
        } else {
            /* Maximize non embedded window */
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        }

        if (sys->hparent) {
            /* Hide the previous window */
            RECT rect;
            GetClientRect(hwnd, &rect);
            SetParent(sys->hwnd, hwnd);
            SetWindowPos(sys->hwnd, 0, 0, 0,
                         rect.right, rect.bottom,
                         SWP_NOZORDER|SWP_FRAMECHANGED);

            HWND topLevelParent = GetAncestor(sys->hparent, GA_ROOT);
            ShowWindow(topLevelParent, SW_HIDE);
        }
        SetForegroundWindow(hwnd);
    } else {
        msg_Dbg(obj, "leaving fullscreen mode");

        /* Change window style, no borders and no title bar */
        SetWindowLong(hwnd, GWL_STYLE, EventThreadGetWindowStyle(sys->event));

        if (sys->hparent) {
            RECT rect;
            GetClientRect(sys->hparent, &rect);
            SetParent(sys->hwnd, sys->hparent);
            SetWindowPos(sys->hwnd, 0, 0, 0,
                         rect.right, rect.bottom,
                         SWP_NOZORDER|SWP_FRAMECHANGED);

            HWND topLevelParent = GetAncestor(sys->hparent, GA_ROOT);
            ShowWindow(topLevelParent, SW_SHOW);
            SetForegroundWindow(sys->hparent);
            ShowWindow(hwnd, SW_HIDE);
        } else {
            /* return to normal window for non embedded vout */
            SetWindowPlacement(hwnd, &window_placement);
            ShowWindow(hwnd, SW_SHOWNORMAL);
        }
    }
    return VLC_SUCCESS;
}

#else /* VLC_WINSTORE_APP */

void CommonManage(vout_display_t *vd, vout_display_sys_win32_t *sys)
{
    /* just check the rendering size didn't change */
    UpdateRects(vd, sys, false);
}
#endif /* VLC_WINSTORE_APP */

int CommonControl(vout_display_t *vd, vout_display_sys_win32_t *sys, int query, va_list args)
{
    switch (query) {
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED: /* const vout_display_cfg_t *p_cfg */
    case VOUT_DISPLAY_CHANGE_ZOOM:           /* const vout_display_cfg_t *p_cfg */
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP: {
        const vout_display_cfg_t *cfg = va_arg(args, const vout_display_cfg_t *);
        sys->vdcfg = *cfg;
        UpdateRects(vd, sys, true);
        return VLC_SUCCESS;
    }
#if !VLC_WINSTORE_APP
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:   /* const vout_display_cfg_t *p_cfg */
    {   /* Update dimensions */
        const vout_display_cfg_t *cfg = va_arg(args, const vout_display_cfg_t *);
        RECT rect_window = {
            .top    = 0,
            .left   = 0,
            .right  = cfg->display.width,
            .bottom = cfg->display.height,
        };

        if (!cfg->is_fullscreen && !sys->b_windowless) {
            AdjustWindowRect(&rect_window, EventThreadGetWindowStyle(sys->event), 0);
            SetWindowPos(sys->hwnd, 0, 0, 0,
                         RECTWidth(rect_window),
                         RECTHeight(rect_window), SWP_NOMOVE);
        }
        sys->vdcfg = *cfg;
        UpdateRects(vd, sys, false);
        return VLC_SUCCESS;
    }
    case VOUT_DISPLAY_CHANGE_WINDOW_STATE: {       /* unsigned state */
        const unsigned state = va_arg(args, unsigned);
        const bool is_on_top = (state & VOUT_WINDOW_STATE_ABOVE) != 0;
#ifdef MODULE_NAME_IS_direct3d9
        if (sys->use_desktop && is_on_top)
            return VLC_EGENERIC;
#endif
        HMENU hMenu = GetSystemMenu(sys->hwnd, FALSE);

        if (is_on_top && !(GetWindowLong(sys->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            CheckMenuItem(hMenu, IDM_TOGGLE_ON_TOP, MF_BYCOMMAND | MFS_CHECKED);
            SetWindowPos(sys->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        } else if (!is_on_top && (GetWindowLong(sys->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            CheckMenuItem(hMenu, IDM_TOGGLE_ON_TOP, MF_BYCOMMAND | MFS_UNCHECKED);
            SetWindowPos(sys->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE);
        }
        sys->is_on_top = is_on_top;
        return VLC_SUCCESS;
    }
    case VOUT_DISPLAY_CHANGE_FULLSCREEN: {
        bool fs = va_arg(args, int);
        if (CommonControlSetFullscreen(VLC_OBJECT(vd), sys, fs))
            return VLC_EGENERIC;
        UpdateRects(vd, sys, false);
        return VLC_SUCCESS;
    }

    case VOUT_DISPLAY_RESET_PICTURES:
        vlc_assert_unreachable();
#endif
    default:
        return VLC_EGENERIC;
    }
}
