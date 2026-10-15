// uiimagecache.cpp
//
// Distributed under the MIT license, see LICENSE for details.

#include "uiimagecache.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <ctime>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "uicolorconfig.h"
#include "uiconfig.h"

namespace
{

// kitty's table of combining marks that stand for the numbers 0, 1, 2 and so on in a
// placeholder cell. The first sixty-four are enough for a photo of up to sixty-four
// columns and rows. Taken from kitty's own output for a large image.
const wchar_t s_Diacritics[] =
{
  0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B, 0x034C,
  0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
  0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592,
  0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
  0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615,
  0x0616, 0x0617, 0x0657, 0x0658,
};
const int s_DiacriticCount = (int)(sizeof(s_Diacritics) / sizeof(s_Diacritics[0]));

const wchar_t s_PlaceholderChar = 0x10EEEE;
const int s_FirstImageId = 16; // below this the terminal is sent a colour name, not an index
const int s_MaxImages = 64;
const int s_ThumbMaxPx = 640;
const int s_ThumbTimeoutMs = 20000;

struct Entry
{
  UiImage image;
  std::string thumbPath;
};

std::mutex s_Mutex;
std::condition_variable s_CondVar;
std::deque<std::pair<std::string, std::string>> s_Queue; // path, mime type
std::map<std::string, Entry> s_Entries;
std::thread s_Thread;
bool s_Stop = false;

struct Shown
{
  int id = 0;
  int cols = 0;
  int rows = 0;
  uint64_t lastFrame = 0;
};

// Only touched from the UI thread
std::map<std::string, Shown> s_Shown;
uint64_t s_Frame = 0;

std::string Base64(const std::string& p_In)
{
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < p_In.size())
  {
    uint32_t v = ((uint8_t)p_In[i] << 16) | ((uint8_t)p_In[i + 1] << 8) | (uint8_t)p_In[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
    i += 3;
  }

  if (i + 1 == p_In.size())
  {
    uint32_t v = ((uint8_t)p_In[i] << 16);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += "==";
  }
  else if (i + 2 == p_In.size())
  {
    uint32_t v = ((uint8_t)p_In[i] << 16) | ((uint8_t)p_In[i + 1] << 8);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += "=";
  }

  return out;
}

std::string LowerExt(const std::string& p_Path)
{
  const size_t slash = p_Path.find_last_of('/');
  const size_t dot = p_Path.find_last_of('.');
  if ((dot == std::string::npos) || ((slash != std::string::npos) && (dot < slash))) return "";

  std::string ext = p_Path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// The decoder to force, so the file's content is never sniffed into a different and
// more dangerous format. Empty when the type is not one that is shown.
std::string CoderFor(const std::string& p_Path, const std::string& p_MimeType)
{
  const std::string ext = LowerExt(p_Path);
  if ((ext == "jpg") || (ext == "jpeg")) return "jpeg";
  if (ext == "png") return "png";
  if (ext == "gif") return "gif";
  if (ext == "webp") return "webp";
  if ((ext == "heic") || (ext == "heif")) return "heic";
  if (ext == "avif") return "avif";

  if ((p_MimeType == "image/jpeg") || (p_MimeType == "image/jpg")) return "jpeg";
  if (p_MimeType == "image/png") return "png";
  if (p_MimeType == "image/gif") return "gif";
  if (p_MimeType == "image/webp") return "webp";
  if ((p_MimeType == "image/heic") || (p_MimeType == "image/heif")) return "heic";
  if (p_MimeType == "image/avif") return "avif";
  return "";
}

// The decoder named by the file's first bytes, from the same short list. A file's
// extension is a poor guide: the helper stores types it does not know as ".bin", and an
// iPhone photo may arrive as HEIC under a ".jpg" name.
std::string SniffCoder(const std::string& p_Path)
{
  FILE* f = fopen(p_Path.c_str(), "rb");
  if (f == nullptr) return "";

  unsigned char b[16] = { 0 };
  const size_t n = fread(b, 1, sizeof(b), f);
  fclose(f);

  if ((n >= 3) && (b[0] == 0xFF) && (b[1] == 0xD8) && (b[2] == 0xFF)) return "jpeg";
  if ((n >= 8) && (memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)) return "png";
  if ((n >= 4) && (memcmp(b, "GIF8", 4) == 0)) return "gif";
  if ((n >= 12) && (memcmp(b, "RIFF", 4) == 0) && (memcmp(b + 8, "WEBP", 4) == 0)) return "webp";
  if ((n >= 12) && (memcmp(b + 4, "ftyp", 4) == 0))
  {
    const std::string brand(reinterpret_cast<const char*>(b + 8), 4);
    if ((brand == "avif") || (brand == "avis")) return "avif";
    static const char* heicBrands[] = { "heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1" };
    for (const char* heic : heicBrands)
    {
      if (brand == heic) return "heic";
    }
  }

  return "";
}

bool FindOnPath(const std::string& p_Name)
{
  const char* path = getenv("PATH");
  if (path == nullptr) return false;

  std::string dirs = path;
  size_t start = 0;
  while (start <= dirs.size())
  {
    size_t end = dirs.find(':', start);
    if (end == std::string::npos) end = dirs.size();
    const std::string candidate = dirs.substr(start, end - start) + "/" + p_Name;
    if (access(candidate.c_str(), X_OK) == 0) return true;
    start = end + 1;
  }

  return false;
}

uint64_t Fnv1a(const std::string& p_Str)
{
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : p_Str)
  {
    h ^= c;
    h *= 1099511628211ULL;
  }

  return h;
}

std::string ThumbDir()
{
  static const std::string dir = FileUtil::GetApplicationDir() + "/thumbs";
  return dir;
}

// The width and height from a PNG's header, so no second program is needed
bool ReadPngSize(const std::string& p_Path, int& p_W, int& p_H)
{
  FILE* f = fopen(p_Path.c_str(), "rb");
  if (f == nullptr) return false;

  unsigned char b[24];
  const size_t n = fread(b, 1, sizeof(b), f);
  fclose(f);
  if (n != sizeof(b)) return false;

  static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (memcmp(b, sig, 8) != 0) return false;
  if (memcmp(b + 12, "IHDR", 4) != 0) return false;

  p_W = (int)((b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19]);
  p_H = (int)((b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23]);
  return (p_W > 0) && (p_H > 0);
}

// Runs ImageMagick without a shell, so a file name is never interpreted. Returns true
// on a zero exit within the time limit.
bool RunMagick(const std::string& p_Coder, const std::string& p_Src, const std::string& p_Dst)
{
  const std::string input = p_Coder + ":" + p_Src + "[0]";
  const std::string output = "png:" + p_Dst;
  const std::string geometry = std::to_string(s_ThumbMaxPx) + "x" + std::to_string(s_ThumbMaxPx) + ">";

  std::vector<const char*> argv =
  {
    "magick", "-limit", "memory", "512MiB", "-limit", "map", "512MiB", "-limit", "disk", "1GiB",
    "-limit", "time", "20", input.c_str(), "-auto-orient", "-thumbnail", geometry.c_str(), "-strip",
    output.c_str(), nullptr,
  };

  const pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0)
  {
    const int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0)
    {
      dup2(devNull, STDIN_FILENO);
      dup2(devNull, STDOUT_FILENO);
      dup2(devNull, STDERR_FILENO);
    }

