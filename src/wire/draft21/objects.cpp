#include "moq/interop/wire/draft21/objects.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
namespace moq::interop::wire::draft21 {
namespace {
std::optional<DecodeError> validate_object_properties(
    const KeyValues& entries, std::span<const std::byte> property_bytes,
    std::size_t offset, std::uint64_t group_id, std::uint64_t object_id) {
    // One nested immutable list is legal; another wrapper is malformed.
    bool immutable_seen = false, group_gap_seen = false, object_gap_seen = false;
    auto check = [&](const KeyValues& list, bool nested, std::size_t list_offset) -> std::optional<DecodeError> {
        for (const auto& entry : list) {
            const auto type = entry.type;
            if (type == 4 || type == 0x0e || type == 0x22 || type == 0x30 ||
                (type >= 0x4000 && type <= 0x7fff))
                return DecodeError{DecodeErrorCode::ProtocolViolation, list_offset,
                                   "Track-only Property used on Object"};
            if (type == 0x0b && (nested || immutable_seen))
                return DecodeError{DecodeErrorCode::ProtocolViolation, list_offset,
                                   "nested or duplicate Object Immutable Properties"};
            if (type == 0x0b) immutable_seen = true;
            if (type == 0x3c || type == 0x3e) {
                auto& seen = type == 0x3c ? group_gap_seen : object_gap_seen;
                const auto gap = std::get<std::uint64_t>(entry.value);
                if (seen || gap > (type == 0x3c ? group_id : object_id))
                    return DecodeError{DecodeErrorCode::ProtocolViolation, list_offset,
                                       "duplicate or excessive Object prior gap"};
                seen = true;
            }
        }
        return std::nullopt;
    };
    if (auto error = check(entries, false, offset)) return error;
    Cursor source(property_bytes, offset);
    for (const auto& entry : entries) {
        // Rewalk the validated source block to preserve actual offsets,
        // including non-minimal varint widths, for immutable values.
        (void)read_vi64(source);
        if ((entry.type & 1u) == 0u) {
            (void)read_vi64(source);
            continue;
        }
        const auto raw = read_length_prefixed_bytes(source, 65535);
        const auto bytes = std::get<std::span<const std::byte>>(raw);
        if (entry.type != 0x0b) continue;
        const auto nested_offset = source.offset() - bytes.size();
        Cursor nested(bytes, nested_offset);
        auto decoded = decode_key_values_to_end(nested);
        if (auto* error = std::get_if<DecodeError>(&decoded)) return *error;
        // Semantic findings in this list must also refer to its wire block.
        if (const auto error = check(std::get<KeyValues>(decoded), true,
                                     nested_offset)) return error;
    }
    return std::nullopt;
}
}  // namespace

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
                                       sizeof(KeyValue)));
        for (const auto& property : current_properties_) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&property.value)) {
                total = saturating_add(total, bytes->capacity());
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
        auto decoded = decode_key_values_to_end(cursor);
        if (auto* entries = std::get_if<KeyValues>(&decoded)) {
            current_properties_ = std::move(*entries);
            if (auto error = validate_object_properties(current_properties_, property_bytes_,
                                                         properties_payload_offset_, group_id_,
                                                         current_object_id_)) {
                fail(result, error->code, error->offset, error->detail);
                return;
            }
            std::vector<std::byte>().swap(property_bytes_);
            phase_ = Phase::PayloadLength;
        } else if (const auto* error = std::get_if<DecodeError>(&decoded)) {
            fail(result, error->code, error->offset, error->detail);
        } else {
            fail(result, DecodeErrorCode::KeyValueFormattingError,
                 properties_payload_offset_, "incomplete Object Properties");
        }
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
            std::nullopt,
            std::nullopt,
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
        if (code == DecodeErrorCode::ProtocolViolation ||
            code == DecodeErrorCode::KeyValueFormattingError) {
            result.observations.push_back({DecoderObservationKind::ShouldClose,
                                           public_phase(), error_offset, detail});
        }
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
    KeyValues current_properties_;
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

class FetchDecoder::Impl {
public:
    Impl(FetchGroupOrderResolver resolver, Limits limits)
        : resolver_(std::move(resolver)), limits_(limits) {}

    FetchPushResult push(std::span<const std::byte> bytes, bool fin) {
        FetchPushResult result;
        if (terminal_) {
            result.local_api_misuse = true;
            return result;
        }
        if (bytes.size() > std::numeric_limits<std::size_t>::max() - offset_) {
            fail(result, DecodeErrorCode::OffsetOverflow, offset_,
                 "FETCH stream offset overflows size_t");
            return result;
        }

        std::size_t index = 0;
        while (index < bytes.size() && !terminal_) {
            switch (phase_) {
                case Phase::Type:
                    if (const auto value = consume_vi(bytes, index)) {
                        if (*value != 0x05u) {
                            fail(result, DecodeErrorCode::ProtocolViolation,
                                 vi_offset_, "stream is not FETCH_HEADER");
                        } else {
                            raw_type_ = *value;
                            phase_ = Phase::RequestId;
                        }
                    }
                    break;
                case Phase::RequestId:
                    if (const auto value = consume_vi(bytes, index)) {
                        request_id_ = *value;
                        group_order_ = resolver_ ? resolver_(request_id_)
                                                : std::nullopt;
                        header_complete_ = true;
                        result.header = FetchHeader{raw_type_, request_id_, 0,
                                                    offset_};
                        phase_ = Phase::Flags;
                    }
                    break;
                case Phase::Flags:
                    current_offset_ = vi_size_ == 0u ? offset_ : current_offset_;
                    if (const auto value = consume_vi(bytes, index)) {
                        flags_ = *value;
                        if (flags_ > 0x7fu && flags_ != 0x8cu &&
                            flags_ != 0x10cu && flags_ != 0x20cu) {
                            fail(result, DecodeErrorCode::ProtocolViolation,
                                 current_offset_,
                                 "unsupported FETCH Serialization Flags");
                            break;
                        }
                        if (flags_ == 0x8cu || flags_ == 0x10cu || flags_ == 0x20cu) {
                            phase_ = Phase::RangeGroup;
                            break;
                        }
                        if ((flags_ & 0x40u) != 0u &&
                            (flags_ & 0x03u) != 0u) {
                            result.observations.push_back({
                                DecoderObservationKind::ShouldViolation,
                                FetchDecodePhase::SerializationFlags,
                                current_offset_,
                                "Datagram FETCH Object should set its two least significant flag bits to zero"});
                        }
                        begin_object(result);
                    }
                    break;
                case Phase::GroupDelta:
                    if (const auto value = consume_vi(bytes, index)) {
                        group_delta_ = *value;
                        if (!resolve_group(result)) break;
                        next_after_group(result);
                    }
                    break;
                case Phase::SubgroupId:
                    if (const auto value = consume_vi(bytes, index)) {
                        current_subgroup_ = *value;
                        next_after_subgroup(result);
                    }
                    break;
                case Phase::ObjectDelta:
                    if (const auto value = consume_vi(bytes, index)) {
                        object_delta_ = *value;
                        if (!resolve_object(result)) break;
                        next_after_object();
                    }
                    break;
                case Phase::Priority:
                    current_priority_ =
                        std::to_integer<std::uint8_t>(bytes[index]);
                    ++index;
                    ++offset_;
                    next_after_priority();
                    break;
                case Phase::PropertiesLength:
                    if (const auto value = consume_vi(bytes, index)) {
                        if (*value > std::numeric_limits<std::size_t>::max()) {
                            fail(result, DecodeErrorCode::LengthNotRepresentable,
                                 vi_offset_,
                                 "FETCH Object Properties length is not representable");
                            break;
                        }
                        properties_length_ = static_cast<std::size_t>(*value);
                        if (properties_length_ >
                            limits_.maximum_object_properties_length) {
                            fail(result, DecodeErrorCode::LengthExceedsLimit,
                                 vi_offset_,
                                 "FETCH Object Properties exceed configured limit");
                            break;
                        }
                        property_bytes_.reserve(properties_length_);
                        if (properties_length_ == 0u) {
                            phase_ = Phase::PayloadLength;
                        } else {
                            properties_offset_ = offset_;
                            phase_ = Phase::Properties;
                        }
                    }
                    break;
                case Phase::Properties:
                    consume_properties(bytes, index, result);
                    break;
                case Phase::PayloadLength:
                    if (const auto value = consume_vi(bytes, index)) {
                        payload_length_ = *value;
                        payload_remaining_ = *value;
                        if (payload_remaining_ == 0u) finish_object(result);
                        else phase_ = Phase::Payload;
                    }
                    break;
                case Phase::Payload:
                    consume_payload(bytes, index, result);
                    break;
                case Phase::RangeGroup:
                    if (const auto value = consume_vi(bytes, index)) {
                        current_group_ = *value;
                        phase_ = Phase::RangeObject;
                    }
                    break;
                case Phase::RangeObject:
                    if (const auto value = consume_vi(bytes, index)) {
                        current_object_ = *value;
                        finish_range(result);
                    }
                    break;
            }
        }
        if (fin && !terminal_) finish_stream(result);
        return result;
    }

    [[nodiscard]] std::size_t buffered_byte_count() const noexcept {
        auto total = saturating_add(vi_size_, property_bytes_.capacity());
        total = saturating_add(total, retained_payload_.capacity());
        total = saturating_add(
            total, saturating_multiply(properties_.capacity(),
                                       sizeof(KeyValue)));
        for (const auto& property : properties_) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&property.value))
                total = saturating_add(total, bytes->capacity());
        }
        return total;
    }

