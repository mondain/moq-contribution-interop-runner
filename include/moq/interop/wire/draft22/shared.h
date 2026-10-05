#pragma once

// Draft 21 wire modules that draft 22 reuses unchanged. A using-declaration makes the
// draft 22 name the same entity as the draft 21 one, so this file is the single record of
// what is shared. The evidence that each module is unchanged is the draft 22 text:
// tests/golden/draft22_wire_audit_test.cpp pins that the only wire figures that changed
// between drafts 21 and 22 are LOCATION_FILTER Parameter and PUBLISH_NAMESPACE Message
// (the latter has no draft 21 decoder), that the registry tables are identical, and that
// the message type table matches draft21::classify_message_type.
//
// Naming note: draft 22 renamed "Forwarding Preference" to "Delivery Mode" (editorial, no
// wire change); the re-exported ObjectForwardingPreference keeps its draft 21 name.
//
// Not shared, because draft 22 changes them: location_filter, publish (PublishParameter
// carries a LocationFilter), publisher_request (its variant holds the draft 22 PublishMessage).

#include "moq/interop/wire/draft21/control.h"
#include "moq/interop/wire/draft21/goaway.h"
#include "moq/interop/wire/draft21/key_values.h"
#include "moq/interop/wire/draft21/message_types.h"
#include "moq/interop/wire/draft21/objects.h"
#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/request_ok.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/wire/draft21/token.h"

namespace moq::interop::wire::draft22 {

// key_values.h
using draft21::KeyValue;
using draft21::KeyValues;
using draft21::KeyValueEncodeError;
using draft21::decode_key_values;
using draft21::decode_key_values_to_end;
using draft21::encode_key_values;

// token.h
using draft21::TokenAliasType;
using draft21::Token;
using draft21::TokenEncodeError;
using draft21::decode_token;
using draft21::encode_token;

// message_types.h
using draft21::StreamRole;
using draft21::MessageKind;
using draft21::MessageTypeInfo;
using draft21::classify_message_type;
using draft21::decode_message_type;

// request_frame.h
using draft21::RequestFrame;
using draft21::decode_request_frame;

// request_error.h
using draft21::RedirectTarget;
using draft21::RequestErrorMessage;
using draft21::RequestErrorEncodeError;
using draft21::decode_request_error;
using draft21::encode_request_error;

// request_ok.h
using draft21::encode_empty_publish_ok;

// goaway.h
using draft21::GoawayMessage;
using draft21::GoawayEncodeError;
using draft21::decode_goaway;
using draft21::encode_goaway;

// setup.h
using draft21::SetupOption;
using draft21::SetupMessage;
using draft21::SetupEncodeError;
using draft21::decode_setup;
using draft21::encode_setup;

// control.h
using draft21::ControlMessage;
using draft21::decode_control_message;

// publish_done.h
using draft21::PublishDoneMessage;
using draft21::valid_reason_phrase;
using draft21::decode_publish_done;

// successful_response.h
using draft21::ResponseContext;
using draft21::ResponseParameter;
using draft21::SuccessfulResponse;
using draft21::validate_track_properties;
using draft21::decode_successful_response;

// publish.h: only the two-varint Location struct, which the response parameters share.
using draft21::Location;

// objects.h
using draft21::Limits;
using draft21::ObjectForwardingPreference;
using draft21::ObjectEvent;
using draft21::SubgroupHeader;
using draft21::DecoderObservationKind;
using draft21::SubgroupDecodePhase;
using draft21::DecoderObservation;
using draft21::SubgroupPushResult;
using draft21::SubgroupDecoder;
using draft21::FetchGroupOrder;
using draft21::FetchGroupOrderResolver;
using draft21::FetchHeader;
using draft21::FetchRangeKind;
using draft21::FetchRangeEvent;
using draft21::FetchEvent;
using draft21::FetchDecodePhase;
using draft21::FetchDecoderObservation;
using draft21::FetchPushResult;
using draft21::FetchDecoder;

}  // namespace moq::interop::wire::draft22
