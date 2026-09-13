//! Line-delimited JSON control channel.
//!
//! One JSON object per line in each direction. The sidecar serves it on a Unix
//! socket (`CP_CORE_SOCKET`) so the C++20 core, or an operator with `socat`,
//! can ask for status without ever linking to this process. That socket is the
//! whole boundary: the GPL-3.0 AP logic in here and the core stay in separate
//! address spaces.
//!
//! Bounds: a request line longer than the configured cap is rejected before it
//! is parsed, a reply that would exceed the cap is replaced by an error reply,
//! the per-client outbox is a [`Ring`] with a hard cap, and the number of
//! concurrent clients is capped. Nothing here allocates in proportion to input.

use serde::{Deserialize, Serialize};

use crate::bounds::{Ring, IPC_CLIENT_CAP, IPC_LINE_CAP, IPC_REPLY_CAP};

pub const OP_CONFIG: &str = "config";
pub const OP_STATUS: &str = "status";
pub const OP_PREFLIGHT: &str = "preflight";
pub const OP_AP_STATE: &str = "ap-state";
pub const OP_EVENTS: &str = "events";
pub const OP_DIAG: &str = "diag";
/// Versioned real-protocol telemetry for the separate C++ core/web consumer.
pub const OP_TELEMETRY: &str = "telemetry";
pub const OP_SHUTDOWN: &str = "shutdown";

/// Every operation the sidecar answers.
pub const KNOWN_OPS: [&str; 8] = [OP_CONFIG, OP_STATUS, OP_PREFLIGHT, OP_AP_STATE, OP_EVENTS, OP_DIAG, OP_TELEMETRY, OP_SHUTDOWN];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum IpcError {
    /// Line exceeded the configured byte cap.
    LineTooLong,
    /// Body was not a JSON object with an `op` string.
    Malformed,
    /// `op` is not in [`KNOWN_OPS`].
    UnknownOp,
    /// The serialised reply would exceed the byte cap.
    ReplyTooLarge,
    /// No client slot free.
    TooManyClients,
}

impl IpcError {
    pub fn as_str(self) -> &'static str {
        match self {
            IpcError::LineTooLong => "line-too-long",
            IpcError::Malformed => "malformed",
            IpcError::UnknownOp => "unknown-op",
            IpcError::ReplyTooLarge => "reply-too-large",
            IpcError::TooManyClients => "too-many-clients",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Request {
    pub op: String,
    /// Echoed back so a client can match replies to requests.
    #[serde(default)]
    pub id: u64,
    /// Only meaningful for [`OP_EVENTS`]: keep the connection and stream.
    #[serde(default)]
    pub follow: bool,
    /// Only meaningful for [`OP_EVENTS`]: how many ring entries to replay first.
    #[serde(default)]
    pub replay: usize,
}

impl Request {
    pub fn known_op(&self) -> bool {
        KNOWN_OPS.contains(&self.op.as_str())
    }
}

/// Envelope for every reply.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Reply<'a, T> {
    pub id: u64,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub payload: Option<&'a T>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<&'a str>,
}

/// Parse one request line. `max_line_bytes` comes from the validated config and
/// is applied before any JSON work happens.
pub fn parse_request(line: &str, max_line_bytes: usize) -> Result<Request, IpcError> {
    let limit = max_line_bytes.min(IPC_LINE_CAP);
    if line.len() > limit {
        return Err(IpcError::LineTooLong);
    }
    let trimmed = line.trim_end_matches(['\n', '\r']);
    if trimmed.is_empty() {
        return Err(IpcError::Malformed);
    }
    let request: Request = serde_json::from_str(trimmed).map_err(|_| IpcError::Malformed)?;
    if !request.known_op() {
        return Err(IpcError::UnknownOp);
    }
    if request.replay > crate::bounds::EVENT_RING_CAP {
        return Err(IpcError::Malformed);
    }
    Ok(request)
}

/// Encode a success reply as one line, or an error reply when the payload would
/// not fit [`IPC_REPLY_CAP`] or the caller's own limit.
pub fn encode_reply<T: Serialize>(id: u64, payload: &T, max_line_bytes: usize) -> Result<String, IpcError> {
    let limit = max_line_bytes.min(IPC_REPLY_CAP);
    let line = serde_json::to_string(&Reply { id, ok: true, payload: Some(payload), error: None }).map_err(|_| IpcError::ReplyTooLarge)?;
    if line.len() > limit {
        return Err(IpcError::ReplyTooLarge);
    }
    Ok(line)
}

/// Encode a failure reply. Always fits: the fields are fixed-size strings.
pub fn encode_error(id: u64, error: IpcError) -> String {
    serde_json::to_string(&Reply::<()> { id, ok: false, payload: None, error: Some(error.as_str()) })
        .unwrap_or_else(|_| format!("{{\"id\":{id},\"ok\":false,\"error\":\"{}\"}}", error.as_str()))
}

/// Per-client send queue. A client that stops reading is dropped rather than
/// allowed to grow the process.
#[derive(Debug)]
pub struct Outbox {
    lines: Ring<String>,
}

impl Outbox {
    pub fn new(capacity: usize) -> Self {
        Self { lines: Ring::new(capacity.min(crate::bounds::IPC_OUTBOX_CAP)) }
    }