    execvp("magick", const_cast<char* const*>(argv.data()));
    _exit(127);
  }

  int waited = 0;
  while (true)
  {
    int status = 0;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
    if (r < 0) return false;

    if (waited >= s_ThumbTimeoutMs)
    {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return false;
    }

    usleep(20 * 1000);
    waited += 20;
  }
}

void MakeThumb(const std::string& p_Src, const std::string& p_MimeType)
{
  Entry entry;
  entry.image.state = UiImage::Failed;

  do
  {
    struct stat st;
    if ((p_Src.empty()) || (p_Src[0] != '/') || (stat(p_Src.c_str(), &st) != 0) || !S_ISREG(st.st_mode)) break;

    std::string coder = SniffCoder(p_Src);
    if (coder.empty()) coder = CoderFor(p_Src, p_MimeType);
    if (coder.empty()) break;

    const std::string key = p_Src + "|" + std::to_string((long long)st.st_size) + "|" +
      std::to_string((long long)st.st_mtime);
    char name[40];
    snprintf(name, sizeof(name), "%016llx.png", (unsigned long long)Fnv1a(key));
    const std::string dir = ThumbDir();
    mkdir(dir.c_str(), 0700);
    const std::string dst = dir + "/" + name;

    int w = 0;
    int h = 0;
    if (!ReadPngSize(dst, w, h))
    {
      const std::string tmp = dst + ".part";
      unlink(tmp.c_str());
      if (!RunMagick(coder, p_Src, tmp) || (rename(tmp.c_str(), dst.c_str()) != 0) || !ReadPngSize(dst, w, h))
      {
        unlink(tmp.c_str());
        LOG_WARNING("could not make a thumbnail for %s", p_Src.c_str());
        break;
      }
    }

    entry.thumbPath = dst;
    entry.image.pxW = w;
    entry.image.pxH = h;
    entry.image.state = UiImage::Ready;
  } while (false);

  std::lock_guard<std::mutex> lock(s_Mutex);
  s_Entries[p_Src] = entry;
}

