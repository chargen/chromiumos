// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/panel_manager.h"

#include "base/logging.h"
#include "window_manager/atom_cache.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/motion_event_coalescer.h"
#include "window_manager/panel.h"
#include "window_manager/panel_bar.h"
#include "window_manager/panel_dock.h"
#include "window_manager/panel_container.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

namespace window_manager {

using chromeos::NewPermanentCallback;
using std::make_pair;
using std::map;
using std::tr1::shared_ptr;
using std::vector;

// Frequency with which we should update the position of dragged panels.
static const int kDraggedPanelUpdateMs = 25;

// How long should the animation when detaching panels from containers take?
static const int kDetachPanelAnimMs = 100;

// Width of panel docks, chosen because 1280 - 256 = 1024.
static const int kPanelDockWidth = 256;

PanelManager::PanelManager(WindowManager* wm)
    : wm_(wm),
      dragged_panel_(NULL),
      dragged_panel_event_coalescer_(
          new MotionEventCoalescer(
              NewPermanentCallback(
                  this, &PanelManager::HandlePeriodicPanelDragMotion),
              kDraggedPanelUpdateMs)),
      panel_bar_(new PanelBar(this)),
      left_panel_dock_(
          new PanelDock(this, PanelDock::DOCK_TYPE_LEFT, kPanelDockWidth)),
      right_panel_dock_(
          new PanelDock(this, PanelDock::DOCK_TYPE_RIGHT, kPanelDockWidth)),
      saw_map_request_(false),
      event_consumer_registrar_(new EventConsumerRegistrar(wm_, this)) {
  event_consumer_registrar_->RegisterForChromeMessages(
      WmIpc::Message::WM_SET_PANEL_STATE);
  event_consumer_registrar_->RegisterForChromeMessages(
      WmIpc::Message::WM_NOTIFY_PANEL_DRAGGED);
  event_consumer_registrar_->RegisterForChromeMessages(
      WmIpc::Message::WM_NOTIFY_PANEL_DRAG_COMPLETE);

  RegisterContainer(panel_bar_.get());
  RegisterContainer(left_panel_dock_.get());
  RegisterContainer(right_panel_dock_.get());
}

PanelManager::~PanelManager() {
  dragged_panel_ = NULL;
}

bool PanelManager::IsInputWindow(XWindow xid) {
  return container_input_xids_.count(xid) || panel_input_xids_.count(xid);
}

bool PanelManager::HandleWindowMapRequest(Window* win) {
  saw_map_request_ = true;

  if (win->type() != WmIpc::WINDOW_TYPE_CHROME_PANEL_CONTENT &&
      win->type() != WmIpc::WINDOW_TYPE_CHROME_PANEL_TITLEBAR)
    return false;

  DoInitialSetupForWindow(win);
  win->MapClient();
  return true;
}

void PanelManager::HandleWindowMap(Window* win) {
  CHECK(win);

  if (win->type() != WmIpc::WINDOW_TYPE_CHROME_PANEL_CONTENT &&
      win->type() != WmIpc::WINDOW_TYPE_CHROME_PANEL_TITLEBAR)
    return;

  // Handle initial setup for existing windows for which we never saw a map
  // request event.
  if (!saw_map_request_)
    DoInitialSetupForWindow(win);

  switch (win->type()) {
    case WmIpc::WINDOW_TYPE_CHROME_PANEL_TITLEBAR:
      // Don't do anything with panel titlebars when they're first
      // mapped; we'll handle them after we see the corresponding content
      // window.
      break;

    case WmIpc::WINDOW_TYPE_CHROME_PANEL_CONTENT: {
      if (win->type_params().empty()) {
        LOG(WARNING) << "Panel " << win->xid_str() << " is missing type "
                     << "parameter for titlebar window";
        break;
      }
      Window* titlebar_win = wm_->GetWindow(win->type_params().at(0));
      if (!titlebar_win) {
        LOG(WARNING) << "Unable to find titlebar "
                     << XidStr(win->type_params()[0])
                     << " for panel " << win->xid_str();
        break;
      }

      // TODO(derat): Make the second param required after Chrome has been
      // updated.
      bool expanded = win->type_params().size() >= 2 ?
          win->type_params().at(1) : false;
      VLOG(1) << "Adding " << (expanded ? "expanded" : "collapsed")
              << " panel with content window " << win->xid_str()
              << " and titlebar window " << titlebar_win->xid_str();

      shared_ptr<Panel> panel(new Panel(this, win, titlebar_win, expanded));
      panel->SetTitlebarWidth(panel->content_width());

      vector<XWindow> input_windows;
      panel->GetInputWindows(&input_windows);
      for (vector<XWindow>::const_iterator it = input_windows.begin();
           it != input_windows.end(); ++it) {
        CHECK(panel_input_xids_.insert(make_pair(*it, panel.get())).second);
      }

      CHECK(panels_.insert(make_pair(win->xid(), panel)).second);
      CHECK(panels_by_titlebar_xid_.insert(
              make_pair(titlebar_win->xid(), panel.get())).second);

      AddPanelToContainer(panel.get(),
                          panel_bar_.get(),
                          PanelContainer::PANEL_SOURCE_NEW);
      break;
    }

    default:
      NOTREACHED() << "Unhandled window type " << win->type();
  }
}

void PanelManager::HandleWindowUnmap(Window* win) {
  CHECK(win);
  Panel* panel = GetPanelByWindow(*win);
  if (!panel)
    return;

  PanelContainer* container = GetContainerForPanel(*panel);
  if (container)
    RemovePanelFromContainer(panel, container);
  if (panel == dragged_panel_)
    HandlePanelDragComplete(panel, true);  // removed=true

  // If the panel was focused, assign the focus to another panel, or
  // failing that, let the window manager decide what to do with it.
  if (panel->content_win()->focused()) {
    if (!panel_bar_->TakeFocus())
      wm_->TakeFocus();
  }

  vector<XWindow> input_windows;
  panel->GetInputWindows(&input_windows);
  for (vector<XWindow>::const_iterator it = input_windows.begin();
       it != input_windows.end(); ++it) {
    CHECK(panel_input_xids_.erase(*it) == 1);
  }

  CHECK(panels_by_titlebar_xid_.erase(panel->titlebar_xid()) == 1);
  CHECK(panels_.erase(panel->content_xid()) == 1);
}

void PanelManager::HandleWindowConfigureRequest(
    Window* win, int req_x, int req_y, int req_width, int req_height) {
  Panel* panel = GetPanelByWindow(*win);
  if (!panel)
    return;
  panel->HandleWindowConfigureRequest(win, req_x, req_y, req_width, req_height);
}

void PanelManager::HandleButtonPress(XWindow xid,
                                     int x, int y,
                                     int x_root, int y_root,
                                     int button,
                                     XTime timestamp) {
  // If this is a container's input window, notify the container.
  PanelContainer* container = FindWithDefault(
      container_input_xids_, xid, static_cast<PanelContainer*>(NULL));
  if (container) {
    container->HandleInputWindowButtonPress(
        xid, x, y, x_root, y_root, button, timestamp);
    return;
  }

  // If this is a panel's input window, notify the panel.
  Panel* panel = FindWithDefault(
      panel_input_xids_, xid, static_cast<Panel*>(NULL));
  if (panel) {
    panel->HandleInputWindowButtonPress(xid, x, y, button, timestamp);
    return;
  }

  // If it's a panel's content window, notify the panel's container.
  Window* win = wm_->GetWindow(xid);
  if (win) {
    Panel* panel = GetPanelByWindow(*win);
    if (panel) {
      container = GetContainerForPanel(*panel);
      if (container)
        container->HandlePanelButtonPress(panel, button, timestamp);
      return;
    }
  }
}

void PanelManager::HandleButtonRelease(XWindow xid,
                                       int x, int y,
                                       int x_root, int y_root,
                                       int button,
                                       XTime timestamp) {
  // We only care if button releases happened in container or panel input
  // windows -- there's no current need to notify containers about button
  // releases in their panels.
  PanelContainer* container = FindWithDefault(
      container_input_xids_, xid, static_cast<PanelContainer*>(NULL));
  if (container) {
    container->HandleInputWindowButtonRelease(
        xid, x, y, x_root, y_root, button, timestamp);
    return;
  }

  Panel* panel = FindWithDefault(
      panel_input_xids_, xid, static_cast<Panel*>(NULL));
  if (panel) {
    panel->HandleInputWindowButtonRelease(xid, x, y, button, timestamp);
    return;
  }
}

void PanelManager::HandlePointerEnter(XWindow xid,
                                      int x, int y,
                                      int x_root, int y_root,
                                      XTime timestamp) {
  PanelContainer* container = FindWithDefault(
      container_input_xids_, xid, static_cast<PanelContainer*>(NULL));
  if (container) {
    container->HandleInputWindowPointerEnter(
        xid, x, y, x_root, y_root, timestamp);
    return;
  }

  // If it's a panel's titlebar window, notify the panel's container.
  Window* win = wm_->GetWindow(xid);
  if (win) {
    Panel* panel = GetPanelByWindow(*win);
    if (panel) {
      container = GetContainerForPanel(*panel);
      if (container && xid == panel->titlebar_xid())
        container->HandlePanelTitlebarPointerEnter(panel, timestamp);
      return;
    }
  }
}

void PanelManager::HandlePointerLeave(XWindow xid,
                                      int x, int y,
                                      int x_root, int y_root,
                                      XTime timestamp) {
  PanelContainer* container = FindWithDefault(
      container_input_xids_, xid, static_cast<PanelContainer*>(NULL));
  if (container)
    container->HandleInputWindowPointerLeave(
        xid, x, y, x_root, y_root, timestamp);
}

void PanelManager::HandlePointerMotion(XWindow xid,
                                       int x, int y,
                                       int x_root, int y_root,
                                       XTime timestamp) {
  Panel* panel = FindWithDefault(
      panel_input_xids_, xid, static_cast<Panel*>(NULL));
  if (panel)
    panel->HandleInputWindowPointerMotion(xid, x, y);
}

void PanelManager::HandleChromeMessage(const WmIpc::Message& msg) {
  switch (msg.type()) {
    case WmIpc::Message::WM_SET_PANEL_STATE: {
      XWindow xid = msg.param(0);
      Panel* panel = GetPanelByXid(xid);
      if (!panel) {
        LOG(WARNING) << "Ignoring WM_SET_PANEL_STATE message for non-panel "
                     << "window " << xid;
        return;
      }
      PanelContainer* container = GetContainerForPanel(*panel);
      if (container)
        container->HandleSetPanelStateMessage(panel, msg.param(1));
      break;
    }
    case WmIpc::Message::WM_NOTIFY_PANEL_DRAGGED: {
      XWindow xid = msg.param(0);
      Panel* panel = GetPanelByXid(xid);
      if (!panel) {
        LOG(WARNING) << "Ignoring WM_NOTIFY_PANEL_DRAGGED message for "
                     << "non-panel window " << XidStr(xid);
        return;
      }
      if (dragged_panel_ && panel != dragged_panel_)
        HandlePanelDragComplete(dragged_panel_, false);  // removed=false
      dragged_panel_ = panel;
      if (!dragged_panel_event_coalescer_->IsRunning())
        dragged_panel_event_coalescer_->Start();
      // We want the right edge of the panel, but pre-IPC-version-1 Chrome
      // sends us the left edge of the titlebar instead.
      int drag_x = (wm()->wm_ipc_version() >= 1) ?
                   msg.param(1) : msg.param(1) + panel->titlebar_width();
      int drag_y = msg.param(2);
      dragged_panel_event_coalescer_->StorePosition(drag_x, drag_y);
      break;
    }
    case WmIpc::Message::WM_NOTIFY_PANEL_DRAG_COMPLETE: {
      XWindow xid = msg.param(0);
      Panel* panel = GetPanelByXid(xid);
      if (!panel) {
        LOG(WARNING) << "Ignoring WM_NOTIFY_PANEL_DRAG_COMPLETE message for "
                     << "non-panel window " << XidStr(xid);
        return;
      }
      HandlePanelDragComplete(panel, false);  // removed=false
      break;
    }
    default:
      return;
  }
}

void PanelManager::HandleClientMessage(XWindow xid,
                                       XAtom message_type,
                                       const long data[5]) {
  Panel* panel = GetPanelByXid(xid);
  if (!panel)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    VLOG(1) << "Got _NET_ACTIVE_WINDOW request to focus " << XidStr(xid)
            << " (requestor says its currently-active window is "
            << XidStr(data[2]) << "; real active window is "
            << XidStr(wm_->active_window_xid()) << ")";
    PanelContainer* container = GetContainerForPanel(*panel);
    if (container)
      container->HandleFocusPanelMessage(panel);
  }
}

