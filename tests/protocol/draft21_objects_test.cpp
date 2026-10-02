#include "moq/interop/wire/draft21/objects.h"
#include <gtest/gtest.h>
#include <limits>
namespace moq::interop::wire::draft21 {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> v) { Bytes r;for(auto n:v)r.push_back(static_cast<std::byte>(n));return r; }
FetchDecoder decoder(FetchGroupOrder order=FetchGroupOrder::Ascending, Limits limits={}) {
 return FetchDecoder([order](std::uint64_t id)->std::optional<FetchGroupOrder>{return id==7?std::optional{order}:std::nullopt;},limits);
}
TEST(Draft21Fetch, LiteralNonzeroFirstObjectAndZeroPayloadHaveNoStatusByte) {
 auto d=decoder();auto r=d.push(b({5,7,0x1f,9,3,11,99,0,0,0}),true);
 ASSERT_FALSE(r.error);ASSERT_TRUE(r.header);EXPECT_EQ(r.header->request_id,7u);
 ASSERT_EQ(r.events.size(),2u);const auto& o=std::get<ObjectEvent>(r.events[0]);
 EXPECT_EQ(o.group_id,9u);EXPECT_EQ(o.object_id,11u);EXPECT_EQ(o.subgroup_id,3u);
 EXPECT_EQ(o.publisher_priority,99u);EXPECT_EQ(o.request_id,7u);EXPECT_EQ(o.serialization_flags,0x1fu);
 EXPECT_EQ(o.stream_offset,2u);EXPECT_EQ(o.stream_end_offset,8u);EXPECT_EQ(o.payload_length,0u);EXPECT_FALSE(o.status);
 EXPECT_EQ(std::get<ObjectEvent>(r.events[1]).object_id,12u);EXPECT_TRUE(r.clean_fin);
}
TEST(Draft21Fetch, EveryByteFragmentAndEveryIncompleteFinAreDistinguished) {
 const auto data=b({5,7,0x1c,9,11,99,3,1,2,3});
 for(std::size_t n=0;n<data.size();++n) {
  if(n==2)continue; // Complete empty FETCH response is valid.
  auto d=decoder();auto r=d.push(std::span{data}.first(n),true);EXPECT_FALSE(r.clean_fin);EXPECT_TRUE(r.error);
 }
 auto d=decoder();std::vector<FetchEvent> events;
 for(auto byte:data) {auto r=d.push(std::span{&byte,1},false);ASSERT_FALSE(r.error);events.insert(events.end(),r.events.begin(),r.events.end());}
 ASSERT_EQ(events.size(),1u);EXPECT_EQ(std::get<ObjectEvent>(events[0]).retained_payload,b({1,2,3}));
 EXPECT_TRUE(d.push({},true).clean_fin);EXPECT_TRUE(d.push({},false).local_api_misuse);
 auto empty=decoder();EXPECT_TRUE(empty.push(b({5,7}),true).clean_fin);
}
TEST(Draft21Fetch, MissingFirstFieldsAndUnknownFlagsHaveExplicitPhases) {
 for(auto flags:{0x18u,0x14u,0x0cu,0x1du,0x1eu}) {
  auto d=decoder();auto r=d.push(b({5,7,flags}),false);ASSERT_TRUE(r.error);ASSERT_FALSE(r.observations.empty());
  EXPECT_EQ(r.observations.back().phase,FetchDecodePhase::SerializationFlags);EXPECT_EQ(r.error->offset,2u);
 }
 auto unknown=decoder();EXPECT_TRUE(unknown.push(b({5,7,0x80,0x80}),false).error);
 auto d=decoder();auto r=d.push(b({5,7,0x5f,9,11,99,0}),true);ASSERT_FALSE(r.error);EXPECT_TRUE(r.clean_fin);
 EXPECT_FALSE(std::get<ObjectEvent>(r.events[0]).subgroup_id);
}
TEST(Draft21Fetch, RangesPreserveActualPriorityAndSubgroupAndHaveNoPayloadLength) {
 for(const auto& marker:{b({0x80,0x8c}),b({0x81,0x0c}),b({0x82,0x0c})}) {
  auto data=b({5,7,0x1f,9,3,11,99,0});data.insert(data.end(),marker.begin(),marker.end());
  const auto tail=b({12,4,1,0});data.insert(data.end(),tail.begin(),tail.end());
  auto d=decoder();auto r=d.push(data,true);ASSERT_FALSE(r.error);ASSERT_EQ(r.events.size(),3u);
  const auto& range=std::get<FetchRangeEvent>(r.events[1]);EXPECT_EQ(range.group_id,12u);EXPECT_EQ(range.request_id,7u);
  const auto& o=std::get<ObjectEvent>(r.events[2]);EXPECT_EQ(o.group_id,12u);EXPECT_EQ(o.object_id,5u);
  EXPECT_EQ(o.subgroup_id,3u);EXPECT_EQ(o.publisher_priority,99u);EXPECT_TRUE(r.clean_fin);
 }
 auto d=decoder();auto r=d.push(b({5,7,0x82,0x0c,8,9,0}),false);EXPECT_TRUE(r.error);
 auto leading=decoder();auto valid=leading.push(b({5,7,0x82,0x0c,8,9,0x10,99,0}),true);
 ASSERT_FALSE(valid.error);ASSERT_EQ(valid.events.size(),2u);
 const auto& after=std::get<ObjectEvent>(valid.events[1]);EXPECT_EQ(after.group_id,8u);EXPECT_EQ(after.object_id,10u);
 auto lead_delta=decoder();auto delta=lead_delta.push(b({5,7,0x82,0x0c,8,9,0x1c,0,3,99,0}),true);
 ASSERT_FALSE(delta.error);EXPECT_EQ(std::get<ObjectEvent>(delta.events[1]).group_id,9u);

}
TEST(Draft21Fetch, DeltaOrderAndUint64BoundsAreChecked) {
 for(auto order:{FetchGroupOrder::Ascending,FetchGroupOrder::Descending}) {
  auto d=decoder(order);auto r=d.push(b({5,7,0x1c,9,11,99,0,0x0c,2,4,0}),true);ASSERT_FALSE(r.error);ASSERT_EQ(r.events.size(),2u);
  EXPECT_EQ(std::get<ObjectEvent>(r.events[1]).group_id,order==FetchGroupOrder::Ascending?12u:6u);
  EXPECT_EQ(std::get<ObjectEvent>(r.events[1]).object_id,4u);
 }
 auto desc=decoder(FetchGroupOrder::Descending);EXPECT_TRUE(desc.push(b({5,7,0x1c,0,0,99,0,8,0}),false).error);
 for(auto flags:{0u,4u,8u,2u}) {
  auto data=b({5,7,0x1f});const auto max=b({255,255,255,255,255,255,255,255,255});
  data.insert(data.end(),max.begin(),max.end());data.insert(data.end(),max.begin(),max.end());data.insert(data.end(),max.begin(),max.end());
  const auto tail=b({99,0,flags,1});data.insert(data.end(),tail.begin(),tail.end());
  auto d=decoder();auto r=d.push(data,false);
  if(flags==2u) {ASSERT_FALSE(r.observations.empty());EXPECT_EQ(r.observations.back().kind,DecoderObservationKind::DraftAmbiguity);}
  else EXPECT_TRUE(r.error);
 }
 FetchDecoder unknown([](auto)->std::optional<FetchGroupOrder>{return {};});auto r=unknown.push(b({5,7,0x1c,9,11,99,0,8,0}),false);
 ASSERT_FALSE(r.observations.empty());EXPECT_EQ(r.observations.back().kind,DecoderObservationKind::DraftAmbiguity);EXPECT_FALSE(r.clean_fin);
}
TEST(Draft21Fetch, DeltaEncodedPropertiesScopeAndBoundedRetention) {
 auto d=decoder(FetchGroupOrder::Ascending,{32,2});auto r=d.push(b({5,7,0x3c,9,11,99,4,2,1,4,2,4,1,2,3,4}),true);
 ASSERT_FALSE(r.error);ASSERT_EQ(r.events.size(),1u);const auto& o=std::get<ObjectEvent>(r.events[0]);
 ASSERT_EQ(o.properties.size(),2u);EXPECT_EQ(o.properties[0].type,2u);EXPECT_EQ(o.properties[1].type,6u);EXPECT_EQ(o.retained_payload,b({1,2}));
 for(const auto& props:{b({4,0}),b({0x0e,0}),b({0x22,1}),b({0x30,0}),b({0xc0,0x40,0,0}),b({0x0b,2,0x0b,0}),b({0x3c,1,0,1}),b({0x3e,12}),b({3,2,1})}) {
  auto data=b({5,7,0x3c,9,11,99,static_cast<unsigned>(props.size())});data.insert(data.end(),props.begin(),props.end());data.push_back(std::byte{0});
  auto invalid=decoder();EXPECT_TRUE(invalid.push(data,true).error);
 }
 auto limited=decoder(FetchGroupOrder::Ascending,{2,2});EXPECT_TRUE(limited.push(b({5,7,0x3c,9,11,99,3}),false).error);
 auto many=b({5,7,0x1c,9,11,99,0});for(unsigned i=0;i<10000;++i){many.push_back(std::byte{0});many.push_back(std::byte{0});}
 auto stream=decoder();auto trailing=stream.push(many,true);ASSERT_FALSE(trailing.error);EXPECT_EQ(trailing.events.size(),10001u);EXPECT_LE(stream.buffered_byte_count(),9u);
 auto large=decoder(FetchGroupOrder::Ascending,{32,2});ASSERT_FALSE(large.push(b({5,7,0x1c,9,11,99,0x80,100}),false).error);
 Bytes payload(100,std::byte{1});auto final=large.push(payload,true);ASSERT_FALSE(final.error);EXPECT_EQ(std::get<ObjectEvent>(final.events[0]).retained_payload.size(),2u);
}
TEST(Draft21Fetch, WideVarintsFragmentedPropertiesAndPartialRangesRetainPhaseEvidence) {
 const auto data=b({5,7,0x3f,0x80,200,0x80,128,0x81,0,99,4,0x0b,2,2,1,0});
 for(std::size_t split=0;split<=data.size();++split) {
  auto d=decoder();auto first=d.push(std::span{data}.first(split),false);ASSERT_FALSE(first.error);
  auto last=d.push(std::span{data}.subspan(split),true);ASSERT_FALSE(last.error);EXPECT_TRUE(last.clean_fin);
  const auto& events=first.events.empty()?last.events:first.events;ASSERT_EQ(events.size(),1u);
  const auto& o=std::get<ObjectEvent>(events[0]);EXPECT_EQ(o.group_id,200u);EXPECT_EQ(o.subgroup_id,128u);EXPECT_EQ(o.object_id,256u);
  EXPECT_EQ(o.properties.front().type,0x0bu);
 }
 auto d=decoder();auto r=d.push(b({5,7,0x82,0x0c,8}),true);ASSERT_TRUE(r.error);
 ASSERT_FALSE(r.observations.empty());EXPECT_EQ(r.observations.back().phase,FetchDecodePhase::RangeObjectId);
 auto properties=decoder();auto truncated=properties.push(b({5,7,0x3c,9,11,99,3,2}),true);
 ASSERT_TRUE(truncated.error);EXPECT_EQ(truncated.observations.back().phase,FetchDecodePhase::Properties);
 auto malformed=decoder();auto kv=malformed.push(b({5,7,0x3c,9,11,99,1,2}),true);
 ASSERT_TRUE(kv.error);EXPECT_EQ(kv.error->code,DecodeErrorCode::ProtocolViolation);EXPECT_EQ(kv.error->offset,7u);
 auto wrong=decoder();EXPECT_TRUE(wrong.push(b({6,7}),false).error);
 auto moved=decoder();auto destination=std::move(moved);EXPECT_TRUE(moved.push({},false).local_api_misuse);
 EXPECT_TRUE(destination.push(b({5,7}),true).clean_fin);
}

