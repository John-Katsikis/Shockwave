// A transport that writes a packet capture as it goes (Network/CaptureTransport.h).
#include <Network/CaptureTransport.h>

#include <array>
#include <chrono>

namespace shockwave::net {

namespace {

class CapturingTransport final : public ITransport, private TransportSink {
public:
    CapturingTransport(std::unique_ptr<ITransport> inner, CaptureWriter& writer)
        : m_inner(std::move(inner)), m_writer(writer), m_start(std::chrono::steady_clock::now()) {}

    const CaptureCounters& counters() const { return m_counters; }

    PeerId localPeer() const override { return m_inner->localPeer(); }
    uint32_t endpointGeneration() const override { return m_inner->endpointGeneration(); }
    NetResult<void> listen(const TransportAddress& address, const TransportLimits& limits) override {
        return m_inner->listen(address, limits);
    }
    NetResult<ConnectionId> connect(const TransportAddress& address) override {
        return m_inner->connect(address);   // its peer comes with the Connected event
    }
    NetResult<void> accept(ConnectionId connection) override { return m_inner->accept(connection); }
    void close(ConnectionId connection, ReasonCode reason) override { m_inner->close(connection, reason); }
    void closeAll(ReasonCode reason) override { m_inner->closeAll(reason); }

    PollResult poll(TransportSink& sink, const ReceiveBudget& budget) override {
        m_sink = &sink;
        const PollResult result = m_inner->poll(*this, budget);
        m_sink = nullptr;
        return result;
    }

    SendResult send(ConnectionId connection, std::span<const uint8_t> bytes, Delivery delivery) override {
        const SendResult result = m_inner->send(connection, bytes, delivery);
        if (result == SendResult::Queued) record(CaptureDirection::Sent, delivery, peerOf(connection), bytes);
        return result;
    }

    std::optional<ConnectionStats> stats(ConnectionId connection) const override { return m_inner->stats(connection); }

private:
    void onEvent(const TransportEvent& event) override {
        if (event.kind != TransportEventKind::Closed) remember(event.connection, event.peer);
        m_sink->onEvent(event);
    }
    void onMessage(const ReceivedMessage& message) override {
        record(CaptureDirection::Received, message.delivery, message.peer, message.bytes);
        m_sink->onMessage(message);
    }

    // Connections to peers, so sends can name theirs; a fixed table, no allocation.
    void remember(ConnectionId connection, PeerId peer) {
        if (!peer.valid()) return;
        for (Link& link : m_links)
            if (link.connection == connection) {
                link.peer = peer;
                return;
            }
        m_links[m_nextLink] = {connection, peer};
        m_nextLink = (m_nextLink + 1) % m_links.size();
    }
    PeerId peerOf(ConnectionId connection) const {
        for (const Link& link : m_links)
            if (link.connection == connection) return link.peer;
        return {};
    }
    // 1, 2, 3... by first appearance; 0 for a peer we couldn't tell.
    uint64_t opaque(PeerId peer) {
        if (!peer.valid()) return 0;
        for (size_t i = 0; i < m_peerCount; ++i)
            if (m_peers[i] == peer) return i + 1;
        if (m_peerCount == m_peers.size()) return 0;
        m_peers[m_peerCount++] = peer;
        return m_peerCount;
    }

    void record(CaptureDirection direction, Delivery delivery, PeerId peer, std::span<const uint8_t> bytes) {
        CaptureRecord r;
        r.timeUs = uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start).count());
        r.direction = direction;
        r.delivery = delivery;
        r.peer = opaque(peer);
        r.wire = bytes;
        switch (m_writer.write(r)) {
        case CaptureWrite::Written: ++m_counters.records; break;
        case CaptureWrite::Redacted:
            ++m_counters.records;
            ++m_counters.redacted;
            break;
        case CaptureWrite::Full:
        case CaptureWrite::Failed: ++m_counters.dropped; break;
        }
    }

    struct Link {
        ConnectionId connection;
        PeerId peer;
    };
    std::unique_ptr<ITransport> m_inner;
    CaptureWriter& m_writer;
    std::chrono::steady_clock::time_point m_start;
    TransportSink* m_sink = nullptr;
    std::array<Link, kMaxTransportConnections> m_links{};
    size_t m_nextLink = 0;
    std::array<PeerId, 256> m_peers{};
    size_t m_peerCount = 0;
    CaptureCounters m_counters;
};

}   // namespace

std::unique_ptr<ITransport> makeCapturingTransport(std::unique_ptr<ITransport> inner, CaptureWriter& writer) {
    return std::make_unique<CapturingTransport>(std::move(inner), writer);
}

const CaptureCounters* captureCounters(const ITransport& transport) {
    const auto* capturing = dynamic_cast<const CapturingTransport*>(&transport);
    return capturing ? &capturing->counters() : nullptr;
}

}   // namespace shockwave::net
