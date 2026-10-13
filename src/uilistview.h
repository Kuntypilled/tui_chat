// uilistview.h
//
// Copyright (c) 2019-2021 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <ncurses.h>

#include "uiviewbase.h"

class UiListView : public UiViewBase
{
public:
  UiListView(const UiViewParams& p_Params);
  virtual ~UiListView();

  virtual void Draw();

  // Index into the drawn chat list for screen row p_Y, or -1.
  int GetChatIndexAt(int p_Y) const;

private:
  WINDOW* m_PaddedWin = nullptr;
  int m_PaddedH = 0;
  int m_PaddedW = 0;
  int m_DrawnOffset = 0;
  int m_DrawnCount = 0;
};
