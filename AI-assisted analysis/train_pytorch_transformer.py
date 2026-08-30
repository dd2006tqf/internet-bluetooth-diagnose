#!/usr/bin/env python3

# 模块职责：本文件实现文件名所对应的日志、数据处理或网络诊断功能。
# 维护提示：扩展公共函数或命令行入口时，应说明输入、输出、异常、外部依赖和降级路径，
# 并保持既有 CLI/API 行为不变。

"""
Temporal Transformer 异常检测模型（PyTorch 实现）
使用真正的深度学习框架，自动处理类别不平衡
"""

import numpy as np
import pandas as pd
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
import os
import sys
import json

# ============================================================
# PyTorch 模型定义
# ============================================================

import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset, WeightedRandomSampler

# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class TemporalTransformer(nn.Module):
    """时序 Transformer 异常检测模型"""

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def __init__(self, feature_dim=5, seq_len=6, d_model=16, n_heads=2, n_layers=2, dropout=0.1):
        super().__init__()
        self.feature_dim = feature_dim
        self.seq_len = seq_len
        self.d_model = d_model

        # 输入投影
        self.input_proj = nn.Linear(feature_dim, d_model)

        # 位置编码
        self.pos_encoding = nn.Parameter(torch.randn(1, seq_len, d_model) * 0.1)

        # Transformer 编码器
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=n_heads,
            dim_feedforward=d_model * 4,
            dropout=dropout,
            batch_first=True
        )
        self.transformer = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)

        # 输出投影（输出 2 个类别：正常/异常）
        self.output_proj = nn.Linear(d_model, 2)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def forward(self, x):
        """
        x: (batch, seq_len, feature_dim)
        returns: (batch, 2) 分类 logits
        """
        # 输入投影
        h = self.input_proj(x)  # (batch, seq_len, d_model)

        # 加入位置编码
        h = h + self.pos_encoding

        # Transformer 编码
        h = self.transformer(h)  # (batch, seq_len, d_model)

        # 使用最后一个时间点
        h_last = h[:, -1, :]  # (batch, d_model)

        # 输出投影（2 个类别）
        output = self.output_proj(h_last)  # (batch, 2)

        return output


# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class AnomalyDetector:
    """异常检测器"""

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def __init__(self, feature_dim=5, seq_len=6, d_model=16, n_heads=2, n_layers=2):
        self.device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
        self.model = TemporalTransformer(
            feature_dim=feature_dim,
            seq_len=seq_len,
            d_model=d_model,
            n_heads=n_heads,
            n_layers=n_layers
        ).to(self.device)
        self.scaler = StandardScaler()
        self.threshold = None

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def prepare_data(self, X, labels):
        """准备训练数据"""
        # 归一化
        X_flat = X.reshape(X.shape[0], -1)
        X_norm = self.scaler.fit_transform(X_flat)
        X_norm = X_norm.reshape(X.shape)

        # 划分数据集
        X_train, X_test, y_train, y_test = train_test_split(
            X_norm, labels, test_size=0.2, random_state=42
        )

        return X_train, X_test, y_train, y_test

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def create_balanced_sampler(self, labels):
        """创建平衡采样器，处理类别不平衡"""
        class_counts = np.bincount(labels)
        class_weights = 1.0 / class_counts
        sample_weights = class_weights[labels]
        sampler = WeightedRandomSampler(
            weights=sample_weights,
            num_samples=len(labels),
            replacement=True
        )
        return sampler

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def train(self, X_train, y_train, epochs=50, batch_size=64, lr=0.001):
        """训练模型"""
        # 转换为 tensor
        X_tensor = torch.FloatTensor(X_train).to(self.device)
        y_tensor = torch.LongTensor(y_train).to(self.device)

        # 创建平衡采样器
        sampler = self.create_balanced_sampler(y_train)
        dataset = TensorDataset(X_tensor, y_tensor)
        dataloader = DataLoader(dataset, batch_size=batch_size, sampler=sampler)

        # 优化器
        optimizer = optim.Adam(self.model.parameters(), lr=lr)

        # 损失函数（带权重的交叉熵，处理类别不平衡）
        class_weights = torch.FloatTensor([1.0, 5.0]).to(self.device)  # 异常样本权重更高
        criterion = nn.CrossEntropyLoss(weight=class_weights)

        # 训练循环
        self.model.train()
        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for epoch in range(epochs):
            total_loss = 0
            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for batch_x, batch_y in dataloader:
                optimizer.zero_grad()
                output = self.model(batch_x)
                loss = criterion(output, batch_y)
                loss.backward()
                optimizer.step()
                total_loss += loss.item()

            # 条件判断：根据当前输入或运行状态选择处理分支。
            if (epoch + 1) % 10 == 0:
                avg_loss = total_loss / len(dataloader)
                print(f"   Epoch [{epoch+1}/{epochs}] Loss: {avg_loss:.6f}")

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def fit_threshold(self, X_normal, percentile=95):
        """拟合异常阈值"""
        self.model.eval()
        X_tensor = torch.FloatTensor(X_normal).to(self.device)

        # 资源上下文：利用上下文管理器保证文件、锁或连接按作用域释放。
        with torch.no_grad():
            output = self.model(X_tensor)
            # 使用 softmax 获取异常概率
            probs = torch.softmax(output, dim=1)
            anomaly_probs = probs[:, 1].cpu().numpy()

        self.threshold = np.percentile(anomaly_probs, percentile)
        print(f"异常阈值 ({percentile}th percentile): {self.threshold:.6f}")
        return self.threshold

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def predict(self, X):
        """预测异常"""
        self.model.eval()
        X_tensor = torch.FloatTensor(X).to(self.device)

        # 资源上下文：利用上下文管理器保证文件、锁或连接按作用域释放。
        with torch.no_grad():
            output = self.model(X_tensor)
            # 使用 softmax 获取概率
            probs = torch.softmax(output, dim=1)
            # 异常概率 = 第 1 类的概率
            anomaly_probs = probs[:, 1].cpu().numpy()

        predictions = (anomaly_probs > self.threshold).astype(int)
        return predictions, anomaly_probs

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def evaluate(self, X_test, y_test):
        """评估模型"""
        predictions, scores = self.predict(X_test)

        tp = np.sum((predictions == 1) & (y_test == 1))
        fp = np.sum((predictions == 1) & (y_test == 0))
        tn = np.sum((predictions == 0) & (y_test == 0))
        fn = np.sum((predictions == 0) & (y_test == 1))

        accuracy = (tp + tn) / len(y_test)
        precision = tp / (tp + fp) if (tp + fp) > 0 else 0
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

        print(f"   准确率: {accuracy:.4f}")
        print(f"   精确率: {precision:.4f}")
        print(f"   召回率: {recall:.4f}")
        print(f"   F1 分数: {f1:.4f}")

        return {
            'accuracy': accuracy,
            'precision': precision,
            'recall': recall,
            'f1': f1
        }

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def save(self, path):
        """保存模型"""
        checkpoint = {
            'model_state_dict': self.model.state_dict(),
            'threshold': self.threshold,
            'scaler_mean': getattr(self.scaler, 'mean_', None),
            'scaler_scale': getattr(self.scaler, 'scale_', None)
        }
        torch.save(checkpoint, path)
        print(f"模型已保存到: {path}")

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def load(self, path):
        """加载模型"""
        checkpoint = torch.load(path, map_location=self.device)
        self.model.load_state_dict(checkpoint['model_state_dict'])
        self.threshold = checkpoint['threshold']
        self.scaler.mean_ = checkpoint['scaler_mean']
        self.scaler.scale_ = checkpoint['scaler_scale']
        print(f"模型已加载: {path}")


# ============================================================
# 训练流程
# ============================================================

