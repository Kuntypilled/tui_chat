// gmclient.cpp
//
// Distributed under the MIT license, see LICENSE for details.

#include "gmclient.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"

static const std::string s_Network = "gmessages";

const rapidjson::Value* GmReply::Result() const
{
  if (!doc || !doc->IsObject()) return nullptr;
  auto it = doc->FindMember("result");
  return (it != doc->MemberEnd()) ? &it->value : nullptr;
}

GmClient::GmClient(const std::string& p_SocketPath)
  : m_SocketPath(p_SocketPath)
{
}

GmClient::~GmClient()
{
  Close();
}

bool GmClient::Connect()
{
  std::lock_guard<std::mutex> lock(m_ConnectMutex);
  if (m_Connected) return true;

  // a previous reader has finished, release its socket before reconnecting
  ReleaseSocket();

  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (m_SocketPath.empty() || (m_SocketPath.size() >= sizeof(addr.sun_path)))
  {
    LOG_ERROR("helper socket path is empty or too long");
    return false;
  }

  strncpy(addr.sun_path, m_SocketPath.c_str(), sizeof(addr.sun_path) - 1);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    close(fd);
    return false;
  }

  m_Fd = fd;
  m_Closing = false;
  m_Connected = true;
  m_Thread = std::thread(&GmClient::ReadLoop, this);
  return true;
}

void GmClient::Close()
{
  std::lock_guard<std::mutex> lock(m_ConnectMutex);
  m_Closing = true;
  if (m_Fd >= 0)
  {
    shutdown(m_Fd, SHUT_RDWR);
  }

  ReleaseSocket();
}

void GmClient::ReleaseSocket()
{
  if (m_Thread.joinable() && (m_Thread.get_id() != std::this_thread::get_id()))
  {
    m_Thread.join();
  }

  if (m_Fd >= 0)
  {
    close(m_Fd);
    m_Fd = -1;
  }

  m_Connected = false;
}

bool GmClient::IsConnected() const
{
  return m_Connected;
}

void GmClient::SetEventHandler(const EventHandler& p_Handler)
{
  std::lock_guard<std::mutex> lock(m_HandlerMutex);
  m_EventHandler = p_Handler;
}

void GmClient::SetDisconnectHandler(const DisconnectHandler& p_Handler)
{
  std::lock_guard<std::mutex> lock(m_HandlerMutex);
  m_DisconnectHandler = p_Handler;
}

bool GmClient::WriteAll(const std::string& p_Data)
{
  std::lock_guard<std::mutex> lock(m_WriteMutex);
  size_t sent = 0;
  while (sent < p_Data.size())
  {
    ssize_t n = send(m_Fd, p_Data.data() + sent, p_Data.size() - sent, MSG_NOSIGNAL);
    if (n < 0)
    {
      if (errno == EINTR) continue;
      return false;
    }

    sent += static_cast<size_t>(n);
  }

  return true;
}

GmReply GmClient::Call(const std::string& p_Method, const std::string& p_ParamsJson, int p_TimeoutMs)
{
  GmReply failure;
  std::shared_ptr<Pending> pending = std::make_shared<Pending>();
  std::string id;

  {
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (!m_Connected)
    {
      failure.error = "not connected to the helper";
      return failure;
    }

    id = std::to_string(++m_NextId);
    m_Pending[id] = pending;
  }

  // method names are internal constants and the id is numeric, so neither needs escaping
  std::string frame = "{\"id\":\"" + id + "\",\"network\":\"" + s_Network + "\",\"method\":\"" + p_Method + "\"";
  if (!p_ParamsJson.empty())
  {
    frame += ",\"params\":" + p_ParamsJson;
  }

  frame += "}\n";

  // never log the frame itself, params can contain message text
  LOG_TRACE("helper call %s id %s", p_Method.c_str(), id.c_str());

  if (!WriteAll(frame))
  {
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    m_Pending.erase(id);
    failure.error = "could not write to the helper";
    return failure;
  }

  std::unique_lock<std::mutex> lock(m_PendingMutex);
  const bool done = m_PendingCondVar.wait_for(lock, std::chrono::milliseconds(p_TimeoutMs),
                                              [&]() { return pending->done; });
  m_Pending.erase(id);
  if (!done)
  {
    failure.error = "the helper did not answer in time";
    return failure;
  }

  return pending->reply;
}

void GmClient::ReadLoop()
{
  std::string buffer;
  char chunk[16384];
  while (true)
  {
    ssize_t n = recv(m_Fd, chunk, sizeof(chunk), 0);
    if (n < 0)
    {
      if (errno == EINTR) continue;
      break;
    }

    if (n == 0) break;

    buffer.append(chunk, static_cast<size_t>(n));

    size_t pos = 0;
    while (true)
    {
      size_t eol = buffer.find('\n', pos);
      if (eol == std::string::npos) break;

      if (eol > pos)
      {
        HandleLine(buffer.substr(pos, eol - pos));
      }

      pos = eol + 1;
    }

    buffer.erase(0, pos);
  }

  DisconnectHandler disconnectHandler;
  {
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    m_Connected = false;
    for (auto& entry : m_Pending)
    {
      if (!entry.second->done)
      {
        entry.second->done = true;
        entry.second->reply.error = "the helper connection closed";
      }
    }

    m_PendingCondVar.notify_all();
  }

  if (!m_Closing)
  {
    std::lock_guard<std::mutex> lock(m_HandlerMutex);
    disconnectHandler = m_DisconnectHandler;
  }

  if (disconnectHandler)
  {
    disconnectHandler();
  }
}

void GmClient::HandleLine(const std::string& p_Line)
{
  GmJson doc = std::make_shared<rapidjson::Document>();
  doc->Parse(p_Line.c_str());
  if (doc->HasParseError() || !doc->IsObject()) return;

  auto idIt = doc->FindMember("id");
  if ((idIt != doc->MemberEnd()) && idIt->value.IsString() && (idIt->value.GetStringLength() > 0))
  {
    const std::string id = idIt->value.GetString();
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    auto it = m_Pending.find(id);
    if (it != m_Pending.end())
    {
      GmReply& reply = it->second->reply;
      auto okIt = doc->FindMember("ok");
      reply.ok = (okIt != doc->MemberEnd()) && okIt->value.IsBool() && okIt->value.GetBool();
      auto errIt = doc->FindMember("error");
      if ((errIt != doc->MemberEnd()) && errIt->value.IsString())
      {
        reply.error = errIt->value.GetString();
      }

      reply.doc = doc;
      it->second->done = true;
      m_PendingCondVar.notify_all();
    }

    return;
  }

  auto eventIt = doc->FindMember("event");
  if ((eventIt == doc->MemberEnd()) || !eventIt->value.IsString()) return;

  // the helper serves several services over one socket, only one is wanted here
  auto netIt = doc->FindMember("network");
  if ((netIt != doc->MemberEnd()) && netIt->value.IsString() && (s_Network != netIt->value.GetString())) return;

  EventHandler handler;
  {
    std::lock_guard<std::mutex> lock(m_HandlerMutex);
    handler = m_EventHandler;
  }

  if (!handler) return;

  static const rapidjson::Value s_Null;
  auto dataIt = doc->FindMember("data");
  handler(eventIt->value.GetString(), (dataIt != doc->MemberEnd()) ? dataIt->value : s_Null);
}
