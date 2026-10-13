// gmclient.h
//
// Connection to the omachatd helper: newline-delimited JSON over a Unix socket.
// Requests are matched to replies by id, and pushed events go to a handler.
//
// tui_chat is not affiliated with nchat or OmaChat. This file talks to the
// helper over its socket only and does not use any of its code.
//
// Distributed under the MIT license, see LICENSE for details.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define CEREAL_RAPIDJSON_HAS_STDSTRING 1
#include "cereal/external/rapidjson/document.h"

typedef std::shared_ptr<rapidjson::Document> GmJson;

struct GmReply
{
  bool ok = false;
  std::string error; // the helper's text, or a local failure such as a timeout
  GmJson doc; // the whole reply frame, null when the call failed locally

  // The "result" member of the reply, or nullptr when absent.
  const rapidjson::Value* Result() const;
};

class GmClient
{
public:
  typedef std::function<void(const std::string& p_Event, const rapidjson::Value& p_Data)> EventHandler;
  typedef std::function<void()> DisconnectHandler;

  explicit GmClient(const std::string& p_SocketPath);
  ~GmClient();

  bool Connect();
  void Close();
  bool IsConnected() const;

  // Blocking request. p_ParamsJson is a JSON object text or empty.
  GmReply Call(const std::string& p_Method, const std::string& p_ParamsJson = "", int p_TimeoutMs = 20000);

  // Handlers run on the reader thread and must not call Call().
  void SetEventHandler(const EventHandler& p_Handler);
  void SetDisconnectHandler(const DisconnectHandler& p_Handler);

private:
  struct Pending
  {
    bool done = false;
    GmReply reply;
  };

  void ReadLoop();
  void HandleLine(const std::string& p_Line);
  bool WriteAll(const std::string& p_Data);
  void ReleaseSocket();

private:
  std::string m_SocketPath;
  int m_Fd = -1;
  std::atomic<bool> m_Connected { false };
  std::atomic<bool> m_Closing { false };
  std::thread m_Thread;
  std::mutex m_ConnectMutex;
  std::mutex m_WriteMutex;

  std::mutex m_PendingMutex;
  std::condition_variable m_PendingCondVar;
  std::map<std::string, std::shared_ptr<Pending>> m_Pending;
  uint64_t m_NextId = 0;

  std::mutex m_HandlerMutex;
  EventHandler m_EventHandler;
  DisconnectHandler m_DisconnectHandler;
};
