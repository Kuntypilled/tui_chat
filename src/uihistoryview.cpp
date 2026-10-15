// uihistoryview.cpp
//
// Copyright (c) 2019-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uihistoryview.h"

#include "appconfig.h"
#include "apputil.h"
#include "fileutil.h"
#include "log.h"
#include "protocolutil.h"
#include "strutil.h"
#include "timeutil.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uiimagecache.h"
#include "uimodel.h"

UiHistoryView::UiHistoryView(const UiViewParams& p_Params)
  : UiViewBase(p_Params)
{
  if (m_Enabled)
  {
    int hpad = (m_X == 0) ? 0 : 1;
    int vpad = 1;
    int paddedY = m_Y + vpad;
    int paddedX = m_X + hpad;
    m_PaddedH = m_H - (vpad * 2);
    m_PaddedW = m_W - (hpad * 2);
    m_PaddedWin = newwin(m_PaddedH, m_PaddedW, paddedY, paddedX);

    static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
    static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
    werase(m_Win);
    wbkgd(m_Win, attributeTextNormal | colorPairTextRecv | ' ');
    wrefresh(m_Win);
  }
}

UiHistoryView::~UiHistoryView()
{
  if (m_PaddedWin != nullptr)
  {
    delwin(m_PaddedWin);
    m_PaddedWin = nullptr;
  }
}

// Repeats a string, used for the dashes along a message box's borders.
static std::wstring RepeatWString(const std::wstring& p_Str, int p_Count)
{
  std::wstring result;
  for (int i = 0; i < p_Count; ++i)
  {
    result += p_Str;
  }

  return result;
}

// The label cut into the top border of a message's box: the sender's name followed by
// the details (time, read receipt, edited and pinned marks). It must come out no wider
// than p_MaxWidth columns. What gives way, in this order:
//   1. nothing, when it all fits
//   2. the marks (read receipt, edited, pinned), keeping the whole name and the time
//   3. the end of the name, replaced by an ellipsis, keeping the time
//   4. the time too, when not even a letter of the name fits beside it
static std::wstring FitHeaderLabel(const std::wstring& p_Sender, const std::wstring& p_Time,
                                   const std::wstring& p_Marks, int p_MaxWidth)
{
  static const std::wstring ellipsis = L"…";
  const int maxWidth = std::max(0, p_MaxWidth);

  if (StrUtil::WStringWidth(p_Sender + p_Time + p_Marks) <= maxWidth) return p_Sender + p_Time + p_Marks;
  if (StrUtil::WStringWidth(p_Sender + p_Time) <= maxWidth) return p_Sender + p_Time;

  // Cut the name down to the room beside the time, leaving one column for the ellipsis.
  // Room for at least one letter is needed.
  const int room = maxWidth - StrUtil::WStringWidth(p_Time);
  if (room >= 2)
  {
    return StrUtil::TrimPadWString(p_Sender, room - 1) + ellipsis + p_Time;
  }

  // Not even that fits, so show the name alone, cut to the width if it is too wide
  if (StrUtil::WStringWidth(p_Sender) <= maxWidth) return p_Sender;
  if (maxWidth >= 2) return StrUtil::TrimPadWString(p_Sender, maxWidth - 1) + ellipsis;

  return StrUtil::TrimPadWString(p_Sender, maxWidth);
}