private:
    enum class Phase {
        Type,
        RequestId,
        Flags,
        GroupDelta,
        SubgroupId,
        ObjectDelta,
        Priority,
        PropertiesLength,
        Properties,
        PayloadLength,
        Payload,
        RangeGroup,
        RangeObject,
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
            const auto bits = static_cast<unsigned>(8u - vi_expected_);
            const auto mask = static_cast<std::uint8_t>(
                (std::uint32_t{1} << bits) - 1u);
            value = first & mask;
        }
        for (std::size_t i = 1; i < vi_expected_; ++i) {
            value = (value << 8u) |
                    std::to_integer<std::uint8_t>(vi_bytes_[i]);
        }
        vi_size_ = 0;
        vi_expected_ = 0;
        return value;
    }

    void begin_object(FetchPushResult& result) {
        group_present_ = (flags_ & 0x08u) != 0u;
        object_present_ = (flags_ & 0x04u) != 0u;
        priority_present_ = (flags_ & 0x10u) != 0u;
        properties_present_ = (flags_ & 0x20u) != 0u;
        datagram_ = (flags_ & 0x40u) != 0u;
        subgroup_mode_ = static_cast<unsigned>(flags_ & 0x03u);
        if ((!prior_group_ && (!group_present_ || !object_present_)) ||
            (!last_actual_priority_ && !priority_present_) ||
            (!datagram_ && !last_actual_subgroup_ &&
             (subgroup_mode_ == 1u || subgroup_mode_ == 2u))) {
            fail(result, DecodeErrorCode::ProtocolViolation, current_offset_,
                 "FETCH Object references nonexistent prior Object state");
            return;
        }
        properties_.clear();
        property_bytes_.clear();
        retained_payload_.clear();
        properties_length_ = 0;
        payload_length_ = 0;
        payload_remaining_ = 0;
        if (group_present_) phase_ = Phase::GroupDelta;
        else {
            current_group_ = *prior_group_;
            next_after_group(result);
        }
    }

    bool resolve_group(FetchPushResult& result) {
        if (!prior_group_) {
            current_group_ = group_delta_;
            return true;
        }
        if (!group_order_) {
            result.observations.push_back({
                DecoderObservationKind::DraftAmbiguity,
                FetchDecodePhase::GroupIdDelta, vi_offset_,
                "FETCH Group delta cannot be resolved without Group Order"});
            terminal_ = true;
            return false;
        }
        if (*group_order_ == FetchGroupOrder::Ascending) {
            if (*prior_group_ == std::numeric_limits<std::uint64_t>::max() ||
                group_delta_ > std::numeric_limits<std::uint64_t>::max() -
                                   *prior_group_ - 1u) {
                fail(result, DecodeErrorCode::ProtocolViolation, vi_offset_,
                     "ascending FETCH Group ID delta overflows uint64");
                return false;
            }
            current_group_ = *prior_group_ + group_delta_ + 1u;
        } else {
            if (group_delta_ == std::numeric_limits<std::uint64_t>::max() ||
                group_delta_ + 1u > *prior_group_) {
                fail(result, DecodeErrorCode::ProtocolViolation, vi_offset_,
                     "descending FETCH Group ID delta underflows uint64");
                return false;
            }
            current_group_ = *prior_group_ - (group_delta_ + 1u);
        }
        return true;
    }

    void next_after_group(FetchPushResult& result) {
        if (datagram_) {
            current_subgroup_.reset();
            next_after_subgroup(result);
            return;
        }
        switch (subgroup_mode_) {
            case 0:
                current_subgroup_ = 0;
                next_after_subgroup(result);
                break;
            case 1:
                if (!last_actual_subgroup_) {
                    fail(result, DecodeErrorCode::ProtocolViolation,
                         current_offset_,
                         "FETCH Object references absent prior Subgroup ID");
                    break;
                }
                current_subgroup_ = last_actual_subgroup_;
                next_after_subgroup(result);
                break;
            case 2:
                if (!last_actual_subgroup_) {
                    fail(result, DecodeErrorCode::ProtocolViolation,
                         current_offset_,
                         "FETCH Object references absent prior Subgroup ID");
                    break;
                }
                if (*last_actual_subgroup_ ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    result.observations.push_back({
                        DecoderObservationKind::DraftAmbiguity,
                        FetchDecodePhase::SubgroupId, current_offset_,
                        "draft does not prescribe handling for FETCH Subgroup ID plus-one overflow"});
                    terminal_ = true;
                } else {
                    current_subgroup_ = *last_actual_subgroup_ + 1u;
                    next_after_subgroup(result);
                }
                break;
            case 3:
                phase_ = Phase::SubgroupId;
                break;
        }
    }

    void next_after_subgroup(FetchPushResult& result) {
        if (object_present_) phase_ = Phase::ObjectDelta;
        else {
            current_object_ = *prior_object_;
            if (*prior_object_ == std::numeric_limits<std::uint64_t>::max()) {
                fail(result, DecodeErrorCode::ProtocolViolation,
                     current_offset_,
                     "implicit FETCH Object ID increment overflows uint64");
            } else {
                current_object_ = *prior_object_ + 1u;
                next_after_object();
            }
        }
    }

    bool resolve_object(FetchPushResult& result) {
        if (!prior_object_) {
            current_object_ = object_delta_;
            return true;
        }
        if (group_present_) {
            current_object_ = object_delta_;
            return true;
        }
        if (object_delta_ > std::numeric_limits<std::uint64_t>::max() -
                                *prior_object_) {
            fail(result, DecodeErrorCode::ProtocolViolation, vi_offset_,
                 "FETCH Object ID delta overflows uint64");
            return false;
        }
        current_object_ = *prior_object_ + object_delta_;
        return true;
    }

    void next_after_object() {
        if (priority_present_) phase_ = Phase::Priority;
        else {
            current_priority_ = last_actual_priority_;
            next_after_priority();
        }
    }

    void next_after_priority() {
        phase_ = properties_present_ ? Phase::PropertiesLength
                                     : Phase::PayloadLength;
    }

    void consume_properties(std::span<const std::byte> bytes,
                            std::size_t& index, FetchPushResult& result) {
        const auto needed = properties_length_ - property_bytes_.size();
        const auto count = std::min(needed, bytes.size() - index);
        property_bytes_.insert(
            property_bytes_.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(index),
            bytes.begin() + static_cast<std::ptrdiff_t>(index + count));
        index += count;
        offset_ += count;
        if (property_bytes_.size() != properties_length_) return;
        Cursor cursor(property_bytes_, properties_offset_);
        auto decoded = decode_key_values_to_end(cursor);
        if (auto* entries = std::get_if<KeyValues>(&decoded)) {
            properties_ = std::move(*entries);
            if (auto error = validate_object_properties(properties_, property_bytes_, properties_offset_,
                                                        current_group_, current_object_)) {
                fail(result, error->code, error->offset, error->detail);
                return;
            }
            std::vector<std::byte>().swap(property_bytes_);
            phase_ = Phase::PayloadLength;
        } else if (const auto* error = std::get_if<DecodeError>(&decoded)) {
            fail(result, error->code, error->offset, error->detail);
        } else {
            fail(result, DecodeErrorCode::KeyValueFormattingError,
                 properties_offset_, "incomplete Object Properties");
        }
    }

    void consume_payload(std::span<const std::byte> bytes,
                         std::size_t& index, FetchPushResult& result) {
        const auto count64 = std::min<std::uint64_t>(
            payload_remaining_, bytes.size() - index);
        const auto count = static_cast<std::size_t>(count64);
        const auto available_retention =
            limits_.maximum_retained_payload_length - retained_payload_.size();
        const auto retain = std::min(count, available_retention);
        retained_payload_.insert(
            retained_payload_.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(index),
            bytes.begin() + static_cast<std::ptrdiff_t>(index + retain));
        index += count;
        offset_ += count;
        payload_remaining_ -= count64;
        if (payload_remaining_ == 0u) finish_object(result);
    }

    void finish_object(FetchPushResult& result) {
        ObjectEvent event{};
        event.track_alias = std::nullopt;
        event.group_id = current_group_;
        event.object_id = current_object_;
        event.publisher_priority = current_priority_;
        event.properties = std::move(properties_);
        event.payload_length = payload_length_;
        event.retained_payload = std::move(retained_payload_);
        event.forwarding_preference = datagram_
                                          ? ObjectForwardingPreference::Datagram
                                          : ObjectForwardingPreference::Subgroup;
        event.subgroup_id = current_subgroup_;
        event.first_object = !have_actual_;
        event.priority_inherited = !priority_present_;
        event.stream_offset = current_offset_;
        event.stream_end_offset = offset_;
        event.request_id = request_id_;
        event.serialization_flags = flags_;
        result.events.emplace_back(std::move(event));
        prior_group_ = current_group_;
        prior_object_ = current_object_;
        last_actual_subgroup_ = current_subgroup_;
        last_actual_priority_ = current_priority_;
        have_actual_ = true;
        reset_record();
    }

    void finish_range(FetchPushResult& result) {
        result.events.emplace_back(FetchRangeEvent{
            flags_ == 0x8cu ? FetchRangeKind::NonExistent
                            : flags_ == 0x10cu ? FetchRangeKind::Unknown
                                              : FetchRangeKind::TimedOut,
            flags_, current_group_, current_object_, current_offset_, offset_, request_id_});
        prior_group_ = current_group_;
        prior_object_ = current_object_;
        reset_record();
    }

    void reset_record() {
        phase_ = Phase::Flags;
    }

    void finish_stream(FetchPushResult& result) {
        if (header_complete_ && phase_ == Phase::Flags && vi_size_ == 0u) {
            result.clean_fin = true;
        } else {
            fail(result, DecodeErrorCode::ProtocolViolation, offset_,
                 "FIN terminates incomplete FETCH header or serialized record");
        }
        terminal_ = true;
    }

    FetchDecodePhase public_phase() const noexcept {
        switch (phase_) {
            case Phase::Type: return FetchDecodePhase::Type;
            case Phase::RequestId: return FetchDecodePhase::RequestId;
            case Phase::Flags: return FetchDecodePhase::SerializationFlags;
            case Phase::GroupDelta: return FetchDecodePhase::GroupIdDelta;
            case Phase::SubgroupId: return FetchDecodePhase::SubgroupId;
            case Phase::ObjectDelta: return FetchDecodePhase::ObjectIdDelta;
            case Phase::Priority: return FetchDecodePhase::Priority;
            case Phase::PropertiesLength: return FetchDecodePhase::PropertiesLength;
            case Phase::Properties: return FetchDecodePhase::Properties;
            case Phase::PayloadLength: return FetchDecodePhase::PayloadLength;
            case Phase::Payload: return FetchDecodePhase::Payload;
            case Phase::RangeGroup: return FetchDecodePhase::RangeGroupId;
            case Phase::RangeObject: return FetchDecodePhase::RangeObjectId;
        }
        return FetchDecodePhase::Type;
    }

    void fail(FetchPushResult& result, DecodeErrorCode code,
              std::size_t error_offset, std::string detail) {
        result.observations.push_back({DecoderObservationKind::ShouldClose,
                                       public_phase(), error_offset, detail});
        result.error = DecodeError{code, error_offset, std::move(detail)};
        terminal_ = true;
    }

    FetchGroupOrderResolver resolver_;
    Limits limits_;
    Phase phase_{Phase::Type};
    bool terminal_{false};
    bool header_complete_{false};
    std::size_t offset_{0};
    std::array<std::byte, 9> vi_bytes_{};
    std::size_t vi_size_{0};
    std::size_t vi_expected_{0};
    std::size_t vi_offset_{0};
    std::uint64_t raw_type_{0};
    std::uint64_t request_id_{0};
    std::optional<FetchGroupOrder> group_order_;

    std::size_t current_offset_{0};
    std::uint64_t flags_{0};
    bool group_present_{false};
    bool object_present_{false};
    bool priority_present_{false};
    bool properties_present_{false};
    bool datagram_{false};
    unsigned subgroup_mode_{0};
    std::uint64_t group_delta_{0};
    std::uint64_t object_delta_{0};
    std::uint64_t current_group_{0};
    std::uint64_t current_object_{0};
    std::optional<std::uint64_t> current_subgroup_;
    std::optional<std::uint8_t> current_priority_;
    std::size_t properties_length_{0};
    std::size_t properties_offset_{0};
    std::vector<std::byte> property_bytes_;
    KeyValues properties_;
    std::uint64_t payload_length_{0};
    std::uint64_t payload_remaining_{0};
    std::vector<std::byte> retained_payload_;

    bool have_actual_{false};
    std::optional<std::uint64_t> prior_group_;
    std::optional<std::uint64_t> prior_object_;
    std::optional<std::uint64_t> last_actual_subgroup_;
    std::optional<std::uint8_t> last_actual_priority_;
};

FetchDecoder::FetchDecoder(FetchGroupOrderResolver resolver, Limits limits)
    : impl_(std::make_unique<Impl>(std::move(resolver), limits)) {}
FetchDecoder::~FetchDecoder() = default;
FetchDecoder::FetchDecoder(FetchDecoder&&) noexcept = default;
FetchDecoder& FetchDecoder::operator=(FetchDecoder&&) noexcept = default;

FetchPushResult FetchDecoder::push(std::span<const std::byte> bytes, bool fin) {
    if (!impl_) {
        FetchPushResult result;
        result.local_api_misuse = true;
        return result;
    }
    return impl_->push(bytes, fin);
}

std::size_t FetchDecoder::buffered_byte_count() const noexcept {
    return impl_ ? impl_->buffered_byte_count() : 0u;
}

}