    /// Queue one reply line. `false` means the queue was already full: the
    /// caller should close the client.
    pub fn push(&mut self, line: String) -> bool {
        if self.lines.len() >= self.lines.capacity() {
            return false;
        }
        self.lines.push(line);
        true
    }

    /// Take everything queued, oldest first.
    pub fn drain(&mut self) -> Vec<String> {
        let mut out = Vec::with_capacity(self.lines.len());
        out.extend(self.lines.iter().cloned());
        self.lines.clear();
        out
    }

    pub fn len(&self) -> usize {
        self.lines.len()
    }

    pub fn is_empty(&self) -> bool {
        self.lines.is_empty()
    }
}

/// Client-slot accounting, capped at [`IPC_CLIENT_CAP`].
#[derive(Debug, Default)]
pub struct ClientSlots {
    in_use: usize,
    cap: usize,
    refused: u64,
}

impl ClientSlots {
    pub fn new(cap: usize) -> Self {
        Self { in_use: 0, cap: cap.clamp(1, IPC_CLIENT_CAP), refused: 0 }
    }

    pub fn acquire(&mut self) -> Result<(), IpcError> {
        if self.in_use >= self.cap {
            self.refused += 1;
            return Err(IpcError::TooManyClients);
        }
        self.in_use += 1;
        Ok(())
    }

    pub fn release(&mut self) {
        self.in_use = self.in_use.saturating_sub(1);
    }

    pub fn in_use(&self) -> usize {
        self.in_use
    }

    pub fn cap(&self) -> usize {
        self.cap
    }

