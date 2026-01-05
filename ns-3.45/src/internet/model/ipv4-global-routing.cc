//
// Copyright (c) 2008 University of Washington
// SPDX-License-Identifier: GPL-2.0-only
//

#include "ipv4-global-routing.h"

#include "global-route-manager.h"
#include "ipv4-route.h"
#include "ipv4-routing-table-entry.h"

#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/ipv4.h"

#include <iomanip>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <limits>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <list>
#include <unordered_map>

// 文件监控需要的头文件
#include <sys/stat.h>
#include <ctime>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Ipv4GlobalRouting");

NS_OBJECT_ENSURE_REGISTERED(Ipv4GlobalRouting);

// 匿名命名空间: ECMP 权重、文件监控、流缓存
namespace
{
// ==================== 原有的权重管理 ====================
// (nodeId, N备选数) -> 该节点此N下的权重前缀（缓存）
static std::map<std::pair<uint32_t, size_t>, std::vector<uint32_t>> g_ecmpWeightCacheByNodeN;

// 节点原始权重表：nodeId -> 一串权重
static std::map<uint32_t, std::vector<uint32_t>> g_nodeRawWeights;
static bool g_weightsLoaded = false;

// 文件监控相关 - 使用纳秒级时间戳
static uint64_t g_lastFileModTimeNano = 0;
static bool g_enableAutoReload = true;  // 默认启用自动重载
static uint32_t g_weightVersion = 0;    // 权重配置版本号（仅用于日志）

// ==================== 新增：流缓存配置 ====================
// 绝对超时：流最多存活时间（防止老流使用过期权重）
static const uint64_t ABSOLUTE_TIMEOUT = 4ULL * 1000000000ULL;  // 600秒 = 10分钟（纳秒）

// 空闲超时：最后一个包后的等待时间（检测流结束）
static const uint64_t IDLE_TIMEOUT = 2ULL * 1000000000ULL;       // 60秒（纳秒）

// LRU 容量上限
static const size_t MAX_FLOW_CACHE_SIZE = 100000;  // 10万条流

// 统计计数器
static uint64_t g_cacheLookupCount = 0;     // 查询次数
static uint64_t g_cacheHitCount = 0;        // 命中次数
static uint64_t g_cacheExpiredCount = 0;    // 过期淘汰次数
static uint64_t g_cacheLRUEvictCount = 0;   // LRU淘汰次数

// 打印保护：确保统计信息只打印一次
static bool g_statsPrinted = false;

// ==================== 流标识（五元组）====================
struct FlowKey
{
    uint32_t srcIP;
    uint32_t dstIP;
    uint16_t srcPort;
    uint16_t dstPort;
    uint8_t protocol;

    FlowKey(uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint8_t proto)
        : srcIP(sip), dstIP(dip), srcPort(sp), dstPort(dp), protocol(proto)
    {}

    bool operator==(const FlowKey& other) const
    {
        return srcIP == other.srcIP &&
               dstIP == other.dstIP &&
               srcPort == other.srcPort &&
               dstPort == other.dstPort &&
               protocol == other.protocol;
    }
};

// FlowKey 的哈希函数
struct FlowKeyHash
{
    std::size_t operator()(const FlowKey& key) const
    {
        // 使用 FNV-1a 哈希
        std::size_t h = 14695981039346656037ULL;
        h ^= key.srcIP; h *= 1099511628211ULL;
        h ^= key.dstIP; h *= 1099511628211ULL;
        h ^= key.srcPort; h *= 1099511628211ULL;
        h ^= key.dstPort; h *= 1099511628211ULL;
        h ^= key.protocol; h *= 1099511628211ULL;
        return h;
    }
};

// ==================== 缓存条目 ====================
struct FlowCacheEntry
{
    uint32_t selectedIndex;      // 选中的路径索引
    uint64_t createTime;         // 创建时间（纳秒）
    uint64_t lastAccessTime;     // 最后访问时间（纳秒）
    uint32_t weightVersion;      // 权重版本号
    uint64_t packetCount;        // 包计数（统计用）

