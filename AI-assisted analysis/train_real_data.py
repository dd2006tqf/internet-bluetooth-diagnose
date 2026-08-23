#!/usr/bin/env python3
"""
真实数据训练脚本
从 CSV 加载网络指标，训练 AutoEncoder 异常检测模型
"""

import numpy as np
import pandas as pd
import json
import os
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
import sys

sys.path.insert(0, os.path.dirname(__file__))
from feature_extractor import ConfigurableFeatureExtractor
from train_autoencoder import AutoEncoder, AutoEncoderTrainer

class RealDataTrainer:
    """真实数据训练器"""

    def __init__(self, config_path=None):
        if config_path is None:
            config_path = os.path.join(os.path.dirname(__file__), 'config', 'features.json')
        self.extractor = ConfigurableFeatureExtractor(config_path)
        self.scaler = StandardScaler()

    def load_csv(self, csv_path):
        """加载 CSV 数据"""
        df = pd.read_csv(csv_path)
        print(f"加载数据: {len(df)} 条")
        print(f"时间范围: {df['timestamp'].min()} ~ {df['timestamp'].max()}")
        return df

    def prepare_features(self, df):
        """准备特征矩阵"""
        feature_cols = ['rtt', 'jitter', 'rssi', 'tcp_loss', 'score']
        X = df[feature_cols].values

        # 处理异常值
        X = np.nan_to_num(X, nan=0.0)
        X = np.clip(X, -1000, 10000)

        # 创建滑动窗口
        window_size = self.extractor.get_window_size()
        features = []
        for i in range(len(X) - window_size + 1):
            window = X[i:i+window_size]
            features.append(window)

        X_windowed = np.array(features)
        print(f"特征矩阵: {X_windowed.shape}")
        return X_windowed

    def detect_anomalies(self, X):
        """基于统计方法检测异常"""
        labels = np.zeros(len(X), dtype=np.int32)

        for i in range(len(X)):
            for t in range(X.shape[1]):
                for d in range(X.shape[2]):
                    mean = np.mean(X[:, t, d])
                    std = np.std(X[:, t, d])
                    if std > 0 and abs(X[i, t, d] - mean) > 3 * std:
                        labels[i] = 1
                        break
                if labels[i] == 1:
                    break

        print(f"异常样本: {np.sum(labels == 1)} / {len(labels)}")
        return labels

    def train(self, csv_path):
        """完整训练流程"""
        # 1. 加载数据
        df = self.load_csv(csv_path)

        # 2. 准备特征
        X = self.prepare_features(df)

        # 3. 检测异常标签
        labels = self.detect_anomalies(X)

        # 4. 归一化
        X_flat = X.reshape(X.shape[0], -1)
        X_norm = self.scaler.fit_transform(X_flat)
        X_norm = X_norm.reshape(X.shape)

        # 5. 划分数据集
        X_train, X_test, y_train, y_test = train_test_split(
            X_norm, labels, test_size=0.2, random_state=42
        )

        # 6. 训练 AutoEncoder（只用正常数据）
        X_train_normal = X_train[y_train == 0]

        print(f"\n训练集: {X_train_normal.shape} (仅正常数据)")
        print(f"测试集: {X_test.shape}")

        trainer = AutoEncoderTrainer(config_path=None)
        trainer.model = AutoEncoder(input_size=self.extractor.get_input_size())

        print("\n开始训练...")
        history = trainer.train(X_train_normal, X_train_normal, epochs=100, batch_size=32)

        # 7. 评估
        print("\n评估模型...")
        metrics = trainer.evaluate(X_test, y_test)

        # 8. 保存
        output_dir = os.path.join(os.path.dirname(__file__), '..', 'models', 'real_data')
        os.makedirs(output_dir, exist_ok=True)
        trainer.export_onnx(f'{output_dir}/network_autoencoder.onnx')
        trainer.save(f'{output_dir}/network_autoencoder.npz')

        # 保存 scaler
        import joblib
        joblib.dump(self.scaler, f'{output_dir}/scaler.pkl')

        print(f"\n模型和评估结果已保存到 {output_dir}")
        return metrics


if __name__ == '__main__':
    csv_path = sys.argv[1] if len(sys.argv) > 1 else '../data/real/network_data.csv'
    trainer = RealDataTrainer()
    metrics = trainer.train(csv_path)
