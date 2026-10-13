// gmchat.cpp
//
// Distributed under the MIT license, see LICENSE for details.

#include "gmchat.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cereal/external/rapidjson/stringbuffer.h"
#include "cereal/external/rapidjson/writer.h"

#include "fileutil.h"
#include "gmhelper.h"
#include "log.h"
#include "status.h"
#include "strutil.h"
#include "timeutil.h"

// nchat requires "<protocol>_<something>" profile ids. The helper serves one
// Google account, so there is only ever one profile.
static const std::string s_ProfileSuffix = "_1";

// Participant ids and chat ids come from different places in the helper, so
// participant contacts get a prefix to keep them from ever colliding with a chat.
static const std::string s_ParticipantPrefix = "p:";

extern "C" GmChat* CreateGmChat()
{
  return new GmChat();
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static const rapidjson::Value* GetMember(const rapidjson::Value& p_Obj, const char* p_Key)
{
  if (!p_Obj.IsObject()) return nullptr;

  auto it = p_Obj.FindMember(p_Key);
  return (it != p_Obj.MemberEnd()) ? &it->value : nullptr;
}

static std::string GetStr(const rapidjson::Value& p_Obj, const char* p_Key)
{
  const rapidjson::Value* value = GetMember(p_Obj, p_Key);
  if ((value == nullptr) || !value->IsString()) return std::string();

  return std::string(value->GetString(), value->GetStringLength());
}

static bool GetBool(const rapidjson::Value& p_Obj, const char* p_Key)
{
  const rapidjson::Value* value = GetMember(p_Obj, p_Key);
  return (value != nullptr) && value->IsBool() && value->GetBool();
}

static int64_t GetInt(const rapidjson::Value& p_Obj, const char* p_Key)
{
  const rapidjson::Value* value = GetMember(p_Obj, p_Key);
  if ((value == nullptr) || !value->IsNumber()) return 0;

  return value->IsInt64() ? value->GetInt64() : static_cast<int64_t>(value->GetDouble());
}

static std::string Quote(const std::string& p_Str)
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(p_Str.c_str(), static_cast<rapidjson::SizeType>(p_Str.size()));
  return std::string(buffer.GetString(), buffer.GetSize());
}

static std::string StripPlus(const std::string& p_Number)
{
  return (!p_Number.empty() && (p_Number[0] == '+')) ? p_Number.substr(1) : p_Number;
}

static std::string AttachmentLabels(const rapidjson::Value& p_Attachments)
{
  std::string labels;
  if (!p_Attachments.IsArray()) return labels;

  for (const auto& attachment : p_Attachments.GetArray())
  {
    std::string label;
    if (GetBool(attachment, "isImage")) label = "[Photo]";
    else if (GetBool(attachment, "isGif")) label = "[GIF]";
    else if (GetBool(attachment, "isVideo")) label = "[Video]";
    else if (GetBool(attachment, "isAudio")) label = "[Audio]";
    else
    {
      const std::string name = GetStr(attachment, "name");
      label = name.empty() ? "[File]" : ("[File: " + name + "]");
    }

    labels += (labels.empty() ? "" : " ") + label;
  }

  return labels;
}

// ---------------------------------------------------------------------------
// Protocol identity and profile handling
// ---------------------------------------------------------------------------

GmChat::GmChat()
{
  m_ProfileId = GetName() + s_ProfileSuffix;
}

GmChat::~GmChat()
{
  Logout();
}

std::string GmChat::GetProfileId() const
{
  return m_ProfileId;
}

std::string GmChat::GetProfileDisplayName() const
{
  const std::string configured = m_Config.Get("profile_display_name");
  return configured.empty() ? "Google Messages" : configured;
}

bool GmChat::HasFeature(ProtocolFeature p_ProtocolFeature) const
{
  // the helper sends contacts together with chats, so the UI need not ask
  static int customFeatures = FeatureAutoGetContactsOnLogin;
  return (p_ProtocolFeature & customFeatures);
}

bool GmChat::IsGroupChat(const std::string& p_ChatId) const
{
  std::lock_guard<std::mutex> lock(m_StateMutex);
  return m_GroupChats.count(p_ChatId) > 0;
}

std::string GmChat::GetSelfId() const
{
  std::lock_guard<std::mutex> lock(m_StateMutex);
  return m_SelfId;
}

void GmChat::InitConfig()
{
  const std::map<std::string, std::string> defaultConfig =
  {
    { "profile_display_name", "Google Messages" },
    { "helper_path", "" }, // empty means search for omachatd
    { "socket_path", "" }, // empty means the helper's own default
    { "start_helper", "1" },
  };
  const std::string configPath(m_ProfileDir + std::string("/gmchat.conf"));
  m_Config = Config(configPath, defaultConfig);
}

void GmChat::AttachClient()
{
  m_Client.reset(new GmClient(GetSocketPath()));
  m_Client->SetEventHandler([this](const std::string& p_Event, const rapidjson::Value& p_Data)
  {
    OnEvent(p_Event, p_Data);
  });
  m_Client->SetDisconnectHandler([this]()
  {
    OnDisconnect();
  });
}

