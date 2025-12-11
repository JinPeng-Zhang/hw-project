import torch
from stable_baselines3 import A2C, PPO, TD3

# 强化学习对应库
import numpy as np
import gym
import matplotlib.pyplot as plt
import time
from typing import Callable
from stable_baselines3 import A2C
from stable_baselines3.common.env_util import make_vec_env
from stable_baselines3.common.evaluation import evaluate_policy
from stable_baselines3.common.monitor import Monitor
import pandas as pd
from torch.cuda.amp import autocast, GradScaler
import warnings 
warnings.filterwarnings("ignore")
import os
from stable_baselines3.common.logger import configure
from stable_baselines3.common.callbacks import CallbackList, BaseCallback
from stable_baselines3.common.results_plotter import load_results, ts2xy, plot_results
from stable_baselines3.common.vec_env import VecMonitor


# 超参数
eta, alpha, beta, gamma, delta, epsilon=1,1,1,1,1,1
timestp=10000
lr=0.001
n_steps=128
ent_coe0f=0.01
vf_coef=0.25
policy_kwargs = dict(activation_fn=torch.nn.GELU, net_arch=[128,128])
device =  'cuda'

# scaler = GradScaler()
# with autocast():
#    # 在此处执行需要自动混合精度的计算
#    ...
#    # 反向传播和参数更新
#    scaler.scale(loss).backward()
#    scaler.step(optimizer)
#    scaler.update()


def A2Cmodel(env,lr,batch_size,device):
    policy_kwargs = dict(activation_fn=torch.nn.GELU, 
                         net_arch=[128,128])
    model = A2C('MlpPolicy', 
                env, 
                verbose=1,
                learning_rate=lr,
                # tensorboard_log="./a2c_custom",
                # gamma=gamma,
                # nsteps=n_steps,
                # en_coef=ent_coef,
               # vf_coef=vf_coef,
                # policy_kwargs=policy_kwargs,
                device=device
                )
    return model
# # 训练优化
# model.to(device)
# env = env.to(device)