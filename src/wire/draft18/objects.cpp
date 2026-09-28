#include "moq/interop/wire/draft18/objects.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace moq::interop::wire::draft18 {
namespace {

constexpr std::uint64_t kPaddingDatagramType = 0x132b3e29;

DecodeError protocol_violation(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, std::move(detail)};
}

DecodeError truncated_field(const NeedMore& need, std::string detail) {
    return protocol_violation(need.offset, std::move(detail));
}

DecodeError malformed_properties(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::KeyValueFormattingError, offset,
            std::move(detail)};
}

bool is_valid_object_type(std::uint64_t type) {
    if (type <= 0x0f) return true;
    if (type < 0x20 || type > 0x2f) return false;
    return (type & 0x02u) == 0u;
}

DatagramDecodeResult decode_padding(Cursor& working) {
    std::optional<std::size_t> first_nonzero_offset;
    while (working.remaining() != 0) {
        const auto byte_offset = working.offset();
        const auto byte_result = read_bytes(working, 1);
        if (const auto* need = std::get_if<NeedMore>(&byte_result)) {
            return truncated_field(*need, "padding byte is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&byte_result)) return *error;
        if (std::get<std::span<const std::byte>>(byte_result).front() !=
            std::byte{0}) {
            if (!first_nonzero_offset) first_nonzero_offset = byte_offset;
        }
    }
    return DiscardedPaddingDatagram{first_nonzero_offset};
}

}  // namespace

DatagramDecodeResult decode_datagram(std::span<const std::byte> bytes,
                                     const Limits& limits) {
    Cursor working(bytes);
    const auto type_offset = working.offset();
    const auto type_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) {
        return truncated_field(*need, "datagram Type is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&type_result)) return *error;
    const auto type = std::get<std::uint64_t>(type_result);

    if (type == kPaddingDatagramType) {
        auto padding_result = decode_padding(working);
        return padding_result;
    }
    if (!is_valid_object_type(type)) {
        return protocol_violation(type_offset, "unknown or invalid datagram type");
    }

    const auto track_alias_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&track_alias_result)) {
        return truncated_field(*need, "Track Alias is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&track_alias_result)) return *error;
    const auto group_id_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&group_id_result)) {
        return truncated_field(*need, "Group ID is truncated");
    }
    if (const auto* error = std::get_if<DecodeError>(&group_id_result)) return *error;

    std::uint64_t object_id = 0;
    if ((type & 0x04u) == 0u) {
        const auto object_id_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&object_id_result)) {
            return truncated_field(*need, "Object ID is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&object_id_result)) return *error;
        object_id = std::get<std::uint64_t>(object_id_result);
    }

    std::optional<std::uint8_t> publisher_priority;
    if ((type & 0x08u) == 0u) {
        const auto priority_result = read_bytes(working, 1);
        if (const auto* need = std::get_if<NeedMore>(&priority_result)) {
            return truncated_field(*need, "Publisher Priority is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&priority_result)) return *error;
        publisher_priority = std::to_integer<std::uint8_t>(
            std::get<std::span<const std::byte>>(priority_result).front());
    }

    KeyValuePairs properties;
    if ((type & 0x01u) != 0u) {
        const auto properties_length_offset = working.offset();
        const auto properties_length_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&properties_length_result)) {
            return truncated_field(*need, "Object Properties length is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&properties_length_result)) {
            return *error;
        }
        const auto declared = std::get<std::uint64_t>(properties_length_result);
        if (declared == 0) {
            return protocol_violation(properties_length_offset,
                                      "present Object Properties are empty");
        }
        if (declared > std::numeric_limits<std::size_t>::max()) {
            return DecodeError{DecodeErrorCode::LengthNotRepresentable,
                               properties_length_offset,
                               "Object Properties length is not representable"};
        }
        const auto properties_length = static_cast<std::size_t>(declared);
        if (properties_length > limits.maximum_object_properties_length) {
            return DecodeError{DecodeErrorCode::LengthExceedsLimit,
                               properties_length_offset,
                               "Object Properties exceed configured limit"};
        }
        const auto properties_result =
            decode_key_value_pairs(working, properties_length, limits);
        if (auto* decoded = std::get_if<KeyValuePairs>(&properties_result)) {
            properties = std::move(*decoded);
        } else if (const auto* need = std::get_if<NeedMore>(&properties_result)) {
            return malformed_properties(
                need->offset,
                "Object Properties are shorter than their declared length");
        } else if (const auto* error = std::get_if<DecodeError>(&properties_result)) {
            if (error->code == DecodeErrorCode::InvalidValue) {
                return malformed_properties(error->offset,
                                            "Object Property KVP is malformed");
            }
            return *error;
        } else {
            return std::get<DraftAmbiguity>(properties_result);
        }
    }

    std::optional<std::uint64_t> status;
    std::size_t payload_length = 0;
    std::vector<std::byte> retained_payload;
    if ((type & 0x20u) != 0u) {
        const auto status_offset = working.offset();
        const auto status_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&status_result)) {
            return truncated_field(*need, "Object Status is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&status_result)) return *error;
        status = std::get<std::uint64_t>(status_result);
        if (working.remaining() != 0) {
            return protocol_violation(working.offset(),
                                      "bytes follow Object Status");
        }
        if (!properties.empty() && *status != 0) {
            return protocol_violation(status_offset,
                                      "non-Normal status has Object Properties");
        }
    } else {
        payload_length = working.remaining();
        const auto retained_length =
            std::min(payload_length, limits.maximum_retained_payload_length);
        const auto retained_result = read_bytes(working, retained_length);
        if (const auto* need = std::get_if<NeedMore>(&retained_result)) {
            return truncated_field(*need, "Object payload evidence is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&retained_result)) return *error;
        const auto retained =
            std::get<std::span<const std::byte>>(retained_result);
        retained_payload.assign(retained.begin(), retained.end());
        const auto discarded_result = read_bytes(working, working.remaining());
        if (const auto* need = std::get_if<NeedMore>(&discarded_result)) {
            return truncated_field(*need, "Object payload is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&discarded_result)) return *error;
    }

    return ObjectEvent{
        type,
        std::get<std::uint64_t>(track_alias_result),
        std::get<std::uint64_t>(group_id_result),
        object_id,
        publisher_priority,
        (type & 0x02u) != 0u,
        std::move(properties),
        status,
        payload_length,
        std::move(retained_payload),
        ObjectForwardingPreference::Datagram,
        std::nullopt,
        std::nullopt,
        false,
        (type & 0x08u) != 0u,
        0,
        bytes.size(),
    };
}

class SubgroupDecoder::Impl {
public:
    explicit Impl(Limits limits) : limits_(limits) {}

    SubgroupPushResult push(std::span<const std::byte> bytes, bool fin) {
        SubgroupPushResult result;
        if (terminal_) {
            result.local_api_misuse = true;
            return result;
        }
        if (bytes.size() > std::numeric_limits<std::size_t>::max() - offset_) {
            fail(result, DecodeErrorCode::OffsetOverflow, offset_,
                 "subgroup stream offset overflows size_t");
            return result;
        }

        std::size_t index = 0;
        while (index < bytes.size() && !terminal_) {
            if (phase_ == Phase::ObjectDelta && terminal_status_) {
                fail(result, DecodeErrorCode::ProtocolViolation, offset_,
                     "Object follows End of Group or End of Track status");
                break;
            }

            switch (phase_) {
                case Phase::Type:
                    if (const auto value = consume_vi(bytes, index)) {
                        raw_type_ = *value;
                        if (!valid_subgroup_type(raw_type_)) {
                            fail(result, DecodeErrorCode::ProtocolViolation,
                                 vi_offset_,
                                 "unknown, invalid, or reserved subgroup stream type");
                            break;
                        }
                        properties_present_ = (raw_type_ & 0x01u) != 0u;
                        subgroup_mode_ = static_cast<unsigned>((raw_type_ & 0x06u) >> 1u);
                        end_of_group_ = (raw_type_ & 0x08u) != 0u;
                        priority_inherited_ = (raw_type_ & 0x20u) != 0u;
                        first_object_ = (raw_type_ & 0x40u) != 0u;
                        phase_ = Phase::TrackAlias;
                    }
                    break;
                case Phase::TrackAlias:
                    if (const auto value = consume_vi(bytes, index)) {
                        track_alias_ = *value;
                        phase_ = Phase::GroupId;
                    }
                    break;
                case Phase::GroupId:
                    if (const auto value = consume_vi(bytes, index)) {
                        group_id_ = *value;
                        if (subgroup_mode_ == 0u) {
                            subgroup_id_ = 0;
                            next_after_subgroup_id(result);
                        } else if (subgroup_mode_ == 1u) {
                            next_after_subgroup_id(result);
                        } else {
                            phase_ = Phase::SubgroupId;
                        }
                    }
                    break;
                case Phase::SubgroupId:
                    if (const auto value = consume_vi(bytes, index)) {
                        subgroup_id_ = *value;
                        next_after_subgroup_id(result);
                    }
                    break;
                case Phase::Priority:
                    publisher_priority_ =
                        std::to_integer<std::uint8_t>(bytes[index]);
                    ++index;
                    ++offset_;
                    finish_header(result);
                    break;
                case Phase::ObjectDelta:
                    if (vi_size_ == 0u) current_object_offset_ = offset_;
                    if (const auto value = consume_vi(bytes, index)) {
                        if (!previous_object_id_) {
                            current_object_id_ = *value;
                            if (subgroup_mode_ == 1u) subgroup_id_ = *value;
                        } else {
                            if (*previous_object_id_ ==
                                    std::numeric_limits<std::uint64_t>::max() ||
                                *value > std::numeric_limits<std::uint64_t>::max() -
                                             *previous_object_id_ - 1u) {
                                fail(result, DecodeErrorCode::ProtocolViolation,
                                     current_object_offset_,
                                     "subgroup Object ID delta overflows uint64");
                                break;
                            }
                            current_object_id_ = *previous_object_id_ + *value + 1u;
                        }
                        current_properties_.clear();
                        properties_length_ = 0;
                        property_bytes_.clear();
                        retained_payload_.clear();
                        current_payload_length_ = 0;
                        payload_remaining_ = 0;
                        current_status_.reset();
                        phase_ = properties_present_ ? Phase::PropertiesLength
                                                     : Phase::PayloadLength;
                    }
                    break;
                case Phase::PropertiesLength:
                    if (const auto value = consume_vi(bytes, index)) {
                        if (*value > std::numeric_limits<std::size_t>::max()) {
                            fail(result, DecodeErrorCode::LengthNotRepresentable,
                                 vi_offset_,
                                 "Object Properties length is not representable");
                            break;
                        }
                        properties_length_ = static_cast<std::size_t>(*value);
                        if (properties_length_ >
                            limits_.maximum_object_properties_length) {
                            fail(result, DecodeErrorCode::LengthExceedsLimit,
                                 vi_offset_,
                                 "Object Properties exceed configured limit");
                            break;
                        }
                        property_bytes_.reserve(properties_length_);
                        if (properties_length_ == 0u) {
                            phase_ = Phase::PayloadLength;
                        } else {
                            properties_payload_offset_ = offset_;
                            phase_ = Phase::PropertiesBytes;
                        }
                    }
                    break;
                case Phase::PropertiesBytes:
                    consume_properties(bytes, index, result);
                    break;
                case Phase::PayloadLength:
                    if (const auto value = consume_vi(bytes, index)) {
                        current_payload_length_ = *value;
                        payload_remaining_ = *value;
                        if (payload_remaining_ == 0u) {
                            phase_ = Phase::Status;
                        } else {
                            phase_ = Phase::Payload;
                        }
                    }
                    break;
                case Phase::Status:
                    if (const auto value = consume_vi(bytes, index)) {
                        status_offset_ = vi_offset_;
                        current_status_ = *value;
                        finish_object(result);
                    }
                    break;
                case Phase::Payload:
                    consume_payload(bytes, index, result);
                    break;
            }
        }

        if (fin && !terminal_) finish_stream(result);
        return result;
    }

    [[nodiscard]] std::size_t buffered_byte_count() const noexcept {
        auto total = vi_size_;
        total = saturating_add(total, property_bytes_.capacity());
        total = saturating_add(total, retained_payload_.capacity());
        total = saturating_add(
            total, saturating_multiply(current_properties_.capacity(),
                                       sizeof(KeyValuePair)));
        for (const auto& property : current_properties_) {
            if (const auto* integer = std::get_if<VarIntValue>(&property.value)) {
                total = saturating_add(total, integer->raw_bytes.capacity());
            } else {
                total = saturating_add(
                    total, std::get<ByteValue>(property.value).bytes.capacity());
            }
        }
        return total;
    }

private:
    enum class Phase {
        Type,
        TrackAlias,
        GroupId,
        SubgroupId,
        Priority,
        ObjectDelta,
        PropertiesLength,
        PropertiesBytes,
        PayloadLength,
        Status,
        Payload,
    };

    static std::size_t vi_width(std::byte first_byte) {
        const auto first = std::to_integer<std::uint8_t>(first_byte);
        if (first == 0xffu) return 9u;
        std::size_t leading_ones = 0;
        auto mask = std::uint8_t{0x80};
        while ((first & mask) != 0u) {
            ++leading_ones;
            mask = static_cast<std::uint8_t>(mask >> 1u);
        }
        return leading_ones + 1u;
    }

    static bool valid_subgroup_type(std::uint64_t type) {
        if (type > 0x7fu || (type & 0x10u) == 0u) return false;
        return (type & 0x06u) != 0x06u;
    }

    static std::size_t saturating_add(std::size_t left,
                                      std::size_t right) noexcept {
        if (right > std::numeric_limits<std::size_t>::max() - left) {
            return std::numeric_limits<std::size_t>::max();
        }
        return left + right;
    }

    static std::size_t saturating_multiply(std::size_t left,
                                           std::size_t right) noexcept {
        if (left != 0u &&
            right > std::numeric_limits<std::size_t>::max() / left) {
            return std::numeric_limits<std::size_t>::max();
        }
        return left * right;
    }

    std::optional<std::uint64_t> consume_vi(std::span<const std::byte> bytes,
                                             std::size_t& index) {
        if (vi_size_ == 0u) {
            vi_offset_ = offset_;
            vi_expected_ = vi_width(bytes[index]);
        }
        while (index < bytes.size() && vi_size_ < vi_expected_) {
            vi_bytes_[vi_size_++] = bytes[index++];
            ++offset_;
        }
        if (vi_size_ != vi_expected_) return std::nullopt;

        const auto first = std::to_integer<std::uint8_t>(vi_bytes_[0]);
        std::uint64_t value = 0;
        if (vi_expected_ < 9u) {
            const auto payload_bits = static_cast<unsigned>(8u - vi_expected_);
            const auto payload_mask = static_cast<std::uint8_t>(
                (std::uint32_t{1} << payload_bits) - 1u);
            value = first & payload_mask;
        }
        for (std::size_t byte_index = 1; byte_index < vi_expected_; ++byte_index) {
            value = (value << 8u) |
                    std::to_integer<std::uint8_t>(vi_bytes_[byte_index]);
        }
        vi_size_ = 0;
        vi_expected_ = 0;
        return value;
    }

    void next_after_subgroup_id(SubgroupPushResult& result) {
        if (priority_inherited_) {
            finish_header(result);
        } else {
            phase_ = Phase::Priority;
        }
    }

    void finish_header(SubgroupPushResult& result) {
        header_ = SubgroupHeader{raw_type_,
                                 track_alias_,
                                 group_id_,
                                 subgroup_id_,
                                 publisher_priority_,
                                 properties_present_,
                                 end_of_group_,
                                 first_object_,
                                 priority_inherited_,
                                 0,
                                 offset_};
        result.header = header_;
        header_complete_ = true;
        phase_ = Phase::ObjectDelta;
    }

    void consume_properties(std::span<const std::byte> bytes,
                            std::size_t& index,
                            SubgroupPushResult& result) {
        const auto needed = properties_length_ - property_bytes_.size();
        const auto available = bytes.size() - index;
        const auto count = std::min(needed, available);
        property_bytes_.insert(property_bytes_.end(), bytes.begin() +
                                                        static_cast<std::ptrdiff_t>(index),
                               bytes.begin() + static_cast<std::ptrdiff_t>(index + count));
        index += count;
        offset_ += count;
        if (property_bytes_.size() != properties_length_) return;

        Cursor cursor(property_bytes_, properties_payload_offset_);
        auto decoded =
            decode_key_value_pairs(cursor, property_bytes_.size(), limits_);
        if (auto* entries = std::get_if<KeyValuePairs>(&decoded)) {
            current_properties_ = std::move(*entries);
            std::vector<std::byte>().swap(property_bytes_);
            phase_ = Phase::PayloadLength;
            return;
        }
        if (const auto* error = std::get_if<DecodeError>(&decoded)) {
            auto code = error->code;
            if (code == DecodeErrorCode::InvalidValue) {
                code = DecodeErrorCode::KeyValueFormattingError;
            }
            fail(result, code, error->offset, error->detail);
            return;
        }
        if (const auto* need = std::get_if<NeedMore>(&decoded)) {
            fail(result, DecodeErrorCode::KeyValueFormattingError, need->offset,
                 "Object Property KVP is malformed");
            return;
        }
        const auto& ambiguity = std::get<DraftAmbiguity>(decoded);
        result.observations.push_back(
            {DecoderObservationKind::DraftAmbiguity,
             SubgroupDecodePhase::Properties, ambiguity.offset,
             ambiguity.detail});
        phase_ = Phase::PayloadLength;
    }

    void consume_payload(std::span<const std::byte> bytes,
                         std::size_t& index,
                         SubgroupPushResult& result) {
        const auto available = bytes.size() - index;
        const auto count64 = std::min<std::uint64_t>(payload_remaining_, available);
        const auto count = static_cast<std::size_t>(count64);
        const auto retain_capacity = limits_.maximum_retained_payload_length -
                                     retained_payload_.size();
        const auto retain = std::min(count, retain_capacity);
        retained_payload_.insert(
            retained_payload_.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(index),
            bytes.begin() + static_cast<std::ptrdiff_t>(index + retain));
        index += count;
        offset_ += count;
        payload_remaining_ -= count64;
        if (payload_remaining_ == 0u) finish_object(result);
    }

    void finish_object(SubgroupPushResult& result) {
        if (current_status_ && *current_status_ != 0u &&
            !current_properties_.empty()) {
            fail(result, DecodeErrorCode::ProtocolViolation, status_offset_,
                 "non-Normal status has nonempty Object Properties");
            return;
        }
        if (current_status_ && *current_status_ != 0u && properties_present_ &&
            properties_length_ == 0u) {
            result.observations.push_back({
                DecoderObservationKind::DraftAmbiguity,
                SubgroupDecodePhase::Status, status_offset_,
                "empty Properties on non-Normal subgroup Object has conflicting draft wording"});
        }
        if (current_status_ && *current_status_ != 0u &&
            *current_status_ != 3u && *current_status_ != 4u) {
            result.observations.push_back({
                DecoderObservationKind::ShouldClose,
                SubgroupDecodePhase::Status, status_offset_,
                "undefined Object Status should be treated as a protocol error"});
        }

        const auto object_id = current_object_id_;
        result.objects.push_back(ObjectEvent{
            std::nullopt,
            track_alias_,
            group_id_,
            object_id,
            publisher_priority_,
            current_status_ && *current_status_ == 3u,
            std::move(current_properties_),
            current_status_,
            current_payload_length_,
            std::move(retained_payload_),
            ObjectForwardingPreference::Subgroup,
            subgroup_id_,
            raw_type_,
            first_object_ && object_count_ == 0u,
            priority_inherited_,
            current_object_offset_,
            offset_,
        });
        previous_object_id_ = object_id;
        ++object_count_;
        if (current_status_ && (*current_status_ == 3u || *current_status_ == 4u)) {
            terminal_status_ = true;
        }
        phase_ = Phase::ObjectDelta;
    }

    void finish_stream(SubgroupPushResult& result) {
        if (!header_complete_) {
            result.observations.push_back({
                DecoderObservationKind::DraftAmbiguity, public_phase(), offset_,
                "draft does not prescribe receiver behavior for FIN in SUBGROUP_HEADER"});
            terminal_ = true;
            return;
        }
        if (phase_ == Phase::ObjectDelta && vi_size_ == 0u) {
            result.clean_fin = true;
            if (subgroup_mode_ == 1u && object_count_ == 0u) {
                result.observations.push_back({
                    DecoderObservationKind::DraftAmbiguity,
                    SubgroupDecodePhase::ObjectIdDelta, offset_,
                    "mode-1 Subgroup ID is undefined when FIN precedes every Object"});
            }
            if (end_of_group_ && previous_object_id_) {
                result.final_object_id = previous_object_id_;
            }
            terminal_ = true;
            return;
        }

        result.observations.push_back({
            DecoderObservationKind::ShouldClose, public_phase(), offset_,
            "FIN terminates subgroup stream in the middle of a serialized Object"});
        terminal_ = true;
    }

    [[nodiscard]] SubgroupDecodePhase public_phase() const noexcept {
        switch (phase_) {
            case Phase::Type:
                return SubgroupDecodePhase::Type;
            case Phase::TrackAlias:
                return SubgroupDecodePhase::TrackAlias;
            case Phase::GroupId:
                return SubgroupDecodePhase::GroupId;
            case Phase::SubgroupId:
                return SubgroupDecodePhase::SubgroupId;
            case Phase::Priority:
                return SubgroupDecodePhase::Priority;
            case Phase::ObjectDelta:
                return SubgroupDecodePhase::ObjectIdDelta;
            case Phase::PropertiesLength:
                return SubgroupDecodePhase::PropertiesLength;
            case Phase::PropertiesBytes:
                return SubgroupDecodePhase::Properties;
            case Phase::PayloadLength:
                return SubgroupDecodePhase::PayloadLength;
            case Phase::Status:
                return SubgroupDecodePhase::Status;
            case Phase::Payload:
                return SubgroupDecodePhase::Payload;
        }
        return SubgroupDecodePhase::Type;
    }

    void fail(SubgroupPushResult& result, DecodeErrorCode code,
              std::size_t error_offset, std::string detail) {
        result.error = DecodeError{code, error_offset, std::move(detail)};
        terminal_ = true;
    }

    Limits limits_;
    Phase phase_{Phase::Type};
    bool terminal_{false};
    bool header_complete_{false};
    bool terminal_status_{false};
    std::size_t offset_{0};
    std::array<std::byte, 9> vi_bytes_{};
    std::size_t vi_size_{0};
    std::size_t vi_expected_{0};
    std::size_t vi_offset_{0};

    std::uint64_t raw_type_{0};
    std::uint64_t track_alias_{0};
    std::uint64_t group_id_{0};
    unsigned subgroup_mode_{0};
    std::optional<std::uint64_t> subgroup_id_;
    std::optional<std::uint8_t> publisher_priority_;
    bool properties_present_{false};
    bool end_of_group_{false};
    bool first_object_{false};
    bool priority_inherited_{false};
    SubgroupHeader header_{};

    std::size_t current_object_offset_{0};
    std::uint64_t current_object_id_{0};
    std::optional<std::uint64_t> previous_object_id_;
    std::size_t object_count_{0};
    std::size_t properties_length_{0};
    std::size_t properties_payload_offset_{0};
    std::vector<std::byte> property_bytes_;
    KeyValuePairs current_properties_;
    std::uint64_t current_payload_length_{0};
    std::uint64_t payload_remaining_{0};
    std::vector<std::byte> retained_payload_;
    std::optional<std::uint64_t> current_status_;
    std::size_t status_offset_{0};
};

SubgroupDecoder::SubgroupDecoder(Limits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

SubgroupDecoder::~SubgroupDecoder() = default;
SubgroupDecoder::SubgroupDecoder(SubgroupDecoder&&) noexcept = default;
SubgroupDecoder& SubgroupDecoder::operator=(SubgroupDecoder&&) noexcept = default;

SubgroupPushResult SubgroupDecoder::push(std::span<const std::byte> bytes,
                                         bool fin) {
    if (!impl_) {
        SubgroupPushResult result;
        result.local_api_misuse = true;
        return result;
    }
    return impl_->push(bytes, fin);
}

std::size_t SubgroupDecoder::buffered_byte_count() const noexcept {
    return impl_ ? impl_->buffered_byte_count() : 0u;
}

}  // namespace moq::interop::wire::draft18