bool GmChat::LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId)
{
  LOG_INFO("load google messages profile");
  if (!p_ProfilesDir.empty() && !p_ProfileId.empty())
  {
    m_ProfileDir = p_ProfilesDir + "/" + p_ProfileId;
    m_ProfileId = p_ProfileId;
  }

  InitConfig();

  AttachClient();

  // This runs in the plain terminal before the full screen UI starts, so a
  // short status line is possible here. A failure does not stop the UI: the
  // worker keeps retrying once logged in.
  bool needsAttention = true;
  switch (Establish(true /*p_Verbose*/))
  {
    case EstablishReady:
      needsAttention = false;
      break;

    case EstablishNotPaired:
      std::cout << "Google Messages is not paired yet. Run nchat with -s to set it up.\n";
      break;

    case EstablishHelperError:
      {
        std::string error;
        std::string hint;
        {
          std::lock_guard<std::mutex> lock(m_StateMutex);
          error = m_HelperError;
          hint = m_HelperHint;
        }

        std::cout << "The Google Messages helper reports a problem"
                  << (error.empty() ? "" : (": " + error)) << "\n";
        if (!hint.empty()) std::cout << hint << "\n";
        std::cout << "Run nchat with -s to pair again.\n";
      }
      break;

    case EstablishNoHelper:
    case EstablishFailed:
    default:
      std::cout << "Continuing without the Google Messages helper. It will be retried in the background.\n";
      break;
  }

  // the full screen UI wipes the terminal, so give the message a moment to be read
  if (needsAttention && isatty(STDIN_FILENO))
  {
    std::cout << "Press Enter to continue.\n";
    std::string line;
    std::getline(std::cin, line);
  }

  return true;
}

bool GmChat::CloseProfile()
{
  m_ProfileId = "";
  return true;
}

// ---------------------------------------------------------------------------
// Helper process and connection
// ---------------------------------------------------------------------------

std::string GmChat::GetSocketPath() const
{
  const std::string configured = m_Config.Get("socket_path");
  return configured.empty() ? GmHelper::DefaultSocketPath() : FileUtil::ExpandPath(configured);
}

bool GmChat::StartHelper(bool p_Verbose)
{
  const std::string binary = GmHelper::FindBinary(m_Config.Get("helper_path"));
  if (binary.empty())
  {
    LOG_ERROR("helper binary not found");
    if (p_Verbose)
    {
      std::cout << "Could not find the omachatd helper program.\n"
                << "Build it with the helper script from this project, or set helper_path in\n"
                << m_ProfileDir << "/gmchat.conf\n";
    }

    return false;
  }

  if (p_Verbose)
  {
    std::cout << "Starting the Google Messages helper...\n";
  }

  const std::string configuredSocket = m_Config.Get("socket_path");
  const std::string socketArg = configuredSocket.empty() ? "" : GetSocketPath();
  const std::string logPath = m_ProfileDir + "/helper.log";
  if (!GmHelper::Start(binary, socketArg, logPath))
  {
    LOG_ERROR("failed to start helper");
    if (p_Verbose) std::cout << "Could not start the helper.\n";
    return false;
  }

  return true;
}

bool GmChat::ConnectWithRetry(int p_TimeoutMs)
{
  const int64_t deadline = TimeUtil::GetCurrentTimeMSec() + p_TimeoutMs;
  while (true)
  {
    if (m_Client->Connect()) return true;

    if (TimeUtil::GetCurrentTimeMSec() >= deadline) return false;

    TimeUtil::Sleep(0.1);
  }
}

GmChat::EstablishResult GmChat::Establish(bool p_Verbose)
{
  if (!m_Client) return EstablishFailed;

  // At most one restart is needed to apply the service selection, the extra
  // attempt covers the helper still being on its way up.
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    if (!m_Client->IsConnected() && !ConnectWithRetry(300))
    {
      if (m_Config.Get("start_helper") != "1")
      {
        if (p_Verbose) std::cout << "The Google Messages helper is not running.\n";
        return EstablishNoHelper;
      }

      if (!StartHelper(p_Verbose)) return EstablishNoHelper;

      if (!ConnectWithRetry(10000))
      {
        if (p_Verbose) std::cout << "The helper did not come up in time.\n";
        return EstablishFailed;
      }
    }

    GmReply config = m_Client->Call("config");
    if (!config.ok)
    {
      LOG_WARNING("helper config failed: %s", config.error.c_str());
      return EstablishFailed;
    }

    std::vector<std::string> enabled;
    bool hasGoogleMessages = false;
    const rapidjson::Value* result = config.Result();
    const rapidjson::Value* services = (result != nullptr) ? GetMember(*result, "enabledServices") : nullptr;
    if ((services != nullptr) && services->IsArray())
    {
      for (const auto& service : services->GetArray())
      {
        if (!service.IsString()) continue;

        enabled.push_back(service.GetString());
        hasGoogleMessages = hasGoogleMessages || (enabled.back() == "gmessages");
      }
    }

    if (hasGoogleMessages) break;

    if (p_Verbose)
    {
      std::cout << "Turning on Google Messages in the helper. The helper restarts once to apply it.\n";
    }

    enabled.push_back("gmessages");
    std::string params = "{\"enabledServices\":[";
    for (size_t i = 0; i < enabled.size(); ++i)
    {
      params += (i == 0 ? "" : ",") + Quote(enabled[i]);
    }

    params += "]}";

    GmReply set = m_Client->Call("setEnabledServices", params);
    if (!set.ok)
    {
      LOG_WARNING("helper setEnabledServices failed: %s", set.error.c_str());
      if (p_Verbose) std::cout << "The helper refused the change: " << set.error << "\n";
      return EstablishFailed;
    }

    // the helper exits on purpose once it has answered, then it is started again
    for (int i = 0; (i < 50) && m_Client->IsConnected(); ++i)
    {
      TimeUtil::Sleep(0.1);
    }

    m_Client->Close();
  }

  GmReply status = m_Client->Call("status");
  if (!status.ok)
  {
    LOG_WARNING("helper status failed: %s", status.error.c_str());
    return EstablishFailed;
  }

  const rapidjson::Value* statusResult = status.Result();
  const std::string state = (statusResult != nullptr) ? GetStr(*statusResult, "state") : std::string();
  LOG_INFO("helper state %s", state.c_str());
  if (statusResult != nullptr)
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_HelperError = GetStr(*statusResult, "error");
    m_HelperHint = GetStr(*statusResult, "hint");
  }

  ApplyState(state);

  {
    std::lock_guard<std::mutex> lock(m_ProcessMutex);
    m_NeedReconnect = false;
  }

  if ((state == "connected") || (state == "connecting")) return EstablishReady;
  if ((state == "unpaired") || (state == "pairing") || (state == "gaiaPairing")) return EstablishNotPaired;
  if (state == "error") return EstablishHelperError;
  return EstablishFailed;
}

