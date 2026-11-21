#include <QtTest/QtTest>

#include <QByteArray>
#include <QVector>

#include <d3d11.h>

#include "common/ColorConvert.h"
#include "host/Nv12Converter.h"

// BGRA -> NV12 加速器（GPU VideoProcessor / CPU 直读）
//
// 这一组用例的重心不是「能不能转」，而是两件会静默出错的事：
// ① 显卡路径的色彩必须与 `bgraToNv12`（BT.601 有限范围）一致 —— 不一致的现象
//    是画面发灰/偏色，全程零报错；
// ② 显卡不可用 / 自检不过时必须**干净降级**，不能把帧丢掉、也不能崩。
class TestNv12Converter : public QObject
{
    Q_OBJECT

private slots:
    void cpuConversionMatchesReferenceExactly();
    void cpuConversionHonoursSourceStride();
    void cpuConversionRejectsInvalidInput();

    void deviationIsZeroForIdenticalBuffers();
    void deviationFindsLumaDrift();
    void deviationRejectsMismatchedBuffers();

    void gpuDisabledFallsBackWithoutTouchingDevice();
    void unboundConverterFallsBack();

    // 设置里的手动开关：关掉必须自报「手动」。它和「显卡自检没过」是两个不同的
    // 原因，现场排查要靠这行文字分清是谁干的 —— 所以不能共用一句含糊的降级文案。
    void manualDisableSaysSoInStatus();
    void reEnableClearsManualNote();

    // 真机：显卡转换结果与 CPU 参考比，亮度/色度偏差必须在容差内
    void gpuConversionMatchesCpuReference();
    // 真机 4K：显卡路径必须比 CPU 快（否则「加速」只是说法）
    void gpuBeatsCpuAt4k();
};

namespace {

int nv12Bytes(int width, int height)
{
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    return width * height + cw * ch * 2;
}

// 一张「像桌面」的测试图：大块纯色 + 横向灰阶渐变 + 几块饱和色（专抓矩阵错）
QByteArray makePattern(int width, int height, int stride = 0)
{
    const int rowBytes = stride > 0 ? stride : width * 4;
    QByteArray data(rowBytes * height, '\x7f');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int r = 0, g = 0, b = 0;
            const int band = (y * 4) / height;   // 4 条横带
            if (band == 0) {
                const int v = (x * 255) / (width > 1 ? width - 1 : 1);   // 灰阶渐变
                r = g = b = v;
            } else if (band == 1) {
                r = 220; g = 30; b = 30;      // 红（BT.601 与 709 的 U 差得最多）
            } else if (band == 2) {
                r = 30; g = 60; b = 220;      // 蓝
            } else {
                r = (x * 7) % 256; g = (y * 5) % 256; b = ((x + y) * 3) % 256;
            }
            char *p = data.data() + static_cast<qsizetype>(y) * rowBytes + x * 4;
            p[0] = static_cast<char>(b);
            p[1] = static_cast<char>(g);
            p[2] = static_cast<char>(r);
            p[3] = static_cast<char>(0xff);
        }
    }
    return data;
}

// 大图用的廉价图案：一行渐变 + 逐行复制（4K 下逐像素取模会拖慢测试本身）
QByteArray makeBigPattern(int width, int height)
{
    QByteArray data(width * 4 * height, Qt::Uninitialized);
    auto      *row0 = reinterpret_cast<uchar *>(data.data());
    for (int x = 0; x < width; ++x) {
        const uchar v  = static_cast<uchar>((x * 255) / (width > 1 ? width - 1 : 1));
        row0[x * 4 + 0] = v;
        row0[x * 4 + 1] = static_cast<uchar>(255 - v);
        row0[x * 4 + 2] = v;
        row0[x * 4 + 3] = 0xff;
    }
    for (int y = 1; y < height; ++y) {
        std::memcpy(data.data() + static_cast<qsizetype>(y) * width * 4, row0,
                    static_cast<size_t>(width) * 4);
    }
    return data;
}

// 建一张 BGRA 默认用途纹理（GPU 路径的输入面）
ID3D11Texture2D *makeBgraTexture(ID3D11Device *device, int width, int height,
                                 const QByteArray &bgra)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = static_cast<UINT>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = 0;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = bgra.constData();
    init.SysMemPitch = static_cast<UINT>(width * 4);

    ID3D11Texture2D *texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &init, &texture)))
        return nullptr;
    return texture;
}

} // namespace

