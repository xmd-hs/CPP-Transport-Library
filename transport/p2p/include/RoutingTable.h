#pragma once

#include <list>
#include <vector>

#include "NodeId.h"

namespace transport
{
namespace p2p
{

class RoutingTable
{
public:
    explicit RoutingTable(const NodeId &self);

    void setSelf(const NodeId &self) { self_ = self; }
    const NodeId &self() const { return self_; }

    bool upsert(const PeerContact &c);
    std::vector<PeerContact> closest(const NodeId &target, size_t n) const;
    std::vector<PeerContact> all() const;
    size_t size() const;

private:
    NodeId self_;
    std::vector<std::list<PeerContact>> buckets_;
};

} // namespace p2p
} // namespace transport