void GmChat::OnDisconnect()
{
  LOG_INFO("helper connection lost");
  ApplyState("disconnected");

  if (m_IsSetup) return;

  {
    std::lock_guard<std::mutex> lock(m_ProcessMutex);
    m_NeedReconnect = true;
  }

  m_ProcessCondVar.notify_all();
}

void GmChat::ApplyState(const std::string& p_State)
{
  bool becameConnected = false;
  bool becameDisconnected = false;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_HelperState = p_State;
    const bool nowConnected = (p_State == "connected");
    if (nowConnected != m_Connected)
    {
      m_Connected = nowConnected;
      becameConnected = m_Active && nowConnected;
      becameDisconnected = m_Active && !nowConnected;
    }
  }

  m_StateCondVar.notify_all();

  if (becameConnected)
  {
    Status::Set(m_ProfileId, Status::FlagOnline);
    std::shared_ptr<ConnectNotify> connectNotify = std::make_shared<ConnectNotify>(m_ProfileId);
    connectNotify->success = true;
    Emit(connectNotify);
  }
  else if (becameDisconnected)
  {
    Status::Clear(m_ProfileId, Status::FlagOnline);
    std::shared_ptr<ConnectNotify> connectNotify = std::make_shared<ConnectNotify>(m_ProfileId);
    connectNotify->success = false;
    Emit(connectNotify);
  }
}

void GmChat::Emit(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  if (m_MessageHandler)
  {
    m_MessageHandler(p_ServiceMessage);
  }
}

// ---------------------------------------------------------------------------
// Login, worker thread and requests
// ---------------------------------------------------------------------------

bool GmChat::Login()
{
  bool connected = false;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_Active = true;
    connected = m_Connected && m_Client && m_Client->IsConnected();
    if (!connected) m_Connected = false; // forget a state that is no longer backed by a connection
  }

  if (!m_Running)
  {
    m_Running = true;
    m_Thread = std::thread(&GmChat::Process, this);
  }

  if (connected)
  {
    // report through the worker so it arrives after the handler is set up
    QueueTask([this]()
    {
      Status::Set(m_ProfileId, Status::FlagOnline);
      std::shared_ptr<ConnectNotify> connectNotify = std::make_shared<ConnectNotify>(m_ProfileId);
      connectNotify->success = true;
      Emit(connectNotify);
    });
  }
  else if (!m_Client || !m_Client->IsConnected())
  {
    {
      std::lock_guard<std::mutex> lock(m_ProcessMutex);
      m_NeedReconnect = true;
    }

    m_ProcessCondVar.notify_all();
  }

  return true;
}

bool GmChat::Logout()
{
  Status::Clear(m_ProfileId, Status::FlagOnline);

  if (m_Running)
  {
    {
      std::lock_guard<std::mutex> lock(m_ProcessMutex);
      m_Running = false;
    }

    m_ProcessCondVar.notify_all();
  }

  if (m_Client)
  {
    // closing the connection does not stop the helper, which is shared with later launches
    m_Client->Close();
  }

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }

  return true;
}

void GmChat::QueueTask(const std::function<void()>& p_Task)
{
  {
    std::lock_guard<std::mutex> lock(m_ProcessMutex);
    m_Tasks.push_back(p_Task);
  }

  m_ProcessCondVar.notify_one();
}

void GmChat::SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  QueueTask([this, p_RequestMessage]()
  {
    PerformRequest(p_RequestMessage);
  });
}

void GmChat::SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler)
{
  m_MessageHandler = p_MessageHandler;
}

