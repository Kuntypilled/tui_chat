// uiimagecache.h
//
// Inline photos for terminals that speak the kitty graphics protocol.
//
// A photo is turned into a small PNG thumbnail by a background thread, handed to the
// terminal once, and then drawn as ordinary text cells ("Unicode placeholders"), so it
// scrolls, clips and redraws together with the message it belongs to.
//
// Distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>

#include <ncurses.h>

struct UiImage
{
  enum State
  {
    Pending,
    Ready,
    Failed,
  };

  State state = Pending;
  int pxW = 0; // thumbnail size in pixels, when Ready
  int pxH = 0;
};

class UiImageCache
{
public:
  // True when the terminal and the settings allow inline photos. Falls back to the
  // plain file name line everywhere else.
  static bool Enabled();

  // True for a file that is, going by its extension or mime type, a photo that can be shown
  static bool IsImageFile(const std::string& p_Path, const std::string& p_MimeType);

  // Starts the first draw of a frame, so images in use this frame are never evicted
  static void BeginFrame();

  // The state of a photo's thumbnail. Never blocks, starts the work when first asked.
  static UiImage Get(const std::string& p_FilePath);

  // The size in cells to show a thumbnail at, within the given limits
  static void Layout(const UiImage& p_Image, int p_MaxCols, int p_MaxRows, int& p_Cols, int& p_Rows);

  // Makes sure the terminal holds the photo at this size. Returns the image id to draw
  // with, or 0 when it cannot be shown.
  static int Show(const std::string& p_FilePath, int p_Cols, int p_Rows);

  // Draws one cell row of a shown photo. Returns false if it could not be drawn.
  static bool DrawRow(WINDOW* p_Win, int p_Y, int p_X, int p_Id, int p_Row, int p_Cols);

  // True while any thumbnail is still being made
  static bool AnyPending();

  // Frees the images in the terminal and stops the background thread
  static void Cleanup();
};
