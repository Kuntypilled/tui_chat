// gmchat.h
//
// Google Messages through the omachatd helper. This adapter only translates
// between nchat's Protocol interface and the helper's JSON socket. All pairing
// and Google protocol work stays in the helper.
//
// tui_chat is not affiliated with nchat, OmaChat or Google.
//
// Distributed under the MIT license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "config.h"
#include "gmclient.h"
#include "protocol.h"

class GmChat : public Protocol
{
public:
  GmChat();
  virtual ~GmChat();
  static std::string GetName() { return "GoogleMessages"; }
  static std::string GetLibName() { return "libgmchat"; }
  static std::string GetCreateFunc() { return "CreateGmChat"; }
  static std::string GetSetupMessage() { return ""; }
  std::string GetProfileId() const;
  std::string GetProfileDisplayName() const;
  bool HasFeature(ProtocolFeature p_ProtocolFeature) const;
  bool IsGroupChat(const std::string& p_ChatId) const;
  std::string GetSelfId() const;

  bool SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId);
  bool LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId);
  bool CloseProfile();

  bool Login();
  bool Logout();

  void SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage);
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler);

private:
  enum EstablishResult
  {
    EstablishReady,
    EstablishNotPaired,
    EstablishHelperError,
    EstablishNoHelper,
    EstablishFailed,
  };

  struct Cursor
  {
    std::string id;
    int64_t timeUs = 0;
  };

  void InitConfig();
  void AttachClient();
  void Process();
  void QueueTask(const std::function<void()>& p_Task);
  void PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage);

  // helper connection
  std::string GetSocketPath() const;
  bool StartHelper(bool p_Verbose);
  bool ConnectWithRetry(int p_TimeoutMs);
  EstablishResult Establish(bool p_Verbose);
  void OnEvent(const std::string& p_Event, const rapidjson::Value& p_Data);
  void OnDisconnect();
  void ApplyState(const std::string& p_State);

  // requests
  void DoGetChats(const std::unordered_set<std::string>& p_ChatIds);
  void DoGetMessages(const std::string& p_ChatId, const std::string& p_FromMsgId, int32_t p_Limit);
  void DoSendMessage(const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  void DoMarkRead(const std::string& p_ChatId, const std::string& p_MsgId);

  // conversion
  void ConvertConversation(const rapidjson::Value& p_Conv, ChatInfo& p_Chat, std::vector<ContactInfo>& p_Contacts);
  bool ConvertMessage(const rapidjson::Value& p_Msg, ChatMessage& p_ChatMessage, std::string& p_ChatId,
                      std::vector<ContactInfo>& p_Contacts, std::string& p_TmpId);
  void Emit(std::shared_ptr<ServiceMessage> p_ServiceMessage);

  // photos, fetched through the helper on a thread of their own so a slow download does
  // not hold up sending
  struct MediaJob
  {
    std::string chatId;
    std::string msgId;
    std::string fileId;
    DownloadFileAction action = DownloadFileActionNone;
  };

  void DoDownloadFile(const std::string& p_ChatId, const std::string& p_MsgId, const std::string& p_FileId,
                      DownloadFileAction p_Action);
  void ProcessMedia();
  void FetchMedia(const MediaJob& p_Job);

  // setup wizard
  bool WaitForState(const std::function<bool()>& p_Predicate, int p_TimeoutMs);
  bool PairWithBrowser();
  bool WaitForPairing();
  bool PairWithQr();

private:
  std::string m_ProfileId;
  std::string m_ProfileDir;
  Config m_Config;
  std::unique_ptr<GmClient> m_Client;
  std::function<void(std::shared_ptr<ServiceMessage>)> m_MessageHandler;

  bool m_Running = false;
  std::thread m_Thread;
  std::deque<std::function<void()>> m_Tasks;
  std::mutex m_ProcessMutex;
  std::condition_variable m_ProcessCondVar;
  bool m_NeedReconnect = false;
  bool m_IsSetup = false;
  bool m_Active = false;

  mutable std::mutex m_StateMutex;
  std::condition_variable m_StateCondVar;
  std::string m_HelperState;
  std::string m_HelperError;
  std::string m_HelperHint;
  std::string m_PairEmoji;
  std::string m_PairQrUrl;
  bool m_Connected = false;
  std::string m_SelfId;
  std::set<std::string> m_GroupChats;
  std::map<std::string, bool> m_ChatUnread;
  std::map<std::string, std::string> m_SentTmpIds; // provisional id -> chat id
  std::map<std::string, Cursor> m_Cursors; // chat id -> where the last page ended
  std::map<std::string, int64_t> m_MessageTimeUs; // message id -> helper timestamp
  std::map<std::string, std::string> m_MediaTypes; // attachment key -> mime type

  std::thread m_MediaThread;
  std::deque<MediaJob> m_MediaQueue;
  std::mutex m_MediaMutex;
  std::condition_variable m_MediaCondVar;
  bool m_MediaStop = false;
};

extern "C" GmChat* CreateGmChat();
