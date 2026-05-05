// Module declarations for protoc-gen-solana generated code.
// The integration test runs protoc to generate .rs files into this tree
// before cargo compiles the crate.

mod protobuf_runtime;
mod hello;

pub use hello::hello_world::*;
pub use hello::types::sample_types::*;

#[cfg(test)]
mod tests {
    use super::*;

    // ── Enum basics ─────────────────────────────────────────────────

    #[test]
    fn enum_default_is_zero_variant() {
        assert_eq!(Priority::default(), Priority::PriorityUnspecified);
        assert_eq!(Status::default(), Status::StatusUnspecified);
        assert_eq!(Greeting::default(), Greeting::GreetingUnspecified);
    }

    #[test]
    fn enum_from_i32_known_values() {
        assert_eq!(Priority::from(3), Priority::PriorityHigh);
        assert_eq!(Status::from(4), Status::StatusFailed);
        assert_eq!(Greeting::from(1), Greeting::GreetingHello);
    }

    #[test]
    fn enum_from_i32_unknown_falls_back_to_default() {
        assert_eq!(Priority::from(99), Priority::default());
        assert_eq!(Status::from(-1), Status::default());
    }

    #[test]
    fn enum_into_i32_roundtrip() {
        let p = Priority::PriorityCritical;
        let v: i32 = p.into();
        assert_eq!(v, 4);
        assert_eq!(Priority::from(v), p);
    }

    // ── Shared message roundtrip ────────────────────────────────────

    #[test]
    fn tagged_id_encode_decode_roundtrip() {
        let original = TaggedId {
            id: 42,
            tag: "test-tag".to_string(),
            priority: Priority::PriorityHigh,
        };
        let bytes = original.encode();
        let decoded = TaggedId::decode(&bytes).expect("decode failed");
        assert_eq!(decoded.id, 42);
        assert_eq!(decoded.tag, "test-tag");
        assert_eq!(decoded.priority, Priority::PriorityHigh);
    }

    #[test]
    fn metadata_encode_decode_roundtrip() {
        let original = Metadata {
            key: "env".to_string(),
            value: "production".to_string(),
        };
        let bytes = original.encode();
        let decoded = Metadata::decode(&bytes).expect("decode failed");
        assert_eq!(decoded.key, "env");
        assert_eq!(decoded.value, "production");
    }

    // ── HelloRequest full roundtrip ─────────────────────────────────

    #[test]
    fn hello_request_full_roundtrip() {
        let original = HelloRequest {
            name: "World".to_string(),
            greeting: Greeting::GreetingHello,
            priority: Priority::PriorityCritical,
            sender: TaggedId {
                id: 1001,
                tag: "sender-alpha".to_string(),
                priority: Priority::PriorityMedium,
            },
            metadata: vec![
                Metadata {
                    key: "source".to_string(),
                    value: "integration-test".to_string(),
                },
                Metadata {
                    key: "version".to_string(),
                    value: "1.0".to_string(),
                },
            ],
            urgent: true,
            timestamp: 1_700_000_000,
        };

        let bytes = original.encode();
        assert!(!bytes.is_empty(), "encoded bytes should not be empty");

        let decoded = HelloRequest::decode(&bytes).expect("decode failed");
        assert_eq!(decoded.name, "World");
        assert_eq!(decoded.greeting, Greeting::GreetingHello);
        assert_eq!(decoded.priority, Priority::PriorityCritical);
        assert_eq!(decoded.sender.id, 1001);
        assert_eq!(decoded.sender.tag, "sender-alpha");
        assert_eq!(decoded.sender.priority, Priority::PriorityMedium);
        assert_eq!(decoded.metadata.len(), 2);
        assert_eq!(decoded.metadata[0].key, "source");
        assert_eq!(decoded.metadata[0].value, "integration-test");
        assert_eq!(decoded.metadata[1].key, "version");
        assert_eq!(decoded.metadata[1].value, "1.0");
        assert_eq!(decoded.urgent, true);
        assert_eq!(decoded.timestamp, 1_700_000_000);
    }

    // ── HelloResponse roundtrip ─────────────────────────────────────

    #[test]
    fn hello_response_roundtrip() {
        let original = HelloResponse {
            message: "Hello, World!".to_string(),
            status: Status::StatusCompleted,
            processed_at: 1_700_000_001,
            responder: TaggedId {
                id: 2002,
                tag: "responder-beta".to_string(),
                priority: Priority::PriorityLow,
            },
        };

        let bytes = original.encode();
        let decoded = HelloResponse::decode(&bytes).expect("decode failed");
        assert_eq!(decoded.message, "Hello, World!");
        assert_eq!(decoded.status, Status::StatusCompleted);
        assert_eq!(decoded.processed_at, 1_700_000_001);
        assert_eq!(decoded.responder.id, 2002);
        assert_eq!(decoded.responder.tag, "responder-beta");
    }

    // ── Default / empty message roundtrip ───────────────────────────

    #[test]
    fn default_hello_request_roundtrip() {
        let original = HelloRequest::default();
        let bytes = original.encode();
        let decoded = HelloRequest::decode(&bytes).expect("decode failed");
        assert_eq!(decoded.name, "");
        assert_eq!(decoded.greeting, Greeting::default());
        assert_eq!(decoded.priority, Priority::default());
        assert_eq!(decoded.metadata.len(), 0);
        assert_eq!(decoded.urgent, false);
        assert_eq!(decoded.timestamp, 0);
    }

    // ── Cross-type enum field in nested message ─────────────────────

    #[test]
    fn nested_enum_field_preserved_through_parent_encode() {
        let req = HelloRequest {
            name: "nested-test".to_string(),
            greeting: Greeting::GreetingHey,
            priority: Priority::PriorityLow,
            sender: TaggedId {
                id: 99,
                tag: "deep".to_string(),
                priority: Priority::PriorityCritical,
            },
            metadata: vec![],
            urgent: false,
            timestamp: 0,
        };

        let bytes = req.encode();
        let decoded = HelloRequest::decode(&bytes).expect("decode failed");

        // The sender's priority (Critical=4) must survive the parent encode/decode
        assert_eq!(decoded.sender.priority, Priority::PriorityCritical);
        // The request's own priority (Low=1) is independent
        assert_eq!(decoded.priority, Priority::PriorityLow);
    }
}
