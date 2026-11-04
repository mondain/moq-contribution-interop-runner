#include "moq/interop/session/draft23_session.h"

#include "moq/interop/wire/draft23/frame.h"
#include "moq/interop/wire/draft23/messages.h"
#include "moq/interop/wire/draft23/setup.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace moq::interop::session {
namespace {
namespace d23 = wire::draft23;
using transport::StreamId;
using transport::TransportStatus;
bool success(TransportStatus status) { return status == TransportStatus::Success; }
}

class Draft23Session::Impl {
public:
    Impl(transport::SessionTransport& transport, Draft23SessionConfig config)
        : transport_(transport), config_(std::move(config)) {
        if (config_.track_name.empty() || config_.track_namespace.size() > 32 ||
            !config_.maximum_streams || !config_.maximum_stream_bytes || !config_.maximum_buffered_bytes)
            throw std::invalid_argument("draft-23 smoke requires a track and positive buffer limits");
        if (config_.probe) stats_.probe = Draft23ProbeResult{};
    }
    enum class Kind { Unknown, Control, Request, Subgroup, Padding };
    struct Stream {
        Kind kind{Kind::Unknown};
        std::vector<std::byte> bytes;
        bool fin{false}, first{true}, local{false}, header{false}, properties{false}, done{false};
        bool media_request{false}, rejected_data_stopped{false};
        std::uint64_t alias{0}, group{0}, previous{0};
        bool has_previous{false};
        std::optional<std::size_t> trace;
        std::optional<std::size_t> request_trace;
    };
    struct Pending { StreamId id; std::vector<std::byte> bytes; std::size_t offset{0}; bool fin{false}; };
    transport::SessionTransport& transport_;
    Draft23SessionConfig config_;
    Draft23SessionStats stats_;
    std::optional<std::string> error_;
    std::optional<std::uint64_t> application_error_;
    std::map<StreamId,Stream> streams_;
    std::deque<Pending> pending_;
    std::optional<StreamId> control_, subscription_, probe_stream_;
    std::optional<std::uint64_t> alias_;
    std::set<std::uint64_t> request_ids_;
    std::set<std::uint64_t> rejected_aliases_;
    std::set<std::pair<std::uint64_t,std::uint64_t>> locations_;
    std::uint64_t peer_message_limit_{65535};
    std::size_t queued_{0};

