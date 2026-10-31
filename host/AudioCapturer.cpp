#include "host/AudioCapturer.h"

#include "host/MfRuntime.h"   // 复用 hrText()：HRESULT → 可读文案

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <QElapsedTimer>
#include <QMutexLocker>

#include <cmath>
#include <cstring>

namespace lanrdp {

namespace {

// 采集循环单轮最长阻塞。50 ms 是「静默期轮询」与「停机响应」的折中：
// 更短白烧 CPU，更长则一安静下来就要等很久才发现有新数据。
constexpr DWORD kPollTimeoutMs = 50;

// 事件驱动模式下 client 自己的缓冲深度（100 ns 单位 ⇒ 200 ms）。
// 它只决定「我们来得及取之前能缓冲多久」，与攒块的 20 ms 无关。
constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;

// 设备失效后最多连续重建几次。无人值守时无限重试只会刷日志、也救不回来。
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

// 组装一个「S16 / PCM」的 WAVEFORMATEXTENSIBLE，向引擎请求这个格式。
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

// float32 → S16LE。超出 ±1.0 削顶，NaN 当 0：
// 不削顶会整数溢出成刺耳的爆音，NaN 转整数是未定义行为。
void convertFloatToS16(const float *src, qint64 samples, QByteArray *out)
{
    const qsizetype base = out->size();
    out->resize(base + samples * 2);
    char *dst = out->data() + base;
    for (qint64 i = 0; i < samples; ++i)
    {
        float v = src[i];
        if (!(v == v))          // NaN
            v = 0.0f;
        else if (v > 1.0f)
            v = 1.0f;
        else if (v < -1.0f)
            v = -1.0f;

        const int      s = static_cast<int>(std::lround(v * 32767.0f));
        const quint16  u = static_cast<quint16>(static_cast<qint16>(s));
        dst[i * 2]       = static_cast<char>(u & 0xFFu);
        dst[i * 2 + 1]   = static_cast<char>((u >> 8) & 0xFFu);
    }
}

// 一次「打开设备 → 采集 → 关闭」的全部 COM 句柄。析构即释放，异常路径不必逐个回来清。
struct DeviceStream
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice           *device     = nullptr;
    IAudioClient        *client     = nullptr;
    IAudioCaptureClient *capture    = nullptr;
    HANDLE               event      = nullptr;

    AudioFormat format;
    bool        float32 = false;

    ~DeviceStream() { close(); }

    void close()
    {
        if (client)
            client->Stop();
        safeRelease(capture);
        safeRelease(client);
        safeRelease(device);
        safeRelease(enumerator);
        if (event)
        {
            ::CloseHandle(event);
            event = nullptr;
        }
    }

    bool    open(QString *error);
    HRESULT read(QByteArray *pcm);
};

bool DeviceStream::open(QString *error)
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

    // loopback 抓的是**渲染端**：取默认播放设备（eRender + eConsole），不需要管理员权限。
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
    {
        *error = hrText("找不到默认播放设备（这台机器可能没有可用的声音输出）", hr);
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    // 首选：请求 S16 / 48 kHz / 立体声，让引擎替我们重采样与格式转换
    //（`AUTOCONVERTPCM` 就是「省掉整个重采样模块」的那面旗）。
    {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&client));
        if (FAILED(hr))
        {
            *error = hrText("无法打开音频客户端", hr);
            return false;
        }

