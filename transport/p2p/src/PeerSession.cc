#include "PeerSession.h"

#include <arpa/inet.h>
#include <cstring>

#include "Logger.h"
#include "TcpConnection.h"

namespace transport
{
namespace p2p
{

namespace
{
constexpr size_t kSendHighWater = 8 * 1024 * 1024;
constexpr size_t kMaxUploadQueue = 128;
}

PeerSession::PeerSession(const TcpConnectionPtr &conn,
                         PieceStore *store,
                         const InfoHash &infoHash,
                         const NodeId &selfId,
                         bool outbound)
    : conn_(conn)
    , store_(store)
    , infoHash_(infoHash)
    , selfId_(selfId)
    , outbound_(outbound)
    , handshaked_(false)
    , recvBuf_(256 * 1024)
{
}

void PeerSession::start()
{
    if (conn_)
    {
        // sendfile / 大缓冲发完后继续吐上传队列，避免 seed 侧空等。
        conn_->setWriteCompleteCallback(
            [self = shared_from_this()](const TcpConnectionPtr &) { self->onWriteComplete(); });
    }
    if (outbound_)
    {
        sendRaw(encodeHandshake(infoHash_, selfId_));
    }
}

void PeerSession::sendRaw(const std::string &frame)
{
    sendRaw(frame.data(), frame.size());
}

void PeerSession::sendRaw(const void *data, size_t len)
{
    if (conn_ && conn_->connected() && data && len)
    {
        conn_->send(data, len);
    }
}

void PeerSession::sendBitfield()
{
    const std::string bf = store_->bitfield();
    sendRaw(encodePeerFrame(PeerMsg::kBitfield, bf.data(), bf.size()));
}

void PeerSession::broadcastHave(uint32_t index)
{
    uint32_t n = htonl(index);
    sendRaw(encodePeerFrame(PeerMsg::kHave, &n, 4));
}

bool PeerSession::peerHas(uint32_t index) const
{
    if (peerBitfield_.empty())
    {
        return false;
    }
    const size_t byte = index / 8;
    if (byte >= peerBitfield_.size())
    {
        return false;
    }
    return (static_cast<uint8_t>(peerBitfield_[byte]) & (1u << (7 - (index % 8)))) != 0;
}

bool PeerSession::interesting(uint32_t index) const
{
    return peerHas(index) && !store_->hasPiece(index);
}

void PeerSession::requestPiece(uint32_t index)
{
    if (!handshaked_ || inflight_.count(index))
    {
        return;
    }
    inflight_.insert(index);
    uint32_t n = htonl(index);
    sendRaw(encodePeerFrame(PeerMsg::kRequest, &n, 4));
}

void PeerSession::cancelPiece(uint32_t index)
{
    inflight_.erase(index);
    uint32_t n = htonl(index);
    sendRaw(encodePeerFrame(PeerMsg::kCancel, &n, 4));
}

void PeerSession::onClosed()
{
    inflight_.clear();
    uploadQueue_.clear();
}

void PeerSession::onWriteComplete()
{
    flushUploadQueue();
}

void PeerSession::flushUploadQueue()
{
    while (!uploadQueue_.empty())
    {
        if (!conn_ || !conn_->connected())
        {
            uploadQueue_.clear();
            return;
        }
        if (conn_->outputBufferBytes() >= kSendHighWater || conn_->isSendingFile())
        {
            return;
        }
        const uint32_t idx = uploadQueue_.front();
        uploadQueue_.pop_front();
        if (!store_->hasPiece(idx))
        {
            continue;
        }
        const uint32_t plen = store_->pieceSize(idx);
        char prefix[14];
        encodePiecePrefix(idx, plen, prefix);
        sendRaw(prefix, sizeof(prefix));
        conn_->sendFile(store_->fd(), store_->pieceOffset(idx), plen);
    }
}

void PeerSession::replyPiece(uint32_t index)
{
    if (!store_->hasPiece(index) || !conn_ || !conn_->connected())
    {
        return;
    }
    if (conn_->outputBufferBytes() >= kSendHighWater || conn_->isSendingFile())
    {
        if (uploadQueue_.size() < kMaxUploadQueue)
        {
            uploadQueue_.push_back(index);
        }
        return;
    }
    const uint32_t plen = store_->pieceSize(index);
    char prefix[14];
    encodePiecePrefix(index, plen, prefix);
    sendRaw(prefix, sizeof(prefix));
    conn_->sendFile(store_->fd(), store_->pieceOffset(index), plen);
}

void PeerSession::onMessage(Buffer *buf)
{
    recvBuf_.append(buf->peek(), buf->readableBytes());
    buf->retrieveAll();

    size_t offset = 0;
    PeerFrame frame;
    while (tryDecodePeerFrame(recvBuf_.peek(), recvBuf_.readableBytes(), &offset, &frame))
    {
        handleFrame(frame);
    }
    if (offset > 0)
    {
        recvBuf_.retrieve(offset);
    }
}

void PeerSession::handleFrame(const PeerFrame &frame)
{
    if (!handshaked_)
    {
        if (frame.type != PeerMsg::kHandshake)
        {
            conn_->shutdown();
            return;
        }
        InfoHash ih;
        NodeId pid;
        if (!decodeHandshake(frame.payload, frame.payloadLen, &ih, &pid) ||
            std::memcmp(ih.bytes, infoHash_.bytes, 20) != 0)
        {
            LOG_ERROR("PeerSession handshake infoHash mismatch\n");
            conn_->shutdown();
            return;
        }
        peerId_ = pid;
        handshaked_ = true;
        if (!outbound_)
        {
            sendRaw(encodeHandshake(infoHash_, selfId_));
        }
        sendBitfield();
        if (readyHandler_)
        {
            readyHandler_(shared_from_this());
        }
        return;
    }

    switch (frame.type)
    {
    case PeerMsg::kBitfield:
        peerBitfield_.assign(frame.payload, frame.payloadLen);
        if (readyHandler_)
        {
            readyHandler_(shared_from_this());
        }
        break;
    case PeerMsg::kHave:
        if (frame.payloadLen >= 4)
        {
            uint32_t idx = 0;
            std::memcpy(&idx, frame.payload, 4);
            idx = ntohl(idx);
            if (peerBitfield_.size() < (idx / 8) + 1)
            {
                peerBitfield_.resize((idx / 8) + 1, 0);
            }
            peerBitfield_[idx / 8] = static_cast<char>(
                static_cast<uint8_t>(peerBitfield_[idx / 8]) | (1u << (7 - (idx % 8))));
            if (haveHandler_)
            {
                haveHandler_(shared_from_this(), idx);
            }
        }
        break;
    case PeerMsg::kRequest:
        if (frame.payloadLen >= 4)
        {
            uint32_t idx = 0;
            std::memcpy(&idx, frame.payload, 4);
            idx = ntohl(idx);
            replyPiece(idx);
        }
        break;
    case PeerMsg::kPiece:
        if (frame.payloadLen >= 4)
        {
            uint32_t idx = 0;
            std::memcpy(&idx, frame.payload, 4);
            idx = ntohl(idx);
            inflight_.erase(idx);
            if (pieceHandler_)
            {
                pieceHandler_(idx, frame.payload + 4, frame.payloadLen - 4);
            }
        }
        break;
    case PeerMsg::kCancel:
        break;
    default:
        break;
    }
}

} // namespace p2p
} // namespace transport
