#!/usr/bin/env python3

# 模块职责：本文件实现文件名所对应的日志、数据处理或网络诊断功能。
# 维护提示：扩展公共函数或命令行入口时，应说明输入、输出、异常、外部依赖和降级路径，
# 并保持既有 CLI/API 行为不变。

"""
网络指标特征提取器（配置驱动版）
从 JSON 配置文件读取特征定义，支持动态扩展指标
用于 NPU AutoEncoder 异常检测模型训练
"""

import numpy as np
import json
import os
from typing import Tuple, List, Dict, Optional
from dataclasses import dataclass

@dataclass
# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class FeatureConfig:
    """单个特征的配置"""
    name: str
    source: str
    column: str
    unit: str
    normal_range: Tuple[float, float]
    anomaly_range: Optional[Tuple[float, float]] = None

# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class ConfigurableFeatureExtractor:
    """可配置的网络指标特征提取器"""

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def __init__(self, config_path: str = None):
        """
        初始化特征提取器

        Args:
            config_path: 配置文件路径，默认使用 config/features.json
        """
        # 条件判断：根据当前输入或运行状态选择处理分支。
        if config_path is None:
            config_path = os.path.join(os.path.dirname(__file__), 'config', 'features.json')

        # 资源上下文：利用上下文管理器保证文件、锁或连接按作用域释放。
        with open(config_path, 'r') as f:
            self.config = json.load(f)

        self.window_size = self.config['window_size']
        self.features = []
        self.feature_names = []

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for feat in self.config['features']:
            self.features.append(FeatureConfig(
                name=feat['name'],
                source=feat['source'],
                column=feat['column'],
                unit=feat['unit'],
                normal_range=(feat['normal']['min'], feat['normal']['max']),
                anomaly_range=(
                    feat.get('anomaly', {}).get('min'),
                    feat.get('anomaly', {}).get('max')
                ) if 'anomaly' in feat else None
            ))
            self.feature_names.append(feat['name'])

        self.feature_dim = len(self.features)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def get_feature_dim(self) -> int:
        """获取特征维度"""
        return self.feature_dim

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def get_window_size(self) -> int:
        """获取窗口大小"""
        return self.window_size

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def get_input_size(self) -> int:
        """获取模型输入大小"""
        return self.window_size * self.feature_dim

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def generate_synthetic_data(self, n_samples: int = 10000,
                                anomaly_ratio: float = 0.1) -> Tuple[np.ndarray, np.ndarray]:
        """
        生成合成训练数据

        Args:
            n_samples: 样本数量
            anomaly_ratio: 异常数据比例

        Returns:
            X: 特征数据 (n_samples, window_size, feature_dim)
            labels: 标签 (n_samples,) - 0=正常, 1=异常
        """
        n_normal = int(n_samples * (1 - anomaly_ratio))
        n_anomaly = n_samples - n_normal

        # 生成正常数据
        X_normal = self._generate_normal_data(n_normal)
        labels_normal = np.zeros(n_normal, dtype=np.int32)

        # 生成异常数据
        X_anomaly = self._generate_anomaly_data(n_anomaly)
        labels_anomaly = np.ones(n_anomaly, dtype=np.int32)

        # 合并并打乱
        X = np.concatenate([X_normal, X_anomaly], axis=0)
        labels = np.concatenate([labels_normal, labels_anomaly], axis=0)

        indices = np.random.permutation(len(X))
        X = X[indices]
        labels = labels[indices]

        return X, labels

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def _generate_normal_data(self, n_samples: int) -> np.ndarray:
        """生成正常网络指标数据"""
        data = np.zeros((n_samples, self.window_size, self.feature_dim))

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i in range(n_samples):
            # 为每个特征生成基础值
            base_values = []
            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for feat in self.features:
                base_values.append(np.random.uniform(*feat.normal_range))

            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for t in range(self.window_size):
                # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
                for d, feat in enumerate(self.features):
                    # 添加噪声
                    base = base_values[d]
                    noise = np.random.normal(0, (feat.normal_range[1] - feat.normal_range[0]) * 0.1)
                    data[i, t, d] = base + noise

        return data

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def _generate_anomaly_data(self, n_samples: int) -> np.ndarray:
        """生成异常网络指标数据"""
        data = np.zeros((n_samples, self.window_size, self.feature_dim))

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i in range(n_samples):
            # 生成正常基础值
            base_values = []
            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for feat in self.features:
                base_values.append(np.random.uniform(*feat.normal_range))

            # 随机选择异常特征
            anomaly_feature_idx = np.random.randint(0, self.feature_dim)
            anomaly_point = np.random.randint(2, self.window_size)

            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for t in range(self.window_size):
                # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
                for d, feat in enumerate(self.features):
                    base = base_values[d]
                    noise = np.random.normal(0, (feat.normal_range[1] - feat.normal_range[0]) * 0.1)

                    # 条件判断：根据当前输入或运行状态选择处理分支。
                    if t >= anomaly_point and d == anomaly_feature_idx and feat.anomaly_range:
                        # 注入异常值
                        data[i, t, d] = np.random.uniform(*feat.anomaly_range)
                    else:
                        data[i, t, d] = base + noise

        return data

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def normalize(self, X: np.ndarray) -> Tuple[np.ndarray, Dict]:
        """
        归一化特征数据

        Args:
            X: 原始特征数据

        Returns:
            X_norm: 归一化后的数据
            stats: 归一化参数
        """
        stats = {}
        X_norm = X.copy()

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for d in range(self.feature_dim):
            channel_data = X[:, :, d]
            mean = np.mean(channel_data)
            std = np.std(channel_data)
            X_norm[:, :, d] = (channel_data - mean) / (std + 1e-8)
            stats[self.feature_names[d]] = {'mean': float(mean), 'std': float(std)}

        return X_norm, stats

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def save_synthetic_data(self, X: np.ndarray, labels: np.ndarray,
                           stats: Dict, output_dir: str):
        """保存合成数据"""
        os.makedirs(output_dir, exist_ok=True)

        np.save(f'{output_dir}/features.npy', X)
        np.save(f'{output_dir}/labels.npy', labels)
        # 资源上下文：利用上下文管理器保证文件、锁或连接按作用域释放。
        with open(f'{output_dir}/normalization_stats.json', 'w') as f:
            json.dump(stats, f, indent=2)

        print(f"数据已保存到 {output_dir}")
        print(f"  特征维度: {self.feature_dim}")
        print(f"  窗口大小: {self.window_size}")
        print(f"  输入大小: {self.get_input_size()}")
        print(f"  features.npy: {X.shape}")
        print(f"  正常样本: {np.sum(labels == 0)}")
        print(f"  异常样本: {np.sum(labels == 1)}")


# 条件判断：根据当前输入或运行状态选择处理分支。
if __name__ == '__main__':
    # 使用配置生成合成数据
    extractor = ConfigurableFeatureExtractor()
    X, labels = extractor.generate_synthetic_data(n_samples=10000)
    X_norm, stats = extractor.normalize(X)

    print(f"特征维度: {extractor.get_feature_dim()}")
    print(f"窗口大小: {extractor.get_window_size()}")
    print(f"输入大小: {extractor.get_input_size()}")
    print(f"特征名称: {extractor.feature_names}")

    # 保存数据
    extractor.save_synthetic_data(X_norm, labels, stats, '../data/synthetic')