void GmChat::Process()
{
  int backoffMs = 1000;
  while (true)
  {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(m_ProcessMutex);
      while (m_Running && m_Tasks.empty() && !m_NeedReconnect)
      {
        m_ProcessCondVar.wait(lock);
      }

      if (!m_Running) break;

      if (!m_Tasks.empty())
      {
        task = m_Tasks.front();
        m_Tasks.pop_front();
      }
    }

    if (task)
    {
      task();
      continue;
    }

    // no task is waiting, so this wake up is a reconnect
    const EstablishResult result = Establish(false /*p_Verbose*/);
    if ((result == EstablishReady) || (result == EstablishNotPaired) || (result == EstablishHelperError))
    {
      backoffMs = 1000;
    }
    else
    {
      std::unique_lock<std::mutex> lock(m_ProcessMutex);
      m_ProcessCondVar.wait_for(lock, std::chrono::milliseconds(backoffMs), [this]() { return !m_Running; });
      backoffMs = std::min(backoffMs * 2, 10000);
    }
  }
}

void GmChat::PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  if (!m_MessageHandler) return;

  switch (p_RequestMessage->GetMessageType())
  {
    case GetChatsRequestType:
      {
        std::shared_ptr<GetChatsRequest> request = std::static_pointer_cast<GetChatsRequest>(p_RequestMessage);
        DoGetChats(request->chatIds);
      }
      break;

    case GetMessagesRequestType:
      {
        std::shared_ptr<GetMessagesRequest> request = std::static_pointer_cast<GetMessagesRequest>(p_RequestMessage);
        DoGetMessages(request->chatId, request->fromMsgId, request->limit);
      }
      break;

    case SendMessageRequestType:
      {
        std::shared_ptr<SendMessageRequest> request = std::static_pointer_cast<SendMessageRequest>(p_RequestMessage);
        DoSendMessage(request->chatId, request->chatMessage);
      }
      break;

    case MarkMessageReadRequestType:
      {
        std::shared_ptr<MarkMessageReadRequest> request =
          std::static_pointer_cast<MarkMessageReadRequest>(p_RequestMessage);
        DoMarkRead(request->chatId, request->msgId);
      }
      break;

    case DeferNotifyRequestType:
      {
        std::shared_ptr<DeferNotifyRequest> request = std::static_pointer_cast<DeferNotifyRequest>(p_RequestMessage);
        Emit(request->serviceMessage);
      }
      break;

    default:
      LOG_DEBUG("unsupported request message %d", p_RequestMessage->GetMessageType());
      break;
  }
}

void GmChat::DoGetChats(const std::unordered_set<std::string>& p_ChatIds)
{
  std::shared_ptr<NewChatsNotify> newChatsNotify = std::make_shared<NewChatsNotify>(m_ProfileId);
  std::shared_ptr<NewContactsNotify> newContactsNotify = std::make_shared<NewContactsNotify>(m_ProfileId);

  GmReply reply = m_Client ? m_Client->Call("conversations", "{\"count\":0}") : GmReply();
  const rapidjson::Value* result = reply.Result();
  if (!reply.ok || (result == nullptr) || !result->IsArray())
  {
    LOG_WARNING("get chats failed: %s", reply.error.c_str());
    newChatsNotify->success = false;
    Emit(newChatsNotify);
    return;
  }

  for (const auto& conversation : result->GetArray())
  {
    ChatInfo chatInfo;
    std::vector<ContactInfo> contacts;
    ConvertConversation(conversation, chatInfo, contacts);
    if (chatInfo.id.empty()) continue;

    if (!p_ChatIds.empty() && (p_ChatIds.count(chatInfo.id) == 0)) continue;

    newChatsNotify->chatInfos.push_back(chatInfo);
    newContactsNotify->contactInfos.insert(newContactsNotify->contactInfos.end(), contacts.begin(), contacts.end());
  }

  newChatsNotify->success = true;

  // contacts first, so names are known by the time the chat list is drawn
  Emit(newContactsNotify);
  Emit(newChatsNotify);
}

void GmChat::DoGetMessages(const std::string& p_ChatId, const std::string& p_FromMsgId, int32_t p_Limit)
{
  std::string params = "{\"conversationID\":" + Quote(p_ChatId) + ",\"count\":" +
    std::to_string(p_Limit > 0 ? p_Limit : 40);

  // The helper pages by a cursor, nchat pages by "messages older than this id".
  // The cursor is the (id, time) pair of the oldest message of the page before.
  if (!p_FromMsgId.empty())
  {
    int64_t timeUs = 0;
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      auto cursorIt = m_Cursors.find(p_ChatId);
      if ((cursorIt != m_Cursors.end()) && (cursorIt->second.id == p_FromMsgId))
      {
        timeUs = cursorIt->second.timeUs;
      }
      else
      {
        auto timeIt = m_MessageTimeUs.find(p_FromMsgId);
        if (timeIt != m_MessageTimeUs.end()) timeUs = timeIt->second;
      }
    }

    if (timeUs != 0)
    {
      params += ",\"cursorID\":" + Quote(p_FromMsgId) + ",\"cursorTime\":" + std::to_string(timeUs);
    }
  }

  params += "}";

  std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
  newMessagesNotify->chatId = p_ChatId;
  newMessagesNotify->fromMsgId = p_FromMsgId;
  newMessagesNotify->cached = false;
  newMessagesNotify->sequence = true;

  GmReply reply = m_Client ? m_Client->Call("messages", params, 30000) : GmReply();
  const rapidjson::Value* result = reply.Result();
  const rapidjson::Value* messages = (result != nullptr) ? GetMember(*result, "messages") : nullptr;
  if (!reply.ok || (messages == nullptr) || !messages->IsArray())
  {
    LOG_WARNING("get messages failed: %s", reply.error.c_str());
    newMessagesNotify->success = false;
    Emit(newMessagesNotify);
    return;
  }

  std::shared_ptr<NewContactsNotify> newContactsNotify = std::make_shared<NewContactsNotify>(m_ProfileId);
  for (const auto& message : messages->GetArray())
  {
    ChatMessage chatMessage;
    std::string chatId;
    std::string tmpId;
    std::vector<ContactInfo> contacts;
    if (!ConvertMessage(message, chatMessage, chatId, contacts, tmpId)) continue;

    newMessagesNotify->chatMessages.push_back(chatMessage);
    newContactsNotify->contactInfos.insert(newContactsNotify->contactInfos.end(), contacts.begin(), contacts.end());
  }

  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    Cursor& cursor = m_Cursors[p_ChatId];
    cursor.id = GetStr(*result, "cursorID");
    cursor.timeUs = GetInt(*result, "cursorTime");
  }

  newMessagesNotify->success = true;

  if (!newContactsNotify->contactInfos.empty())
  {
    Emit(newContactsNotify);
  }

  Emit(newMessagesNotify);
}

