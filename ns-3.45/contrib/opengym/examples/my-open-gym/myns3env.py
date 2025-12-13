import numpy as np
import random
from ns3gym import ns3env
from gym import spaces

filepath =  "EcmpProbability.txt"
obs_length = 1680*3+3  # 观测空间的维度，根据实际情况调整
act_length =   # 动作空间的维度，根据实际情况调整

class MyNs3Env(ns3env.Ns3Env):
    def sample_simArgs(self):
        simArgs_sample = {}
        for k, v in self.simArgs_space.items():
            if isinstance(v, list):
                simArgs_sample[k] = random.choice(v)
            else:
                simArgs_sample[k] = v
        self.simArgs = simArgs_sample
        return simArgs_sample
    
    def __init__(self, simArgs_space, stepTime, *args, **kwargs):
        self.step_time = stepTime
        simArgs_space["--envTimeStep"] = stepTime
        self.simArgs_space = simArgs_space
        super().__init__(simArgs=self.sample_simArgs(), *args, **kwargs)

        # Redefine obs space
        self.observation_space = spaces.Box(0.0, 100000.0, (obs_length,), np.float64)
        print("Observation space:", self.observation_space)

        # Redefine action space
        self.action_space = spaces.Box(0, 1, (act_length,), np.uint64)
        # self.action_space = spaces.Discrete(1000, start=1)
        print("Action space:", self.action_space)


    def transform_obs(self, obs):
        # 吞吐量
        total_throughput = obs[0]   
        # 端到端延迟
        end_to_end_delay = obs[1]  
        # 丢包率
        packet_loss_ratio = obs[2]
        # PFC触发次数，统计
        pfc_triggers = obs[3]  
        # 链路负载计算方差
        link_load_var = np.var(obs[4:])
        
        # 乱序率，重排序out_of_order_ratio = obs[] 
        # 收发队列长度queue_length = obs[]
        
        new_obs = np.array([total_throughput,end_to_end_delay,packet_loss_ratio,link_load_var,pfc_triggers], dtype=np.float64)
        return new_obs


    def transform_action(self, action):
        # 将动作写入文件ecmpprobability.txt
        with open(filepath, 'w') as f:
            f.write('{}'.format(action))
            f.close()
        # 重启ECMP相关的ns3模块以应用新的ECMP概率
        
        # new_action = [np.uint64(action * self.segment_size), np.uint64(0)]
        return 0

    def get_reward(self, obs ,eta, alpha ,beta ,gamma ,sigma ,delta, epsilon):
        # 吞吐量
        total_throughput = obs[0]   
        # 端到端延迟
        end_to_end_delay = obs[1]  
        # 丢包率
        packet_loss_ratio = obs[2]
        # PFC触发次数，统计
        pfc_triggers = obs[3]  
        # 链路负载计算方差
        link_load_var = np.var(obs[4:])

        # 计算奖励函数
        reward = (eta * total_throughput - 
                  alpha * pfc_triggers - 
                  beta * link_load_var - 
                  gamma * end_to_end_delay - 
                  # delta * out_of_order_ratio - 
                  epsilon * packet_loss_ratio)  # 计算奖励  
        return reward

    def reset(self):
        self.sample_simArgs()
        obs = super().reset()
        return self.transform_obs(obs)
  
    def step(self, action):
        obs, reward, done, info = super().step(self.transform_action(action))
        return (self.transform_obs(obs), self.get_reward(obs), done, {'info': info, 'raw_obs': obs})