    static constexpr std::size_t kMaximumTracedStreams = 4096;
    Draft23DataStream* traced(StreamId id, Stream& stream) {
        if (!stream.trace) {
            if (stats_.data_streams.size() >= kMaximumTracedStreams) { stats_.data_streams_truncated = true; return nullptr; }
            stats_.data_streams.push_back({}); stats_.data_streams.back().stream_id = id;
            stream.trace = stats_.data_streams.size() - 1;
        }
        return &stats_.data_streams[*stream.trace];
    }
    Draft23RequestStream& traced_request(StreamId id, Stream& stream) {
        if (!stream.request_trace) {
            stats_.requests.push_back({}); stats_.requests.back().stream_id = id; stats_.requests.back().local = stream.local;
            stream.request_trace = stats_.requests.size() - 1;
        }
        return stats_.requests[*stream.request_trace];
    }
    void fail(std::string detail, std::optional<std::uint64_t> code = {}) {
        if (error_) return;
        error_ = std::move(detail);
        application_error_ = code;
        if (code) {
            const auto bytes = std::as_bytes(std::span(*error_));
            transport_.close(*code, bytes);
        }
    }
    void flush() {
        while (!pending_.empty() && !error_) {
            auto& write = pending_.front();
            const auto remaining = std::span(write.bytes).subspan(write.offset);
            const auto result = transport_.write(write.id, remaining, write.fin);
            if (result.accepted > remaining.size()) { fail("transport accepted more bytes than offered"); return; }
            write.offset += result.accepted;
            queued_ -= result.accepted;
            if (result.status != TransportStatus::Success && result.status != TransportStatus::Partial &&
                result.status != TransportStatus::WouldBlock) { fail("transport could not write draft-23 message"); return; }
            if (write.offset == write.bytes.size()) { pending_.pop_front(); continue; }
            return;
        }
    }
    void queue(StreamId id, std::span<const std::byte> bytes, bool fin=false) {
        if (bytes.size() > config_.maximum_buffered_bytes - std::min(queued_, config_.maximum_buffered_bytes)) {
            fail("harness limit: pending draft-23 writes"); return;
        }
        pending_.push_back({id,{bytes.begin(),bytes.end()},0,fin}); queued_ += bytes.size();
    }
    template<class Message> void send(StreamId id, Message message, bool fin=false) {
        wire::ByteWriter writer(65553);
        if (!d23::encode_message(d23::Message{std::move(message)},writer,peer_message_limit_)) {
            fail("harness could not encode draft-23 message"); return;
        }
        queue(id,writer.bytes(),fin);
    }
    void subscribe() {
        if (subscription_ || stats_.subscription_established || error_) return;
        const auto opened = transport_.open_bidi();
        if (!success(opened.status)) { fail("harness could not open draft-23 subscription stream"); return; }
        if (streams_.size() >= config_.maximum_streams) { fail("harness limit: active streams"); return; }
        subscription_ = opened.stream_id;
        auto& stream = streams_[opened.stream_id]; stream.kind=Kind::Request; stream.local=true;
        send(opened.stream_id,d23::SubscribeMessage{1,config_.track_namespace,config_.track_name,{}});
    }
    void connect(const transport::ConnectionEstablishedEvent& event) {
        if (stats_.connected) { fail("duplicate transport establishment"); return; }
        const std::string alpn(reinterpret_cast<const char*>(event.alpn.data()),event.alpn.size());
        if (alpn != "moqt-23") { fail("transport did not negotiate moqt-23"); return; }
        stats_.connected=true;
        const auto opened=transport_.open_uni();
        if (!success(opened.status)) { fail("harness could not open draft-23 control stream"); return; }
        d23::SetupMessage setup;
        setup.options.push_back({7,std::vector<std::byte>{std::byte{'m'},std::byte{'o'},std::byte{'q'},
            std::byte{'-'},std::byte{'i'},std::byte{'n'},std::byte{'t'},std::byte{'e'},std::byte{'r'},std::byte{'o'},std::byte{'p'}}});
        if (config_.probe) {
            draft23_probe_setup_options(*config_.probe,setup.options);
            std::stable_sort(setup.options.begin(),setup.options.end(),[](const auto& a,const auto& b) { return a.type<b.type; });
        }
        wire::ByteWriter writer(65553);
        if (!d23::encode_setup(setup,writer)) { fail("harness could not encode SETUP"); return; }
        queue(opened.stream_id,writer.bytes());
        // A server-only option probe is the SETUP itself.
        if (config_.probe && config_.probe->channel==Draft23ProbeChannel::SetupOption) stats_.probe->injected=true;
    }
    // The malformed input of a Bidi or Uni probe goes out on a new stream once the publisher's SETUP has arrived.
    void inject_probe() {
        if (!config_.probe || stats_.probe->injected || config_.probe->channel==Draft23ProbeChannel::SetupOption) return;
        const auto opened=config_.probe->channel==Draft23ProbeChannel::Bidi?transport_.open_bidi():transport_.open_uni();
        if (!success(opened.status)) { fail("harness could not open the probe stream"); return; }
        queue(opened.stream_id,draft23_probe_bytes(*config_.probe,peer_message_limit_));
        probe_stream_=opened.stream_id;
        if (config_.probe->channel==Draft23ProbeChannel::Bidi) {
            // The publisher may answer on this stream. Anything it sends is recorded as the probe's reply and otherwise
            // ignored, so a reply never ends the run before the publisher has had its chance to close the session.
            Stream reply; reply.kind=Kind::Padding; reply.local=true;
            streams_[opened.stream_id]=std::move(reply);
        }
        stats_.probe->injected=true;
    }
    void consume(Stream& stream, std::size_t count) {
        stream.bytes.erase(stream.bytes.begin(),stream.bytes.begin()+static_cast<std::ptrdiff_t>(count));
    }
    bool register_request(std::uint64_t id) {
        if ((id&1) || !request_ids_.insert(id).second) {
            if (!(id&1)) stats_.duplicate_request_id=true;
            fail("invalid publisher Request ID",0x4); return false;
        }
        if (request_ids_.size()>config_.maximum_streams) { fail("harness limit: request history"); return false; }
        return true;
    }
    bool namespace_matches(const std::vector<std::vector<std::byte>>& prefix) const {
        return prefix.size() <= config_.track_namespace.size() &&
            std::equal(prefix.begin(),prefix.end(),config_.track_namespace.begin());
    }
    void request(StreamId id, Stream& stream) {
        request_messages(id,stream);
        if (stream.fin && stream.bytes.empty()) {
            auto& record=traced_request(id,stream);
            record.fin=true; record.fin_after_publish_done=record.publish_done.has_value();
        }
    }
    void request_messages(StreamId id, Stream& stream) {
        while (!stream.bytes.empty() && !error_) {
            wire::Cursor cursor(stream.bytes);
            auto decoded=d23::decode_message_with_error(cursor);
            if (std::holds_alternative<wire::NeedMore>(decoded)) {
                if (stream.fin) fail("truncated draft-23 request message",0x300);
                return;
            }
            if (const auto* issue=std::get_if<d23::MessageDecodeFailure>(&decoded)) {
                if (issue->error.code==wire::DecodeErrorCode::LengthExceedsLimit) stats_.oversize_control_message=true;
                fail("draft-23 request decode: "+issue->error.detail,
                    issue->unsupported?std::nullopt:std::optional<std::uint64_t>{d23::session_error_code(*issue)});
                return;
            }
            auto message=std::get<d23::Message>(std::move(decoded));
            {
                wire::Cursor peek(stream.bytes);
                const auto frame=d23::decode_frame(peek,std::numeric_limits<std::uint64_t>::max());
                if (const auto* parsed=std::get_if<d23::Frame>(&frame))
                    traced_request(id,stream).messages.push_back({parsed->type,parsed->body.size()});
            }
            consume(stream,cursor.offset());
            std::visit([&](const auto& value) {
                using T=std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T,d23::PublishNamespaceMessage>) {
                    if (stream.local || !stream.first) { fail("PUBLISH_NAMESPACE on wrong stream",0x303); return; }
                    traced_request(id,stream).request_id=value.request_id;
                    if (!register_request(value.request_id)) return;
                    send(id,d23::RequestOkMessage{});
                    if (namespace_matches(value.track_namespace)) { stats_.namespace_announced=true; subscribe(); }
                } else if constexpr (std::is_same_v<T,d23::PublishMessage>) {
                    if (stream.local || !stream.first) { fail("PUBLISH on wrong stream",0x303); return; }
                    traced_request(id,stream).request_id=value.request_id;
                    if (!register_request(value.request_id)) return;
                    if (value.track_namespace!=config_.track_namespace || value.track_name!=config_.track_name) {
                        rejected_aliases_.insert(value.track_alias);
                        send(id,d23::RequestErrorMessage{0x20,0,{},{}},true);
                        return;
                    }
                    if (stats_.subscription_established || subscription_) {
                        fail("harness smoke does not support simultaneous PUBLISH and SUBSCRIBE"); return;
                    }
                    alias_=value.track_alias; stats_.subscription_alias=value.track_alias;
                    rejected_aliases_.erase(value.track_alias);
                    stream.media_request=true; traced_request(id,stream).established=true;
                    stats_.subscription_established=true;
                    send(id,d23::RequestOkMessage{});
                } else if constexpr (std::is_same_v<T,d23::SubscribeOkMessage>) {
                    if (!stream.local || !stream.first || subscription_!=id) {
                        fail("unexpected SUBSCRIBE_OK",0x303); return;
                    }
                    alias_=value.track_alias; stats_.subscription_alias=value.track_alias; rejected_aliases_.erase(value.track_alias);
                    stats_.subscription_established=true; stream.media_request=true; traced_request(id,stream).established=true;
                } else if constexpr (std::is_same_v<T,d23::RequestErrorMessage>) {
                    fail("publisher rejected request: code="+std::to_string(value.error_code));
                } else if constexpr (std::is_same_v<T,d23::PublishDoneMessage>) {
                    if (stream.first || stream.done || !stream.media_request) {
                        fail("unexpected PUBLISH_DONE",0x303); return;
                    }
                    // FIN/data streams may arrive after this request-stream message.
                    stream.done=true;
                    traced_request(id,stream).publish_done=Draft23PublishDone{value.status_code,value.stream_count};
                } else {
                    fail("harness smoke does not implement this draft-23 request message");
                }
            },message);
            stream.first=false;
        }
    }
    void control(Stream& stream) {
        if (stream.fin) { fail("publisher closed control stream",0x306); return; }
        wire::Cursor cursor(stream.bytes);
        if (!stats_.peer_setup) {
            wire::Cursor probe(stream.bytes);
            const auto seen=d23::decode_setup_observation(probe);
            if (const auto* observed=std::get_if<d23::SetupObservation>(&seen))
                stats_.peer_setup=Draft23PeerSetup{observed->options,observed->frame_length};
        }
        const auto setup=d23::decode_setup(cursor);
        if (std::holds_alternative<wire::NeedMore>(setup)) return;
        if (const auto* issue=std::get_if<wire::DecodeError>(&setup)) {
            if (issue->code==wire::DecodeErrorCode::LengthExceedsLimit) stats_.oversize_control_message=true;
            fail("invalid publisher SETUP: "+issue->detail,
                issue->code==wire::DecodeErrorCode::LengthExceedsLimit?0x401:
                issue->code==wire::DecodeErrorCode::InvalidValue?0x301:0x300); return;
        }
        const auto& value=std::get<d23::SetupMessage>(setup);
        std::set<std::uint64_t> known;
        for (const auto& option:value.options) {
            const bool singleton=option.type==1 || (option.type>=4 && option.type<=9);
            if (singleton && !known.insert(option.type).second) {
                fail("duplicate known SETUP option",0x301); return;
            }
            if (config_.webtransport && (option.type==1 || option.type==5)) {
                fail("publisher used native QUIC SETUP option on WebTransport",option.type==1?0x8:0x19); return;
            }
        }
        peer_message_limit_=value.maximum_control_message_size;
        consume(stream,cursor.offset()); stats_.setup_received=true;
        inject_probe();
        if (config_.subscribe_immediately) subscribe();
        if (!stream.bytes.empty()) fail("harness smoke does not implement post-SETUP control messages");
    }
    void subgroup(StreamId id, Stream& stream) {
        if (!stream.header) {
            wire::Cursor cursor(stream.bytes);
            const auto type=wire::read_vi64(cursor);
            if (!std::holds_alternative<std::uint64_t>(type)) return;
            const auto flags=std::get<std::uint64_t>(type);
            if (flags>=128 || !(flags&0x10) || (flags&6)==6) { fail("invalid subgroup header",0x200); return; }
            const auto alias=wire::read_vi64(cursor), group=wire::read_vi64(cursor);
            if (!std::holds_alternative<std::uint64_t>(alias) || !std::holds_alternative<std::uint64_t>(group)) {
                if (stream.fin) fail("truncated subgroup header",0x200);
                return;
            }
            std::optional<std::uint64_t> explicit_subgroup;
            if ((flags&6)==4) {
                const auto subgroup_id=wire::read_vi64(cursor);
                if (!std::holds_alternative<std::uint64_t>(subgroup_id)) {
                    if (stream.fin) fail("truncated subgroup ID",0x200);
                    return;
                }
                explicit_subgroup=std::get<std::uint64_t>(subgroup_id);
            }
            if (!(flags&0x20) && !std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor,1))) {
                if (stream.fin) fail("truncated publisher priority",0x200);
                return;
            }
            stream.alias=std::get<std::uint64_t>(alias); stream.group=std::get<std::uint64_t>(group);
            stream.properties=(flags&1)!=0; stream.header=true; consume(stream,cursor.offset());
            if (auto* t=traced(id,stream)) {
                t->track_alias=stream.alias; t->group_id=stream.group;
                if ((flags&6)==0) t->subgroup_id=0;
                else if (explicit_subgroup) t->subgroup_id=explicit_subgroup;
            }
        }
        if (rejected_aliases_.contains(stream.alias)) {
            if (!stream.rejected_data_stopped) {
                (void)transport_.stop_sending(id,1); // CANCELLED, section 12.5.
                stream.rejected_data_stopped=true;
            }
            stream.bytes.clear(); return;
        }
        if (!alias_) return; // Response and data streams can arrive in either order.
        if (stream.alias!=*alias_) { fail("harness smoke received data for an unknown Track Alias"); return; }
        while (!stream.bytes.empty() && !error_) {
            wire::Cursor cursor(stream.bytes);
            const auto delta=wire::read_vi64(cursor);
            if (!std::holds_alternative<std::uint64_t>(delta)) break;
            std::uint64_t property_bytes=0;
            if (stream.properties) {
                const auto length=wire::read_vi64(cursor);
                if (!std::holds_alternative<std::uint64_t>(length)) break;
                property_bytes=std::get<std::uint64_t>(length);
                if (property_bytes>config_.maximum_stream_bytes) { fail("harness limit: Object Properties"); return; }
                const auto properties=wire::read_bytes(cursor,static_cast<std::size_t>(property_bytes));
                if (!std::holds_alternative<std::span<const std::byte>>(properties)) break;
                wire::Cursor property_cursor(std::get<std::span<const std::byte>>(properties));
                if (!std::holds_alternative<wire::draft21::KeyValues>(wire::draft21::decode_key_values_to_end(property_cursor))) {
                    fail("malformed Object Properties",0x201); return;
                }
            }
            const auto length=wire::read_vi64(cursor);
            if (!std::holds_alternative<std::uint64_t>(length)) break;
            const auto payload_length=std::get<std::uint64_t>(length);
            if (payload_length>config_.maximum_stream_bytes) { fail("harness limit: Object payload"); return; }
            std::uint64_t status=0;
            if (!payload_length) {
                const auto parsed=wire::read_vi64(cursor);
                if (!std::holds_alternative<std::uint64_t>(parsed)) break;
                status=std::get<std::uint64_t>(parsed);
                if ((status!=0 && status!=3 && status!=4) || (status && property_bytes)) {
                    fail("invalid Object Status or Properties",0x202); return;
                }
            }
            if (!std::holds_alternative<std::span<const std::byte>>(wire::read_bytes(cursor,static_cast<std::size_t>(payload_length)))) break;
            auto object=std::get<std::uint64_t>(delta);
            if (stream.has_previous) {
                if (stream.previous==std::numeric_limits<std::uint64_t>::max() ||
                    object>std::numeric_limits<std::uint64_t>::max()-stream.previous-1) {
                    fail("subgroup Object ID delta overflow",0x400); return;
                }
                object+=stream.previous+1;
            }
            stream.previous=object; stream.has_previous=true;
            consume(stream,cursor.offset());
            if (auto* t=traced(id,stream)) {
                ++t->objects;
                if (!t->first_object_id) { t->first_object_id=object; if (!t->subgroup_id && (t->type_flags&6)==2) t->subgroup_id=object; }
            }
            ++stats_.objects; stats_.payload_bytes+=payload_length;
            if (!status) {
                locations_.insert({stream.group,object});
                stats_.distinct_objects=locations_.size();
                if (locations_.size()>65536) { fail("harness limit: observed Object locations"); return; }
            }
        }
        if (stream.fin && !stream.bytes.empty()) fail("truncated subgroup Object",0x203);
    }
    void process() {
        for (auto& [id,stream]:streams_) {
            if (error_) return;
            if (stream.kind==Kind::Unknown) {
                if ((id&3)==0) stream.kind=Kind::Request;
                else {
                    wire::Cursor cursor(stream.bytes);
                    const auto type=wire::read_vi64(cursor);
                    if (!std::holds_alternative<std::uint64_t>(type)) {
                        if (stream.fin) fail("truncated unidirectional stream type",0x200);
                        continue;
                    }
                    const auto value=std::get<std::uint64_t>(type);
                    if (value==0x2f00) {
                        if (control_ && control_!=id) { fail("duplicate publisher control stream",0x303); return; }
                        control_=id; stream.kind=Kind::Control;
                    } else if (value==0x132b3e28) { stream.kind=Kind::Padding; }
                    else if (value<128 && (value&0x10)) { stream.kind=Kind::Subgroup; if (auto* t=traced(id,stream)) { t->type_flags=value; t->valid_header=true; } }
                    else {
                        if (auto* t=traced(id,stream)) t->type_flags=value; fail(value==5?"harness smoke does not implement FETCH streams":"invalid publisher stream header",value==5?std::nullopt:std::optional<std::uint64_t>{0x200}); return; }
                }
            }
            if (stream.kind==Kind::Control) {
                if (!stats_.setup_received) control(stream);
                else if (stream.fin) fail("publisher closed control stream",0x306);
                else if (!stream.bytes.empty()) fail("harness smoke does not implement post-SETUP control messages");
            } else if (stats_.setup_received) {
                if (stream.kind==Kind::Request) request(id,stream);
                else if (stream.kind==Kind::Subgroup) subgroup(id,stream);
                else if (stream.kind==Kind::Padding) stream.bytes.clear();
            }
        }
        // Revisit early messages after SETUP and data that preceded its subscription response.
        if (stats_.setup_received && !error_) {
            for (auto& [id,stream]:streams_) if (stream.kind==Kind::Request) request(id,stream);
            for (auto& [id,stream]:streams_) if (stream.kind==Kind::Subgroup) subgroup(id,stream);
        }
        for (auto& entry:streams_) if (entry.second.trace && entry.second.fin) stats_.data_streams[*entry.second.trace].fin=true;
        for (auto it=streams_.begin();it!=streams_.end();) {
            if (it->second.fin && it->second.bytes.empty() &&
                (it->second.kind==Kind::Subgroup || it->second.kind==Kind::Padding)) it=streams_.erase(it);
            else ++it;
        }
    }
    void data(const transport::StreamDataEvent& event) {
        if (!stats_.connected) { fail("stream data before transport establishment"); return; }
        auto found=streams_.find(event.stream_id);
        if (found==streams_.end()) {
            if ((event.stream_id&1) || streams_.size()>=config_.maximum_streams) {
                fail((event.stream_id&1)?"unexpected data on local stream":"harness limit: active streams"); return;
            }
            found=streams_.emplace(event.stream_id,Stream{}).first;
        }
        auto& stream=found->second;
        if (stats_.probe && probe_stream_==event.stream_id) {
            stats_.probe->reply_bytes+=event.data.size();
            stats_.probe->reply_fin=stats_.probe->reply_fin||event.fin;
        }
        if (stream.fin) { fail("transport delivered bytes after stream FIN"); return; }
        std::size_t buffered=queued_;
        for (const auto& [id,entry]:streams_) { (void)id; buffered+=entry.bytes.size(); }
        if (event.data.size()>config_.maximum_stream_bytes-std::min(config_.maximum_stream_bytes,stream.bytes.size()) ||
            event.data.size()>config_.maximum_buffered_bytes-std::min(config_.maximum_buffered_bytes,buffered)) {
            fail("harness limit: buffered draft-23 stream bytes"); return;
        }
        stream.bytes.insert(stream.bytes.end(),event.data.begin(),event.data.end()); stream.fin=event.fin;
        process();
    }
    void event(const transport::TransportEvent& event) {
        if (error_) return;
        std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T,transport::ConnectionEstablishedEvent>) connect(value);
            else if constexpr (std::is_same_v<T,transport::StreamDataEvent>) data(value);
            else if constexpr (std::is_same_v<T,transport::PeerResetEvent> || std::is_same_v<T,transport::PeerStopSendingEvent>) {
                if (control_==value.stream_id) fail("publisher reset control stream",0x306);
                else if ((value.stream_id&3)==2) {
                    if constexpr (std::is_same_v<T,transport::PeerResetEvent>)
                        for (auto& traced_stream:stats_.data_streams) if (traced_stream.stream_id==value.stream_id) traced_stream.reset=true;
                    streams_.erase(value.stream_id);
                }
                else fail("publisher reset or stopped smoke stream");
            } else if constexpr (std::is_same_v<T,transport::PeerCloseEvent> || std::is_same_v<T,transport::LocalCloseEvent>) {
                if (stats_.probe && !stats_.probe->close)
                    stats_.probe->close=Draft23ProbeClose{value.error_space==transport::CloseErrorSpace::Application,value.error_code};
                if (value.error_space!=transport::CloseErrorSpace::Application || value.error_code!=0 || !stats_.progressing())
                    fail("session closed: code="+std::to_string(value.error_code));
            }
            else if constexpr (std::is_same_v<T,transport::DatagramEvent>) fail("harness smoke does not implement Object Datagrams");
            else fail("transport failure, timeout, or event overflow");
        },event);
        flush();
    }
};

Draft23Session::Draft23Session(transport::SessionTransport& transport,Draft23SessionConfig config)
    : impl_(std::make_unique<Impl>(transport,std::move(config))) {}
Draft23Session::~Draft23Session()=default;
void Draft23Session::on_event(const transport::TransportEvent& event) { impl_->event(event); }
void Draft23Session::flush() { impl_->flush(); }
const Draft23SessionStats& Draft23Session::stats() const noexcept { return impl_->stats_; }
const std::optional<std::string>& Draft23Session::error() const noexcept { return impl_->error_; }
std::optional<std::uint64_t> Draft23Session::application_error() const noexcept { return impl_->application_error_; }
} // namespace moq::interop::session
