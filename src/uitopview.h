// uitopview.h
//
// Copyright (c) 2019-2021 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>
#include <vector>

#include "uiviewbase.h"

class UiTopView : public UiViewBase
{
public:
  UiTopView(const UiViewParams& p_Params);

  virtual void Draw();

  // The service tab at screen column p_X as last drawn, or empty.
  std::string GetTabAt(int p_X) const;

private:
  struct TabSpan
  {
    int x0 = 0;
    int x1 = 0; // one past the last column
    std::string profileId;
  };

  std::vector<TabSpan> m_TabSpans;
};
