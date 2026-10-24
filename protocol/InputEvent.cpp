#include "protocol/InputEvent.h"

#include <QtEndian>

namespace lanrdp {

namespace {

void appendU16(QByteArray &out, std::uint16_t value)
{
    char buffer[2];
    qToLittleEndian(value, buffer);
    out.append(buffer, 2);
}

bool readU16(const QByteArray &data, int offset, std::uint16_t *out)
{
    if (offset < 0 || offset + 2 > data.size())
        return false;
    *out = qFromLittleEndian<std::uint16_t>(
        reinterpret_cast<const uchar *>(data.constData()) + offset);
    return true;
}

} // namespace

QByteArray encodeInputEvent(const InputEvent &event)
{
    QByteArray payload;

    switch (event.kind) {
    case InputKind::MouseMove:
        payload.append(static_cast<char>(InputKind::MouseMove));
        appendU16(payload, event.nx);
        appendU16(payload, event.ny);
        break;

    case InputKind::MouseButton:
        payload.append(static_cast<char>(InputKind::MouseButton));
        payload.append(static_cast<char>(event.button));
        payload.append(static_cast<char>(event.pressed ? 1 : 0));
        appendU16(payload, event.nx);
        appendU16(payload, event.ny);
        break;

    case InputKind::MouseWheel:
        payload.append(static_cast<char>(InputKind::MouseWheel));
        payload.append(static_cast<char>(static_cast<std::uint16_t>(event.wheelDelta) & 0xFF));
        payload.append(static_cast<char>((static_cast<std::uint16_t>(event.wheelDelta) >> 8) & 0xFF));
        appendU16(payload, event.nx);
        appendU16(payload, event.ny);
        break;

    case InputKind::KeyDown:
    case InputKind::KeyUp:
        payload.append(static_cast<char>(event.kind));
        appendU16(payload, event.vk);
        appendU16(payload, event.flags);
        break;
    }

    return payload;
}

std::optional<InputEvent> decodeInputEvent(const QByteArray &payload)
{
    if (payload.isEmpty())
        return std::nullopt;

    const std::uint8_t rawKind = static_cast<std::uint8_t>(payload.at(0));

    InputEvent event;

    switch (static_cast<InputKind>(rawKind)) {
    case InputKind::MouseMove: {
        if (payload.size() != 5)
            return std::nullopt;
        std::uint16_t nx = 0;
        std::uint16_t ny = 0;
        if (!readU16(payload, 1, &nx) || !readU16(payload, 3, &ny))
            return std::nullopt;
        event.kind = InputKind::MouseMove;
        event.nx   = nx;
        event.ny   = ny;
        return event;
    }

    case InputKind::MouseButton: {
        if (payload.size() != 7)
            return std::nullopt;
        const auto rawButton = static_cast<std::uint8_t>(payload.at(1));
        if (rawButton < static_cast<std::uint8_t>(MouseButtonId::Left)
            || rawButton > static_cast<std::uint8_t>(MouseButtonId::Middle)) {
            return std::nullopt;
        }
        std::uint16_t nx = 0;
        std::uint16_t ny = 0;
        if (!readU16(payload, 3, &nx) || !readU16(payload, 5, &ny))
            return std::nullopt;
        event.kind    = InputKind::MouseButton;
        event.button  = static_cast<MouseButtonId>(rawButton);
        event.pressed = (payload.at(2) != 0);
        event.nx      = nx;
        event.ny      = ny;
        return event;
    }

    case InputKind::MouseWheel: {
        if (payload.size() != 7)
            return std::nullopt;
        const auto lo = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload.at(1)));
        const auto hi = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload.at(2)));
        std::uint16_t nx = 0;
        std::uint16_t ny = 0;
        if (!readU16(payload, 3, &nx) || !readU16(payload, 5, &ny))
            return std::nullopt;
        event.kind       = InputKind::MouseWheel;
        event.wheelDelta = static_cast<std::int16_t>(lo | (hi << 8));
        event.nx         = nx;
        event.ny         = ny;
        return event;
    }

    case InputKind::KeyDown:
    case InputKind::KeyUp: {
        if (payload.size() != 5)
            return std::nullopt;
        std::uint16_t vk    = 0;
        std::uint16_t flags = 0;
        if (!readU16(payload, 1, &vk) || !readU16(payload, 3, &flags))
            return std::nullopt;
        event.kind  = static_cast<InputKind>(rawKind);
        event.vk    = vk;
        event.flags = flags;
        return event;
    }
    }

    return std::nullopt;  // 未知 kind
}

} // namespace lanrdp