# 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
def train_pytorch_transformer():
    """训练 PyTorch Transformer 异常检测模型"""
    print("=" * 60)
    print("PyTorch Temporal Transformer 异常检测模型训练")
    print("=" * 60)

    # 1. 加载数据
    print("\n1. 加载数据...")
    csv_path = '../data/real/network_data.csv'
    df = pd.read_csv(csv_path)
    print(f"   数据量: {len(df)} 条")
    print(f"   时间范围: {df['timestamp'].min()} ~ {df['timestamp'].max()}")

    # 2. 提取特征
    print("\n2. 提取特征...")
    feature_cols = ['rtt', 'jitter', 'rssi', 'tcp_loss', 'score']
    X = df[feature_cols].values
    X = np.nan_to_num(X, nan=0.0)
    X = np.clip(X, -1000, 10000)
    print(f"   特征形状: {X.shape}")

    # 3. 创建滑动窗口
    print("\n3. 创建滑动窗口...")
    window_size = 6
    features = []
    # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
    for i in range(len(X) - window_size + 1):
        window = X[i:i+window_size]
        features.append(window)
    X_windowed = np.array(features)
    print(f"   窗口特征: {X_windowed.shape}")

    # 4. 使用 quality 字段标注异常
    print("\n4. 使用 quality 字段标注异常...")
    quality_map = {'GOOD': 0, 'FAIR': 0, 'POOR': 1, 'BAD': 1}
    quality_values = df['quality'].values
    labels = quality_values[window_size-1:]
    labels = np.array([quality_map.get(str(q), 0) for q in labels])
    print(f"   异常样本: {np.sum(labels == 1)} / {len(labels)}")
    print(f"   正常样本: {np.sum(labels == 0)} / {len(labels)}")

    # 5. 初始化检测器
    print("\n5. 初始化检测器...")
    detector = AnomalyDetector(
        feature_dim=5,
        seq_len=6,
        d_model=16,
        n_heads=2,
        n_layers=2
    )

    # 6. 准备数据
    print("\n6. 准备数据...")
    # 三阶段划分：训练集(60%) + 验证集(20%) + 测试集(20%)
    X_train, X_temp, y_train, y_temp = train_test_split(X_windowed, labels, test_size=0.4, random_state=42)
    X_val, X_test, y_val, y_test = train_test_split(X_temp, y_temp, test_size=0.5, random_state=42)
    print(f"   训练集: {X_train.shape} ({len(X_train)} 条)")
    print(f"   验证集: {X_val.shape} ({len(X_val)} 条)")
    print(f"   测试集: {X_test.shape} ({len(X_test)} 条)")

    # 7. 训练模型
    print("\n7. 训练模型...")
    detector.train(X_train, y_train, epochs=10, batch_size=128, lr=0.001)

    # 8. 在验证集上寻找最佳阈值
    print("\n8. 在验证集上寻找最佳阈值...")
    best_threshold = 0.5
    best_f1 = 0
    best_metrics = {}

    # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
    for threshold in np.arange(0.01, 0.99, 0.01):
        detector.threshold = threshold
        predictions, _ = detector.predict(X_val)

        tp = np.sum((predictions == 1) & (y_val == 1))
        fp = np.sum((predictions == 1) & (y_val == 0))
        tn = np.sum((predictions == 0) & (y_val == 0))
        fn = np.sum((predictions == 0) & (y_val == 1))

        precision = tp / (tp + fp) if (tp + fp) > 0 else 0
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

        # 条件判断：根据当前输入或运行状态选择处理分支。
        if f1 > best_f1:
            best_f1 = f1
            best_threshold = threshold
            best_metrics = {
                'precision': precision,
                'recall': recall,
                'f1': f1
            }

    detector.threshold = best_threshold
    print(f"   最佳阈值: {best_threshold:.2f}")
    print(f"   验证集精确率: {best_metrics['precision']:.4f}")
    print(f"   验证集召回率: {best_metrics['recall']:.4f}")
    print(f"   验证集 F1: {best_metrics['f1']:.4f}")

    # 9. 在独立测试集上评估（最终结果）
    print("\n9. 在独立测试集上评估（最终结果）...")
    metrics = detector.evaluate(X_test, y_test)

    # 10. 保存模型
    print("\n10. 保存模型...")
    output_dir = os.path.join(os.path.dirname(__file__), '..', 'models', 'pytorch_transformer')
    os.makedirs(output_dir, exist_ok=True)
    detector.save(f'{output_dir}/transformer_model.pth')

    # 保存 scaler
    import joblib
    joblib.dump(detector.scaler, f'{output_dir}/scaler.pkl')

    print(f"\n模型已保存到: {output_dir}")
    print(f"   - transformer_model.pth (PyTorch 模型)")
    print(f"   - scaler.pkl (归一化参数)")

    # 11. 对比
    print("\n11. 模型对比...")
    print("   AutoEncoder (NumPy): 准确率 91.6%, 精确率 38.8%, 召回率 42.2%, F1 0.40")
    print(f"   Transformer (PyTorch): 准确率 {metrics['accuracy']:.1%}, 精确率 {metrics['precision']:.1%}, 召回率 {metrics['recall']:.1%}, F1 {metrics['f1']:.4f}")

    return metrics


# 条件判断：根据当前输入或运行状态选择处理分支。
if __name__ == '__main__':
    metrics = train_pytorch_transformer()
    print("\n" + "=" * 60)
    print("训练完成!")
    print("=" * 60)
