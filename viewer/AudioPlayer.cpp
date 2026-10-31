#include "viewer/AudioPlayer.h"

#include "host/MfRuntime.h"   // 复用 hrText()

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <QElapsedTimer>
#include <QMutexLocker>

#include <cstring>

namespace lanrdp {

namespace {

constexpr DWORD kPollTimeoutMs = 50;

// 设备侧 client 缓冲的**上限**（100 ns 单位 ⇒ 100 ms）。它不再等于「常驻延迟」：
// 一拍只补到 `kTargetQueuedMs`，这个值仅决定「一拍最多能写多少」，
// 以及设备线程被抢占时还有多少余量可吃。
constexpr REFERENCE_TIME kBufferDuration = 100 * 10000;

// 设备队列的目标深度（ms）。**这是听感延迟里最大的一项**，其余是抖动缓冲
// 60 ms + 攒块 20 ms。比抖动缓冲略小：两级缓冲职责清晰 —— 抖动缓冲管网络抖动，
// 设备队列只管「别让引擎断粮」。
constexpr int kTargetQueuedMs = 50;

constexpr int kMaxRebuildAttempts = 5;
constexpr int kRebuildDelayMs     = 200;

// 「已经稳定跑了这么久」才认为上一场故障结束、把重建预算清零。
// 少了这条，反复拔插蓝牙会变成「每次 open() 成功就重置」⇒ 每 200 ms 重建一次、
// 日志无限刷，还永远到不了上限。
constexpr qint64 kRebuildResetMs = 10000;

template <typename T>
void safeRelease(T *&pointer)
{
    if (pointer)
    {
        pointer->Release();
        pointer = nullptr;
    }
}

void fillS16Format(WAVEFORMATEXTENSIBLE *wf, quint32 sampleRate, quint16 channels)
{
    std::memset(wf, 0, sizeof(*wf));
    wf->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wf->Format.nChannels            = channels;
    wf->Format.nSamplesPerSec       = sampleRate;
    wf->Format.wBitsPerSample       = 16;
    wf->Format.nBlockAlign          = static_cast<WORD>(channels * 2);
    wf->Format.nAvgBytesPerSec      = sampleRate * wf->Format.nBlockAlign;
    wf->Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf->Samples.wValidBitsPerSample = 16;
    wf->dwChannelMask               = channels >= 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                                    : SPEAKER_FRONT_CENTER;
    wf->SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
}

// 一次「打开输出设备 → 播放 → 关闭」的全部 COM 句柄。
struct RenderStream
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice           *device     = nullptr;
    IAudioClient        *client     = nullptr;
    IAudioRenderClient  *render     = nullptr;
    HANDLE               event      = nullptr;
    UINT32               bufferFrames = 0;
    UINT16               blockAlign   = 0;

    ~RenderStream() { close(); }

    void close()
    {
        if (client)
            client->Stop();
        safeRelease(render);
        safeRelease(client);
        safeRelease(device);
        safeRelease(enumerator);
        if (event)
        {
            ::CloseHandle(event);
            event = nullptr;
        }
    }

    bool open(const AudioFormat &format, QString *error);
};

bool RenderStream::open(const AudioFormat &format, QString *error)
{
    close();

    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr))
    {
        *error = hrText("无法枚举音频设备", hr);
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
    {
        *error = hrText("找不到默认播放设备（这台机器可能没有可用的声音输出）", hr);
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(&client));
    if (FAILED(hr))
    {
        *error = hrText("无法打开音频客户端", hr);
        return false;
    }

    // 请求线上那套格式（S16 + 对端采样率 / 声道），让引擎负责重采样与格式转换
    //（`AUTOCONVERTPCM`）：本地声卡是 44.1k 还是 96k 都与对端无关。
    WAVEFORMATEXTENSIBLE requested;
    fillS16Format(&requested, format.sampleRate, format.channels);

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            kBufferDuration, 0, &requested.Format, nullptr);
    if (FAILED(hr))
    {
        *error = hrText("无法初始化音频输出（对端格式本机声卡不接受）", hr);
        return false;
    }

    blockAlign = requested.Format.nBlockAlign;

    event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
    {
        *error = QStringLiteral("无法创建音频事件对象");
        return false;
    }
    hr = client->SetEventHandle(event);
    if (FAILED(hr))
    {
        *error = hrText("无法绑定音频事件", hr);
        return false;
    }

    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render));
    if (FAILED(hr))
    {
        *error = hrText("无法获取音频渲染接口", hr);
        return false;
    }

    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0)
    {
        *error = hrText("读不到音频缓冲大小", hr);
        return false;
    }

    hr = client->Start();
    if (FAILED(hr))
    {
        *error = hrText("无法启动音频输出", hr);
        return false;
    }

    return true;
}

} // namespace