void PanelManager::HandleFocusChange(XWindow xid, bool focus_in) {
  Panel* panel = GetPanelByXid(xid);
  if (!panel)
    return;
  PanelContainer* container = GetContainerForPanel(*panel);
  if (container)
    container->HandlePanelFocusChange(panel, focus_in);
}

void PanelManager::HandleWindowPropertyChange(XWindow xid, XAtom xatom) {
  Panel* panel = GetPanelByXid(xid);
  DCHECK(panel) << "Got property change for non-panel window " << XidStr(xid);
  if (!panel)
    return;
  DCHECK_EQ(wm_->GetXAtom(ATOM_WM_HINTS), xatom);
  PanelContainer* container = GetContainerForPanel(*panel);
  if (container)
    container->HandlePanelUrgencyChange(panel);
}

void PanelManager::HandlePanelResize(Panel* panel) {
  DCHECK(panel);
  PanelContainer* container = GetContainerForPanel(*panel);
  if (container)
    container->HandlePanelResize(panel);
}

void PanelManager::HandleDockVisibilityChange(PanelDock* dock) {
  UpdateAvailableArea();
}

void PanelManager::HandleScreenResize() {
  for (vector<PanelContainer*>::iterator it = containers_.begin();
       it != containers_.end(); ++it) {
    (*it)->HandleScreenResize();
  }
  UpdateAvailableArea();
}