void GmChat::DoSendMessage(const std::string& p_ChatId, const ChatMessage& p_ChatMessage)
{
  std::shared_ptr<SendMessageNotify> sendMessageNotify = std::make_shared<SendMessageNotify>(m_ProfileId);
  sendMessageNotify->chatId = p_ChatId;
  sendMessageNotify->chatMessage = p_ChatMessage;

  std::string params = "{\"conversationID\":" + Quote(p_ChatId) + ",\"text\":" + Quote(p_ChatMessage.text);
  if (!p_ChatMessage.quotedId.empty())
  {
    params += ",\"replyToID\":" + Quote(p_ChatMessage.quotedId);
  }

  params += "}";

  GmReply reply = m_Client ? m_Client->Call("send", params, 30000) : GmReply();
  const rapidjson::Value* result = reply.Result();
  if (!reply.ok || (result == nullptr) || !result->IsObject())
  {
    LOG_WARNING("send failed: %s", reply.error.c_str());
    sendMessageNotify->success = false;
    Emit(sendMessageNotify);
    return;
  }

  ChatMessage sentMessage;
  std::string chatId;
  std::string tmpId;
  std::vector<ContactInfo> contacts;
  if (!ConvertMessage(*result, sentMessage, chatId, contacts, tmpId))
  {
    sendMessageNotify->success = false;
    Emit(sendMessageNotify);
    return;
  }

  if (chatId.empty()) chatId = p_ChatId;

  if (!tmpId.empty())
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_SentTmpIds[tmpId] = chatId;
  }

  // The UI only logs a send result and never adds the message itself, so the
  // sent message is reported as a new message, like any other.
  sendMessageNotify->success = true;
  sendMessageNotify->chatMessage = sentMessage;
  Emit(sendMessageNotify);

  std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
  newMessagesNotify->success = true;
  newMessagesNotify->chatId = chatId;
  newMessagesNotify->chatMessages.push_back(sentMessage);
  newMessagesNotify->cached = false;
  newMessagesNotify->sequence = false;
  Emit(newMessagesNotify);
}

void GmChat::DoMarkRead(const std::string& p_ChatId, const std::string& p_MsgId)
{
  std::shared_ptr<MarkMessageReadNotify> markMessageReadNotify = std::make_shared<MarkMessageReadNotify>(m_ProfileId);
  markMessageReadNotify->chatId = p_ChatId;
  markMessageReadNotify->msgId = p_MsgId;

  const std::string params = "{\"conversationID\":" + Quote(p_ChatId) + ",\"messageID\":" + Quote(p_MsgId) + "}";
  GmReply reply = m_Client ? m_Client->Call("markRead", params) : GmReply();
  markMessageReadNotify->success = reply.ok;
  if (!reply.ok)
  {
    LOG_DEBUG("mark read failed: %s", reply.error.c_str());
  }

  Emit(markMessageReadNotify);
}

// ---------------------------------------------------------------------------
// Events pushed by the helper
// ---------------------------------------------------------------------------