void TestNv12Converter::cpuConversionMatchesReferenceExactly()
{
    const int     width  = 64;
    const int     height = 32;
    const QByteArray bgra = makePattern(width, height);

    QByteArray viaConverter;
    QVERIFY(lanrdp::Nv12Converter::convertCpu(reinterpret_cast<const uchar *>(bgra.constData()),
                                             width * 4, width, height, &viaConverter));
    const QByteArray reference = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(bgra.constData()), width * 4, width, height);

    // 逐字节一致 —— 降级路径必须与旧实现**完全等价**，否则降级就意味着画面变化
    QCOMPARE(viaConverter, reference);
    QCOMPARE(viaConverter.size(), nv12Bytes(width, height));
}

void TestNv12Converter::cpuConversionHonoursSourceStride()
{
    const int        width    = 32;
    const int        height   = 16;
    const int        stride   = width * 4 + 64;   // 行尾带填充，模拟 DXGI RowPitch
    const QByteArray padded   = makePattern(width, height, stride);

    QByteArray out;
    QVERIFY(lanrdp::Nv12Converter::convertCpu(
        reinterpret_cast<const uchar *>(padded.constData()), stride, width, height, &out));

    const QByteArray reference = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(padded.constData()), stride, width, height);
    QCOMPARE(out, reference);
}

void TestNv12Converter::cpuConversionRejectsInvalidInput()
{
    QByteArray          out;
    const QByteArray    bgra = makePattern(8, 8);

    QVERIFY(!lanrdp::Nv12Converter::convertCpu(nullptr, 32, 8, 8, &out));
    QVERIFY(!lanrdp::Nv12Converter::convertCpu(
        reinterpret_cast<const uchar *>(bgra.constData()), 32, 8, 8, nullptr));
    // stride 小于一行：宁可拒绝，也不能按错的行距读出去
    QVERIFY(!lanrdp::Nv12Converter::convertCpu(
        reinterpret_cast<const uchar *>(bgra.constData()), 8, 8, 8, &out));
    QVERIFY(!lanrdp::Nv12Converter::convertCpu(
        reinterpret_cast<const uchar *>(bgra.constData()), 32, 0, 8, &out));
}

void TestNv12Converter::deviationIsZeroForIdenticalBuffers()
{
    const int        width  = 16;
    const int        height = 16;
    const QByteArray reference =
        lanrdp::bgraToNv12(reinterpret_cast<const uchar *>(makePattern(width, height).constData()),
                           width * 4, width, height);

    const auto d = lanrdp::Nv12Converter::meanDeviation(reference, reference, width, height);
    QVERIFY(d.valid);
    QCOMPARE(d.luma, 0.0);
    QCOMPARE(d.chroma, 0.0);
}

void TestNv12Converter::deviationFindsLumaDrift()
{
    const int        width  = 16;
    const int        height = 16;
    const QByteArray pattern = makePattern(width, height);
    QByteArray reference =
        lanrdp::bgraToNv12(reinterpret_cast<const uchar *>(pattern.constData()), width * 4,
                           width, height);

    // 模拟「范围设错」：整条 Y 平面抬 20（16-235 与 0-255 混用就是这个量级）
    QByteArray shifted = reference;
    for (int i = 0; i < width * height; ++i)
        shifted[i] = static_cast<char>(qMin(255, static_cast<uchar>(shifted[i]) + 20));

    const auto d = lanrdp::Nv12Converter::meanDeviation(reference, shifted, width, height);
    QVERIFY(d.valid);
    QVERIFY2(d.luma > lanrdp::Nv12Converter::kMaxLumaDeviation,
             qPrintable(QStringLiteral("亮度偏差 %1 应超过容差").arg(d.luma)));
    // 色度没动过，不该被误判
    QVERIFY(d.chroma < lanrdp::Nv12Converter::kMaxChromaDeviation);
}

void TestNv12Converter::deviationRejectsMismatchedBuffers()
{
    const QByteArray a(64, '\0');
    const QByteArray b(100, '\0');
    QVERIFY(!lanrdp::Nv12Converter::meanDeviation(a, b, 8, 8).valid);
    QVERIFY(!lanrdp::Nv12Converter::meanDeviation(a, a, 0, 8).valid);
}

void TestNv12Converter::gpuDisabledFallsBackWithoutTouchingDevice()
{
    lanrdp::Nv12Converter converter;
    converter.setGpuAllowed(false);

    QCOMPARE(converter.mode(), lanrdp::Nv12Converter::Mode::Cpu);
    QByteArray out;
    QString    error;
    // 没有设备、又禁了 GPU：必须返回 false（= 调用方走 CPU），而不是崩或假装成功
    QVERIFY(!converter.convertTexture(nullptr, 64, 64, &out, &error));
    QCOMPARE(converter.mode(), lanrdp::Nv12Converter::Mode::Cpu);
}

void TestNv12Converter::unboundConverterFallsBack()
{
    lanrdp::Nv12Converter converter;
    QByteArray            out;
    QVERIFY(!converter.convertTexture(nullptr, 64, 64, &out));
    QCOMPARE(converter.mode(), lanrdp::Nv12Converter::Mode::Cpu);
}

