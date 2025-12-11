import numpy as np
import random
from ns3gym import ns3env
from gym import spaces


filepath =  "EcmpProbability.txt"
length = 5  # 动作空间的维度，根据实际情况调整

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
        self.observation_space = spaces.Box(0.0, 100000.0, (4,), np.float64)
        print("Observation space:", self.observation_space)

        # Redefine action space
        self.action_space = spaces.Box(0, 1, (length,), np.uint64)
        # self.action_space = spaces.Discrete(1000, start=1)
        print("Action space:", self.action_space)


    def transform_obs(self, obs):
        link_load_var = obs[0]  # 链路负载的平均值
        pfc_triggers = obs[1]      # PFC触发次数，统计
        end_to_end_delay = obs[2]  # 端到端延迟
        ecn_mark_ratio = obs[3]    # ECN标记比例
        # out_of_order_ratio = obs[4] # 乱序率，重排序
        packet_loss_ratio = obs[5]  # 丢包率
        total_throughput = obs[6]   # 吞吐量
        new_obs = np.array([link_load_var, pfc_triggers, end_to_end_delay, ecn_mark_ratio, out_of_order_ratio, packet_loss_ratio, total_throughput])
        return new_obs


    def transform_action(self, action):
        # 读取文件中的动作值
        with open(filepath, 'r') as f:
            actions = f.readlines()
        new_action = [np.uint64(action * self.segment_size), np.uint64(0)]
        return new_action

    def get_reward(self, obs ,eta, alpha ,beta ,gamma ,sigma ,delta, epsilon):
        link_load_var = obs[0]  # 链路负载的平均值
        pfc_triggers = obs[1]      # PFC触发次数，统计
        end_to_end_delay = obs[2]  # 端到端延迟
        ecn_mark_ratio = obs[3]    # ECN标记比例
        # out_of_order_ratio = obs[4] # 乱序率，重排序
        packet_loss_ratio = obs[5]  # 丢包率
        total_throughput = obs[6]   # 吞吐量
        # 计算奖励函数
        reward = (eta * total_throughput - 
                  alpha * pfc_triggers - 
                  beta * link_load_var - 
                  gamma * end_to_end_delay - 
                  delta * out_of_order_ratio - 
                  epsilon * packet_loss_ratio)  # 计算奖励  
        return reward

    def reset(self):
        self.sample_simArgs()
        obs = super().reset()
        return self.transform_obs(obs)
  
    def step(self, action):
        obs, reward, done, info = super().step(self.transform_action(action))
        return (self.transform_obs(obs), self.get_reward(obs), done, {'info': info, 'raw_obs': obs})