void GmChat::OnEvent(const std::string& p_Event, const rapidjson::Value& p_Data)
{
  if (p_Event == "status")
  {
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      m_HelperError = GetStr(p_Data, "error");
      m_HelperHint = GetStr(p_Data, "hint");
      const std::string emoji = GetStr(p_Data, "emoji");
      if (!emoji.empty()) m_PairEmoji = emoji;
      const std::string qr = GetStr(p_Data, "qrURL");
      if (!qr.empty()) m_PairQrUrl = qr;
    }

    ApplyState(GetStr(p_Data, "state"));
    return;
  }

  if (p_Event == "emoji")
  {
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      m_PairEmoji = GetStr(p_Data, "emoji");
    }

    m_StateCondVar.notify_all();
    return;
  }

  if (p_Event == "qr")
  {
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      m_PairQrUrl = GetStr(p_Data, "url");
    }

    m_StateCondVar.notify_all();
    return;
  }

  bool active = false;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    active = m_Active;
  }

  // before login the UI asks for chats and history itself, so there is no one to tell
  if (!active) return;

  if (p_Event == "conversation")
  {
    ChatInfo chatInfo;
    std::vector<ContactInfo> contacts;
    ConvertConversation(p_Data, chatInfo, contacts);
    if (chatInfo.id.empty()) return;

    std::shared_ptr<NewContactsNotify> newContactsNotify = std::make_shared<NewContactsNotify>(m_ProfileId);
    newContactsNotify->contactInfos = contacts;
    Emit(newContactsNotify);

    std::shared_ptr<NewChatsNotify> newChatsNotify = std::make_shared<NewChatsNotify>(m_ProfileId);
    newChatsNotify->success = true;
    newChatsNotify->chatInfos.push_back(chatInfo);
    Emit(newChatsNotify);
    return;
  }

  if (p_Event == "message")
  {
    ChatMessage chatMessage;
    std::string chatId;
    std::string tmpId;
    std::vector<ContactInfo> contacts;
    if (!ConvertMessage(p_Data, chatMessage, chatId, contacts, tmpId)) return;

    // The final copy of a message sent from here has a new id. Replace the
    // provisional one the UI has been showing.
    std::string replacedId;
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      auto it = m_SentTmpIds.find(tmpId);
      if (!tmpId.empty() && (it != m_SentTmpIds.end()) && (chatMessage.id != tmpId))
      {
        replacedId = tmpId;
        m_SentTmpIds.erase(it);
      }
    }

    if (!replacedId.empty())
    {
      std::shared_ptr<DeleteMessageNotify> deleteMessageNotify = std::make_shared<DeleteMessageNotify>(m_ProfileId);
      deleteMessageNotify->success = true;
      deleteMessageNotify->chatId = chatId;
      deleteMessageNotify->msgId = replacedId;
      deleteMessageNotify->isOutgoing = true;
      Emit(deleteMessageNotify);
    }

    if (!contacts.empty())
    {
      std::shared_ptr<NewContactsNotify> newContactsNotify = std::make_shared<NewContactsNotify>(m_ProfileId);
      newContactsNotify->contactInfos = contacts;
      Emit(newContactsNotify);
    }

    std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
    newMessagesNotify->success = true;
    newMessagesNotify->chatId = chatId;
    newMessagesNotify->chatMessages.push_back(chatMessage);
    newMessagesNotify->cached = false;
    newMessagesNotify->sequence = false;
    Emit(newMessagesNotify);
  }
}

// ---------------------------------------------------------------------------
// Conversion between the helper's shapes and nchat's
// ---------------------------------------------------------------------------

void GmChat::ConvertConversation(const rapidjson::Value& p_Conv, ChatInfo& p_Chat,
                                 std::vector<ContactInfo>& p_Contacts)
{
  p_Chat.id = GetStr(p_Conv, "id");
  if (p_Chat.id.empty()) return;

  p_Chat.isUnread = GetBool(p_Conv, "unread");
  p_Chat.isPinned = GetBool(p_Conv, "pinned");
  // the helper's timestamps are microseconds, nchat's are milliseconds
  p_Chat.lastMessageTime = GetInt(p_Conv, "timestamp") / 1000;

  const bool isGroup = GetBool(p_Conv, "isGroup");
  std::string name = GetStr(p_Conv, "name");
  std::string otherNumber;
  std::string selfId = GetStr(p_Conv, "outgoingID");

  const rapidjson::Value* participants = GetMember(p_Conv, "participants");
  if ((participants != nullptr) && participants->IsArray())
  {
    for (const auto& participant : participants->GetArray())
    {
      const std::string id = GetStr(participant, "id");
      if (id.empty()) continue;

      ContactInfo contact;
      contact.id = s_ParticipantPrefix + id;
      const std::string number = GetStr(participant, "number");
      contact.name = GetStr(participant, "name");
      if (contact.name.empty()) contact.name = number;
      contact.phone = StripPlus(number);
      contact.isSelf = GetBool(participant, "isMe");
      if (contact.isSelf)
      {
        if (selfId.empty()) selfId = id;
      }
      else if (otherNumber.empty())
      {
        otherNumber = number;
        if (name.empty()) name = contact.name;
      }

      p_Contacts.push_back(contact);
    }
  }

  ContactInfo chatContact;
  chatContact.id = p_Chat.id;
  chatContact.name = name.empty() ? p_Chat.id : name;
  if (!isGroup) chatContact.phone = StripPlus(otherNumber);
  p_Contacts.push_back(chatContact);

  std::lock_guard<std::mutex> lock(m_StateMutex);
  if (isGroup) m_GroupChats.insert(p_Chat.id);
  else m_GroupChats.erase(p_Chat.id);

  m_ChatUnread[p_Chat.id] = p_Chat.isUnread;

  if (m_SelfId.empty() && !selfId.empty())
  {
    m_SelfId = s_ParticipantPrefix + selfId;
  }
}

