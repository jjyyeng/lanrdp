#include "app/QualityReport.h"

#include <QCoreApplication>
#include <QStringList>

namespace lanrdp {

// ⚠️ 本文件里每个文案都**直接**写在 QCoreApplication::translate() 的实参位置。
// 不要为了少打几个字抽成 `const char *` 常量，也不要绕一层「翻译助手函数」——
// lupdate 只认字面量，那样抽出来的结果是**一条也收不到**，而且不报错：
// 表现是英文界面里永远显示中文，没人会想到问题出在抽取阶段。
namespace {
// 「没有这个读数」的占位。用一个破折号而不是 0 或空白：空白看起来像界面没渲染完，
// 0 则是一个具体的（错误的）结论。
QString unknown()
{
    return QStringLiteral("—");
}

QString sizeText(const QSize &size)
{
    if (!size.isValid())
        return unknown();
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QString rttText(int rttMs)
{
    if (rttMs < 0)
        return unknown();
    return QCoreApplication::translate("lanrdp", "%1 ms").arg(rttMs);
}

QString decodeText(double decodeMs)
{
    if (decodeMs < 0.0)
        return unknown();
    return QCoreApplication::translate("lanrdp", "%1 ms/帧")
        .arg(QString::number(decodeMs, 'f', 1));
}

} // namespace

QualityVerdict judgeQuality(const QualitySample &sample)
{
    // 「一帧都没收到过」才是没有画面。**不能**用 fps == 0 判 ——
    // 静止的桌面本来就几乎不发帧，那样会把「画面好得很」报成「没画面」，
    // 而且每次不动鼠标都会触发一次误报。
    if (sample.frames == 0)
        return QualityVerdict::NoData;

    // 先看网络：它更常见、更容易被环境改变（Wi-Fi 干扰、中间多一台设备），
    // 而且两项同时不好时，「换根网线」比「换台电脑」更可能是对的建议。
    if (sample.rttMs >= kSlowRttMs)
        return QualityVerdict::NetworkSlow;

    // 解码吃紧与否，看它吃掉了多少**帧预算**：
    //
    // 帧间隔 = 1000 / fps，而 fps 用的是「这一秒真收到几帧」—— 所以这条判据
    // 自带一个恰到好处的性质：解码慢到拖住收帧时，实际帧率会掉、预算随之变大，
    // 而只要解码仍是瓶颈，它占总时长的比例就仍然接近 1，照样报警。
    //
    // 反过来，4K 30 fps 上解码 15 ms 只占 45%，画面根本不会卡 —— 不该报警。
    // 这正是固定 12 ms 门槛会犯的错：它把「4K 软解的常态」当成了故障。
    if (sample.fps > 0.0 && sample.decodeMs >= 0.0) {
        const double frameBudgetMs = 1000.0 / sample.fps;
        if (sample.decodeMs >= frameBudgetMs * kSlowDecodeShare)
            return QualityVerdict::DecodeSlow;
    }

    return QualityVerdict::Good;
}

QString qualityVerdictText(QualityVerdict verdict)
{
    switch (verdict) {
    case QualityVerdict::NoData:
        return QCoreApplication::translate("lanrdp", "还没有收到画面");
    case QualityVerdict::NetworkSlow:
        return QCoreApplication::translate("lanrdp", "网络延迟偏高");
    case QualityVerdict::DecodeSlow:
        return QCoreApplication::translate("lanrdp", "本机解码吃紧");
    case QualityVerdict::Good:
        break;
    }
    return QCoreApplication::translate("lanrdp", "连接良好");
}

QString qualityHudText(const QualitySample &sample)
{
    const QString fps  = QString::number(sample.fps, 'f', 0);
    const QString mbps = QString::number(sample.mbps, 'f', 1);
    const QString size = sizeText(sample.size);

    QStringList lines;

    // 两个角色说的不是同一件事：控制端报「收到多少」，被控端报「发出多少」。
    // 用一个词糊过去（比如都叫「画面」），被控端那侧的读起来就是错的。
    if (sample.hosting) {
        lines << QCoreApplication::translate("lanrdp", "推送 %1 · %2 fps · %3 Mbps")
                     .arg(size, fps, mbps);
    } else {
        lines << QCoreApplication::translate("lanrdp", "画面 %1 · %2 fps · %3 Mbps")
                     .arg(size, fps, mbps);
    }

    // 只有控制端才有「收回来」的这两个数。被控端报「往返 — / 解码 —」
    // 会让人以为读数坏了，而不是「这两项本来就不属于这一侧」。
    if (!sample.hosting) {
        lines << QCoreApplication::translate("lanrdp", "往返 %1 · 解码 %2")
                     .arg(rttText(sample.rttMs), decodeText(sample.decodeMs));
    }

    lines << QCoreApplication::translate("lanrdp", "累计 %1 帧").arg(sample.frames);
    lines << QCoreApplication::translate("lanrdp", "诊断 %1")
                 .arg(qualityVerdictText(judgeQuality(sample)));

    return lines.join(QLatin1Char('\n'));
}

} // namespace lanrdp