void UiHistoryView::Draw()
{
  if (!m_Enabled || !m_Dirty) return;
  m_Dirty = false;

  curs_set(0);

  UiImageCache::BeginFrame();
  bool anyImagePending = false;

  static int colorPairTextSent = UiColorConfig::GetColorPair("history_text_sent_color");
  static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
  static int colorPairTextQuoted = UiColorConfig::GetColorPair("history_text_quoted_color");
  static int colorPairTextReaction = UiColorConfig::GetColorPair("history_text_reaction_color");
  static int colorPairTextAttachment = UiColorConfig::GetColorPair("history_text_attachment_color");
  static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
  static int attributeTextSelected = UiColorConfig::GetAttribute("history_text_attr_selected");

  static int colorPairNameSent = UiColorConfig::GetColorPair("history_name_sent_color");
  static int colorPairNameRecv = UiColorConfig::GetColorPair("history_name_recv_color");
  static int attributeNameNormal = UiColorConfig::GetAttribute("history_name_attr");
  static int attributeNameSelected = UiColorConfig::GetAttribute("history_name_attr_selected");

  static std::wstring attachmentIndicator =
    StrUtil::ToWString(UiConfig::GetStr("attachment_indicator") + " ");
  static std::wstring quoteIndicator = L"> ";

  std::pair<std::string, std::string>& currentChat = m_Model->GetCurrentChatLocked();
  const bool emojiEnabled = m_Model->GetEmojiEnabledLocked();
  static const bool developerMode = AppUtil::GetDeveloperMode();

  std::vector<std::string>& messageVec =
    m_Model->GetMessageVecLocked(currentChat.first, currentChat.second);
  std::unordered_map<std::string, ChatMessage>& messages =
    m_Model->GetMessagesLocked(currentChat.first, currentChat.second);
  int messageOffset = std::max(m_Model->GetMessageOffsetLocked(currentChat.first, currentChat.second), 0);

  // Messages are drawn no wider than a share of the pane, and the ones sent from
  // here sit against the right edge. That leaves the two sides of a conversation
  // visibly apart, as in a phone's message app.
  static const bool sentAlignRight = UiConfig::GetBool("history_sent_align_right");
  static const int maxWidthPercent = std::min(100, std::max(30, UiConfig::GetNum("history_max_width_percent")));
  const int wrapWidth = std::max(std::min(m_PaddedW, 20), (m_PaddedW * maxWidthPercent) / 100);

  // Each message sits in a box, a border and a space of padding either side of the text.
  const int contentWidth = std::max(1, wrapWidth - 4);

  werase(m_PaddedWin);
  wbkgd(m_PaddedWin, attributeTextNormal | colorPairTextRecv | ' ');

  m_HistoryShowCount = 0;

  bool firstMessage = true;
  int y = m_PaddedH - 1;
  for (auto it = std::next(messageVec.begin(), messageOffset); it != messageVec.end(); ++it)
  {
    bool isSelectedMessage = firstMessage && m_Model->GetSelectMessageActiveLocked();

    auto msgIt = messages.find(*it);
    if (msgIt == messages.end())
    {
      LOG_WARNING("message %s missing", it->c_str());
      continue;
    }

    ChatMessage& msg = msgIt->second;

    int attributeText = isSelectedMessage ? attributeTextSelected : attributeTextNormal;
    int colorPairText = [&]()
    {
      if (msg.isOutgoing) return colorPairTextSent;

      if (msg.senderId == currentChat.second) return colorPairTextRecv;

      static bool isUserColor = UiColorConfig::IsUserColor("history_text_recv_group_color");
      if (!isUserColor)
      {
        static int colorPairGroup = UiColorConfig::GetColorPair("history_text_recv_group_color");
        return colorPairGroup;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_text_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    std::vector<std::wstring> wlines;
    if (!msg.text.empty())
    {
      std::string text = msg.text;
      StrUtil::SanitizeMessageStr(text);
      if (!emojiEnabled)
      {
        text = StrUtil::Textize(text);
      }

      wlines = StrUtil::WordWrap(StrUtil::ToWString(text), contentWidth, false, false, false, 2);
    }

    // Quoted message
    if (!msg.quotedId.empty())
    {
      std::string quotedText;
      auto quotedIt = messages.find(msg.quotedId);
      if (quotedIt != messages.end())
      {
        if (!quotedIt->second.text.empty())
        {
          quotedText = StrUtil::Split(quotedIt->second.text, '\n').at(0);
          if (!emojiEnabled)
          {
            quotedText = StrUtil::Textize(quotedText);
          }
        }
        else if (!quotedIt->second.fileInfo.empty())
        {
          FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(quotedIt->second.fileInfo);
          quotedText = FileUtil::BaseName(fileInfo.filePath);
        }
      }
      else
      {
        m_Model->FetchCachedMessageLocked(currentChat.first, currentChat.second, msg.quotedId);
        quotedText = "";
      }

      int maxQuoteLen = contentWidth - 3;
      std::wstring quote = quoteIndicator + StrUtil::ToWString(quotedText);
      if (StrUtil::WStringWidth(quote) > maxQuoteLen)
      {
        quote = StrUtil::TrimPadWString(quote, maxQuoteLen) + L"...";
      }

      wlines.insert(wlines.begin(), quote);
    }

    // File attachment
    int imageCols = 0;
    int imageRows = 0;
    int photoId = 0;
    if (!msg.fileInfo.empty())
    {
      FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);

      // special case handling selection-triggered download, and handling cache's old setting
      static const bool isAttachmentPrefetchAll =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchAll);
      static const bool isAttachmentPrefetchSelected =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchSelected);
      static const bool isAttachmentPrefetchNone =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchNone);

      // A photo is fetched as soon as it is on screen when it is to be shown inline,
      // unless fetching is switched off altogether
      const bool isInlinePhoto = UiImageCache::Enabled() && !isAttachmentPrefetchNone &&
        UiImageCache::IsImageFile(fileInfo.filePath, fileInfo.fileType);
      if (isAttachmentPrefetchAll || isInlinePhoto || (isSelectedMessage && isAttachmentPrefetchSelected))
      {
        if (!UiModel::IsAttachmentDownloaded(fileInfo) && UiModel::IsAttachmentDownloadable(fileInfo))
        {
          m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                            fileInfo.fileId, DownloadFileActionNone);
          fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);
        }
      }

      std::string fileName = FileUtil::BaseName(fileInfo.filePath);
      std::string fileStatus;
      if (fileInfo.fileStatus == FileStatusNone)
      {
        // should not happen
        static const std::string statusNone = " -";
        fileStatus = statusNone;
      }
      else if (fileInfo.fileStatus == FileStatusNotDownloaded)
      {
        static const std::string statusNotDownloaded = " " + UiConfig::GetStr("downloadable_indicator");
        fileStatus = statusNotDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloaded)
      {
        static const std::string statusDownloaded = "";
        fileStatus = statusDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloading)
      {
        static const std::string statusDownloading = " " + UiConfig::GetStr("syncing_indicator");
        fileStatus = statusDownloading;
      }
      else if (fileInfo.fileStatus == FileStatusDownloadFailed)
      {
        static const std::string statusDownloadFailed = " " + UiConfig::GetStr("failed_indicator");
        fileStatus = statusDownloadFailed;
      }

      // A photo that is on disk is shown in the box. Until its thumbnail is made, or when it
      // cannot be shown, the file name line stays.
      int imageId = 0;
      if (UiImageCache::Enabled() && (fileInfo.fileStatus == FileStatusDownloaded) &&
          UiImageCache::IsImageFile(fileInfo.filePath, fileInfo.fileType))
      {
        const UiImage image = UiImageCache::Get(fileInfo.filePath, fileInfo.fileType);
        if (image.state == UiImage::Pending)
        {
          anyImagePending = true;
        }
        else if (image.state == UiImage::Ready)
        {
          const int maxCols = std::min(contentWidth, 40);
          const int maxRows = std::min(14, std::max(3, m_PaddedH - 6));
          UiImageCache::Layout(image, maxCols, maxRows, imageCols, imageRows);
          imageId = UiImageCache::Show(fileInfo.filePath, imageCols, imageRows);
          if (imageId == 0)
          {
            imageCols = 0;
            imageRows = 0;
          }
        }
      }

      if (imageId != 0)
      {
        // blank text for the cells the photo covers, so the box is sized around them
        wlines.insert(wlines.begin(), imageRows, std::wstring(imageCols, L' '));
      }
      else
      {
        std::wstring fileStr = attachmentIndicator + StrUtil::ToWString(fileName + fileStatus);
        wlines.insert(wlines.begin(), fileStr);
      }
      photoId = imageId;
    }

    // Reactions
    int reactionLines = 0;
    static bool reactionsEnabled = UiConfig::GetBool("reactions_enabled");
    if (reactionsEnabled)
    {
      std::string selfEmoji;
      auto sit = msg.reactions.senderEmojis.find(s_ReactionsSelfId);
      if (sit != msg.reactions.senderEmojis.end())
      {
        selfEmoji = sit->second;
      }

      // Allow also if we have self emoji, even if not yet consolidated into count
      if (!msg.reactions.emojiCounts.empty() || !selfEmoji.empty())
      {
        bool foundSelf = false;
        std::string reactionsText;
        std::multimap<float, std::string> emojiCountsSorted;
        for (const auto& emojiCount : msg.reactions.emojiCounts)
        {
          float count = emojiCount.second;
          if (emojiCount.first == selfEmoji)
          {
            count += 0.1; // for equal count, prioritize own selected reaction
            foundSelf = true;
          }

          emojiCountsSorted.insert(std::make_pair(count, emojiCount.first));
        }

        if (!foundSelf && !selfEmoji.empty())
        {
          LOG_DEBUG("insert missing reaction for self");
          emojiCountsSorted.insert(std::make_pair(1.1, selfEmoji));
        }

        bool firstReaction = true;
        for (auto emojiCount = emojiCountsSorted.rbegin(); emojiCount != emojiCountsSorted.rend(); ++emojiCount)
        {
          reactionsText += (firstReaction ? " " : "  ");
          if (emojiCount->second == selfEmoji)
          {
            // Highlight own reaction emoji
            reactionsText += "" + emojiCount->second + "*";
          }
          else
          {
            reactionsText += emojiCount->second;
          }

          if (emojiCount->first > 1.5)
          {
            reactionsText += " " + FileUtil::GetSuffixedCount(static_cast<ssize_t>(emojiCount->first));
          }

          firstReaction = false;
        }

        if (!reactionsText.empty())
        {
          if (!emojiEnabled)
          {
            reactionsText = StrUtil::Textize(reactionsText);
          }

          const int maxReactionsLen = contentWidth - 4;
          std::wstring reactions = StrUtil::ToWString(reactionsText);
          if (StrUtil::WStringWidth(reactions) > maxReactionsLen)
          {
            reactions = StrUtil::TrimPadWString(reactions, maxReactionsLen) + L"... ";
          }
          else
          {
            reactions += L" ";
          }

          wlines.insert(wlines.end(), reactions);
          reactionLines = 1;
        }
      }
    }

    // Leave room for the top and bottom border
    const int maxMessageLines = std::max(2, m_PaddedH - 2);
    if (firstMessage && ((int)wlines.size() > maxMessageLines))
    {
      wlines.resize(maxMessageLines - 1);
      wlines.push_back(L"[...]");
      reactionLines = 0;
    }

    // Name, time and receipt go on the top border of the message's box, so they are
    // worked out before anything is drawn: the box is as wide as the longest of them
    // and the text. A message cut off at the top of the pane is not marked as read, as
    // its name and time are not on screen.
    const int bodyLines = (int)wlines.size();
    const bool fitsOnScreen = ((y - bodyLines - 1) >= 0);

    int attributeName = isSelectedMessage ? attributeNameSelected : attributeNameNormal;
    int colorPairName = [&]()
    {
      if (msg.isOutgoing) return colorPairNameSent;

      if (msg.senderId == currentChat.second) return colorPairNameRecv;

      // A group's names follow the received name colour unless a colour of their own
      // is set, so the two sides of a group chat are told apart like those of a chat
      // with one person.
      static bool isUserColor = UiColorConfig::IsUserColor("history_name_recv_group_color");
      if (!isUserColor)
      {
        static bool hasGroupColor = UiColorConfig::HasForeground("history_name_recv_group_color");
        static int colorPairGroup = UiColorConfig::GetColorPair("history_name_recv_group_color");
        return hasGroupColor ? colorPairGroup : colorPairNameRecv;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_name_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    std::string name = m_Model->GetContactNameLocked(currentChat.first, msg.senderId);
    if (!emojiEnabled)
    {
      name = StrUtil::Textize(name);
    }

    std::wstring wsender = StrUtil::ToWString(name);
    std::wstring wtime;
    if (developerMode)
    {
      wtime = L" (" + StrUtil::ToWString(std::to_string(msg.timeSent)) + L")";
    }
    else
    {
      if (msg.timeSent != std::numeric_limits<int64_t>::max())
      {
        wtime = L" (" + StrUtil::ToWString(TimeUtil::GetTimeString(msg.timeSent, false /* p_IsExport */)) + L")";
      }
    }

    if (fitsOnScreen)
    {
      m_Model->MarkReadLocked(currentChat.first, currentChat.second, *it, (!msg.isOutgoing && !msg.isRead));
    }

    static const std::string readIndicator = " " + UiConfig::GetStr("read_indicator");
    static const std::string editedIndicator = " " + UiConfig::GetStr("edited_indicator");
    static const std::string pinnedIndicator = " " + UiConfig::GetStr("pinned_indicator");
    std::wstring wreceipt = StrUtil::ToWString(msg.isRead ? readIndicator : "");
    std::wstring wedited = StrUtil::ToWString(msg.isEdited ? editedIndicator : "");
    std::wstring wpinned = StrUtil::ToWString(msg.isPinned ? pinnedIndicator : "");
    std::wstring wmarks = wreceipt + wedited + wpinned;

    if (developerMode)
    {
      wmarks = wmarks +
        L" msg " + StrUtil::ToWString(msg.id) +
        L" user " + StrUtil::ToWString(msg.senderId);
    }

    // A box is a corner, a space of padding, the text, a space of padding and a corner,
    // so four columns wider than its text. The top border needs a corner, a dash and a
    // space before the label, and a space, at least one dash and a corner after it.
    const int maxLabelWidth = std::max(0, wrapWidth - 6);
    const std::wstring wlabel = FitHeaderLabel(wsender, wtime, wmarks, maxLabelWidth);
    const int labelWidth = StrUtil::WStringWidth(wlabel);

    int textWidth = 0;
    for (const std::wstring& wline : wlines)
    {
      textWidth = std::max(textWidth, StrUtil::WStringWidth(wline));
    }

    const int boxWidth = std::min(std::max(textWidth + 4, labelWidth + 6), wrapWidth);
    const int innerWidth = std::max(0, boxWidth - 4);
    const bool alignRight = sentAlignRight && msg.isOutgoing;
    const int boxX = alignRight ? std::max(0, m_PaddedW - boxWidth) : 0;

    // The border takes the name's colour but not its bold, so the label stands out
    // along the border. A selected message has its whole box reversed.
    const int borderStyle = (isSelectedMessage ? attributeNameSelected : (int)A_NORMAL) | colorPairName;
    const int labelStyle = attributeName | colorPairName;
    const int lineStyleNormal = attributeTextNormal | colorPairTextRecv;

    auto put = [&](int p_Y, int p_X, const std::wstring& p_Str, int p_Style)
    {
      if ((p_X < 0) || (p_X >= m_PaddedW)) return;
      wattron(m_PaddedWin, p_Style);
      mvwaddnwstr(m_PaddedWin, p_Y, p_X, p_Str.c_str(), std::min((int)p_Str.size(), m_PaddedW - p_X));
      wattroff(m_PaddedWin, p_Style);
    };

    // Bottom border
    put(y, boxX, L"└" + RepeatWString(L"─", boxWidth - 2) + L"┘", borderStyle);
    if (--y < 0) break;

    // Text, drawn from the last line upwards
    for (auto wline = wlines.rbegin(); wline != wlines.rend(); ++wline)
    {
      // the first imageRows lines of the message are the photo, one cell row each
      const int lineIndex = (int)wlines.size() - 1 - (int)std::distance(wlines.rbegin(), wline);
      if ((photoId != 0) && (lineIndex < imageRows))
      {
        put(y, boxX, L"│", borderStyle);
        put(y, boxX + 1, L" ", lineStyleNormal);
        put(y, boxX + boxWidth - 2, L" ", lineStyleNormal);
        put(y, boxX + boxWidth - 1, L"│", borderStyle);
        if (!UiImageCache::DrawRow(m_PaddedWin, y, boxX + 2, photoId, lineIndex, imageCols))
        {
          put(y, boxX + 2, L"[photo]", lineStyleNormal);
        }

        if (--y < 0) break;
        continue;
      }

      bool isAttachment = (wline->rfind(attachmentIndicator, 0) == 0);
      bool isQuote = (wline->rfind(quoteIndicator, 0) == 0);
      bool isReaction = (reactionLines == 1) && (std::distance(wline, wlines.rbegin()) == 0);

      int lineStyle = attributeText | colorPairText;
      if (isAttachment)
      {
        lineStyle = attributeText | colorPairTextAttachment;
      }
      else if (isQuote)
      {
        lineStyle = attributeText | colorPairTextQuoted;
      }
      else if (isReaction)
      {
        lineStyle = attributeTextNormal | colorPairTextReaction;
      }

      put(y, boxX, L"│", borderStyle);
      put(y, boxX + 1, L" " + StrUtil::TrimPadWString(*wline, innerWidth) + L" ", lineStyle);
      put(y, boxX + boxWidth - 1, L"│", borderStyle);

      if (--y < 0) break;
    }

    if (y < 0) break;

    // Top border with the name and time cut into it, a space either side
    if (labelWidth > 0)
    {
      put(y, boxX, L"┌─ ", borderStyle);
      put(y, boxX + 3, wlabel, labelStyle);
      put(y, boxX + 3 + labelWidth,
          L" " + RepeatWString(L"─", boxWidth - labelWidth - 5) + L"┐", borderStyle);
    }
    else
    {
      put(y, boxX, L"┌" + RepeatWString(L"─", boxWidth - 2) + L"┐", borderStyle);
    }

    ++m_HistoryShowCount;

    if (--y < 0) break;

    if (--y < 0) break;

    firstMessage = false;
  }

  wrefresh(m_PaddedWin);

  // keep drawing while thumbnails are being made, so photos appear as soon as they are
  if (anyImagePending)
  {
    m_Dirty = true;
  }
}

int UiHistoryView::GetHistoryShowCount()
{
  return m_HistoryShowCount;
}