bool GmChat::ConvertMessage(const rapidjson::Value& p_Msg, ChatMessage& p_ChatMessage, std::string& p_ChatId,
                            std::vector<ContactInfo>& p_Contacts, std::string& p_TmpId)
{
  p_ChatMessage.id = GetStr(p_Msg, "id");
  if (p_ChatMessage.id.empty()) return false;

  p_ChatId = GetStr(p_Msg, "conversationID");
  p_TmpId = GetStr(p_Msg, "tmpID");

  const int64_t timeUs = GetInt(p_Msg, "timestamp");
  p_ChatMessage.timeSent = timeUs / 1000;
  p_ChatMessage.isOutgoing = GetBool(p_Msg, "fromMe");
  p_ChatMessage.isDeleted = GetBool(p_Msg, "deleted");
  p_ChatMessage.quotedId = GetStr(p_Msg, "replyToID");

  std::string text = GetStr(p_Msg, "text");
  const rapidjson::Value* attachments = GetMember(p_Msg, "attachments");
  if (attachments != nullptr)
  {
    const std::string labels = AttachmentLabels(*attachments);
    if (!labels.empty())
    {
      text += (text.empty() ? "" : "\n") + labels;
    }
  }

  p_ChatMessage.text = text;

  const std::string senderId = GetStr(p_Msg, "senderID");
  const std::string senderName = GetStr(p_Msg, "senderName");
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_MessageTimeUs[p_ChatMessage.id] = timeUs;

    if (p_ChatMessage.isOutgoing)
    {
      p_ChatMessage.senderId = m_SelfId.empty() ? "self" : m_SelfId;
      // a tick means the recipient has read it, which Google only reports over RCS
      p_ChatMessage.isRead = (GetStr(p_Msg, "delivery") == "read");
    }
    else
    {
      auto unreadIt = m_ChatUnread.find(p_ChatId);
      p_ChatMessage.isRead = (unreadIt == m_ChatUnread.end()) || !unreadIt->second;
    }
  }

  if (!p_ChatMessage.isOutgoing)
  {
    p_ChatMessage.senderId = senderId.empty() ? p_ChatId : (s_ParticipantPrefix + senderId);
    if (!senderId.empty() && !senderName.empty())
    {
      ContactInfo contact;
      contact.id = s_ParticipantPrefix + senderId;
      contact.name = senderName;
      p_Contacts.push_back(contact);
    }
  }

  const rapidjson::Value* reactions = GetMember(p_Msg, "reactions");
  if ((reactions != nullptr) && reactions->IsArray())
  {
    for (const auto& reaction : reactions->GetArray())
    {
      const std::string emoji = GetStr(reaction, "emoji");
      const int64_t count = GetInt(reaction, "count");
      if (!emoji.empty() && (count > 0))
      {
        p_ChatMessage.reactions.emojiCounts[emoji] = static_cast<size_t>(count);
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Setup wizard (nchat -s), runs in the plain terminal
// ---------------------------------------------------------------------------

bool GmChat::WaitForState(const std::function<bool()>& p_Predicate, int p_TimeoutMs)
{
  std::unique_lock<std::mutex> lock(m_StateMutex);
  return m_StateCondVar.wait_for(lock, std::chrono::milliseconds(p_TimeoutMs), p_Predicate);
}

static bool AskYesNo(const std::string& p_Question)
{
  std::cout << p_Question << " [y/N]: ";
  std::string answer;
  std::getline(std::cin, answer);
  return (answer == "y") || (answer == "Y") || (answer == "yes");
}

bool GmChat::SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId)
{
  m_IsSetup = true;

  std::cout << "\n"
            << "Google Messages setup\n"
            << "\n"
            << "tui_chat is not affiliated with, authorized or endorsed by Google. It reaches\n"
            << "Google Messages through a separate helper program (omachatd, from the OmaChat\n"
            << "project) that uses an unofficial client library. Google can change things at\n"
            << "any time and break it. What Google does about accounts that use unofficial\n"
            << "clients has not been measured, so consider pairing a secondary account first.\n"
            << "\n"
            << "The helper keeps your message content and its own login on disk, in folders\n"
            << "only your user can read:\n"
            << "  " << GmHelper::DataDir() << "\n"
            << "  " << GmHelper::CacheDir() << "\n"
            << "\n";

  if (!AskYesNo("Continue"))
  {
    m_IsSetup = false;
    return false;
  }

  m_ProfileId = GetName() + s_ProfileSuffix;
  m_ProfileDir = p_ProfilesDir + "/" + m_ProfileId;
  mkdir(m_ProfileDir.c_str(), 0700);
  p_ProfileId = m_ProfileId;

  InitConfig();
  AttachClient();

  const EstablishResult established = Establish(true /*p_Verbose*/);
  if ((established != EstablishReady) && (established != EstablishNotPaired) &&
      (established != EstablishHelperError))
  {
    std::cout << "Could not reach the helper, so setup cannot continue.\n";
    m_IsSetup = false;
    return false;
  }

  bool paired = false;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    paired = (m_HelperState == "connected");
  }

  if (paired)
  {
    std::cout << "The helper is already paired with a Google account. Nothing more to do.\n";
    m_IsSetup = false;
    return true;
  }

  std::cout << "How would you like to pair?\n"
            << "  1. Sign in with the Google account from your browser (works with current Messages)\n"
            << "  2. Scan a QR code (older versions of Messages only)\n"
            << "Choose [1]: ";
  std::string choice;
  std::getline(std::cin, choice);

  // On success the connection stays open: the UI starts right after setup and
  // uses this same object.
  const bool ok = (choice == "2") ? PairWithQr() : PairWithBrowser();
  m_IsSetup = false;
  return ok;
}

bool GmChat::PairWithBrowser()
{
  std::cout << "\n"
            << "What happens next, in plain words\n"
            << "\n"
            << "To sign in as you, the helper reads your Google sign-in cookies out of a\n"
            << "Chromium-based browser (Chrome, Chromium or Brave) on this machine. It looks\n"
            << "through every profile of those browsers, one after another, and uses the\n"
            << "first one that is signed in to Google Messages. Those cookies are encrypted on\n"
            << "disk, so the helper asks your desktop keyring for each browser's key (through\n"
            << "the secret-tool program) and your keyring may ask you to unlock it.\n"
            << "\n"
            << "The cookies are read and used by the helper. tui_chat itself never sees them.\n"
            << "Before you continue, make sure messages.google.com/web is open and loaded in\n"
            << "the browser profile you want to use.\n"
            << "\n";

  if (!AskYesNo("Let the helper read those cookies and start pairing"))
  {
    std::cout << "Pairing cancelled. Nothing was read.\n";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_PairEmoji.clear();
    m_HelperError.clear();
    m_HelperHint.clear();
  }

  GmReply reply = m_Client->Call("pairFromBrowser", "", 90000);
  if (!reply.ok)
  {
    std::string hint;
    {
      std::lock_guard<std::mutex> lock(m_StateMutex);
      hint = m_HelperHint;
    }

    std::cout << "Pairing could not start: " << reply.error << "\n";
    if (!hint.empty()) std::cout << hint << "\n";
    return false;
  }

  std::cout << "Waiting for Google to show a confirmation emoji...\n";

  // either the emoji arrives, or the helper reports a failure
  bool failed = false;
  const bool gotEmoji = WaitForState([this, &failed]()
  {
    if (!m_PairEmoji.empty()) return true;

    if ((m_HelperState == "error") || (m_HelperState == "connected"))
    {
      failed = (m_HelperState == "error");
      return true;
    }

    return false;
  }, 120000);

  std::string emoji;
  std::string error;
  std::string hint;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    emoji = m_PairEmoji;
    error = m_HelperError;
    hint = m_HelperHint;
  }

  if (!gotEmoji || failed || emoji.empty())
  {
    std::cout << "Pairing did not get as far as the emoji.\n";
    if (!error.empty()) std::cout << error << "\n";
    if (!hint.empty()) std::cout << hint << "\n";
    return false;
  }

  std::cout << "\n"
            << "On your phone, open Google Messages, go to Device pairing, and confirm this\n"
            << "emoji:\n"
            << "\n"
            << "    " << emoji << "\n"
            << "\n"
            << "Waiting for the phone (up to three minutes)...\n";

  WaitForState([this]()
  {
    return (m_HelperState == "connected") || (m_HelperState == "error") || (m_HelperState == "unpaired");
  }, 190000);

  std::string state;
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    state = m_HelperState;
    error = m_HelperError;
    hint = m_HelperHint;
  }

  if (state == "connected")
  {
    std::cout << "Paired. Google Messages is connected.\n";
    return true;
  }

  std::cout << "Pairing did not complete.\n";
  if (!error.empty()) std::cout << error << "\n";
  if (!hint.empty()) std::cout << hint << "\n";
  return false;
}