TEST(Draft21Fetch, PropertyTypeOverflowAndOddLengthLimitPreserveProtocolViolation) {
 for(const auto& props:{b({255,255,255,255,255,255,255,255,255,0,1}),b({3,0xc1,0,0})}) {
  auto data=b({5,7,0x3c,9,11,99,static_cast<unsigned>(props.size())});
  data.insert(data.end(),props.begin(),props.end());data.push_back(std::byte{0});
  auto d=decoder();auto r=d.push(data,true);ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->code,DecodeErrorCode::ProtocolViolation);
 }
}

TEST(Draft21Fetch, NestedImmutableErrorUsesActualWireValueOffset) {
 auto d=decoder();auto r=d.push(b({5,7,0x3c,9,11,99,3,0x0b,1,2}),true);
 ASSERT_TRUE(r.error);EXPECT_EQ(r.error->offset,9u);
}

TEST(Draft21Subgroup, FragmentedHeaderAndObjectsPreserveIdentityAndFlags) {
 const auto data=b({0x5d,7,9,3,99,11,4,2,1,4,2,3,1,2,3,0,0,0,0});
 for(std::size_t split=0;split<=data.size();++split) {
  SubgroupDecoder d;
  auto first=d.push(std::span{data}.first(split),false);
  auto last=d.push(std::span{data}.subspan(split),true);
  ASSERT_FALSE(first.error);ASSERT_FALSE(last.error);EXPECT_TRUE(last.clean_fin);
  const auto& header=first.header?first.header:last.header;ASSERT_TRUE(header);
  EXPECT_EQ(header->track_alias,7u);EXPECT_EQ(header->subgroup_id,3u);
  EXPECT_EQ(header->stream_end_offset,5u);EXPECT_TRUE(header->first_object);
  auto objects=first.objects;objects.insert(objects.end(),last.objects.begin(),last.objects.end());
  ASSERT_EQ(objects.size(),2u);const auto& o=objects.front();
  EXPECT_EQ(o.group_id,9u);EXPECT_EQ(o.object_id,11u);EXPECT_EQ(o.publisher_priority,99u);
  EXPECT_EQ(o.forwarding_preference,ObjectForwardingPreference::Subgroup);
  EXPECT_EQ(o.subgroup_id,3u);EXPECT_EQ(o.subgroup_header_type,0x5du);
  EXPECT_TRUE(o.first_object);EXPECT_FALSE(objects.back().first_object);
  EXPECT_EQ(o.stream_offset,5u);EXPECT_EQ(o.stream_end_offset,15u);
  EXPECT_EQ(o.retained_payload,b({1,2,3}));ASSERT_EQ(o.properties.size(),2u);
  EXPECT_EQ(o.properties[1].type,6u);EXPECT_FALSE(o.status);
  EXPECT_EQ(objects.back().object_id,12u);EXPECT_EQ(objects.back().status,0u);
  EXPECT_EQ(last.final_object_id,12u);
 }
}
TEST(Draft21Subgroup, ModesInheritedPriorityAndLiteralFirstObjectFlag) {
 for(auto type:{0x30u,0x32u,0x34u}) {
  auto data=b({type,0,0});if(type==0x34u)data.push_back(std::byte{9});
  const auto tail=b({11,0,0});data.insert(data.end(),tail.begin(),tail.end());
  SubgroupDecoder d;auto r=d.push(data,true);ASSERT_FALSE(r.error);ASSERT_TRUE(r.header);
  ASSERT_EQ(r.objects.size(),1u);EXPECT_TRUE(r.clean_fin);const auto& o=r.objects[0];
  EXPECT_EQ(o.subgroup_id,type==0x30u?0u:type==0x32u?11u:9u);
  EXPECT_FALSE(o.first_object);EXPECT_TRUE(o.priority_inherited);EXPECT_FALSE(o.publisher_priority);
  EXPECT_EQ(o.stream_offset,type==0x34u?4u:3u);
 }
}
TEST(Draft21Subgroup, InvalidTypesAndOverflowKeepProtocolErrorsDistinctFromLimits) {
 for(auto type:{0u,5u,0x16u,0x76u,128u,255u}) {
  ByteWriter writer(9);ASSERT_TRUE(write_vi64(type,writer));SubgroupDecoder d;
  auto r=d.push(writer.bytes(),false);ASSERT_TRUE(r.error);
  EXPECT_EQ(r.error->code,DecodeErrorCode::ProtocolViolation);EXPECT_EQ(r.error->offset,0u);
 }
 ByteWriter writer(32);for(auto v:{0x30ull,0ull,0ull,std::numeric_limits<unsigned long long>::max(),0ull,0ull,0ull})ASSERT_TRUE(write_vi64(v,writer));
 SubgroupDecoder d;auto r=d.push(writer.bytes(),false);ASSERT_TRUE(r.error);ASSERT_EQ(r.objects.size(),1u);
 EXPECT_EQ(r.objects[0].object_id,std::numeric_limits<std::uint64_t>::max());
 EXPECT_EQ(r.error->code,DecodeErrorCode::ProtocolViolation);EXPECT_EQ(r.error->offset,14u);
 SubgroupDecoder limited({2,0});auto cap=limited.push(b({0x31,0,0,0,3}),false);
 ASSERT_TRUE(cap.error);EXPECT_EQ(cap.error->code,DecodeErrorCode::LengthExceedsLimit);
 EXPECT_TRUE(cap.observations.empty());
}
TEST(Draft21Subgroup, FinAndMovedFromApiDistinguishIncompleteAndCleanStreams) {
 const auto data=b({0x15,7,9,3,99,11,2,2,1,2,1,2});
 for(std::size_t n=0;n<data.size();++n) {
  SubgroupDecoder d;auto r=d.push(std::span{data}.first(n),true);
  if(n==5){EXPECT_TRUE(r.clean_fin);continue;}
  EXPECT_FALSE(r.clean_fin);EXPECT_FALSE(r.error);ASSERT_FALSE(r.observations.empty());
  EXPECT_EQ(r.observations.back().kind,n<5?DecoderObservationKind::DraftAmbiguity:DecoderObservationKind::ShouldClose);
  EXPECT_TRUE(d.push({},false).local_api_misuse);
 }
 SubgroupDecoder d;auto moved=std::move(d);EXPECT_TRUE(d.push({},false).local_api_misuse);
 EXPECT_TRUE(moved.push(b({0x30,0,0}),true).clean_fin);
 SubgroupDecoder mode1;auto no_id=mode1.push(b({0x32,0,0}),true);
 EXPECT_TRUE(no_id.clean_fin);ASSERT_FALSE(no_id.observations.empty());
 EXPECT_EQ(no_id.observations.back().kind,DecoderObservationKind::DraftAmbiguity);
}
TEST(Draft21Subgroup, PropertiesNestedImmutableScopeAndRawBytesArePreserved) {
 const auto good=b({0x31,7,9,11,8,0x0b,6,0x80,2,0x80,1,0,2,1,5});
 SubgroupDecoder d;auto r=d.push(good,true);ASSERT_FALSE(r.error);ASSERT_EQ(r.objects.size(),1u);
 ASSERT_EQ(r.objects[0].properties.size(),1u);
 EXPECT_EQ(std::get<Bytes>(r.objects[0].properties[0].value),b({0x80,2,0x80,1,0,2}));
 for(const auto& props:{b({4,0}),b({0x0e,0}),b({0x22,1}),b({0x30,0}),b({0xc0,0x40,0,0}),b({0x0b,2,4,0}),b({0x0b,2,0x0b,0}),b({0x3e,12}),b({3,2,1})}) {
  auto data=b({0x31,7,9,11,static_cast<unsigned>(props.size())});data.insert(data.end(),props.begin(),props.end());data.insert(data.end(),{std::byte{0},std::byte{0}});
  SubgroupDecoder invalid;EXPECT_TRUE(invalid.push(data,true).error);
 }
 SubgroupDecoder nested;auto bad=nested.push(b({0x31,7,9,11,3,0x0b,1,2}),true);
 ASSERT_TRUE(bad.error);EXPECT_EQ(bad.error->offset,7u);
}
TEST(Draft21Subgroup, StatusRulesAndBoundedPayloadRetention) {
 for(auto status:{3u,4u}) {
  SubgroupDecoder d;auto r=d.push(b({0x30,0,0,11,0,status}),true);
  ASSERT_FALSE(r.error);ASSERT_EQ(r.objects.size(),1u);EXPECT_EQ(r.objects[0].status,status);
  EXPECT_EQ(r.objects[0].end_of_group,status==3u);
  SubgroupDecoder nonnormal;auto bad=nonnormal.push(b({0x31,0,0,11,2,2,1,0,status}),true);
  ASSERT_TRUE(bad.error);EXPECT_EQ(bad.error->code,DecodeErrorCode::ProtocolViolation);
 }
 SubgroupDecoder unknown;auto r=unknown.push(b({0x30,0,0,11,0,2}),true);
 ASSERT_FALSE(r.error);ASSERT_FALSE(r.observations.empty());EXPECT_EQ(r.observations[0].kind,DecoderObservationKind::ShouldClose);
 SubgroupDecoder d({32,2});ASSERT_FALSE(d.push(b({0x30,0,0,11,0x80,100}),false).error);
 Bytes payload(100,std::byte{1});std::vector<ObjectEvent> objects;
 for(auto byte:payload){auto part=d.push(std::span{&byte,1},false);ASSERT_FALSE(part.error);EXPECT_LE(d.buffered_byte_count(),32u);objects.insert(objects.end(),part.objects.begin(),part.objects.end());}
 EXPECT_TRUE(d.push({},true).clean_fin);ASSERT_EQ(objects.size(),1u);EXPECT_EQ(objects[0].retained_payload.size(),2u);EXPECT_EQ(objects[0].payload_length,100u);
}

