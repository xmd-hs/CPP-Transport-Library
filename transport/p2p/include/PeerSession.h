#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

#include "Buffer.h"
#include "Callbacks.h"
#include "NodeId.h"
#include "PeerWire.h"
#include "PieceStore.h"

namespace transport
{
namespace p2p
{

class PeerSession : public std::enable_shared_from_this<PeerSession>
{
public:
    using PieceHandler =
        std::function<void(uint32_t index, const void *data, size_t len)>;
    using HaveHandler = std::function<void(const std::shared_ptr<PeerSession> &, uint32_t index)>;
    using ReadyHandler = std::function<void(const std::shared_ptr<PeerSession> &)>;

    PeerSession(const TcpConnectionPtr &conn,
                PieceStore *store,
                const InfoHash &infoHash,
                const NodeId &selfId,
                bool outbound);

    void setPieceHandler(PieceHandler cb) { pieceHandler_ = std::move(cb); }
    void setHaveHandler(HaveHandler cb) { haveHandler_ = std::move(cb); }
    void setReadyHandler(ReadyHandler cb) { readyHandler_ = std::move(cb); }

    void start();
    void onMessage(Buffer *buf);
    void onClosed();

    bool interesting(uint32_t index) const;
    bool peerHas(uint32_t index) const;
    size_t inflight() const { return inflight_.size(); }
    bool hasInflight(uint32_t index) const { return inflight_.count(index) != 0; }
    void requestPiece(uint32_t index);
    void cancelPiece(uint32_t index);
    void broadcastHave(uint32_t index);
    void sendBitfield();

    const NodeId &peerId() const { return peerId_; }
    TcpConnectionPtr connection() const { return conn_; }
    bool handshaked() const { return handshaked_; }

private:
    void sendRaw(const std::string &frame);
    void sendRaw(const void *data, size_t len);
    void handleFrame(const PeerFrame &frame);
    void replyPiece(uint32_t index);
    void flushUploadQueue();
    void onWriteComplete();

    TcpConnectionPtr conn_;
    PieceStore *store_;
    InfoHash infoHash_;
    NodeId selfId_;
    NodeId peerId_{};
    bool outbound_;
    bool handshaked_;
    Buffer recvBuf_;
    std::string peerBitfield_;
    std::unordered_set<uint32_t> inflight_;
    std::deque<uint32_t> uploadQueue_;

    PieceHandler pieceHandler_;
    HaveHandler haveHandler_;
    ReadyHandler readyHandler_;
};

} // namespace p2p
} // namespace transport