// Thumbnails are cheap to make again, so ones not written in a month are removed
void PruneThumbs()
{
  const std::string dir = ThumbDir();
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return;

  const time_t cutoff = time(nullptr) - (30 * 24 * 60 * 60);
  while (struct dirent* e = readdir(d))
  {
    const std::string name = e->d_name;
    if ((name.size() < 5) || (name.compare(name.size() - 4, 4, ".png") != 0)) continue;

    const std::string path = dir + "/" + name;
    struct stat st;
    if ((stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode) && (st.st_mtime < cutoff))
    {
      unlink(path.c_str());
    }
  }

  closedir(d);
}

void Worker()
{
  PruneThumbs();

  while (true)
  {
    std::string src;
    std::string mime;
    {
      std::unique_lock<std::mutex> lock(s_Mutex);
      s_CondVar.wait(lock, [] { return s_Stop || !s_Queue.empty(); });
      if (s_Stop) return;

      src = s_Queue.front().first;
      mime = s_Queue.front().second;
      s_Queue.pop_front();
    }

    MakeThumb(src, mime);
  }
}

void WriteTerminal(const std::string& p_Data)
{
  size_t done = 0;
  while (done < p_Data.size())
  {
    const ssize_t n = write(STDOUT_FILENO, p_Data.data() + done, p_Data.size() - done);
    if (n <= 0) return;
    done += (size_t)n;
  }
}

} // namespace

bool UiImageCache::Enabled()
{
  static const bool enabled = []()
  {
    if (!UiConfig::GetBool("inline_images")) return false;
    if (!isatty(STDOUT_FILENO)) return false;
    if (!has_colors() || (COLORS < 256) || (COLOR_PAIRS < 256)) return false;
    if (getenv("TMUX") != nullptr) return false;

    const char* term = getenv("TERM");
    const bool kitty = (getenv("KITTY_WINDOW_ID") != nullptr) ||
      ((term != nullptr) && (strstr(term, "kitty") != nullptr));
    if (!kitty) return false;

    if (!FindOnPath("magick"))
    {
      LOG_WARNING("inline images need ImageMagick (magick) on the path");
      return false;
    }

    return true;
  }();

  return enabled;
}

bool UiImageCache::IsImageFile(const std::string& p_Path, const std::string& p_MimeType)
{
  return !CoderFor(p_Path, p_MimeType).empty();
}

void UiImageCache::BeginFrame()
{
  ++s_Frame;
}

UiImage UiImageCache::Get(const std::string& p_FilePath, const std::string& p_MimeType)
{
  std::lock_guard<std::mutex> lock(s_Mutex);
  auto it = s_Entries.find(p_FilePath);
  if (it != s_Entries.end()) return it->second.image;

  // A name of a kind that is not shown, or one with no usable path, is not worth a thread
  if ((p_FilePath.empty()) || (p_FilePath[0] != '/') ||
      (CoderFor(p_FilePath, p_MimeType).empty() && SniffCoder(p_FilePath).empty()))
  {
    UiImage failed;
    failed.state = UiImage::Failed;
    return failed;
  }

  Entry pending;
  s_Entries[p_FilePath] = pending;
  s_Queue.push_back(std::make_pair(p_FilePath, p_MimeType));
  if (!s_Thread.joinable())
  {
    s_Thread = std::thread(Worker);
  }

  s_CondVar.notify_one();
  return pending.image;
}

void UiImageCache::Layout(const UiImage& p_Image, int p_MaxCols, int p_MaxRows, int& p_Cols, int& p_Rows)
{
  // A cell is roughly twice as tall as it is wide. The terminal's real size is used when
  // it reports one.
  double cellW = 10.0;
  double cellH = 20.0;
  struct winsize ws;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) && (ws.ws_xpixel > 0) && (ws.ws_ypixel > 0) &&
      (ws.ws_col > 0) && (ws.ws_row > 0))
  {
    cellW = (double)ws.ws_xpixel / ws.ws_col;
    cellH = (double)ws.ws_ypixel / ws.ws_row;
  }

  p_MaxCols = std::max(1, std::min(p_MaxCols, s_DiacriticCount));
  p_MaxRows = std::max(1, std::min(p_MaxRows, s_DiacriticCount));

  const double aspect = (double)p_Image.pxH / (double)std::max(1, p_Image.pxW);
  int cols = p_MaxCols;
  int rows = (int)((cols * cellW * aspect / cellH) + 0.5);
  if (rows > p_MaxRows)
  {
    rows = p_MaxRows;
    cols = (int)((rows * cellH / (cellW * aspect)) + 0.5);
  }

  p_Cols = std::max(1, std::min(cols, p_MaxCols));
  p_Rows = std::max(1, std::min(rows, p_MaxRows));
}

