/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sim-stats-collector.h"
#include "ns3/point-to-point-net-device.h"
#include <iomanip>
#include <iostream>
#include <cmath>

using namespace std;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SimStatsCollector");
NS_OBJECT_ENSURE_REGISTERED (SimStatsCollector);

// 内部统计结构实现
struct DevRuntimeStats {
    uint64_t txBytes = 0; uint64_t txPackets = 0;
    uint64_t dropBytes = 0; uint64_t dropPackets = 0;
    std::vector<double> queueSnapshots;
    bool firstTxLogged = false;

    void Tx(Ptr<const Packet> p) {
        txBytes += p->GetSize(); txPackets++;
        if (!firstTxLogged) firstTxLogged = true;
    }
    void Drop(Ptr<const Packet> p) { dropBytes += p->GetSize(); dropPackets++; }
};

struct LinkRuntimeStats {
    std::shared_ptr<DevRuntimeStats> sa;
    std::shared_ptr<DevRuntimeStats> sb;
    Ptr<Queue<Packet>> qA;
    Ptr<Queue<Packet>> qB;
    uint64_t lastTxBytesTotal = 0;
    double lastSampleTime = -1.0;
    std::vector<double> utilSnapshots;
    std::vector<double> sampleTimestamps;
};

TypeId SimStatsCollector::GetTypeId (void) {
    static TypeId tid = TypeId ("ns3::SimStatsCollector").SetParent<Object>().SetGroupName ("GenericTopology").AddConstructor<SimStatsCollector>();
    return tid;
}

SimStatsCollector::SimStatsCollector() {}
SimStatsCollector::~SimStatsCollector() {}

void SimStatsCollector::Setup(const std::vector<LinkInfo>& links, const std::vector<FlowInfo>& flows, double appsStop) {
    m_linkInfos = links; m_flowInfos = flows; m_appsStop = appsStop;
    m_monitor = m_flowmonHelper.InstallAll();
    double startMin = 0.1; double sampleStart = std::max(0.0, startMin);

    for (const auto& link : links) {
        auto stats = std::make_shared<LinkRuntimeStats>();
        stats->sa = std::make_shared<DevRuntimeStats>(); stats->sb = std::make_shared<DevRuntimeStats>();
        stats->qA = link.devA->GetQueue(); stats->qB = link.devB->GetQueue();
        stats->lastSampleTime = sampleStart; stats->lastTxBytesTotal = 0; 
        link.devA->TraceConnectWithoutContext("MacTx", MakeCallback(&DevRuntimeStats::Tx, stats->sa.get()));
        link.devB->TraceConnectWithoutContext("MacTx", MakeCallback(&DevRuntimeStats::Tx, stats->sb.get()));
        if(stats->qA) stats->qA->TraceConnectWithoutContext("Drop", MakeCallback(&DevRuntimeStats::Drop, stats->sa.get()));
        if(stats->qB) stats->qB->TraceConnectWithoutContext("Drop", MakeCallback(&DevRuntimeStats::Drop, stats->sb.get()));
        m_runtimeStats.push_back(stats);
    }
    for (double t = sampleStart; t <= m_appsStop + 1e-12; t += SAMPLE_INTERVAL) {
        double subDt = SAMPLE_INTERVAL / SUB_SAMPLE_COUNT;
        for (uint32_t s=0; s<SUB_SAMPLE_COUNT; ++s) Simulator::Schedule(Seconds(t + (s+0.5)*subDt), &SimStatsCollector::SubSamplingRoutine, this);
        Simulator::Schedule(Seconds(t + SAMPLE_INTERVAL), &SimStatsCollector::SamplingRoutine, this);
    }
}

void SimStatsCollector::SubSamplingRoutine() {
    if (m_accumulator.qA_sum.empty()) { m_accumulator.qA_sum.resize(m_runtimeStats.size(), 0); m_accumulator.qB_sum.resize(m_runtimeStats.size(), 0); }
    for (size_t i=0; i<m_runtimeStats.size(); ++i) {
        auto& rs = m_runtimeStats[i];
        m_accumulator.qA_sum[i] += (rs->qA ? rs->qA->GetNPackets() : 0); m_accumulator.qB_sum[i] += (rs->qB ? rs->qB->GetNPackets() : 0);
    }
    m_accumulator.count++;
}

void SimStatsCollector::SamplingRoutine() {
    const double now = Simulator::Now().GetSeconds();
    for (size_t i=0; i<m_runtimeStats.size(); ++i) {
        auto& rs = m_runtimeStats[i]; const auto& linkInfo = m_linkInfos[i];
        rs->sampleTimestamps.push_back(now);
        double qA = 0.0, qB = 0.0;
        if (m_accumulator.count > 0) { qA = (double)m_accumulator.qA_sum[i] / m_accumulator.count; qB = (double)m_accumulator.qB_sum[i] / m_accumulator.count; }
        rs->sa->queueSnapshots.push_back(qA); rs->sb->queueSnapshots.push_back(qB);
        uint64_t currTx = rs->sa->txBytes + rs->sb->txBytes; double util = 0.0;
        if (rs->lastSampleTime >= 0) {
            double dt = now - rs->lastSampleTime;
            if (dt > 0) {
                double cap = (double)linkInfo.dataRateBps / 8.0 * dt * 2.0;
                double delta = (double)(currTx - rs->lastTxBytesTotal);
                if (cap > 0) util = std::clamp(100.0 * delta / cap, 0.0, 100.0);
            }
        }
        rs->utilSnapshots.push_back(util); rs->lastSampleTime = now; rs->lastTxBytesTotal = currTx;
    }
    m_accumulator.qA_sum.assign(m_runtimeStats.size(), 0); m_accumulator.qB_sum.assign(m_runtimeStats.size(), 0); m_accumulator.count = 0;
}

