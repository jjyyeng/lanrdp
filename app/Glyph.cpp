#include "app/Glyph.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lanrdp::glyph {

namespace {

// 设计网格边长。所有坐标都按它写，出图时整体缩放。
constexpr qreal kGrid = 24.0;

// 网格单位下的线宽。小图标 2.0 ⇒ 20px 下约 1.7 设备像素；
// 大图标按原来的比例给 2.4（68px 下约 6.8），细了会显得发飘。
constexpr qreal kStroke    = 2.0;
constexpr qreal kStrokeBig = 2.4;

// 禁用态的透明度系数：明显变淡，但不至于完全看不见。
constexpr qreal kDisabledAlpha = 0.45;

// 大图标（空态正中）用的屏幕几何：占满 80% 宽度，撑得住 68px。
// 和下面小图标一样，纵向关于画布中线 12 对称（含支架与描边后重心在 12）。
const QRectF kBigScreenRect(2.4, 4.3, 19.2, 12.5);

// 小图标（按钮里 20px）用的屏幕几何。**没有支架** —— 那三条短线在 20px 下
// 只会糊成一个点，不如把宽度全留给屏幕和箭头。
// 两侧各留 2 格间隙，朝内朝外只是镜像，视觉重量一致。
//
// ⚠️ 纵向必须**关于画布中线 12 对称**（8.0 + 8.0/2 = 12）。按钮里的图标是
// 整块居中摆放的，图形重心一旦偏离画布中心，就会和文字错开半个字高。
const QRectF kSmallScreenRect(1.0, 8.0, 11.0, 8.0);
constexpr qreal kSmallScreenCenterY = 12.0;   // 8.0 + 8.0/2，箭头对齐这条线
constexpr qreal kArrowGap  = 14.0;            // 箭头起止的外缘（两侧对称）
constexpr qreal kArrowBase = 18.0;            // 三角底边
constexpr qreal kArrowTip  = 22.0;            // 顶点（朝外到此，朝内镜像到 kArrowGap）

QPainterPath monitorPath(const QRectF &screen, bool withStand)
{
    QPainterPath p;
    p.addRoundedRect(screen, 1.6, 1.6);
    if (!withStand)
        return p;

    // 支架 + 底座，关于屏幕水平中心对称。比例沿用空态图标原本的构图：
    // 支架长度 ≈ 屏高 0.23，底座半宽 ≈ 屏宽 0.225。
    const qreal cx = screen.center().x();
    const qreal footY = screen.bottom() + screen.height() * 0.23;
    const qreal footHalf = screen.width() * 0.225;

    p.moveTo(cx, screen.bottom());
    p.lineTo(cx, footY);
    p.moveTo(cx - footHalf, footY);
    p.lineTo(cx + footHalf, footY);
    return p;
}

// 箭头用**实心三角 + 短杆**，不用「两条斜撇」：
// 斜撇在 20px 下宽度不足 3 个设备像素，抗锯齿一糊就跟屏幕粘在一起了。
QPainterPath arrowHeadPath(Shape shape)
{
    QPainterPath p;
    const qreal tipX = (shape == Shape::MonitorOut) ? kArrowTip : kArrowGap;

    p.moveTo(kArrowBase, kSmallScreenCenterY - 3.5);
    p.lineTo(tipX,       kSmallScreenCenterY);
    p.lineTo(kArrowBase, kSmallScreenCenterY + 3.5);
    p.closeSubpath();
    return p;
}

QPainterPath arrowShaftPath(Shape shape)
{
    QPainterPath p;
    const bool outgoing = (shape == Shape::MonitorOut);
    p.moveTo(outgoing ? kArrowGap : kArrowTip, kSmallScreenCenterY);
    p.lineTo(kArrowBase, kSmallScreenCenterY);
    return p;
}

// ---- 锁（收回 / 恢复操作）----
//
// 锁体 12×9.5、锁梁从锁体顶边起拱 —— 比例照常见挂锁来。**不画钥匙孔**：
// 18px 下一个孔只剩 1 个设备像素，只会把锁体中间糊出一块脏点。
// 收回态（闭锁）与恢复态（开锁）靠**锁梁左腿**区分：闭锁时左腿落到锁体上，
// 开锁时锁梁向左上弹开、左腿悬在半空，跟锁体之间留一道缝。
const QRectF kLockBody(6.0, 10.5, 12.0, 9.5);
constexpr qreal kLockLegLeft  = 9.0;
constexpr qreal kLockLegRight = 15.0;
constexpr qreal kLockLegTop   = 7.5;    // 锁梁直腿的起弯高度
constexpr qreal kLockArchTop  = 3.6;    // 拱顶控制点（实际拱顶 ≈ 4.6）

QPainterPath lockBodyPath()
{
    QPainterPath p;
    p.addRoundedRect(kLockBody, 2.0, 2.0);
    return p;
}

QPainterPath lockShacklePath(bool open)
{
    QPainterPath p;
    if (!open) {
        p.moveTo(kLockLegLeft, kLockBody.top());
        p.lineTo(kLockLegLeft, kLockLegTop);
        p.cubicTo(kLockLegLeft, kLockArchTop, kLockLegRight, kLockArchTop,
                  kLockLegRight, kLockLegTop);
        p.lineTo(kLockLegRight, kLockBody.top());
        return p;
    }
    // 开锁：右腿还插在锁体上，锁梁向左上甩开 —— 左端停在半空并**高过拱顶的右半边**，
    // 这样缺口落在锁体上方而不是侧面。缺口放在侧面时（同样的拱、只是左腿不落地）
    // 18px 下几乎看不出来，跟闭锁长得一样。
    p.moveTo(kLockLegRight, kLockBody.top());
    p.lineTo(kLockLegRight, kLockLegTop);
    p.cubicTo(kLockLegRight, kLockArchTop + 0.4, kLockLegLeft + 0.6, kLockArchTop - 0.2,
              kLockLegLeft - 0.8, kLockLegTop - 2.0);
    return p;
}

// ---- 鼠标指针（操作权）----
//
// 「谁在操作键鼠」最直白的图形。**必须带尾巴**：去掉尾巴只剩一个三角，
// 18px 下会被看成播放键或纸飞机。尾巴也是实心 / 空心两态的识别特征 ——
// 两态用同一条路径，只差「填充 or 只描边」，形状完全一致，不会认成两个图标。
QPainterPath pointerPath()
{
    QPainterPath p;
    p.moveTo(7.0, 3.2);
    p.lineTo(7.0, 19.0);
    p.lineTo(10.2, 15.8);   // 内侧凹角
    p.lineTo(12.4, 20.8);   // 尾巴左下
    p.lineTo(14.8, 19.8);   // 尾巴右下
    p.lineTo(12.6, 14.8);
    p.lineTo(17.2, 14.3);   // 右侧翼
    p.closeSubpath();
    return p;
}

// ---- 【发送文件】托盘 + 朝上的箭头 ----
//
// 「文件送出去」用托盘加一枚朝上的箭头表示。**不画纸**：纸张的折角在 18px 下
// 只剩一个像素的斜切，看着像缺了角的方块；托盘的三条线更干净，也留得出箭头的空位。
//
// 托盘两侧的竖线只走到 kTrayTopY 就停 —— 再往上收口就成了「一个方框」，
// 箭头会被压在框里，小尺寸下反而分不清是「导出」还是「保存到本地」。
constexpr qreal kSendCenterX   = 12.0;
constexpr qreal kSendTipY      = 2.8;    // 三角顶点
constexpr qreal kSendHeadBaseY = 8.6;    // 三角底边
constexpr qreal kSendHeadHalf  = 4.0;
constexpr qreal kSendShaftEndY = 15.4;   // 杆的下端（与托盘底之间留一道缝）
constexpr qreal kTrayTopY      = 11.4;
constexpr qreal kTrayBottomY   = 20.6;
constexpr qreal kTrayLeftX     = 2.8;
constexpr qreal kTrayRightX    = 21.2;
constexpr qreal kTrayRadius    = 2.6;

QPainterPath sendHeadPath()
{
    QPainterPath p;
    p.moveTo(kSendCenterX, kSendTipY);
    p.lineTo(kSendCenterX - kSendHeadHalf, kSendHeadBaseY);
    p.lineTo(kSendCenterX + kSendHeadHalf, kSendHeadBaseY);
    p.closeSubpath();
    return p;
}

QPainterPath sendShaftPath()
{
    QPainterPath p;
    p.moveTo(kSendCenterX, kSendHeadBaseY);
    p.lineTo(kSendCenterX, kSendShaftEndY);
    return p;
}

QPainterPath trayPath()
{
    QPainterPath p;
    p.moveTo(kTrayLeftX, kTrayTopY);
    p.lineTo(kTrayLeftX, kTrayBottomY - kTrayRadius);
    p.quadTo(kTrayLeftX, kTrayBottomY, kTrayLeftX + kTrayRadius, kTrayBottomY);
    p.lineTo(kTrayRightX - kTrayRadius, kTrayBottomY);
    p.quadTo(kTrayRightX, kTrayBottomY, kTrayRightX, kTrayBottomY - kTrayRadius);
    p.lineTo(kTrayRightX, kTrayTopY);
    return p;
}

// ---- 眼睛：本机预览 / 看明文口令 ----
//
// 上下两道弧在两端交汇成一枚杏仁形，中间一颗实心瞳孔 —— 全靠这条实心圆点
// 来认出「这是眼睛」，所以它必须够大（直径约 5.4 格，18px 下还有 4 个设备像素）；
// 再小就只剩一个圈里一点墨，看不出是什么。
//
// 划掉的那一版把眼睛缩到七成多：斜杠要斜穿整块画布，眼睛再占满宽度就没地方
// 落笔 —— 两条墨迹叠在一起，18px 下只剩一团。
constexpr qreal kEyeHalfW = 9.2;    // 眼角到中线的水平距离
constexpr qreal kEyeRise  = 5.6;    // 弧顶距中线的距离
constexpr qreal kPupilR   = 2.7;

// 划掉的那一版只缩一点点（8.0 对 9.2）：缩太多，同一个按钮切换状态时
// 图形会明显小一圈，看着像换了颗按钮。斜杠靠「在眼睛周围断开」来让位，
// 不靠把眼睛压小。
constexpr qreal kEyeOffHalfW  = 8.0;
constexpr qreal kEyeOffRise   = 4.9;
constexpr qreal kEyeOffPupilR = 2.4;

// 斜杠从右上角划到左下角（与「禁止」同向），两端都探出眼睛之外才算「划过」。
constexpr qreal kEyeOffFromX = 20.2;
constexpr qreal kEyeOffFromY = 3.8;

QPainterPath eyeOutlinePath(qreal halfW, qreal rise)
{
    // 二次曲线在参数 0.5 处到达 0.25·P0 + 0.5·P1 + 0.25·P2 ⇒ 想让弧顶离中线
    // kEyeRise，控制点就得放到中线**两侧 2 倍**的距离上（1 倍会被削掉一半）。
    const qreal cy  = kGrid / 2.0;
    const qreal top = cy - 2.0 * rise;
    const qreal bot = cy + 2.0 * rise;

    QPainterPath p;
    p.moveTo(cy - halfW, cy);
    p.quadTo(cy, top, cy + halfW, cy);   // 上眼皮
    p.quadTo(cy, bot, cy - halfW, cy);   // 下眼皮
    return p;
}

QPainterPath pupilPath(qreal r)
{
    QPainterPath p;
    p.addEllipse(QPointF(kGrid / 2.0, kGrid / 2.0), r, r);
    return p;
}

QPainterPath eyeOffSlashPath()
{
    QPainterPath slash;
    slash.moveTo(kEyeOffFromX, kEyeOffFromY);
    slash.lineTo(kGrid - kEyeOffFromX, kGrid - kEyeOffFromY);

    // 斜杠在眼睛周围断开一圈：两笔都是 2 格粗，18px 下贴在一起会熔成一团墨，
    // 眼睛就看不出是眼睛了。用「描边成面再相减」做这道缝，缝宽随图标一起缩放。
    QPainterPath eye = eyeOutlinePath(kEyeOffHalfW, kEyeOffRise);
    eye.addPath(pupilPath(kEyeOffPupilR));

    QPainterPathStroker halo;
    halo.setWidth(kStroke + 3.2);
    halo.setCapStyle(Qt::RoundCap);
    halo.setJoinStyle(Qt::RoundJoin);

    QPainterPathStroker thick;
    thick.setWidth(kStrokeBig);
    thick.setCapStyle(Qt::RoundCap);

    return thick.createStroke(slash).subtracted(halo.createStroke(eye));
}

// ---- 已连几个控制端：一个人形 ----
//
// 两个人形并排塞进 18px 会粘成一块墨，所以只画一个 —— 它说的是「有人连着」，
// 到底几个由按钮上的数字交代。头是小圆，肩是半弧，都走描边，与别的图标同一种笔。
constexpr qreal kHeadCy        = 8.2;
constexpr qreal kHeadR         = 3.5;
constexpr qreal kShoulderLeft  = 4.9;
constexpr qreal kShoulderRight = 19.1;
constexpr qreal kShoulderFoot  = 20.2;
// 半弧顶点的实际高度是 (y0 + 3c + 3c + y3) / 8（控制点对称时），
// 想让顶点落在 14.6 就得把控制点放到 12.7 而不是 14.6。
constexpr qreal kShoulderCtrlY = 12.7;

QPainterPath headPath()
{
    QPainterPath p;
    p.addEllipse(QPointF(kGrid / 2.0, kHeadCy), kHeadR, kHeadR);
    return p;
}

QPainterPath shoulderPath()
{
    QPainterPath p;
    p.moveTo(kShoulderLeft, kShoulderFoot);
    p.cubicTo(kShoulderLeft, kShoulderCtrlY, kShoulderRight, kShoulderCtrlY,
              kShoulderRight, kShoulderFoot);
    return p;
}

// ---- 【开始 / 停止监听】播放三角与方块 ----
//
// 开始 / 停止在别处（媒体键、录音键、服务开关）全是这两个形状，不需要第二套隐喻。
// 三角要比方块**大一圈**：方块天生更占面积，两者套同一个包围盒时三角看着会缩着，
// 并排的两颗按钮就一大一小。
constexpr qreal kPlayLeft   = 8.0;
constexpr qreal kPlayTop    = 4.4;
constexpr qreal kPlayBottom = 19.6;
constexpr qreal kPlayTipX   = 19.8;
const QRectF    kStopRect(6.2, 6.2, 11.6, 11.6);

QPainterPath playPath()
{
    QPainterPath p;
    p.moveTo(kPlayLeft, kPlayTop);
    p.lineTo(kPlayLeft, kPlayBottom);
    p.lineTo(kPlayTipX, kGrid / 2.0);
    p.closeSubpath();
    return p;
}

// ---- 重连：环形箭头 ----
//
// 一圈开了口、口子上接一枚实心三角，方向顺着弧走。开口放在**左上**、箭头指向
// 右上方 —— 这是刷新类图标最常见的样子，缩小后仍然像「再来一次」而不是「一个圈」。
// ⚠️ 圆心**不在 12**：箭头从缺口顶伸长出去，整幅的墨迹范围会比圆环本身更高，
// 圆心照 12 摆就会整体偏上（实测偏 1.5 格 ≈ 4px）。往下让一截，包围盒才回到中线。
constexpr qreal kRingCenter   = 13.45;
constexpr qreal kRingRadius   = 7.4;
constexpr qreal kRingFrom     = 100.0;   // 缺口起点（0° = 三点方向，逆时针为正）
constexpr qreal kRingSweep    = -300.0;  // 顺时针扫过去，缺口留 60°
constexpr qreal kRingHeadLen  = 5.0;
constexpr qreal kRingHeadHalf = 3.1;

QPainterPath ringArcPath()
{
    const QRectF box(kRingCenter - kRingRadius, kRingCenter - kRingRadius,
                     kRingRadius * 2.0, kRingRadius * 2.0);

    QPainterPath p;
    p.arcMoveTo(box, kRingFrom);
    p.arcTo(box, kRingFrom, kRingSweep);
    return p;
}

QPainterPath ringHeadPath()
{
    const qreal rad = qDegreesToRadians(kRingFrom);
    const QPointF start(kRingCenter + kRingRadius * std::cos(rad),
                        kRingCenter - kRingRadius * std::sin(rad));
    const QPointF dir(std::sin(rad), std::cos(rad));   // 顺时针走时的切向
    const QPointF perp(-dir.y(), dir.x());
    const QPointF base = start;                        // 三角从弧端接着走，不压在弧上
    const QPointF tip  = start + dir * kRingHeadLen;

    QPainterPath p;
    p.moveTo(tip);
    p.lineTo(base + perp * kRingHeadHalf);
    p.lineTo(base - perp * kRingHeadHalf);
    p.closeSubpath();
    return p;
}

// ---- 断开：圆圈 + 一道斜杠 ----
//
// 不用链条断裂 / 拔插头那类形状：两个环节之间那道缝在 18px 下只有两个像素，
// 抗锯齿一糊就变成「一个糊掉的圈」。圆圈加斜杠形状极简，跟【关闭】的叉也差得远。
constexpr qreal kDisconnectRadius = 7.8;
constexpr qreal kDisconnectSlashFrom = 5.4;
constexpr qreal kDisconnectSlashTo   = 18.6;

QPainterPath disconnectRingPath()
{
    QPainterPath p;
    p.addEllipse(QPointF(kGrid / 2.0, kGrid / 2.0), kDisconnectRadius, kDisconnectRadius);
    return p;
}

QPainterPath disconnectSlashPath()
{
    QPainterPath p;
    p.moveTo(kDisconnectSlashFrom, kDisconnectSlashFrom);
    p.lineTo(kDisconnectSlashTo, kDisconnectSlashTo);
    return p;
}

// ---- 关闭这一格：叉 ----
constexpr qreal kCloseInset = 6.2;

QPainterPath closePath()
{
    QPainterPath p;
    p.moveTo(kCloseInset, kCloseInset);
    p.lineTo(kGrid - kCloseInset, kGrid - kCloseInset);
    p.moveTo(kGrid - kCloseInset, kCloseInset);
    p.lineTo(kCloseInset, kGrid - kCloseInset);
    return p;
}

// ---- 新连接：屏幕 + 加号 ----
//
// 屏幕挪到左上角、让出右下角给加号。加号**做粗**（线宽比别处大）：它只有
// 七格长，还按 2.0 的线宽画，18px 下会被看成屏幕边上粘了一小块脏点。
const QRectF    kNewConnScreenRect(1.4, 3.1, 12.0, 8.8);
constexpr qreal kNewConnPlusCx   = 18.4;
constexpr qreal kNewConnPlusCy   = 16.1;
constexpr qreal kNewConnPlusHalf = 4.2;
constexpr qreal kPlusStroke      = 2.8;

QPainterPath newConnPlusPath()
{
    QPainterPath p;
    p.moveTo(kNewConnPlusCx - kNewConnPlusHalf, kNewConnPlusCy);
    p.lineTo(kNewConnPlusCx + kNewConnPlusHalf, kNewConnPlusCy);
    p.moveTo(kNewConnPlusCx, kNewConnPlusCy - kNewConnPlusHalf);
    p.lineTo(kNewConnPlusCx, kNewConnPlusCy + kNewConnPlusHalf);
    return p;
}

// ---- 【占满 / 还原】共用的一对对角箭头 ----
//
// 只沿一根对角线（右上 / 左下）画两根：四角各画一根在 18px 下会糊成一朵花。
// 两根都要**够长** —— 杆短了在 18px 下只剩两个方块，看不出是箭头：
// 三角 4.5 + 杆 6~7 格（合计 10.5~11.5，几乎顶到画布中线），角上留 2.5 格。
//
// 这一对按钮**没有文字**（只靠图标 + 悬停提示），所以两个形状必须一眼分得出：
// 占满 = 顶点贴在画布角上、箭头朝外，两根杆在中线附近接上，连成一个双向箭头；
// 还原 = 顶点收到中线两侧、箭头朝内，中间留出空隙，看着像两头对撞。
// 两者的轮廓差得很远（一头一尾 vs 中间对撞），小尺寸下也不会认错。
constexpr qreal kDiagInset = 2.5;    // 占满态：顶点离画布边的距离
constexpr qreal kDiagHead  = 4.5;    // 三角沿箭身方向的长度
constexpr qreal kDiagHalf  = 3.0;    // 三角的半个宽度（垂直于箭身）
constexpr qreal kDiagShaftOut = 7.0; // 占满态的杆长（顶点在角上，要多走一段才撑满）
constexpr qreal kDiagShaftIn  = 6.0; // 还原态的杆长（顶点已靠近中线，再长就顶出画布角）
constexpr qreal kDiagInner = 3.5;    // 还原态：顶点到中线沿对角线的距离
constexpr qreal kDiagUnit  = 0.70710678118654752;   // √2/2：对角线方向的单位分量

// `upperRight` 选哪一枚（右上那枚 / 左下那枚），两枚关于画布中心点对称。
QPointF diagTip(Shape shape, bool upperRight)
{
    if (shape == Shape::Maximize) {
        const qreal edge = kGrid - kDiagInset;
        return upperRight ? QPointF(edge, kDiagInset) : QPointF(kDiagInset, edge);
    }
    const qreal c = kGrid / 2.0;
    const qreal d = kDiagInner * kDiagUnit;
    return upperRight ? QPointF(c + d, c - d) : QPointF(c - d, c + d);
}

// 箭头**指向**的单位方向。占满时朝外（指向画布角），还原时朝内（指向中线）。
QPointF diagDir(Shape shape, bool upperRight)
{
    const qreal sign    = upperRight ? 1.0 : -1.0;
    const qreal outward = (shape == Shape::Maximize) ? 1.0 : -1.0;
    return QPointF(sign * kDiagUnit * outward, -sign * kDiagUnit * outward);
}

QPainterPath diagHeadPath(const QPointF &tip, const QPointF &dir)
{
    const QPointF base = tip - dir * kDiagHead;
    const QPointF perp(-dir.y(), dir.x());

    QPainterPath p;
    p.moveTo(tip);
    p.lineTo(base + perp * kDiagHalf);
    p.lineTo(base - perp * kDiagHalf);
    p.closeSubpath();
    return p;
}

QPainterPath diagShaftPath(const QPointF &tip, const QPointF &dir, qreal length)
{
    const QPointF base = tip - dir * kDiagHead;

    QPainterPath p;
    p.moveTo(base);
    p.lineTo(base - dir * length);
    return p;
}

// 两枚箭头合成一条路径：头（填充）和杆（描边）分开画，笔刷不一样。
QPainterPath diagArrowPath(Shape shape, bool heads)
{
    const qreal shaft = (shape == Shape::Maximize) ? kDiagShaftOut : kDiagShaftIn;

    QPainterPath p;
    for (bool upperRight : { true, false }) {
        const QPointF tip = diagTip(shape, upperRight);
        const QPointF dir = diagDir(shape, upperRight);
        p.addPath(heads ? diagHeadPath(tip, dir) : diagShaftPath(tip, dir, shaft));
    }
    return p;
}

void drawGlyph(QPainter &p, Shape shape, const QColor &color)
{
    p.setRenderHint(QPainter::Antialiasing, true);

    if (shape == Shape::Monitor) {
        p.setPen(QPen(color, kStrokeBig, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(monitorPath(kBigScreenRect, /*withStand=*/true));
        return;
    }

    p.setPen(QPen(color, kStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    if (shape == Shape::Lock || shape == Shape::Unlock) {
        p.drawPath(lockBodyPath());
        p.drawPath(lockShacklePath(shape == Shape::Unlock));
        return;
    }

    if (shape == Shape::Pointer || shape == Shape::PointerIdle) {
        // 两态同一路径：实心 = 握着操作权，空心 = 还没拿到。
        if (shape == Shape::Pointer)
            p.setBrush(color);
        p.drawPath(pointerPath());
        return;
    }

    if (shape == Shape::SendFile) {
        p.drawPath(trayPath());          // 托盘与箭杆都描边
        p.drawPath(sendShaftPath());
        p.setBrush(color);
        p.drawPath(sendHeadPath());      // 箭头实心
        return;
    }

    if (shape == Shape::Eye) {
        p.drawPath(eyeOutlinePath(kEyeHalfW, kEyeRise));
        p.setBrush(color);
        p.drawPath(pupilPath(kPupilR));         // 瞳孔实心
        return;
    }

    if (shape == Shape::EyeOff) {
        p.drawPath(eyeOutlinePath(kEyeOffHalfW, kEyeOffRise));
        p.setBrush(color);
        p.drawPath(pupilPath(kEyeOffPupilR));
        // 斜杠已经是「面」，直接用填充画，别再描一次边（描边会让它胖回去、堵掉那道缝）。
        p.drawPath(eyeOffSlashPath());
        return;
    }

    if (shape == Shape::Person) {
        p.drawPath(headPath());
        p.drawPath(shoulderPath());
        return;
    }

    if (shape == Shape::Listen) {
        p.setBrush(color);
        p.drawPath(playPath());
        return;
    }

    if (shape == Shape::StopListen) {
        p.setBrush(color);
        QPainterPath box;
        box.addRoundedRect(kStopRect, 1.8, 1.8);
        p.drawPath(box);
        return;
    }

    if (shape == Shape::Reconnect) {
        p.drawPath(ringArcPath());
        p.setBrush(color);
        p.drawPath(ringHeadPath());
        return;
    }

    if (shape == Shape::Disconnect) {
        p.drawPath(disconnectRingPath());
        p.drawPath(disconnectSlashPath());
        return;
    }

    if (shape == Shape::CloseCell) {
        p.drawPath(closePath());
        return;
    }

    if (shape == Shape::NewConnection) {
        p.drawPath(monitorPath(kNewConnScreenRect, /*withStand=*/false));
        p.setPen(QPen(color, kPlusStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(newConnPlusPath());
        return;
    }

    if (shape == Shape::Maximize || shape == Shape::Restore) {
        p.drawPath(diagArrowPath(shape, /*heads=*/false));
        p.setBrush(color);
        p.drawPath(diagArrowPath(shape, /*heads=*/true));
        return;
    }

    // 屏幕（小尺寸不带支架，见 kSmallScreenRect 处的说明）
    p.drawPath(monitorPath(kSmallScreenRect, /*withStand=*/false));

    // 箭头（杆描边、头填充）：一个指出屏幕，一个指进屏幕。
    p.drawPath(arrowShaftPath(shape));
    p.setBrush(color);
    p.drawPath(arrowHeadPath(shape));
}

// 「任意尺寸都清晰」就落在这一层：每次要位图都按目标尺寸重画一遍，
// 而不是把某次的结果存起来再用（存下来就退化成位图了）。
class GlyphEngine : public QIconEngine
{
public:
    GlyphEngine(Shape shape, QColor color)
        : shape_(shape), color_(std::move(color))
    {
    }

    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        const qreal side = std::min(rect.width(), rect.height());
        if (side <= 0)
            return;   // 被缩到 0 宽时不画，也避免下面除出 0 缩放

        const qreal scale = side / kGrid;
        p->save();
        // 等比缩放到矩形内并居中 —— 目标尺寸不是正方形时也不会把图标拉扁。
        p->translate(rect.x() + (rect.width() - kGrid * scale) / 2.0,
                     rect.y() + (rect.height() - kGrid * scale) / 2.0);
        p->scale(scale, scale);
        drawGlyph(*p, shape_, colorFor(mode));
        p->restore();
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        // 覆写它而不是只靠默认实现：
        // · 默认实现会把 size×scale 取整后转给 pixmap()，再由 Qt 用 QImage 中转一次；
        // · 这里直接按设备像素开 QPixmap 画，尺寸精确、也省掉那次格式转换。
        const QSize pixels(qRound(size.width() * scale), qRound(size.height() * scale));
        QPixmap pm(pixels);
        if (pm.isNull())
            return pm;
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        // ⚠️ 这里必须传**逻辑**尺寸，不能传上面的 pixels。
        // 设过 devicePixelRatio 的 QPixmap 上，QPainter 的坐标是逻辑坐标、Qt 自己
        // 会按 DPR 放大到设备像素；再按像素尺寸去画，缩放就被乘了两次 ——
        // 150%/200% 缩放下图标会画大一圈并被裁掉大半（实测 dpr=2 时整张空白）。
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine *clone() const override { return new GlyphEngine(*this); }

private:
    QColor colorFor(QIcon::Mode mode) const
    {
        if (mode != QIcon::Disabled)
            return color_;
        QColor faded = color_;
        faded.setAlphaF(faded.alphaF() * kDisabledAlpha);
        return faded;
    }

    Shape  shape_;
    QColor color_;
};

} // namespace

QIcon icon(Shape shape, const QColor &color)
{
    return QIcon(new GlyphEngine(shape, color));
}

QPixmap pixmap(Shape shape, int size, qreal devicePixelRatio, const QColor &color)
{
    if (size <= 0)
        return {};
    // 低于 1 的 DPR 没有意义（会让位图比显示尺寸还小，反而更糊）。
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatio);
    return GlyphEngine(shape, color).scaledPixmap(QSize(size, size), QIcon::Normal,
                                                  QIcon::Off, dpr);
}

} // namespace lanrdp::glyph