    FlowCacheEntry()
        : selectedIndex(0), createTime(0), lastAccessTime(0), 
          weightVersion(0), packetCount(0)
    {}
};

// ==================== 流缓存数据结构 ====================
// 主存储：FlowKey -> FlowCacheEntry
static std::unordered_map<FlowKey, FlowCacheEntry, FlowKeyHash> g_flowCache;

// LRU 链表：头部=最新访问，尾部=最旧访问
static std::list<FlowKey> g_lruList;

// 辅助索引：FlowKey -> LRU链表中的迭代器
static std::unordered_map<FlowKey, std::list<FlowKey>::iterator, FlowKeyHash> g_lruIterMap;

// ==================== 64-bit FNV-1a 常量（用于路径哈希）====================
static constexpr uint64_t FNV64_OFFSET = 14695981039346656037ull;
static constexpr uint64_t FNV64_PRIME  = 1099511628211ull;

static inline void Fnv64MixByte(uint64_t& h, uint8_t b)
{
    h ^= b;
    h *= FNV64_PRIME;
}
static inline void Fnv64Mix16(uint64_t& h, uint16_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffu));
}
static inline void Fnv64Mix32(uint64_t& h, uint32_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 16) & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 24) & 0xffu));
}
static inline void Fnv64Mix64(uint64_t& h, uint64_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 16) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 24) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 32) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 40) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 48) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 56) & 0xffull));
}

// 64-bit 雪崩压缩为 32-bit
static inline uint32_t Avalanche64To32(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<uint32_t>(x ^ (x >> 32));
}

// 节点重盐
static uint64_t
GetNodeHeavySalt(Ptr<Ipv4> ipv4)
{
    if (!ipv4 || !ipv4->GetObject<Node>())
    {
        return 0x9e3779b97f4a7c15ull;
    }
    uint32_t nodeId = ipv4->GetObject<Node>()->GetId();

    uint64_t h = FNV64_OFFSET;
    Fnv64Mix32(h, nodeId);

    uint32_t nIf = ipv4->GetNInterfaces();
    Fnv64Mix32(h, nIf);
    for (uint32_t i = 0; i < nIf; ++i)
    {
        uint32_t nAddr = ipv4->GetNAddresses(i);
        Fnv64Mix32(h, nAddr);
        for (uint32_t j = 0; j < nAddr; ++j)
        {
            Ipv4InterfaceAddress ifa = ipv4->GetAddress(i, j);
            Fnv64Mix32(h, ifa.GetLocal().Get());
            Fnv64Mix32(h, ifa.GetMask().Get());
            Fnv64Mix32(h, ifa.GetBroadcast().Get());
        }
    }
    Fnv64Mix32(h, nodeId ^ 0x85ebca6bu);
    if (h == 0) h = 0x9e3779b97f4a7c15ull;
    return h;
}