int UiImageCache::Show(const std::string& p_FilePath, int p_Cols, int p_Rows)
{
  std::string thumbPath;
  {
    std::lock_guard<std::mutex> lock(s_Mutex);
    auto it = s_Entries.find(p_FilePath);
    if ((it == s_Entries.end()) || (it->second.image.state != UiImage::Ready)) return 0;
    thumbPath = it->second.thumbPath;
  }

  auto shown = s_Shown.find(p_FilePath);
  if ((shown != s_Shown.end()) && (shown->second.cols == p_Cols) && (shown->second.rows == p_Rows))
  {
    shown->second.lastFrame = s_Frame;
    return shown->second.id;
  }

  int id = 0;
  if (shown != s_Shown.end())
  {
    id = shown->second.id; // same photo at a new size, send it again under the same id
  }
  else
  {
    // lowest unused id, else the one least recently drawn that is not drawn this frame
    std::vector<bool> used(s_MaxImages, false);
    for (const auto& s : s_Shown)
    {
      used[s.second.id - s_FirstImageId] = true;
    }

    for (int i = 0; i < s_MaxImages; ++i)
    {
      if (!used[i])
      {
        id = s_FirstImageId + i;
        break;
      }
    }

    if (id == 0)
    {
      auto oldest = s_Shown.end();
      for (auto s = s_Shown.begin(); s != s_Shown.end(); ++s)
      {
        if ((s->second.lastFrame != s_Frame) &&
            ((oldest == s_Shown.end()) || (s->second.lastFrame < oldest->second.lastFrame)))
        {
          oldest = s;
        }
      }

      if (oldest == s_Shown.end()) return 0; // everything is in use on screen right now

      id = oldest->second.id;
      s_Shown.erase(oldest);
    }
  }

  // Transmit and create a virtual placement in one go. Nothing moves the cursor and no
  // reply is asked for, so nothing comes back on the keyboard.
  const std::string seq = "\x1b_Ga=T,U=1,f=100,t=f,C=1,q=2,i=" + std::to_string(id) +
    ",c=" + std::to_string(p_Cols) + ",r=" + std::to_string(p_Rows) + ";" + Base64(thumbPath) + "\x1b\\";
  WriteTerminal(seq);

  Shown s;
  s.id = id;
  s.cols = p_Cols;
  s.rows = p_Rows;
  s.lastFrame = s_Frame;
  s_Shown[p_FilePath] = s;
  return id;
}

bool UiImageCache::DrawRow(WINDOW* p_Win, int p_Y, int p_X, int p_Id, int p_Row, int p_Cols)
{
  const int pair = UiColorConfig::GetForegroundPair(p_Id);
  if ((pair <= 0) || (p_Row < 0) || (p_Row >= s_DiacriticCount)) return false;

  for (int col = 0; (col < p_Cols) && (col < s_DiacriticCount); ++col)
  {
    wchar_t chars[4] = { s_PlaceholderChar, s_Diacritics[p_Row], s_Diacritics[col], 0 };
    cchar_t cell;
    if (setcchar(&cell, chars, A_NORMAL, (short)pair, nullptr) != OK) return false;
    mvwadd_wch(p_Win, p_Y, p_X + col, &cell);
  }

  return true;
}

bool UiImageCache::AnyPending()
{
  std::lock_guard<std::mutex> lock(s_Mutex);
  return !s_Queue.empty() ||
    std::any_of(s_Entries.begin(), s_Entries.end(),
                [](const std::pair<const std::string, Entry>& e) { return e.second.image.state == UiImage::Pending; });
}

void UiImageCache::Cleanup()
{
  {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Stop = true;
  }

  s_CondVar.notify_all();
  if (s_Thread.joinable())
  {
    s_Thread.join();
  }

  if (!s_Shown.empty())
  {
    WriteTerminal("\x1b_Ga=d,d=A,q=2\x1b\\");
    s_Shown.clear();
  }
}
