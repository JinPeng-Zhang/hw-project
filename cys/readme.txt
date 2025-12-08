generic-topology这个库放在src目录下
拓扑文件记得用现在这个(之前的是所有链路带宽一样，现在把核心层-汇聚层，汇聚层-汇聚层带宽设为了汇聚层-边缘层的4倍)

sim-stats-collector库最后会返回一个results：
通过  
	double myTput = results.global.throughputMbps;
  	double myDelay = results.global.avgDelayMs;
  	double myLoss = results.global.lossRatePct;
可以获取三个全局指标
通过
          LinkTimeSeries myLinkData = results.links[idx];
          std::vector<double> arrQueueA = myLinkData.queueSnapshotsA;
          std::vector<double> arrQueueB = myLinkData.queueSnapshotsB;
          std::vector<double> arrUtil   = myLinkData.utilSnapshots;
可以获取每条链路的实时队列以及链路利用率
详细看我那个module.cc的示例。

延迟，带宽的设置：
见topo-traffic-builder.cc里的：
        double delay_ms = (w * 2.0) / 100.0;
        double bandwidth_mbps = 50.0 / w; 
这个w是auto.txt文件里面的第三列，现在50.0 / w的话等于汇聚层-边缘层带宽0.5Mbps，核心层-汇聚层和汇聚层-汇聚层带宽2Mbps，要设多大带宽改这个50就行