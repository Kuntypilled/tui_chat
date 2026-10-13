// gmhelper.h
//
// Finding and starting the omachatd helper. The helper is started detached so
// it keeps running after tui_chat exits and is shared by later launches until
// the machine is shut down or the user logs out.
//
// Distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>

namespace GmHelper
{
  // Where the helper listens when no socket path is configured.
  std::string DefaultSocketPath();

  // Where the helper keeps message content and its login, for user-facing text.
  std::string DataDir();
  std::string CacheDir();

  // Configured path first, then TUI_CHAT_HELPER, then the tui_chat data dir,
  // then PATH. Empty when not found.
  std::string FindBinary(const std::string& p_Configured);

  // Starts the helper detached. p_SocketPath is only passed on when non-empty.
  bool Start(const std::string& p_Binary, const std::string& p_SocketPath, const std::string& p_LogPath);

  bool WaitForSocket(const std::string& p_SocketPath, int p_TimeoutMs);
}