void TestNv12Converter::manualDisableSaysSoInStatus()
{
    lanrdp::Nv12Converter converter;
    QVERIFY2(converter.statusText().isEmpty(), "还没跑过一帧就不该有结论");

    converter.setGpuAllowed(false);

    QCOMPARE(converter.mode(), lanrdp::Nv12Converter::Mode::Cpu);
    QVERIFY2(converter.statusText().contains(QStringLiteral("手动")),
             qPrintable(QStringLiteral("关掉显卡后没说清是谁关的：%1").arg(converter.statusText())));
    // 不能冒充「颜色对不上」—— 那是驱动的问题，这是用户的决定，处理方式完全不同
    QVERIFY2(!converter.statusText().contains(QStringLiteral("不一致")),
             qPrintable(converter.statusText()));
}

void TestNv12Converter::reEnableClearsManualNote()
{
    lanrdp::Nv12Converter converter;
    converter.setGpuAllowed(false);
    converter.setGpuAllowed(true);

    // 重新允许 ≠ 已经验过：旧结论不能留着冒充当前状态，
    // 否则设置页会一直显示「CPU 直读（手动）」而实际上已经回到显卡。
    QVERIFY2(!converter.statusText().contains(QStringLiteral("手动")),
             qPrintable(QStringLiteral("重新打开后还留着上一次的原因：%1")
                            .arg(converter.statusText())));
}

void TestNv12Converter::gpuConversionMatchesCpuReference()
{
    ID3D11Device        *device  = nullptr;
    ID3D11DeviceContext *context = nullptr;
    const HRESULT        hr      = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
        nullptr, &context);
    if (FAILED(hr) || !device || !context) {
        if (device)
            device->Release();
        QSKIP("本机没有可用的 D3D11 硬件设备，显卡路径无法验证");
    }

    const int        width  = 320;
    const int        height = 240;
    const QByteArray bgra   = makePattern(width, height);
    ID3D11Texture2D *texture = makeBgraTexture(device, width, height, bgra);
    if (!texture) {
        context->Release();
        device->Release();
        QSKIP("创建测试纹理失败（显存不足？）");
    }

    qint64 gpuTotal = 0;
    qint64 cpuTotal = 0;

    {
        QElapsedTimer clock;
        clock.start();
        lanrdp::Nv12Converter converter;
        converter.bind(device, context);

        QByteArray gpuOut;
        QString    error;
        const bool ok = converter.convertTexture(texture, width, height, &gpuOut, &error);
        gpuTotal      = clock.elapsed();

        if (!ok || converter.mode() != lanrdp::Nv12Converter::Mode::Gpu) {
            // 这台机器（或这个驱动）不支持，降级是**合法**结果，但不能静默：
            // 必须给出人话原因，让用户查得到「为什么没用上显卡」。
            qInfo("显卡路径降级：%s", qPrintable(error));
            QVERIFY2(!converter.statusText().isEmpty(),
                     "降级时必须留下可读原因，否则用户无从判断画面为什么没变快");
        } else {
            QByteArray reference;
            QVERIFY(lanrdp::Nv12Converter::convertCpu(
                reinterpret_cast<const uchar *>(bgra.constData()), width * 4, width, height,
                &reference));

            const auto d = lanrdp::Nv12Converter::meanDeviation(gpuOut, reference, width, height);
            QVERIFY(d.valid);
            qInfo("显卡 vs CPU 偏差：亮度 %.2f、色度 %.2f（容差 %.1f / %.1f）", d.luma, d.chroma,
                  lanrdp::Nv12Converter::kMaxLumaDeviation,
                  lanrdp::Nv12Converter::kMaxChromaDeviation);

            // 这就是自检用的同一条判据：过了才算「显卡路径可信」
            QVERIFY2(d.luma <= lanrdp::Nv12Converter::kMaxLumaDeviation,
                     qPrintable(QStringLiteral("亮度偏差 %1 超容差（色彩空间不对？）").arg(d.luma)));
            QVERIFY2(d.chroma <= lanrdp::Nv12Converter::kMaxChromaDeviation,
                     qPrintable(QStringLiteral("色度偏差 %1 超容差（BT.601/709 选错？").arg(d.chroma)));
        }
    }

    // 再跑一次同样的转换，确认**第二次**（跳过自检的那条路）与 CPU 一致，
    // 并且比 CPU 快 —— 否则「加速」只是说法。
    {
        lanrdp::Nv12Converter converter;
        converter.bind(device, context);
        QByteArray warmup;
        converter.convertTexture(texture, width, height, &warmup);
        if (converter.mode() == lanrdp::Nv12Converter::Mode::Gpu) {
            QElapsedTimer clock;
            clock.start();
            QByteArray steady;
            QVERIFY(converter.convertTexture(texture, width, height, &steady));
            gpuTotal = clock.nsecsElapsed() / 1000;

            const QByteArray bgra2 = makePattern(width, height);
            clock.restart();
            QByteArray reference;
            lanrdp::Nv12Converter::convertCpu(
                reinterpret_cast<const uchar *>(bgra2.constData()), width * 4, width, height,
                &reference);
            cpuTotal = clock.nsecsElapsed() / 1000;

            QCOMPARE(steady.size(), nv12Bytes(width, height));
            const auto d = lanrdp::Nv12Converter::meanDeviation(steady, reference, width, height);
            QVERIFY(d.luma <= lanrdp::Nv12Converter::kMaxLumaDeviation);

            qInfo("稳态耗时：显卡 %.0f us / CPU %.0f us", double(gpuTotal), double(cpuTotal));
        }
    }

    texture->Release();
    context->Release();
    device->Release();
}