// 去掉行内注释并修剪空白
static std::string
StripCommentAndTrim(const std::string& line)
{
    std::string s = line;
    auto pos = s.find('#');
    if (pos != std::string::npos) s = s.substr(0, pos);
    auto notspace = [](char ch){ return !std::isspace(static_cast<unsigned char>(ch)); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

// 获取文件修改时间（纳秒级精度）
#ifdef __APPLE__
static uint64_t GetFileModTimeNano(const std::string& filename)
{
    struct stat fileStat;
    if (stat(filename.c_str(), &fileStat) == 0) {
        return static_cast<uint64_t>(fileStat.st_mtimespec.tv_sec) * 1000000000ULL 
             + static_cast<uint64_t>(fileStat.st_mtimespec.tv_nsec);
    }
    return 0;
}
#elif defined(__linux__)
static uint64_t GetFileModTimeNano(const std::string& filename)
{
    struct stat fileStat;
    if (stat(filename.c_str(), &fileStat) == 0) {
        return static_cast<uint64_t>(fileStat.st_mtim.tv_sec) * 1000000000ULL 
             + static_cast<uint64_t>(fileStat.st_mtim.tv_nsec);
    }
    return 0;
}
#else
// 其他系统回退到秒级精度
static uint64_t GetFileModTimeNano(const std::string& filename)
{
    struct stat fileStat;
    if (stat(filename.c_str(), &fileStat) == 0) {
        return static_cast<uint64_t>(fileStat.st_mtime) * 1000000000ULL;
    }
    return 0;
}
#endif

// 从指定文件加载权重（返回加载的节点ID集合）
static std::set<uint32_t>
LoadWeightsFromFile(const std::string& filename)
{
    std::set<uint32_t> loadedNodes;
    std::ifstream fin(filename);
    if (!fin.good())
    {
        NS_LOG_WARN("无法打开权重文件: " << filename);
        return loadedNodes;
    }

    std::string line;
    uint32_t lineNo = 0;
    while (std::getline(fin, line))
    {
        ++lineNo;
        std::string s = StripCommentAndTrim(line);
        if (s.empty()) continue;

        std::istringstream iss(s);
        std::string key;
        iss >> key;
        if (!iss) continue;

        // 解析 nodeId
        if (!key.empty() && key.back() == ':')
        {
            key.pop_back();
        }
        errno = 0;
        char* endp = nullptr;
        unsigned long nid64 = std::strtoul(key.c_str(), &endp, 10);
        if (errno != 0 || endp == key.c_str())
        {
            NS_LOG_WARN(filename << " 第 " << lineNo << " 行节点号解析失败: " << key);
            continue;
        }
        uint32_t nodeId = static_cast<uint32_t>(nid64);

        // 解析权重序列
        std::vector<uint32_t> w;
        std::string tok;
        while (iss >> tok)
        {
            errno = 0;
            char* ep = nullptr;
            unsigned long long val = std::strtoull(tok.c_str(), &ep, 10);
            if (errno != 0 || ep == tok.c_str())
            {
                NS_LOG_WARN(filename << " 第 " << lineNo << " 行存在无法解析的权重: " << tok);
                continue;
            }
            if (val > std::numeric_limits<uint32_t>::max())
            {
                NS_LOG_WARN(filename << " 第 " << lineNo << " 行权重过大, 截断: " << val);
                val = std::numeric_limits<uint32_t>::max();
            }
            w.push_back(static_cast<uint32_t>(val));
        }

        if (w.empty())
        {
            NS_LOG_WARN(filename << " 第 " << lineNo << " 行无有效权重");
            continue;
        }

        // 更新到全局权重表
        g_nodeRawWeights[nodeId] = std::move(w);
        loadedNodes.insert(nodeId);
    }

    return loadedNodes;
}

// 初始化：从 Config 文件加载所有节点权重
static bool
LoadAllNodeWeightsFile()
{
    const std::string configFile = "ecmpProbabilityConfig.txt";
    
    // 清空旧数据
    g_nodeRawWeights.clear();
    g_ecmpWeightCacheByNodeN.clear();

    auto nodes = LoadWeightsFromFile(configFile);
    
    if (nodes.empty())
    {
        NS_LOG_WARN("初始配置文件 " << configFile << " 中未加载到任何节点权重");
        return false;
    }

    // 记录运行时文件的初始修改时间
    g_lastFileModTimeNano = GetFileModTimeNano("ecmpProbability.txt");

    NS_LOG_INFO("已从 " << configFile << " 加载 " << nodes.size() 
                << " 个节点的权重配置 (version=" << g_weightVersion << ")");
    return true;
}

// 热重载：从 ecmpProbability.txt 增量更新（保守策略：不清空流缓存）
static void CheckAndReloadWeightsFile()
{
    if (!g_enableAutoReload) return;
    
    const std::string runtimeFile = "ecmpProbability.txt";
    uint64_t currentModTime = GetFileModTimeNano(runtimeFile);
    
    // 文件修改时间变化（纳秒级比较）
    if (currentModTime > 0 && currentModTime != g_lastFileModTimeNano)
    {
        NS_LOG_INFO("检测到 " << runtimeFile << " 变化，增量更新中...");
        
        // 清空权重缓存但保留基础权重表
        g_ecmpWeightCacheByNodeN.clear();
        g_weightVersion++;
        
        // 增量加载：只更新 ecmpProbability.txt 中的节点
        auto updatedNodes = LoadWeightsFromFile(runtimeFile);
        
        g_lastFileModTimeNano = currentModTime;
        
        // 保守策略：不清空流缓存，让老流继续使用旧路径直到过期
        // 如需激进策略（立即重选所有流），取消下面三行注释：
        // g_flowCache.clear();
        // g_lruList.clear();
        // g_lruIterMap.clear();
        
        std::cout << "[" << Simulator::Now().GetSeconds() 
                  << "s] 增量更新权重: 修改了 " << updatedNodes.size() 
                  << " 个节点 (version=" << g_weightVersion 
                  << ", 流缓存保留策略=保守)" << std::endl;
        
        // 打印更新的节点
        if (!updatedNodes.empty())
        {
            std::cout << "  更新的节点: ";
            for (uint32_t nid : updatedNodes)
            {
                std::cout << nid << " ";
                if (g_nodeRawWeights.count(nid))
                {
                    std::cout << "(";
                    const auto& weights = g_nodeRawWeights[nid];
                    for (size_t i = 0; i < std::min(size_t(4), weights.size()); ++i)
                    {
                        if (i > 0) std::cout << ":";
                        std::cout << weights[i];
                    }
                    if (weights.size() > 4) std::cout << "...";
                    std::cout << ") ";
                }
            }
            std::cout << std::endl;
        }
    }
}

// 获取某节点在候选数 N 下的权重前缀
static bool
GetNodeWeights(uint32_t nodeId, size_t n, std::vector<uint32_t>& out)
{
    auto key = std::make_pair(nodeId, n);
    auto it = g_ecmpWeightCacheByNodeN.find(key);
    if (it != g_ecmpWeightCacheByNodeN.end())
    {
        out = it->second;
        return true;
    }

    if (!g_weightsLoaded)
    {
        LoadAllNodeWeightsFile();
        g_weightsLoaded = true;
    }

    auto itRaw = g_nodeRawWeights.find(nodeId);
    if (itRaw == g_nodeRawWeights.end())
    {
        return false;
    }
    const auto& raw = itRaw->second;
    if (raw.size() < n)
    {
        NS_LOG_WARN("节点 " << nodeId << " 权重数量不足: 需要 " << n << " 实际 " << raw.size());
        return false;
    }

    std::vector<uint32_t> w(raw.begin(), raw.begin() + n);
    uint64_t sum = 0;
    for (uint32_t v : w) sum += v;
    if (sum == 0)
    {
        NS_LOG_WARN("节点 " << nodeId << " 权重前缀总和为 0 (N=" << n << ")");
        return false;
    }

    g_ecmpWeightCacheByNodeN.emplace(key, w);
    out = std::move(w);
    return true;
}

// ==================== 新增：流缓存管理函数 ====================

// 判断缓存条目是否过期
static bool IsFlowExpired(const FlowCacheEntry& entry, uint64_t currentTime)
{
    // 绝对超时：从创建开始超过 ABSOLUTE_TIMEOUT
    if (currentTime > entry.createTime && 
        (currentTime - entry.createTime) > ABSOLUTE_TIMEOUT)
    {
        return true;
    }
    
    // 空闲超时：从最后访问超过 IDLE_TIMEOUT
    if (currentTime > entry.lastAccessTime && 
        (currentTime - entry.lastAccessTime) > IDLE_TIMEOUT)
    {
        return true;
    }
    
    return false;
}

// 从缓存中移除流（同时删除 HashMap 和 LRU 链表）
static void RemoveFlowFromCache(const FlowKey& key)
{
    auto itCache = g_flowCache.find(key);
    if (itCache == g_flowCache.end())
    {
        return;  // 不存在，无需删除
    }
    
    // 从 LRU 链表中移除
    auto itLRU = g_lruIterMap.find(key);
    if (itLRU != g_lruIterMap.end())
    {
        g_lruList.erase(itLRU->second);
        g_lruIterMap.erase(itLRU);
    }
    
    // 从 HashMap 中移除
    g_flowCache.erase(itCache);
}

// LRU 淘汰：移除链表尾部（最旧）的流
static void EvictLRU()
{
    if (g_lruList.empty())
    {
        return;
    }
    
    // 获取尾部（最旧）的流
    FlowKey oldestKey = g_lruList.back();
    
    // 从链表移除
    g_lruList.pop_back();
    
    // 从辅助索引移除
    g_lruIterMap.erase(oldestKey);
    
    // 从 HashMap 移除
    g_flowCache.erase(oldestKey);
    
    g_cacheLRUEvictCount++;
    
    NS_LOG_DEBUG("LRU淘汰流: 缓存大小=" << g_flowCache.size() 
                 << " LRU淘汰总数=" << g_cacheLRUEvictCount);
}

// 移动流到 LRU 链表头部（最新）
static void MoveToLRUHead(const FlowKey& key)
{
    auto itLRU = g_lruIterMap.find(key);
    if (itLRU == g_lruIterMap.end())
    {
        // 不应该发生：缓存中存在但 LRU 链表中不存在
        NS_LOG_ERROR("MoveToLRUHead: 流不在 LRU 链表中");
        return;
    }
    
    // 从当前位置删除
    g_lruList.erase(itLRU->second);
    
    // 插入到头部
    g_lruList.push_front(key);
    g_lruIterMap[key] = g_lruList.begin();
}

// 查询流缓存（返回 true 表示命中且未过期）
static bool LookupFlowCache(const FlowKey& key, uint32_t& selectedIndex)
{
    g_cacheLookupCount++;
    
    uint64_t currentTime = static_cast<uint64_t>(Simulator::Now().GetNanoSeconds());
    
    auto it = g_flowCache.find(key);
    if (it == g_flowCache.end())
    {
        // 未命中
        return false;
    }
    
    FlowCacheEntry& entry = it->second;
    
    // 检查是否过期
    if (IsFlowExpired(entry, currentTime))
    {
        NS_LOG_DEBUG("流缓存过期: srcIP=" << key.srcIP 
                     << " dstIP=" << key.dstIP
                     << " srcPort=" << key.srcPort
                     << " dstPort=" << key.dstPort
                     << " proto=" << static_cast<uint32_t>(key.protocol)
                     << " 创建时间=" << (currentTime - entry.createTime) / 1e9 << "s前"
                     << " 空闲时间=" << (currentTime - entry.lastAccessTime) / 1e9 << "s");
        
        RemoveFlowFromCache(key);
        g_cacheExpiredCount++;
        return false;
    }
    
    // 命中且未过期
    g_cacheHitCount++;
    selectedIndex = entry.selectedIndex;
    
    // 更新访问时间
    entry.lastAccessTime = currentTime;
    entry.packetCount++;
    
    // 移到 LRU 头部
    MoveToLRUHead(key);
    
    NS_LOG_DEBUG("流缓存命中: selectedIndex=" << selectedIndex
                 << " 包计数=" << entry.packetCount
                 << " 命中率=" << (100.0 * g_cacheHitCount / g_cacheLookupCount) << "%");
    
    return true;
}

// 插入流缓存
static void InsertFlowCache(const FlowKey& key, uint32_t selectedIndex)
{
  std::cout << "[流缓存] 插入: "
            << Ipv4Address(key.srcIP) << ":" << key.srcPort
            << " -> " << Ipv4Address(key.dstIP) << ":" << key.dstPort
            << " proto=" << (int)key.protocol
            << " at t=" << Simulator::Now().GetSeconds() << "s"
            << std::endl;
    uint64_t currentTime = static_cast<uint64_t>(Simulator::Now().GetNanoSeconds());
    
    // 检查容量限制
    if (g_flowCache.size() >= MAX_FLOW_CACHE_SIZE)
    {
        EvictLRU();
    }
    
    // 创建新条目
    FlowCacheEntry entry;
    entry.selectedIndex = selectedIndex;
    entry.createTime = currentTime;
    entry.lastAccessTime = currentTime;
    entry.weightVersion = g_weightVersion;
    entry.packetCount = 1;
    
    // 插入 HashMap
    g_flowCache[key] = entry;
    
    // 插入 LRU 链表头部
    g_lruList.push_front(key);
    g_lruIterMap[key] = g_lruList.begin();
    
    NS_LOG_DEBUG("流缓存插入: selectedIndex=" << selectedIndex
                 << " 缓存大小=" << g_flowCache.size()
                 << " 权重版本=" << g_weightVersion);
}

// 打印流缓存统计信息（带保护，只打印一次）
static void PrintFlowCacheStats()
{
    // 检查是否已经打印过
    if (g_statsPrinted)
    {
        return;
    }
    g_statsPrinted = true;
    
    if (g_cacheLookupCount == 0)
    {
        std::cout << "[流缓存统计] 无查询记录" << std::endl;
        return;
    }
    
    double hitRate = 100.0 * g_cacheHitCount / g_cacheLookupCount;
    
    std::cout << "\n========== 流缓存统计 ==========" << std::endl;
    std::cout << "查询次数:     " << g_cacheLookupCount << std::endl;
    std::cout << "命中次数:     " << g_cacheHitCount << std::endl;
    std::cout << "命中率:       " << std::fixed << std::setprecision(2) << hitRate << "%" << std::endl;
    std::cout << "过期淘汰:     " << g_cacheExpiredCount << std::endl;
    std::cout << "LRU淘汰:      " << g_cacheLRUEvictCount << std::endl;
    std::cout << "当前缓存大小: " << g_flowCache.size() << " / " << MAX_FLOW_CACHE_SIZE << std::endl;
    std::cout << "权重版本:     " << g_weightVersion << std::endl;
    std::cout << "超时配置:     绝对=" << ABSOLUTE_TIMEOUT / 1000000000ULL << "s, 空闲=" 
              << IDLE_TIMEOUT / 1000000000ULL << "s" << std::endl;
    std::cout << "==============================\n" << std::endl;
}

} // anonymous namespace

TypeId
Ipv4GlobalRouting::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::Ipv4GlobalRouting")
            .SetParent<Object>()
            .SetGroupName("Internet")
            .AddAttribute("RandomEcmpRouting",
                          "Set to true if packets are randomly routed among ECMP; set to false for "
                          "using only one route consistently",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_randomEcmpRouting),
                          MakeBooleanChecker())
            .AddAttribute("RespondToInterfaceEvents",
                          "Set to true if you want to dynamically recompute the global routes upon "
                          "Interface notification events (up/down, or add/remove address)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_respondToInterfaceEvents),
                          MakeBooleanChecker())
            .AddAttribute("PerflowEcmpRouting",
                          "Set to true to select ECMP path via a stable hash of the flow "
                          "(prefer TCP/UDP 5-tuple; fallback to 3-tuple src/dst/proto). "
                          "When true, this overrides RandomEcmpRouting.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_perflowEcmpRouting),
                          MakeBooleanChecker());
    return tid;
}