// Draws the code with qrencode when it is installed. The address comes from the
// helper, so it is passed as an argument and never through a shell.
static bool DrawQr(const std::string& p_Url)
{
  std::cout.flush();
  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0)
  {
    execlp("qrencode", "qrencode", "-t", "ANSIUTF8", "-m", "1", p_Url.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

bool GmChat::PairWithQr()
{
  {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_PairQrUrl.clear();
    m_HelperError.clear();
  }

  GmReply reply = m_Client->Call("startPairing", "", 30000);
  if (!reply.ok)
  {
    std::cout << "Pairing could not start: " << reply.error << "\n";
    return false;
  }

  std::string lastUrl;
  std::cout << "On your phone, open Google Messages, go to Device pairing, choose QR code\n"
            << "scanner and scan the code below. The code refreshes every 30 seconds.\n\n";

  const int64_t deadline = TimeUtil::GetCurrentTimeMSec() + 190000;
  while (TimeUtil::GetCurrentTimeMSec() < deadline)
  {
    std::string url;
    std::string state;
    {
      std::unique_lock<std::mutex> lock(m_StateMutex);
      m_StateCondVar.wait_for(lock, std::chrono::milliseconds(1000));
      url = m_PairQrUrl;
      state = m_HelperState;
    }

    if (state == "connected")
    {
      std::cout << "Paired. Google Messages is connected.\n";
      return true;
    }

    if ((state == "error") || (state == "unpaired"))
    {
      std::cout << "Pairing did not complete.\n";
      return false;
    }

    if (!url.empty() && (url != lastUrl))
    {
      lastUrl = url;
      if (!DrawQr(url))
      {
        std::cout << "(qrencode is not installed) Open this link as a QR code: " << url << "\n";
      }
    }
  }

  std::cout << "Timed out waiting for the phone.\n";
  return false;
}