int framesToWrite(int padding, int bufferFrames, int targetFrames,
                  qsizetype availableBytes, int blockAlign)
{
    if (blockAlign <= 0 || bufferFrames <= 0 || targetFrames <= 0)
        return 0;

    const int freeFrames = bufferFrames - padding;
    if (freeFrames <= 0)
        return 0;

    // 只补到目标深度，**不写满空闲区** —— 写满就是把延迟钉在 client 缓冲长度上。
    const int deficit = targetFrames - padding;
    if (deficit <= 0)
        return 0;

    const qsizetype availableFrames = availableBytes / blockAlign;
    if (availableFrames <= 0)
        return 0;

    int frames = qMin(freeFrames, deficit);
    if (availableFrames < frames)
        frames = static_cast<int>(availableFrames);
    return frames;
}

AudioPlayer::AudioPlayer(QObject *parent)
    : QThread(parent)
{
    qRegisterMetaType<lanrdp::AudioFormat>("lanrdp::AudioFormat");
}

AudioPlayer::~AudioPlayer()
{
    stopAndWait();
}

bool AudioPlayer::begin(const AudioFormat &format)
{
    if (!isValidAudioFormat(format))
        return false;

    {
        QMutexLocker lock(&mutex_);
        if (hasFormat_ && format_.sampleRate == format.sampleRate
            && format_.channels == format.channels && format_.blockMs == format.blockMs
            && isRunning())
            return true;   // 同一格式，已经在跑

        // 换格式的口子从这里就关上：这期间到达的数据一律丢弃。否则旧线程还在跑，
        // 会拿**旧** blockAlign 去切**新**格式的字节 —— 进设备就是一段错位噪声。
        hasFormat_ = false;
        lastError_.clear();
    }

    // 顺序不能换：① 旧线程停到彻底退出 → ② 改格式 → ③ 起新线程。
    // 反过来（先 configure 再停）正好留出上面说的那个窗口。
    if (isRunning())
    {
        // 换格式（对端换了默认设备）：停掉再起。停是同步的（循环最长阻塞 50 ms）。
        stopRequested_.store(true);
        if (!wait(3000))
        {
            // ⚠️ 等不到就**必须放弃**，不能往下走：`start()` 对已在运行的线程是空操作，
            // 于是会变成「旧线程 + 新格式 + hasFormat_ 已置新」—— 又回到错位噪声那个窗口。
            // `hasFormat_` 入口已经置 false，这里不必再动（锁外写它本身就是数据竞争）。
            // `failed` 要发：这条路的后果和线程里放弃一样 —— 播放彻底不可用。
            const QString reason = QStringLiteral("音频播放线程未能在切换格式前退出");
            setError(reason);
            emit failed(reason);
            return false;
        }
    }

    buffer_.configure(format);   // 清掉旧格式残留，并按新格式重算对齐与上下限

    {
        QMutexLocker lock(&mutex_);
        format_    = format;
        hasFormat_ = true;
    }

    // 停机标志只能在启动路径清零：`start()` 与线程跑到 `run()` 之间若有人
    // `stopAndWait()`，`run()` 入口再清一次就把这次停机丢了（详见头文件）。
    stopRequested_.store(false);
    start();
    return true;
}

void AudioPlayer::push(const QByteArray &pcm)
{
    {
        QMutexLocker lock(&mutex_);
        if (!hasFormat_)
            return;
    }
    buffer_.push(pcm);
}

void AudioPlayer::reset()
{
    buffer_.clear();
    flushRequested_.store(true);
}

void AudioPlayer::stopAndWait()
{
    if (!isRunning())
        return;

    stopRequested_.store(true);
    if (!wait(3000))
    {
        // 同 AudioCapturer：不能留一个还在跑的 QThread 对象被析构。
        setError(QStringLiteral("音频播放线程未在 3 秒内退出"));
        wait();
    }
}

bool AudioPlayer::isActive() const
{
    return isRunning();
}

QString AudioPlayer::lastError() const
{
    QMutexLocker lock(&mutex_);
    return lastError_;
}

qsizetype AudioPlayer::droppedBytes() const
{
    return buffer_.droppedBytes();
}

quint64 AudioPlayer::underruns() const
{
    return buffer_.underruns();
}

int AudioPlayer::queuedMs() const
{
    return queuedMs_.load();
}

quint64 AudioPlayer::flushCount() const
{
    return flushCount_.load();
}

quint64 AudioPlayer::starvedCount() const
{
    return starvedCount_.load();
}

void AudioPlayer::setError(const QString &text)
{
    QMutexLocker lock(&mutex_);
    lastError_ = text;
}