        WAVEFORMATEXTENSIBLE requested;
        fillS16Format(&requested, 48000, 2);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                kBufferDuration, 0, &requested.Format, nullptr);
        if (SUCCEEDED(hr))
        {
            format          = AudioFormat();
            format.sampleRate = 48000;
            format.channels   = 2;
            format.blockMs    = 20;
            float32           = false;
        }
        else
        {
            // 失败的 Initialize 会让 client 不可再用 ⇒ 整个重新 Activate。
            safeRelease(client);
        }
    }

    if (!client)
    {
        // 退路：按设备自己的混音格式开流，自己转。
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&client));
        if (FAILED(hr))
        {
            *error = hrText("无法打开音频客户端", hr);
            return false;
        }

        WAVEFORMATEX *mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix)
        {
            *error = hrText("读不到设备的混音格式", hr);
            return false;
        }

        const WORD tag     = mix->wFormatTag;
        const bool ext     = (tag == WAVE_FORMAT_EXTENSIBLE);
        const auto *extFmt = ext ? reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix) : nullptr;
        const bool isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT)
                             || (extFmt && extFmt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const bool isPcm16 = mix->wBitsPerSample == 16
                             && ((tag == WAVE_FORMAT_PCM)
                                 || (extFmt && extFmt->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

        const quint32 rate     = mix->nSamplesPerSec;
        const quint16 channels = mix->nChannels;
        const WORD    bits     = mix->wBitsPerSample;
        const bool    usable   = isFloat || isPcm16;
        const bool    channelsOk = channels >= 1 && channels <= kAudioMaxChannels;

        if (usable && channelsOk)
        {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, mix,
                                    nullptr);
        }
        ::CoTaskMemFree(mix);

        if (!usable)
        {
            *error = QStringLiteral("系统音频格式不受支持（%1 Hz / %2 声道 / %3 位）")
                         .arg(rate)
                         .arg(channels)
                         .arg(bits);
            return false;
        }
        if (!channelsOk)
        {
            // ⚠️ 不能把声道数「裁到 8」凑合：`read()` 里 samples = frames × channels，
            // 裁剪后算出的字节数与设备真实布局对不上，退路会产出错位噪声。
            // 线上格式也表达不了 >8 声道 ⇒ 直接判不支持，让用户看到明确原因。
            *error = QStringLiteral("系统音频是 %1 声道（超过支持的 %2），无法采集")
                         .arg(channels)
                         .arg(kAudioMaxChannels);
            return false;
        }
        if (FAILED(hr))
        {
            *error = hrText("无法初始化音频采集", hr);
            return false;
        }

        format.sampleRate    = rate;
        format.channels      = static_cast<quint8>(channels);
        format.bitsPerSample = kAudioBitsPerSample;
        format.blockMs       = 20;
        float32              = isFloat;
    }

    if (!isValidAudioFormat(format))
    {
        *error = QStringLiteral("系统音频格式超出支持范围（%1 Hz / %2 声道）")
                     .arg(format.sampleRate)
                     .arg(format.channels);
        return false;
    }

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

    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
    if (FAILED(hr))
    {
        *error = hrText("无法获取音频采集接口", hr);
        return false;
    }

    hr = client->Start();
    if (FAILED(hr))
    {
        *error = hrText("无法启动音频采集", hr);
        return false;
    }

    return true;
}

HRESULT DeviceStream::read(QByteArray *pcm)
{
    for (;;)
    {
        UINT32 packetFrames = 0;
        HRESULT hr = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr))
            return hr;
        if (packetFrames == 0)
            return S_OK;

        BYTE  *data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY)
            return S_OK;
        if (FAILED(hr))
            return hr;

        const qint64 samples = static_cast<qint64>(frames) * format.channels;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            // 静音包：`data` 可能是空指针，但**帧数是真的** —— 必须按帧数补 0。
            // 跳过它等于把时间轴上这一段剪掉，攒出来的块就与真实时长对不上了。
            pcm->append(static_cast<int>(samples * 2), '\0');
        }
        else if (float32)
        {
            convertFloatToS16(reinterpret_cast<const float *>(data), samples, pcm);
        }
        else if (data && samples > 0)
        {
            pcm->append(reinterpret_cast<const char *>(data), samples * 2);
        }

        capture->ReleaseBuffer(frames);
    }
}

} // namespace

AudioCapturer::AudioCapturer(QObject *parent)
    : QThread(parent)
{
    qRegisterMetaType<lanrdp::AudioFormat>("lanrdp::AudioFormat");
}

AudioCapturer::~AudioCapturer()
{
    stopAndWait();
}

