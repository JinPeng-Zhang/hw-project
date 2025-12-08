/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef SIM_STATS_COLLECTOR_H
#define SIM_STATS_COLLECTOR_H

#include "common-defs.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/queue.h"
#include <memory>

namespace ns3 {

struct LinkRuntimeStats;

class SimStatsCollector : public Object {
public:
    static TypeId GetTypeId(void);
    SimStatsCollector();
    virtual ~SimStatsCollector();

    void Setup(const std::vector<LinkInfo>& links, 
               const std::vector<FlowInfo>& flows, 
               double appsStop);

    // 【修改】返回类型改为 SimResult 结构体
    SimResult CollectAndPrint();

private:
    void SamplingRoutine();
    void SubSamplingRoutine();
    
    std::vector<LinkInfo> m_linkInfos;
    std::vector<FlowInfo> m_flowInfos;
    double m_appsStop;
    
    std::vector<std::shared_ptr<LinkRuntimeStats>> m_runtimeStats;
    FlowMonitorHelper m_flowmonHelper;
    Ptr<FlowMonitor> m_monitor;

    struct {
        std::vector<uint64_t> qA_sum;
        std::vector<uint64_t> qB_sum;
        uint32_t count = 0;
    } m_accumulator;
    const uint32_t SUB_SAMPLE_COUNT = 1000;
    const double SAMPLE_INTERVAL = 0.5;
};

} // namespace ns3
#endif