bool PanelManager::TakeFocus() {
  return panel_bar_->TakeFocus();
}

Panel* PanelManager::GetPanelByXid(XWindow xid) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return NULL;
  return GetPanelByWindow(*win);
}

Panel* PanelManager::GetPanelByWindow(const Window& win) {
  shared_ptr<Panel> panel = FindWithDefault(
      panels_, win.xid(), shared_ptr<Panel>());
  if (panel.get())
    return panel.get();

  return FindWithDefault(
      panels_by_titlebar_xid_, win.xid(), static_cast<Panel*>(NULL));
}

void PanelManager::RegisterContainer(PanelContainer* container) {
  vector<XWindow> input_xids;
  container->GetInputWindows(&input_xids);
  for (vector<XWindow>::const_iterator it = input_xids.begin();
       it != input_xids.end(); ++it) {
    VLOG(1) << "Registering input window " << *it << " for container "
            << container;
    CHECK(container_input_xids_.insert(make_pair(*it, container)).second);
  }
  containers_.push_back(container);
}

void PanelManager::DoInitialSetupForWindow(Window* win) {
  win->MoveClientOffscreen();
}

void PanelManager::HandlePeriodicPanelDragMotion() {
  DCHECK(dragged_panel_);
  if (!dragged_panel_)
    return;

  const int x = dragged_panel_event_coalescer_->x();
  const int y = dragged_panel_event_coalescer_->y();

  bool container_handled_drag = false;
  bool panel_was_detached = false;
  PanelContainer* container = GetContainerForPanel(*dragged_panel_);
  if (container) {
    if (container->HandleNotifyPanelDraggedMessage(dragged_panel_, x, y)) {
      container_handled_drag = true;
    } else {
      VLOG(1) << "Container " << container << " told us to detach panel "
              << dragged_panel_->xid_str() << " at (" << x << ", " << y << ")";
      RemovePanelFromContainer(dragged_panel_, container);
      panel_was_detached = true;
    }
  }

  if (!container_handled_drag) {
    if (panel_was_detached) {
      dragged_panel_->SetTitlebarWidth(dragged_panel_->content_width());
      dragged_panel_->StackAtTopOfLayer(StackingManager::LAYER_DRAGGED_PANEL);
    }

    // Offer the panel to all of the containers.  If we find one that wants
    // it, attach it; otherwise we just move the panel to the dragged location.
    bool panel_was_reattached = false;
    for (vector<PanelContainer*>::iterator it = containers_.begin();
         it != containers_.end(); ++it) {
      if ((*it)->ShouldAddDraggedPanel(dragged_panel_, x, y)) {
        VLOG(1) << "Container " << *it << " told us to attach panel "
                << dragged_panel_->xid_str()
                << " at (" << x << ", " << y << ")";
        AddPanelToContainer(dragged_panel_,
                            *it,
                            PanelContainer::PANEL_SOURCE_DRAGGED);
        CHECK((*it)->HandleNotifyPanelDraggedMessage(dragged_panel_, x, y));
        panel_was_reattached = true;
        break;
      }
    }
    if (!panel_was_reattached) {
      dragged_panel_->Move(
          x, y, false, panel_was_detached ? kDetachPanelAnimMs : 0);
    }
  }
}