void AudioCapturer::stopAndWait()
{
    if (!isRunning())
        return;

    stopRequested_.store(true);
    if (!wait(3000))
    {
        // 不该发生：循环最长阻塞 50 ms。等不到通常意味着某个设备调用卡住了 ——
        // 此时**只能继续等**：让线程在对象析构后仍活着是未定义行为（Qt 会打印
        // "Destroyed while thread is still running" 并可能当场崩）。
        setError(QStringLiteral("音频采集线程未在 3 秒内退出"));
        wait();
    }
}

QString AudioCapturer::lastError() const
{
    QMutexLocker lock(&mutex_);
    return lastError_;
}

AudioFormat AudioCapturer::format() const
{
    QMutexLocker lock(&mutex_);
    return format_;
}

void AudioCapturer::setError(const QString &text)
{
    QMutexLocker lock(&mutex_);
    lastError_ = text;
}

void AudioCapturer::startCapture()
{
    // 停止标志只能在**启动路径**清零，不能放在 `run()` 入口：
    // `start()` 与线程真正跑到 `run()` 之间有一个窗口，若这段时间里有人
    // `stopAndWait()`，`run()` 那一句会把刚置上的 true 清回 false —— 这次停机
    // 就丢了（线程照跑，`wait()` 白等三秒）。放在这里，`start()` 返回时标志
    // 必然已经是 false，任何随后的 `stopAndWait()` 都不会被覆盖。
    stopRequested_.store(false);
    start();
}

void AudioCapturer::run()
{
    // COM 是**线程局部**的：这个线程自己初始化、自己撤销。
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr))
    {
        const QString reason = hrText("音频采集线程无法初始化 COM", comHr);
        setError(reason);
        emit failed(reason);
        return;
    }

    AudioBlockAssembler assembler;
    QElapsedTimer      clock;
    DeviceStream       stream;

    int  rebuilds = 0;
    bool gaveUp   = false;
    QElapsedTimer streamUp;   // 本次成功打开已经跑了多久（未打开过则 invalid）

    for (;;)
    {
        if (stopRequested_.load())
            break;

        QString error;
        if (!stream.open(&error))
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
            // 「一开始就打不开」与「用到一半设备没了」同一条路：退避重试若干次。
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

        {
            QMutexLocker lock(&mutex_);
            format_ = stream.format;
            lastError_.clear();
        }
        assembler.setBlockBytes(audioBlockBytes(stream.format));
        assembler.clear();
        clock.start();
        streamUp.start();
        emit formatChanged(stream.format);

        bool rebuild = false;
        while (!stopRequested_.load() && !rebuild)
        {
            // ⚠️ 静默期 WASAPI **不产生事件**（loopback 抓的是渲染端，没人播放就什么都
            // 没有）。所以超时也要照常读一次 —— 少了这一步就会「一安静就再也不出声」。
            ::WaitForSingleObject(stream.event, kPollTimeoutMs);

            QByteArray     pcm;
            const HRESULT  readHr = stream.read(&pcm);
            if (readHr == AUDCLNT_E_DEVICE_INVALIDATED || readHr == AUDCLNT_E_SERVICE_NOT_RUNNING)
            {
                rebuild = true;   // 换默认设备 / 拔蓝牙：重建后重来
                break;
            }
            if (FAILED(readHr))
            {
                const QString reason = hrText("读取音频数据失败", readHr);
                setError(reason);
                emit failed(reason);
                gaveUp = true;
                break;
            }

            if (pcm.isEmpty())
                continue;

            const auto blocks = assembler.append(pcm.constData(), pcm.size());
            for (const QByteArray &block : blocks)
                emit chunkReady(clock.elapsed(), block, stream.format);
        }

        stream.close();
        if (gaveUp)
            break;

        // 设备刚切走时立刻重取常常又拿到旧的那一个，歇一下再说。
        msleep(kRebuildDelayMs);
    }

    ::CoUninitialize();
}

} // namespace lanrdp
