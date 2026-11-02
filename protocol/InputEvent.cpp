#include "protocol/InputEvent.h"

#include "protocol/LE.h"

namespace lanrdp {

QByteArray encodeInputEvent(const InputEvent &event)
{
    QByteArray payload;

    switch (event.kind) {
    case InputKind::MouseMove:
        payload.append(static_cast<char>(InputKind::MouseMove));
        le::appendU16(payload, event.nx);
        le::appendU16(payload, event.ny);
        break;

    case InputKind::MouseButton:
        payload.append(static_cast<char>(InputKind::MouseButton));
        payload.append(static_cast<char>(event.button));
        payload.append(static_cast<char>(event.pressed ? 1 : 0));
        le::appendU16(payload, event.nx);
        le::appendU16(payload, event.ny);
        break;

    case InputKind::MouseWheel:
        payload.append(static_cast<char>(InputKind::MouseWheel));
        payload.append(static_cast<char>(static_cast<std::uint16_t>(event.wheelDelta) & 0xFF));
        payload.append(static_cast<char>((static_cast<std::uint16_t>(event.wheelDelta) >> 8) & 0xFF));
        le::appendU16(payload, event.nx);
        le::appendU16(payload, event.ny);
        break;

    case InputKind::KeyDown:
    case InputKind::KeyUp:
        payload.append(static_cast<char>(event.kind));
        le::appendU16(payload, event.vk);
        le::appendU16(payload, event.flags);
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
        if (!le::tryReadU16(payload, 1, &nx) || !le::tryReadU16(payload, 3, &ny))
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
        if (!le::tryReadU16(payload, 3, &nx) || !le::tryReadU16(payload, 5, &ny))
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
        if (!le::tryReadU16(payload, 3, &nx) || !le::tryReadU16(payload, 5, &ny))
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
        if (!le::tryReadU16(payload, 1, &vk) || !le::tryReadU16(payload, 3, &flags))
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