SimResult SimStatsCollector::CollectAndPrint() {
    m_monitor->CheckForLostPackets();
    const auto& stats = m_monitor->GetFlowStats();
    
    // 【注意】如果需要启用下方的日志打印，请取消下面这行注释：
    // double simTime = std::max(1e-9, Simulator::Now().GetSeconds());

    SimResult finalResult; 

    /* ====================================================================
       如果你想恢复在终端打印所有链路的详细日志，请取消这段代码的注释
       记得同时取消上面 simTime 的注释
       ====================================================================
    
    NS_LOG_UNCOND("\n==================== 队列长度时间序列统计 ====================");
    NS_LOG_UNCOND("链路总数: " << m_runtimeStats.size());
    NS_LOG_UNCOND("");
    */

    for (size_t i=0; i<m_runtimeStats.size(); ++i) {
        auto& rs = m_runtimeStats[i];
        
        LinkTimeSeries lts;
        lts.nodeA = m_linkInfos[i].nodeA;
        lts.nodeB = m_linkInfos[i].nodeB;
        lts.queueSnapshotsA = rs->sa->queueSnapshots; 
        lts.queueSnapshotsB = rs->sb->queueSnapshots; 
        lts.utilSnapshots = rs->utilSnapshots;        
        finalResult.links.push_back(lts);

        /* ====================================================================
           单条链路详细日志部分
           ====================================================================
        double totalCap = (double)m_linkInfos[i].dataRateBps/8.0 * simTime * 2.0;
        uint64_t totalTx = rs->sa->txBytes + rs->sb->txBytes;
        double totalUtil = totalCap>0 ? 100.0*totalTx/totalCap : 0.0;
        
        double sumQ=0; size_t n=std::min(rs->sa->queueSnapshots.size(), rs->sb->queueSnapshots.size());
        for(size_t k=0; k<n; ++k) sumQ += std::max(rs->sa->queueSnapshots[k], rs->sb->queueSnapshots[k]);
        double avgQ = n>0 ? sumQ/n : 0.0;

        NS_LOG_UNCOND("链路" << i << " (节点" << m_linkInfos[i].nodeA << "->节点" << m_linkInfos[i].nodeB << "):");
        NS_LOG_UNCOND("  平均队列: " << std::fixed << std::setprecision(2) << avgQ << " 包");

        auto printFullVec = [](const std::string& label, const std::vector<double>& v) {
            std::ostringstream oss; oss << "  " << label << ": [";
            for(size_t k=0; k<v.size(); ++k) {
                oss << std::fixed << std::setprecision(2) << v[k];
                if(k < v.size()-1) oss << ", ";
            }
            oss << "]"; NS_LOG_UNCOND(oss.str());
        };

        printFullVec("队列A序列", rs->sa->queueSnapshots);
        printFullVec("队列B序列", rs->sb->queueSnapshots);
        printFullVec("区间利用率序列(%)", rs->utilSnapshots);

        uint64_t drops = rs->sa->dropPackets + rs->sb->dropPackets;
        NS_LOG_UNCOND("  利用率=" << std::fixed << std::setprecision(2) << totalUtil << "% | "
                      << "发送字节=" << totalTx << " | "
                      << "A→B: " << rs->sa->txBytes << " | "
                      << "B→A: " << rs->sb->txBytes << " | "
                      << "丢包数=" << drops);
        NS_LOG_UNCOND("");
        */
    }
    
    double sumDelay=0; uint32_t cnt=0; uint64_t txP=0, lostP=0;
    for (auto const& kv : stats) {
        if (kv.second.rxPackets > 0) { sumDelay += kv.second.delaySum.GetSeconds()/kv.second.rxPackets*1000.0; cnt++; }
        txP += kv.second.txPackets; lostP += kv.second.lostPackets;
    }
    
    uint64_t rxBytes=0;
    for(const auto& f : m_flowInfos) rxBytes += std::min<uint64_t>(f.sinkApp->GetTotalRx(), f.bytesPlanned);
    
    double tput = rxBytes*8.0/(m_appsStop*1e6);
    double delay = cnt>0 ? sumDelay/cnt : 0.0;
    double loss = txP>0 ? 100.0*lostP/txP : 0.0;

    finalResult.global.throughputMbps = tput;
    finalResult.global.avgDelayMs = delay;
    finalResult.global.lossRatePct = loss;

    NS_LOG_UNCOND("---------- 网络整体指标 (库内部计算) ----------");
    NS_LOG_UNCOND("总吞吐量: " << tput << " Mbps");
    NS_LOG_UNCOND("平均时延: " << delay << " ms");
    NS_LOG_UNCOND("丢包率: " << loss << " %");
    NS_LOG_UNCOND("==============================================================\n");

    return finalResult;
}

} // namespace ns3