Ipv4GlobalRouting::Ipv4GlobalRouting()
    : m_randomEcmpRouting(false),
      m_respondToInterfaceEvents(false),
      m_perflowEcmpRouting(false)
{
    NS_LOG_FUNCTION(this);
    m_rand = CreateObject<UniformRandomVariable>();
}

Ipv4GlobalRouting::~Ipv4GlobalRouting()
{
    NS_LOG_FUNCTION(this);
}

void
Ipv4GlobalRouting::AddHostRouteTo(Ipv4Address dest, Ipv4Address nextHop, uint32_t interface)
{
    NS_LOG_FUNCTION(this << dest << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateHostRouteTo(dest, nextHop, interface);
    m_hostRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddHostRouteTo(Ipv4Address dest, uint32_t interface)
{
    NS_LOG_FUNCTION(this << dest << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateHostRouteTo(dest, interface);
    m_hostRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddNetworkRouteTo(Ipv4Address network,
                                     Ipv4Mask networkMask,
                                     Ipv4Address nextHop,
                                     uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, nextHop, interface);
    m_networkRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddNetworkRouteTo(Ipv4Address network, Ipv4Mask networkMask, uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, interface);
    m_networkRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddASExternalRouteTo(Ipv4Address network,
                                        Ipv4Mask networkMask,
                                        Ipv4Address nextHop,
                                        uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, nextHop, interface);
    m_ASexternalRoutes.push_back(route);
}

Ptr<Ipv4Route>
Ipv4GlobalRouting::LookupGlobal(Ipv4Address dest,
                                Ptr<NetDevice> oif,
                                const Ipv4Header* hdr,
                                Ptr<const Packet> payload)
{
    // 检查文件是否变化并增量重新加载（纳秒级精度）
    CheckAndReloadWeightsFile();
    
    Ptr<Ipv4Route> rtentry = nullptr;

    uint32_t nodeId = m_ipv4 ? m_ipv4->GetObject<Node>()->GetId() : 0;

    typedef std::vector<Ipv4RoutingTableEntry*> RouteVec_t;
    RouteVec_t allRoutes;

    // Host routes
    for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
    {
        NS_ASSERT((*i)->IsHost());
        if ((*i)->GetDest() == dest)
        {
            if (oif && oif != m_ipv4->GetNetDevice((*i)->GetInterface()))
            {
                continue;
            }
            allRoutes.push_back(*i);
        }
    }

    // Network routes
    if (allRoutes.empty())
    {
        for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
        {
            Ipv4Mask mask = (*j)->GetDestNetworkMask();
            Ipv4Address entry = (*j)->GetDestNetwork();
            if (mask.IsMatch(dest, entry))
            {
                if (oif && oif != m_ipv4->GetNetDevice((*j)->GetInterface()))
                {
                    continue;
                }
                allRoutes.push_back(*j);
            }
        }
    }

    // External routes
    if (allRoutes.empty())
    {
        for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
        {
            Ipv4Mask mask = (*k)->GetDestNetworkMask();
            Ipv4Address entry = (*k)->GetDestNetwork();
            if (mask.IsMatch(dest, entry))
            {
                if (oif && oif != m_ipv4->GetNetDevice((*k)->GetInterface()))
                {
                    continue;
                }
                allRoutes.push_back(*k);
                break;
            }
        }
    }

    if (allRoutes.empty())
    {
        return nullptr;
    }

    uint32_t selectIndex = 0;

    if (allRoutes.size() > 1)
    {
        if (m_perflowEcmpRouting)
        {
            // ==================== 基于流缓存的 Per-flow ECMP ====================
            bool ok = false;
            uint16_t sport = 0, dport = 0;
            uint8_t proto = 0;

            if (hdr && payload)
            {
                proto = hdr->GetProtocol();

                if (proto == 6) // TCP
                {
                    TcpHeader th;
                    Ptr<Packet> pkt = payload->Copy();
                    if (pkt->PeekHeader(th))
                    {
                        sport = th.GetSourcePort();
                        dport = th.GetDestinationPort();
                        ok = true;
                    }
                }
                else if (proto == 17) // UDP
                {
                    UdpHeader uh;
                    Ptr<Packet> pkt = payload->Copy();
                    if (pkt->PeekHeader(uh))
                    {
                        sport = uh.GetSourcePort();
                        dport = uh.GetDestinationPort();
                        ok = true;
                    }
                }

                if (ok)
                {
                    // 构造 FlowKey
                    FlowKey flowKey(hdr->GetSource().Get(), 
                                   hdr->GetDestination().Get(),
                                   sport, dport, proto);

                    // 先查询流缓存
                    if (LookupFlowCache(flowKey, selectIndex))
                    {
                        // 缓存命中且未过期，直接使用缓存结果
                        NS_LOG_DEBUG("流缓存命中: node=" << nodeId
                                    << " flow=" << hdr->GetSource() << ":" << sport
                                    << " -> " << hdr->GetDestination() << ":" << dport
                                    << " proto=" << static_cast<unsigned>(proto)
                                    << " cachedIndex=" << selectIndex);
                    }
                    else
                    {
                        // 缓存未命中或已过期，重新哈希选路
                        uint64_t h64 = FNV64_OFFSET;

                        // 计算5元组哈希
                        Fnv64Mix32(h64, hdr->GetSource().Get());
                        Fnv64Mix32(h64, hdr->GetDestination().Get());
                        Fnv64Mix16(h64, sport);
                        Fnv64Mix16(h64, dport);
                        Fnv64MixByte(h64, proto);

                        // 节点重盐（第六元组）
                        uint64_t nodeSalt = GetNodeHeavySalt(m_ipv4);
                        Fnv64Mix64(h64, 0x9e3779b97f4a7c15ull);
                        Fnv64Mix64(h64, nodeSalt);

                        uint32_t h32 = Avalanche64To32(h64);
                        
                        std::vector<uint32_t> weights;
                        if (GetNodeWeights(nodeId, allRoutes.size(), weights))
                        {
                            // 加权选择
                            uint64_t total = 0;
                            for (uint32_t v : weights) total += v;

                            uint64_t r = (static_cast<uint64_t>(h32) * total) >> 32;

                            uint64_t acc = 0;
                            size_t idx = 0;
                            for (; idx < weights.size(); ++idx)
                            {
                                acc += weights[idx];
                                if (r < acc) break;
                            }
                            if (idx >= weights.size()) idx = weights.size() - 1;
                            selectIndex = static_cast<uint32_t>(idx);

                            NS_LOG_INFO("新流选路: node=" << nodeId
                                       << " 5-tuple: " << hdr->GetSource() << ":" << sport
                                       << " -> " << hdr->GetDestination() << ":" << dport
                                       << " proto=" << static_cast<unsigned>(proto)
                                       << " hash32=" << h32
                                       << " selected=" << selectIndex
                                       << " (weights version=" << g_weightVersion << ")");
                        }
                        else
                        {
                            // 等权回退
                            selectIndex = (static_cast<uint64_t>(h32) * allRoutes.size()) >> 32;
                            NS_LOG_WARN("No weights for node=" << nodeId 
                                       << ", equal fallback index=" << selectIndex);
                        }

                        // 插入流缓存
                        InsertFlowCache(flowKey, selectIndex);
                    }
                }
                else
                {
                    selectIndex = 0;
                }
            }
            else
            {
                selectIndex = 0;
            }
        }
        else if (m_randomEcmpRouting)
        {
            selectIndex = m_rand->GetInteger(0, allRoutes.size() - 1);
        }
        else
        {
            selectIndex = 0;
        }
    }

    Ipv4RoutingTableEntry* route = allRoutes.at(selectIndex);

    rtentry = Create<Ipv4Route>();
    rtentry->SetDestination(route->GetDest());
    rtentry->SetSource(m_ipv4->GetAddress(route->GetInterface(), 0).GetLocal());
    rtentry->SetGateway(route->GetGateway());
    rtentry->SetOutputDevice(m_ipv4->GetNetDevice(route->GetInterface()));
    return rtentry;
}

uint32_t
Ipv4GlobalRouting::GetNRoutes() const
{
    uint32_t n = 0;
    n += m_hostRoutes.size();
    n += m_networkRoutes.size();
    n += m_ASexternalRoutes.size();
    return n;
}

Ipv4RoutingTableEntry*
Ipv4GlobalRouting::GetRoute(uint32_t index) const
{
    if (index < m_hostRoutes.size())
    {
        uint32_t tmp = 0;
        for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
        {
            if (tmp == index)
            {
                return *i;
            }
            tmp++;
        }
    }
    index -= m_hostRoutes.size();
    uint32_t tmp = 0;
    if (index < m_networkRoutes.size())
    {
        for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
        {
            if (tmp == index)
            {
                return *j;
            }
            tmp++;
        }
    }
    index -= m_networkRoutes.size();
    tmp = 0;
    for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
    {
        if (tmp == index)
        {
            return *k;
        }
        tmp++;
    }
    NS_ASSERT(false);
    return nullptr;
}

void
Ipv4GlobalRouting::RemoveRoute(uint32_t index)
{
    if (index < m_hostRoutes.size())
    {
        uint32_t tmp = 0;
        for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
        {
            if (tmp == index)
            {
                delete *i;
                m_hostRoutes.erase(i);
                return;
            }
            tmp++;
        }
    }
    index -= m_hostRoutes.size();
    uint32_t tmp = 0;
    for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
    {
        if (tmp == index)
        {
            delete *j;
            m_networkRoutes.erase(j);
            return;
        }
        tmp++;
    }
    index -= m_networkRoutes.size();
    tmp = 0;
    for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
    {
        if (tmp == index)
        {
            delete *k;
            m_ASexternalRoutes.erase(k);
            return;
        }
        tmp++;
    }
    NS_ASSERT(false);
}

int64_t
Ipv4GlobalRouting::AssignStreams(int64_t stream)
{
    m_rand->SetStream(stream);
    return 1;
}

void
Ipv4GlobalRouting::DoDispose()
{
    // 打印流缓存统计信息（带保护，只打印一次）
    PrintFlowCacheStats();
    
    for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i = m_hostRoutes.erase(i))
    {
        delete (*i);
    }
    for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j = m_networkRoutes.erase(j))
    {
        delete (*j);
    }
    for (auto l = m_ASexternalRoutes.begin(); l != m_ASexternalRoutes.end();
         l = m_ASexternalRoutes.erase(l))
    {
        delete (*l);
    }

    Ipv4RoutingProtocol::DoDispose();
}

void
Ipv4GlobalRouting::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    std::ostream* os = stream->GetStream();
    std::ios oldState(nullptr);
    oldState.copyfmt(*os);

    *os << std::resetiosflags(std::ios::adjustfield) << std::setiosflags(std::ios::left);

    *os << "Node: " << m_ipv4->GetObject<Node>()->GetId() << ", Time: " << Now().As(unit)
        << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
        << ", Ipv4GlobalRouting table" << std::endl;

    if (GetNRoutes() > 0)
    {
        *os << "Destination     Gateway         Genmask         Flags Metric Ref    Use Iface"
            << std::endl;
        for (uint32_t j = 0; j < GetNRoutes(); j++)
        {
            std::ostringstream dest;
            std::ostringstream gw;
            std::ostringstream mask;
            std::ostringstream flags;
            Ipv4RoutingTableEntry route = GetRoute(j);
            dest << route.GetDest();
            *os << std::setw(16) << dest.str();
            gw << route.GetGateway();
            *os << std::setw(16) << gw.str();
            mask << route.GetDestNetworkMask();
            *os << std::setw(16) << mask.str();
            flags << "U";
            if (route.IsHost())
            {
                flags << "H";
            }
            else if (route.IsGateway())
            {
                flags << "G";
            }
            *os << std::setw(6) << flags.str();
            *os << "-" << "      " << "-" << "      " << "-" << "   ";
            if (!Names::FindName(m_ipv4->GetNetDevice(route.GetInterface())).empty())
            {
                *os << Names::FindName(m_ipv4->GetNetDevice(route.GetInterface()));
            }
            else
            {
                *os << route.GetInterface();
            }
            *os << std::endl;
        }
    }
    *os << std::endl;
    (*os).copyfmt(oldState);
}

