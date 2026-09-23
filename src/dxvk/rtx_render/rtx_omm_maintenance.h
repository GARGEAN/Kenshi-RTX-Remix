#pragma once
#include "rtx_omm_retention.h"
#include <tuple>

namespace dxvk::ommretention {
  // All state is non-owning. One step visits or releases one record; cleanup
  // of the planner itself is incremental too. No unordered-map iterator is
  // retained across frames and no stale snapshot authorizes a GPU deletion.
  struct SweepEntry : Entry { uint64_t generation = 0; };
  inline bool stillEvictable(const SweepEntry& candidate, uint64_t generation,
      uint64_t resource, uint32_t age, uint32_t minAge, uint64_t references) {
    return generation == candidate.generation && resource == candidate.resource && age >= minAge && references == 1;
  }
  class Sweep {
  public:
    enum class Phase { Collect, Validate, Order, Evict, Reset };
    Phase phase = Phase::Collect;
    uint64_t cursor = 0, eligibleBytes = 0, eligibleGroups = 0;
    bool started = false;

    void add(const SweepEntry& entry) {
      auto& node = nodes[entry.resource];
      node.duplicate |= node.live;
      node.live = true;
      node.entry = entry;
      if (entry.parent) ++nodes[entry.parent].children;
    }
    void finishCollect() { phase = Phase::Validate; cursor = 0; started = false; }

    // budget/pressure are sampled afresh by the caller. Returned candidates
    // MUST be looked up and checked for identity, age and refCount()==1.
    bool step(uint64_t budget, bool pressure, bool& trimming, SweepEntry& candidate) {
      if (phase == Phase::Validate) {
        auto it = started ? nodes.upper_bound(cursor) : nodes.begin();
        if (it == nodes.end()) { phase = Phase::Order; cursor = 0; started = false; return false; }
        cursor = it->first; started = true;
        auto& node = it->second;
        if (!node.live) return false;
        auto root = it;
        uint32_t depth = 0;
        while (root->second.entry.parent && depth < 16) {
          root = nodes.find(root->second.entry.parent);
          if (root == nodes.end() || !root->second.live) return false;
          ++depth;
        }
        if (root->second.entry.parent) return false; // Broken/cyclic/deep chain.
        auto& group = groups[root->first];
        group.unused &= !node.duplicate && node.entry.age >= minAge &&
          node.entry.references == 1 + node.children;
        group.bytes += node.entry.bytes;
        group.age = std::min(group.age, node.entry.age);
        group.diskBacked |= node.entry.diskBacked;
        group.entries.emplace(std::make_pair(UINT32_MAX - depth, node.entry.hash), node.entry);
      } else if (phase == Phase::Order) {
        auto it = started ? groups.upper_bound(cursor) : groups.begin();
        if (it == groups.end()) {
          const uint64_t high = std::min(192 * MiB, budget / 4);
          const uint64_t low = pressure ? 0 : std::min(128 * MiB, budget / 8);
          if (eligibleBytes > high || pressure) trimming = true;
          if (eligibleBytes <= low) trimming = false;
          phase = trimming && budget ? Phase::Evict : Phase::Reset;
          return false;
        }
        cursor = it->first; started = true;
        if (it->second.unused) {
          const auto& g = it->second;
          order.emplace(std::make_tuple(!g.diskBacked, UINT32_MAX - g.age, it->first), it->first);
          eligibleBytes += g.bytes;
          ++eligibleGroups;
        }
      } else if (phase == Phase::Evict) {
        const uint64_t low = pressure ? 0 : std::min(128 * MiB, budget / 8);
        if (!budget || eligibleBytes <= low || order.empty()) {
          if (eligibleBytes <= low) trimming = false;
          phase = Phase::Reset;
          return false;
        }
        auto& group = groups.find(order.begin()->second)->second;
        if (group.entries.empty()) { order.erase(order.begin()); return false; }
        auto entry = group.entries.begin();
        candidate = entry->second;
        // Failed revalidation removes a stale candidate from this estimate,
        // not from GPU storage. A later complete sweep recalculates eligibility.
        eligibleBytes -= std::min(eligibleBytes, candidate.bytes);
        group.entries.erase(entry);
        return true;
      } else if (phase == Phase::Reset) {
        if (!nodes.empty()) nodes.erase(nodes.begin());
        else if (!order.empty()) order.erase(order.begin());
        else if (!groups.empty()) {
          auto& g = groups.begin()->second;
          if (!g.entries.empty()) g.entries.erase(g.entries.begin());
          else groups.erase(groups.begin());
        } else {
          phase = Phase::Collect; cursor = eligibleBytes = eligibleGroups = 0; started = false;
          ++cycles;
        }
      }
      return false;
    }
    uint32_t minAge = 600;
    uint64_t cycles = 0;
  private:
    struct Node { SweepEntry entry; uint64_t children = 0; bool live = false, duplicate = false; };
    struct ColdGroup {
      std::map<std::pair<uint32_t,uint64_t>, SweepEntry> entries;
      uint64_t bytes = 0;
      uint32_t age = UINT32_MAX;
      bool unused = true, diskBacked = false;
    };
    std::map<uint64_t, Node> nodes;
    std::map<uint64_t, ColdGroup> groups;
    std::map<std::tuple<bool,uint32_t,uint64_t>, uint64_t> order;
  };

}