void PanelManager::HandlePanelDragComplete(Panel* panel, bool removed) {
  DCHECK(panel);
  DCHECK(dragged_panel_ == panel);
  if (dragged_panel_ != panel)
    return;

  if (dragged_panel_event_coalescer_->IsRunning())
    dragged_panel_event_coalescer_->Stop();
  dragged_panel_ = NULL;

  if (!removed) {
    PanelContainer* container = GetContainerForPanel(*panel);
    if (container) {
      container->HandleNotifyPanelDragCompleteMessage(panel);
    } else {
      VLOG(1) << "Attaching dropped panel " << panel->xid_str()
              << " to panel bar";
      AddPanelToContainer(panel,
                          panel_bar_.get(),
                          PanelContainer::PANEL_SOURCE_DROPPED);
    }
  }
}

void PanelManager::AddPanelToContainer(Panel* panel,
                                       PanelContainer* container,
                                       PanelContainer::PanelSource source) {
  DCHECK(GetContainerForPanel(*panel) == NULL);
  CHECK(containers_by_panel_.insert(make_pair(panel, container)).second);
  container->AddPanel(panel, source);
}

void PanelManager::RemovePanelFromContainer(Panel* panel,
                                            PanelContainer* container) {
  DCHECK(GetContainerForPanel(*panel) == container);
  CHECK_EQ(containers_by_panel_.erase(panel), static_cast<size_t>(1));
  container->RemovePanel(panel);
  panel->RemoveButtonGrab(false);  // remove_pointer_grab=false
  panel->SetResizable(false);
  panel->SetShadowOpacity(1.0, kDetachPanelAnimMs);
  panel->SetExpandedState(true);
}

void PanelManager::UpdateAvailableArea() {
  int avail_x = 0;
  int avail_y = 0;
  int avail_width = wm_->width();
  int avail_height = wm_->height();

  if (left_panel_dock_->IsVisible()) {
    avail_x += left_panel_dock_->width();
    avail_width -= left_panel_dock_->width();
  }
  if (right_panel_dock_->IsVisible())
    avail_width -= right_panel_dock_->width();

  wm_->HandleLayoutManagerAreaChange(
      avail_x, avail_y, avail_width, avail_height);
}

}  // namespace window_manager
