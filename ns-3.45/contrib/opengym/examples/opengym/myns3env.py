import numpy as np
import random
from ns3gym import ns3env
from gym import spaces

filepath =  "./ecmpProbability.txt"
obs_length =  5 # 观测空间的维度，根据实际情况调整
act_length = 464  # 动作空间的维度，根据实际情况调整

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
        self.action_space = spaces.Box(0, 1, (act_length), np.uint64)
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
        link_load_var = obs[4]
        
        # 乱序率，重排序out_of_order_ratio = obs[] 
        # 收发队列长度queue_length = obs[]
        
        # new_obs = np.array([total_throughput,end_to_end_delay,packet_loss_ratio,link_load_var,pfc_triggers], dtype=np.float64)
        new_obs = obs
        return new_obs


    # def transform_action(self, action):
    #     # 读取ecmp概率文件，将“node: w1 w2 w3 ...”格式的内容转换为动作格式,即一个416*4的二维数组
    #     with open(filepath, 'r') as file:
    #         lines = file.readlines()
    #     action = []
    #     for line in lines:
    #         node, weights = line.strip().split(':')
    #         weights = [int(weight) for weight in weights.split()]
    #         action.append(weights)
    #     action = np.array(action).flatten()
    #     return action

    def transform_action(self, action):
        # 将actio写入ecmp概率文件，将“node: w1 w2 w3 ...”格式的内容转换为动作格式,即一个416*4的二维数组
        with open(filepath, 'w') as file:
            for i in range(act_length):
                weights = action[i*4:(i+1)*4]
                weights_str = ' '.join(map(str, weights))
                file.write(f'{i+1}: {weights_str}\n')
        new_action = action
        return new_action

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
        link_load_var = obs[4]
        # 定义所有参数的初始值为1
        eta, alpha ,beta ,gamma ,sigma ,delta, epsilon = 1, 1, 1, 1, 1, 1, 1
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