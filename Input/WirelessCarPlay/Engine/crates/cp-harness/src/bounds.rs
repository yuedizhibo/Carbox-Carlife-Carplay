//! Fixed-capacity containers. Nothing in this crate may grow without bound.

use std::collections::VecDeque;

/// Cap on the status/event ring kept for `cp-native diag` and IPC followers.
pub const EVENT_RING_CAP: usize = 256;
/// Cap on pending IPC replies queued for one client before it is dropped.
///
/// Worst case resident bytes for the control socket:
/// `IPC_CLIENT_CAP * IPC_OUTBOX_CAP * IPC_REPLY_CAP` = 2 * 8 * 16 KiB = 256 KiB.
pub const IPC_OUTBOX_CAP: usize = 8;
/// Cap on concurrently served IPC clients.
pub const IPC_CLIENT_CAP: usize = 2;
/// Cap on one inbound IPC request line, in bytes.
pub const IPC_LINE_CAP: usize = 4096;
/// Cap on one outbound IPC reply line, in bytes. Larger than the request cap
/// because reports are generated, already item-bounded, and must not be
/// truncated into a useless reply.
pub const IPC_REPLY_CAP: usize = 16384;
/// Events carried in one `diag` reply, most recent last.
pub const DIAG_EVENT_CAP: usize = 32;
/// Cap on encoded CarPlay video frames held while waiting for a consumer.
pub const FRAME_CACHE_CAP: usize = 4;
/// Cap on findings/checks reported by one preflight or status run.
pub const REPORT_ITEM_CAP: usize = 64;
/// Cap on command-log entries kept by the AP supervisor.
pub const COMMAND_LOG_CAP: usize = 64;

/// A ring buffer that drops the oldest entry once `capacity` is reached.
///
/// `dropped` counts every eviction so a saturated pipeline is visible in
/// `diag` output instead of silently consuming memory.
#[derive(Debug, Clone)]
pub struct Ring<T> {
    items: VecDeque<T>,
    capacity: usize,
    pushed: u64,
    dropped: u64,
}

impl<T> Ring<T> {
    /// `capacity` is clamped to `1..=EVENT_RING_CAP`, so no caller can create a
    /// ring that grows past the hard bound.
    pub fn new(capacity: usize) -> Self {
        let capacity = capacity.clamp(1, EVENT_RING_CAP);
        Self { items: VecDeque::with_capacity(capacity), capacity, pushed: 0, dropped: 0 }
    }

    pub fn push(&mut self, item: T) {
        if self.items.len() >= self.capacity {
            self.items.pop_front();
            self.dropped += 1;
        }
        self.items.push_back(item);
        self.pushed += 1;
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }

    pub fn pushed(&self) -> u64 {
        self.pushed
    }

    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// Oldest first; double-ended so callers can take the most recent entries.
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = &T> {
        self.items.iter()
    }

    pub fn clear(&mut self) {
        self.items.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn evicts_oldest_and_counts_drops() {
        let mut ring = Ring::new(3);
        for i in 0..7 {
            ring.push(i);
        }
        assert_eq!(ring.len(), 3);
        assert_eq!(ring.capacity(), 3);
        assert_eq!(ring.pushed(), 7);
        assert_eq!(ring.dropped(), 4);
        assert_eq!(ring.iter().copied().collect::<Vec<_>>(), vec![4, 5, 6]);
    }

    #[test]
    fn zero_capacity_is_clamped_to_one() {
        let mut ring = Ring::new(0);
        ring.push('a');
        ring.push('b');
        assert_eq!(ring.capacity(), 1);
        assert_eq!(ring.len(), 1);
        assert_eq!(ring.dropped(), 1);
        assert_eq!(ring.iter().copied().collect::<Vec<_>>(), vec!['b']);
    }

    #[test]
    fn capacity_is_clamped_to_the_hard_bound() {
        let mut ring: Ring<u8> = Ring::new(100_000);
        for i in 0..(EVENT_RING_CAP + 5) {
            ring.push(i as u8);
        }
        assert_eq!(ring.capacity(), EVENT_RING_CAP);
        assert_eq!(ring.len(), EVENT_RING_CAP);
        assert_eq!(ring.dropped(), 5);
    }

    #[test]
    fn clear_keeps_counters() {
        let mut ring = Ring::new(2);
        ring.push(1);
        ring.clear();
        assert!(ring.is_empty());
        assert_eq!(ring.pushed(), 1);
    }
}
