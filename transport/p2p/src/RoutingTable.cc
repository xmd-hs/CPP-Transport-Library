#include "RoutingTable.h"

#include <algorithm>

namespace transport
{
namespace p2p
{

RoutingTable::RoutingTable(const NodeId &self)
    : self_(self)
    , buckets_(kBucketCount)
{
}

bool RoutingTable::upsert(const PeerContact &c)
{
    if (idEqual(c.id, self_))
    {
        return false;
    }
    const int idx = bucketIndex(xorDistance(self_, c.id));
    if (idx < 0 || idx >= kBucketCount)
    {
        return false;
    }
    auto &bucket = buckets_[static_cast<size_t>(idx)];
    for (auto it = bucket.begin(); it != bucket.end(); ++it)
    {
        if (idEqual(it->id, c.id))
        {
            bucket.erase(it);
            bucket.push_front(c);
            return true;
        }
    }
    if (static_cast<int>(bucket.size()) < kBucketSize)
    {
        bucket.push_front(c);
        return true;
    }
    // 桶满：简单丢弃最旧（尾部），生产级应 ping 后再决定。
    bucket.pop_back();
    bucket.push_front(c);
    return true;
}

std::vector<PeerContact> RoutingTable::closest(const NodeId &target, size_t n) const
{
    std::vector<PeerContact> allNodes = all();
    std::sort(allNodes.begin(), allNodes.end(), [&](const PeerContact &a, const PeerContact &b) {
        return closerThan(target, a.id, b.id);
    });
    if (allNodes.size() > n)
    {
        allNodes.resize(n);
    }
    return allNodes;
}

std::vector<PeerContact> RoutingTable::all() const
{
    std::vector<PeerContact> out;
    for (const auto &b : buckets_)
    {
        for (const auto &c : b)
        {
            out.push_back(c);
        }
    }
    return out;
}

size_t RoutingTable::size() const
{
    size_t n = 0;
    for (const auto &b : buckets_)
    {
        n += b.size();
    }
    return n;
}

} // namespace p2p
} // namespace transport