TEST(Draft21Subgroup, EveryByteWideVarintsAndUint64MaximumRemainBounded) {
 ByteWriter writer(128);
 for(auto v:{0x55ull,200ull,256ull,128ull})ASSERT_TRUE(write_vi64(v,writer));
 ASSERT_TRUE(writer.append_byte(std::byte{99}));
 ASSERT_TRUE(write_vi64(std::numeric_limits<std::uint64_t>::max(),writer));
 for(auto v:{4ull,11ull,2ull})ASSERT_TRUE(write_vi64(v,writer));
 ASSERT_TRUE(writer.append_bytes(b({2,1})));for(auto v:{0ull,0ull})ASSERT_TRUE(write_vi64(v,writer));
 SubgroupDecoder d;std::optional<SubgroupHeader> header;std::vector<ObjectEvent> objects;
 for(auto byte:writer.bytes()) {
  auto r=d.push(std::span{&byte,1},false);ASSERT_FALSE(r.error);EXPECT_LE(d.buffered_byte_count(),128u);
  if(r.header)header=r.header;
  objects.insert(objects.end(),r.objects.begin(),r.objects.end());
 }
 ASSERT_TRUE(header);EXPECT_EQ(header->group_id,256u);EXPECT_EQ(header->track_alias,200u);
 ASSERT_EQ(objects.size(),1u);EXPECT_EQ(objects[0].object_id,std::numeric_limits<std::uint64_t>::max());
 EXPECT_EQ(objects[0].subgroup_id,128u);EXPECT_EQ(objects[0].status,0u);
 EXPECT_TRUE(d.push({},true).clean_fin);
}
TEST(Draft21Subgroup, TerminalStatusAndMandatoryPropertiesLengthDoNotInventObjects) {
 SubgroupDecoder terminal;auto after=terminal.push(b({0x30,0,0,0,0,3,0}),false);
 ASSERT_TRUE(after.error);EXPECT_EQ(after.error->offset,6u);ASSERT_EQ(after.objects.size(),1u);
 SubgroupDecoder missing;auto unfinished=missing.push(b({0x31,0,0,9}),true);
 EXPECT_FALSE(unfinished.clean_fin);EXPECT_TRUE(unfinished.objects.empty());ASSERT_FALSE(unfinished.observations.empty());
 EXPECT_EQ(unfinished.observations[0].phase,SubgroupDecodePhase::PropertiesLength);
 SubgroupDecoder empty_properties;auto ambiguous=empty_properties.push(b({0x31,0,0,9,0,0,3}),true);
 EXPECT_FALSE(ambiguous.error);ASSERT_EQ(ambiguous.objects.size(),1u);ASSERT_FALSE(ambiguous.observations.empty());
 EXPECT_EQ(ambiguous.observations[0].kind,DecoderObservationKind::DraftAmbiguity);
 SubgroupDecoder no_retention({32,0});auto payload=no_retention.push(b({0x30,0,0,0,1,42}),true);
 ASSERT_FALSE(payload.error);ASSERT_EQ(payload.objects.size(),1u);EXPECT_TRUE(payload.objects[0].retained_payload.empty());
 EXPECT_EQ(payload.objects[0].payload_length,1u);
}

}
}
