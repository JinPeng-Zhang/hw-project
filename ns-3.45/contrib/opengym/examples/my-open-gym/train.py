import torch
import numpy as np
import os
from typing import Callable

from stable_baselines3 import A2C, PPO, TD3
from stable_baselines3.common.env_util import make_vec_env
from stable_baselines3.common.evaluation import evaluate_policy
from stable_baselines3.common.logger import configure
from stable_baselines3.common.callbacks import CallbackList, BaseCallback
from stable_baselines3.common.results_plotter import load_results, ts2xy, plot_results
from stable_baselines3.common.vec_env import VecMonitor

from tqdm import tqdm
from ns3gym import ns3env
from gym import spaces
import myns3env
import mymodel
from datetime import datetime


current_time = datetime.now()
formatted_time = current_time.strftime('%Y-%m-%d-%H-%M-%S')
log_dir = f"./sb3_log_{formatted_time}/"

class SaveOnBestTrainingRewardCallback(BaseCallback):
    def __init__(self, history_len: int, check_freq: int, log_dir: str, verbose: int = 1):
        super().__init__(verbose)
        self.check_freq = check_freq
        self.log_dir = log_dir
        self.best_save_path = os.path.join(log_dir, "best_model")
        self.latest_save_path = os.path.join(log_dir, "latest_model")
        self.best_mean_reward = -np.inf
        self.history_len = history_len

    def _init_callback(self) -> None:
        # Create folder if needed
        if self.best_save_path is not None:
            os.makedirs(self.best_save_path, exist_ok=True)
        if self.latest_save_path is not None:
            os.makedirs(self.latest_save_path, exist_ok=True)

    def _on_step(self) -> bool:
        if self.n_calls % self.check_freq == 0:
            # Save latest model
            if self.verbose >= 1:
                print(f"Saving latest model to {self.latest_save_path}")
            self.model.save(self.latest_save_path)
          
            # Retrieve training reward
            x, y = ts2xy(load_results(self.log_dir), "timesteps")
            if len(x) > 0:
                # Mean training reward over the last 100 episodes
                mean_reward = np.mean(y[-self.history_len:])
                if self.verbose >= 1:
                    print(f"Num timesteps: {self.num_timesteps}")
                    print(f"Best mean reward: {self.best_mean_reward:.2f} - Last mean reward per episode: {mean_reward:.2f}")
  
                # New best model, you could save the agent here
                if mean_reward > self.best_mean_reward:
                    self.best_mean_reward = mean_reward
                    # Example for saving best model
                    if self.verbose >= 1:
                        print(f"Saving new best model to {self.best_save_path}")
                    self.model.save(self.best_save_path)

        return True


def step_schedule(initial_value: float, n_steps: int) -> Callable[[float], float]:
    def func(progress_remaining: float) -> float:
        lr = initial_value
        for i in range(n_steps):
            if progress_remaining < (i / n_steps):
                lr -= initial_value / n_steps
        return lr

    return func

startSim = True
port = 0
simTime = 60 # seconds
stepTime = 0.5  # seconds
seed = 12
transport_prot = "MyRl"
data_to_transmit = 1000000
error_p = 0.0
mtu = 1500
debug = True
filepath =  "EcmpProbability.txt"

simArgs = {
    "--duration": simTime,
    "--transport_prot": transport_prot,
    "--mtu": mtu,
    "--rl_env": "MyRlTimeBased",
    "--envTimeStep": stepTime,
    "--error_p": error_p,
}

env_kwargs = {
    'port': port,
    'stepTime': stepTime,
    'startSim': startSim,
    'simSeed': seed,
    'simArgs_space': simArgs,
    'debug': debug
}

# # 创建环境
# env = CustomEnv(data)
# env = Monitor(env, file#nme="monitor_log") 
# env = make_vec_env(lambda: env, n_envs=4)

# # 训练模型
# model.learn(total_timesteps=timestp)  # 总时间步长可以根据需要调整
# # 评估模型
# mean_reward, std_reward = evaluate_policy(model, env, n_eval_episodes=10)
# print("Mean reward:", mean_reward, "Std of reward:", std_reward)
# # 输出最终策略
# obs = env.reset()
# while True:
#     action, _states = model.predict(obs)
#     print("Action:", action)
#     obs, rewards, dones, info = env.step(action)
#     if dones:
#         break
# # 保存模型
# model.save("a2c_custom")


monitored_vec_env = VecMonitor(make_vec_env(myns3env.MyNs3Env, n_envs=3, seed=12, env_kwargs=env_kwargs),
                               filename=f'{log_dir}/monitor.csv')

model = mymodel.A2Cmodel(monitored_vec_env, step_schedule(3e-4, 10))
model.set_logger(configure(log_dir, ["stdout", "csv", "tensorboard"]))

save_callback = SaveOnBestTrainingRewardCallback(history_len=1000,
                                                 check_freq=1000,
                                                 log_dir=log_dir)

model.learn(total_timesteps=50000,
            callback=CallbackList([save_callback]),
            progress_bar=True)

model.save(f"{log_dir}/lastrun_{formatted_time}/")

monitored_vec_env.close()