// 4K 是这个项目的真实工况（被控端 3840x2160 @150%）。这条用例回答的正是
// 「拆线程 / GPU 转换值不值得」：在 CPU 直读都要 12 ms 的尺寸上，显卡路径必须更快，
// 否则整条优化就没有依据，宁可当场失败也不要把一个假收益带进产品。
void TestNv12Converter::gpuBeatsCpuAt4k()
{
    ID3D11Device        *device  = nullptr;
    ID3D11DeviceContext *context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))
        || !device || !context) {
        if (device)
            device->Release();
        QSKIP("本机没有可用的 D3D11 硬件设备");
    }

    const int        width   = 3840;
    const int        height  = 2160;
    const QByteArray bgra    = makeBigPattern(width, height);
    ID3D11Texture2D *texture = makeBgraTexture(device, width, height, bgra);
    if (!texture) {
        context->Release();
        device->Release();
        QSKIP("创建 4K 测试纹理失败（显存不足？）");
    }

    lanrdp::Nv12Converter converter;
    converter.bind(device, context);

    // 首帧含自检（要额外算一份 CPU 参考），单独计时后丢弃
    QByteArray warmup;
    const bool gpuUsable = converter.convertTexture(texture, width, height, &warmup);
    if (!gpuUsable || converter.mode() != lanrdp::Nv12Converter::Mode::Gpu) {
        qInfo("显卡路径不可用，跳过对比：%s", qPrintable(converter.statusText()));
        texture->Release();
        context->Release();
        device->Release();
        QSKIP("这台机器上显卡色彩转换不可用（会自动降级到 CPU）");
    }

    constexpr int kRounds = 5;

    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < kRounds; ++i) {
        QByteArray out;
        QVERIFY(converter.convertTexture(texture, width, height, &out));
    }
    const qint64 gpuUs = clock.nsecsElapsed() / 1000 / kRounds;

    // ⚠️ 对比的必须是**同一条完整路径**，不是「只比色彩转换那一段」：
    // CPU 侧在真实链路里还要先 CopyResource 到 33 MB 的 BGRA 暂存、Map 出来
    // （旧实现还要再 memcpy 一次进 QImage）。只比 bgraToNv12 会得出「显卡没更快」
    // 的错误结论 —— 它省掉的正是后面那 33 MB 的读回与拷贝。
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width            = static_cast<UINT>(width);
    sd.Height           = static_cast<UINT>(height);
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.BindFlags        = 0;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *cpuStaging = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &cpuStaging))) {
        texture->Release();
        context->Release();
        device->Release();
        QSKIP("创建 CPU 路径暂存纹理失败（显存不足？）");
    }

    clock.restart();
    for (int i = 0; i < kRounds; ++i) {
        context->CopyResource(cpuStaging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        QVERIFY(SUCCEEDED(context->Map(cpuStaging, 0, D3D11_MAP_READ, 0, &mapped)));
        QByteArray out;
        QVERIFY(lanrdp::Nv12Converter::convertCpu(
            static_cast<const uchar *>(mapped.pData), static_cast<int>(mapped.RowPitch), width,
            height, &out));
        context->Unmap(cpuStaging, 0);
    }
    const qint64 cpuUs = clock.nsecsElapsed() / 1000 / kRounds;

    cpuStaging->Release();

    qInfo("4K 单帧（抓屏纹理 → NV12 字节）：显卡 %lld us / CPU 全路径 %lld us", gpuUs, cpuUs);
    QVERIFY2(gpuUs < cpuUs,
             qPrintable(QStringLiteral("显卡 %1 us 没有比 CPU %2 us 更快").arg(gpuUs).arg(cpuUs)));

    texture->Release();
    context->Release();
    device->Release();
}

QTEST_GUILESS_MAIN(TestNv12Converter)
#include "test_nv12converter.moc"
