/**********************************************************************************************************************
    > File: dedup.h
    > 通用序号去重引擎（云端选收去重与终端下行去重共用同一算法，两侧各自实现、行为一致）
    > 算法：每流一个环形槽位表（seq % N），槽命中同 seq 判重复；
    >       窗口外旧包重复不可检测（测试速率下可忽略），乱序容忍 = N-1 个 seq。
    > 线程模型：外部持锁调用（外层 mutex），本类不自持锁，便于跨锚点共享。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_DEDUP_H
#define VWISE_CLOUD_DEDUP_H

#include "vwise_dup_protocol.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace vwise {
namespace cloud {

struct DedupSlot {
    uint32_t seq = 0;
    bool used = false;
};

/* 单流去重槽位表 */
class SeqDedup {
public:
    SeqDedup() : ring_(VWD_DEDUP_RING_SIZE) {}

    /* 返回 true = 重复包应丢弃；false = 新包（已登记） */
    bool seen(uint32_t seq) {
        DedupSlot &s = ring_[seq & (VWD_DEDUP_RING_SIZE - 1)];
        if (s.used && s.seq == seq) return true;
        s.used = true;
        s.seq = seq;
        return false;
    }

private:
    std::vector<DedupSlot> ring_;
};

/* 多流管理 + 空闲流回收 */
class FlowDedup {
public:
    /* 返回 true = 重复，false = 新包 */
    bool seen(uint32_t flow_id, uint32_t seq, uint64_t now_ns, uint64_t expire_ns) {
        auto &f = flows_[flow_id];
        f.last_ns = now_ns;
        if (flows_.size() > 64 && ++sweep_ % 32 == 0) sweep(now_ns, expire_ns);
        return f.dedup.seen(seq);
    }

    size_t flow_count() const { return flows_.size(); }

    /* 清空流表（调用方持外层锁，与 seen() 相同约定） */
    void clear() {
        flows_.clear();
        sweep_ = 0;
    }

private:
    struct FlowState {
        SeqDedup dedup;
        uint64_t last_ns = 0;
    };
    void sweep(uint64_t now_ns, uint64_t expire_ns) {
        for (auto it = flows_.begin(); it != flows_.end();) {
            if (now_ns - it->second.last_ns > expire_ns) it = flows_.erase(it);
            else ++it;
        }
    }
    std::unordered_map<uint32_t, FlowState> flows_;
    uint64_t sweep_ = 0;
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_DEDUP_H