    pub fn refused(&self) -> u64 {
        self.refused
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::status::EngineEvent;

    #[test]
    fn parses_a_minimal_request() {
        let r = parse_request("{\"op\":\"status\"}\n", 1024).expect("ok");
        assert_eq!(r.op, OP_STATUS);
        assert_eq!(r.id, 0);
        assert!(!r.follow);
        assert_eq!(r.replay, 0);
    }

    #[test]
    fn parses_a_full_request() {
        let r = parse_request("{\"op\":\"events\",\"id\":7,\"follow\":true,\"replay\":8}", 1024).expect("ok");
        assert_eq!(r.op, OP_EVENTS);
        assert_eq!(r.id, 7);
        assert!(r.follow);
        assert_eq!(r.replay, 8);
    }

    #[test]
    fn rejects_unknown_operations() {
        assert_eq!(parse_request("{\"op\":\"reboot\"}", 1024), Err(IpcError::UnknownOp));
    }

    #[test]
    fn rejects_malformed_bodies() {
        for line in ["", "\n", "not json", "[1,2,3]", "{\"id\":1}", "{\"op\":42}", "{\"op\":\"status\",\"extra\":}]"] {
            assert_eq!(parse_request(line, 1024), Err(IpcError::Malformed), "{line:?}");
        }
    }

    #[test]
    fn rejects_overlong_lines_before_parsing() {
        let line = format!("{{\"op\":\"status\",\"pad\":\"{}\"}}", "x".repeat(2000));
        assert_eq!(parse_request(&line, 1024), Err(IpcError::LineTooLong));
        assert!(parse_request(&line, 4096).is_ok());
    }

    #[test]
    fn line_cap_can_never_exceed_the_hard_bound() {
        let line = format!("{{\"op\":\"status\",\"pad\":\"{}\"}}", "x".repeat(5000));
        assert_eq!(parse_request(&line, 1_000_000), Err(IpcError::LineTooLong));
    }

    #[test]
    fn rejects_absurd_event_replay() {
        let line = format!("{{\"op\":\"events\",\"replay\":{}}}", crate::bounds::EVENT_RING_CAP + 1);
        assert_eq!(parse_request(&line, 4096), Err(IpcError::Malformed));
    }

    #[test]
    fn encodes_a_reply_on_one_line() {
        let event = EngineEvent { seq: 1, kind: "startup", detail: "hello".to_string() };
        let line = encode_reply(3, &event, 1024).expect("ok");
        assert!(!line.contains('\n'), "{line}");
        assert!(line.starts_with("{\"id\":3,\"ok\":true,"), "{line}");
        assert!(line.contains("\"kind\":\"startup\""), "{line}");
    }

    #[test]
    fn oversized_reply_becomes_an_error() {
        let event = EngineEvent { seq: 1, kind: "big", detail: "y".repeat(2000) };
        assert_eq!(encode_reply(4, &event, 512), Err(IpcError::ReplyTooLarge));
        let line = encode_error(4, IpcError::ReplyTooLarge);
        assert!(line.len() < 128, "{line}");
        assert!(line.contains("\"ok\":false"), "{line}");
        assert!(line.contains("reply-too-large"), "{line}");
    }

    #[test]
    fn reply_limit_is_capped_by_the_hard_bound() {
        let payload = vec!["z".repeat(1024); 64];
        // A caller cannot lift the reply past IPC_REPLY_CAP by asking for more.
        assert_eq!(encode_reply(5, &payload, 1_000_000), Err(IpcError::ReplyTooLarge));
        let small = vec!["z".repeat(16); 4];
        assert!(encode_reply(5, &small, 1_000_000).is_ok());
    }

    #[test]
    fn error_reply_is_well_formed_for_every_variant() {
        for e in [IpcError::LineTooLong, IpcError::Malformed, IpcError::UnknownOp, IpcError::ReplyTooLarge, IpcError::TooManyClients] {
            let line = encode_error(9, e);
            assert!(line.contains("\"id\":9"), "{line}");
            assert!(line.contains(e.as_str()), "{line}");
            assert!(!line.contains('\n'), "{line}");
        }
    }

    #[test]
    fn outbox_is_bounded_and_reports_full() {
        let mut box_ = Outbox::new(2);
        assert!(box_.push("a".to_string()));
        assert!(box_.push("b".to_string()));
        assert!(!box_.push("c".to_string()));
        assert_eq!(box_.len(), 2);
        assert_eq!(box_.drain(), vec!["a".to_string(), "b".to_string()]);
        assert!(box_.is_empty());
    }

    #[test]
    fn outbox_capacity_is_clamped_to_the_hard_cap() {
        let box_ = Outbox::new(10_000);
        assert!(box_.len() <= crate::bounds::IPC_OUTBOX_CAP);
    }

    #[test]
    fn client_slots_refuse_past_the_cap() {
        let mut slots = ClientSlots::new(2);
        assert!(slots.acquire().is_ok());
        assert!(slots.acquire().is_ok());
        assert_eq!(slots.acquire(), Err(IpcError::TooManyClients));
        assert_eq!(slots.in_use(), 2);
        slots.release();
        assert!(slots.acquire().is_ok());
        assert_eq!(slots.refused(), 1);
    }

    #[test]
    fn client_slot_cap_is_clamped() {
        assert_eq!(ClientSlots::new(1000).cap(), IPC_CLIENT_CAP);
        assert_eq!(ClientSlots::new(0).cap(), 1);
    }

    #[test]
    fn release_never_underflows() {
        let mut slots = ClientSlots::new(1);
        slots.release();
        assert_eq!(slots.in_use(), 0);
    }
}
