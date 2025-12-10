/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef TOPO_TRAFFIC_BUILDER_H
#define TOPO_TRAFFIC_BUILDER_H

#include "common-defs.h"
#include "ns3/internet-module.h"
#include <string>
#include <vector>

namespace ns3 {

class TopoTrafficBuilder {
public:
    TopoTrafficBuilder();
    
    // 一键搭建
    bool BuildAndInstall(std::string topoFilePath, 
                         std::string cdfFilePath,
                         uint32_t flows, 
                         std::string transport,
                         double loadRate, 
                         double linkRefMbps, 
                         double appsStop);

    // Getters
    std::vector<LinkInfo> GetLinks() const { return m_links; }
    std::vector<FlowInfo> GetFlows() const { return m_flows; }
    double GetAppsStopTime() const { return m_appsStop; }

private:
    NodeContainer m_nodes;
    std::vector<LinkInfo> m_links;
    std::vector<FlowInfo> m_flows;
    ApplicationContainer m_apps;
    double m_appsStop;
    std::vector<uint32_t> m_edgeNodes;
};

} // namespace ns3
#endif