Ptr<Ipv4Route>
Ipv4GlobalRouting::RouteOutput(Ptr<Packet> p,
                               const Ipv4Header& header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno& sockerr)
{
    if (header.GetDestination().IsMulticast())
    {
        return nullptr;
    }
    Ptr<Ipv4Route> rtentry = LookupGlobal(header.GetDestination(), oif, &header, p);
    if (rtentry)
    {
        sockerr = Socket::ERROR_NOTERROR;
    }
    else
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
    }
    return rtentry;
}

bool
Ipv4GlobalRouting::RouteInput(Ptr<const Packet> p,
                              const Ipv4Header& header,
                              Ptr<const NetDevice> idev,
                              const UnicastForwardCallback& ucb,
                              const MulticastForwardCallback& mcb,
                              const LocalDeliverCallback& lcb,
                              const ErrorCallback& ecb)
{
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    uint32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    if (m_ipv4->IsDestinationAddress(header.GetDestination(), iif))
    {
        if (!lcb.IsNull())
        {
            lcb(p, header, iif);
            return true;
        }
        return false;
    }

    if (!m_ipv4->IsForwarding(iif))
    {
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    Ptr<Ipv4Route> rtentry = LookupGlobal(header.GetDestination(), nullptr, &header, p);
    if (rtentry)
    {
        ucb(rtentry, p, header);
        return true;
    }
    return false;
}

void
Ipv4GlobalRouting::NotifyInterfaceUp(uint32_t i)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyInterfaceDown(uint32_t i)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(!m_ipv4 && ipv4);
    m_ipv4 = ipv4;
}

} // namespace ns3