void AudioPlayer::run()
{
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr))
    {
        const QString reason = hrText("音频播放线程无法初始化 COM", comHr);
        setError(reason);
        emit failed(reason);
        return;
    }

    AudioFormat format;
    {
        QMutexLocker lock(&mutex_);
        format = format_;
    }

    RenderStream stream;
    int          rebuilds = 0;
    bool         gaveUp   = false;
    QElapsedTimer streamUp;   // 本次成功打开已经跑了多久（未打开过则 invalid）

    for (;;)
    {
        if (stopRequested_.load())
            break;

        QString error;
        if (!stream.open(format, &error))
        {
            if (stopRequested_.load())
                break;
            // 只有「上一段稳定跑过一阵」才算新一场故障，否则预算照扣 —— 见 kRebuildResetMs。
            // 清预算必须同时把计时作废：不清的话 `streamUp` 还挂着「够了 10 s」这个事实，
            // 下一轮打开又失败会再清一次 ⇒ 预算永远扣不满，永不放弃。
            if (streamUp.isValid() && streamUp.elapsed() >= kRebuildResetMs)
            {
                rebuilds = 0;
                streamUp.invalidate();
            }
            if (++rebuilds > kMaxRebuildAttempts)
            {
                setError(error);
                emit failed(error);
                gaveUp = true;
                break;
            }
            msleep(kRebuildDelayMs);
            continue;
        }

        streamUp.start();
        bool rebuild = false;
        bool writing = false;   // 上一拍真的写了数据（用来区分「播着播着断了」与「本来就静音」）
        while (!stopRequested_.load() && !rebuild)
        {
            ::WaitForSingleObject(stream.event, kPollTimeoutMs);

            // 掐掉设备里已排的数据（切格 / 关开关）。必须在渲染线程做：client 属于
            // 本线程的 COM 单元；且 **Stop 必须在前** —— 共享流上对未停止的流调
            // Reset 会返回 AUDCLNT_E_NOT_STOPPED。
            if (flushRequested_.exchange(false))
            {
                stream.client->Stop();
                const HRESULT flushHr = stream.client->Reset();
                stream.client->Start();
                // **只认成功**：无条件自增会让「Reset 其实失败了」也看起来像清过，
                // 用例跟着一起骗人（failed 的那个分支只能靠日志看出来）。
                if (SUCCEEDED(flushHr))
                {
                    flushCount_.fetch_add(1);
                }
                else
                {
                    // 只记错误，不发 `failed` —— 那个信号的含义是「播放彻底不可用」
                    //（主窗口会改状态栏），而这里只是「这次没掐干净」。
                    setError(hrText("清空音频输出缓冲失败（会听到上一路的声音尾巴）", flushHr));
                }
            }

            UINT32 padding = 0;
            HRESULT hr    = stream.client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING)
            {
                rebuild = true;
                break;
            }
            if (FAILED(hr))
            {
                const QString reason = hrText("读取音频输出状态失败", hr);
                setError(reason);
                emit failed(reason);
                gaveUp = true;
                break;
            }

            const int sampleRate   = static_cast<int>(format.sampleRate);
            const int targetFrames = sampleRate * kTargetQueuedMs / 1000;
            const int frames       = framesToWrite(static_cast<int>(padding),
                                                   static_cast<int>(stream.bufferFrames),
                                                   targetFrames,
                                                   buffer_.bufferedBytes(),
                                                   static_cast<int>(stream.blockAlign));

            queuedMs_.store((static_cast<int>(padding) + frames) * 1000 / sampleRate);

            // 队列已达目标 ⇒ 什么都不写，让引擎照常消化已排的帧。
            // 没数据可写 ⇒ 同样不写：补静音凑满等于把延迟推到目标深度上去。
            if (frames == 0)
            {
                // 真「断流」= 设备队列已经空了、手上又没数据 ⇒ 听众那边是真的没声了。
                // 队列里还有帧（padding > 0）不算：那只是我们按节流故意没写。
                // 也**不按拍计数** —— 对端一直静音会让它 20 次/秒地涨，日志跟着刷屏。
                if (writing && padding == 0)
                    starvedCount_.fetch_add(1);
                writing = false;
                continue;
            }

            const qsizetype want  = static_cast<qsizetype>(frames) * stream.blockAlign;
            const auto      taken = buffer_.take(want);
            if (taken.data.isEmpty())
            {
                writing = false;
                continue;
            }

            BYTE *dst = nullptr;
            hr = stream.render->GetBuffer(static_cast<UINT32>(frames), &dst);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING)
            {
                rebuild = true;
                break;
            }
            if (FAILED(hr) || !dst)
            {
                const QString reason = hrText("无法取得音频输出缓冲", hr);
                setError(reason);
                emit failed(reason);
                gaveUp = true;
                break;
            }

            const qsizetype bytes = qMin(taken.data.size(), want);
            std::memcpy(dst, taken.data.constData(), static_cast<std::size_t>(bytes));
            if (bytes < want)
            {
                // 尾巴补 0 而不是把未初始化内存丢给设备（那是噪声，也是信息泄漏）。
                std::memset(dst + bytes, 0, static_cast<std::size_t>(want - bytes));
            }
            stream.render->ReleaseBuffer(static_cast<UINT32>(frames), 0);
            writing = true;
        }

        stream.close();
        if (gaveUp)
            break;
        msleep(kRebuildDelayMs);
    }

    ::CoUninitialize();
}

} // namespace lanrdp
