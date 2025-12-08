/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef GENERIC_TOPOLOGY_COMMON_DEFS_H
#define GENERIC_TOPOLOGY_COMMON_DEFS_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include <vector>
#include <string>

namespace ns3 {

// ==========================================
// 1. 输入部分 (给 Builder 用)
// ==========================================
struct LinkInfo {
    uint32_t nodeA;
    uint32_t nodeB;
    Ptr<PointToPointNetDevice> devA;
    Ptr<PointToPointNetDevice> devB;
    uint64_t dataRateBps;
};

struct FlowInfo {
    uint32_t srcId;
    uint32_t dstId;
    uint32_t bytesPlanned;
    Ptr<PacketSink> sinkApp;
};

// ==========================================
// 2. 输出部分 (给 Collector 返回结果用)
// ==========================================

// 2.1 全局指标结构体
struct GlobalMetrics {
    double throughputMbps; // 总吞吐量
    double avgDelayMs;     // 平均时延
    double lossRatePct;    // 丢包率
};

// 2.2 单条链路的时间序列数据结构体
struct LinkTimeSeries {
    uint32_t nodeA;
    uint32_t nodeB;
    // 这里的 vector 就是你想要的数组
    std::vector<double> queueSnapshotsA; // A->B 队列长度序列
    std::vector<double> queueSnapshotsB; // B->A 队列长度序列
    std::vector<double> utilSnapshots;   // 链路利用率序列
};

// 2.3 总结果包 (包含全局指标 + 所有链路详情)
struct SimResult {
    GlobalMetrics global;
    std::vector<LinkTimeSeries> links;
};

} // namespace ns3

#endif /* GENERIC_TOPOLOGY_COMMON_DEFS_H */
