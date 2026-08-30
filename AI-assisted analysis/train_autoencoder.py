#!/usr/bin/env python3

# 模块职责：本文件实现文件名所对应的日志、数据处理或网络诊断功能。
# 维护提示：扩展公共函数或命令行入口时，应说明输入、输出、异常、外部依赖和降级路径，
# 并保持既有 CLI/API 行为不变。

"""
AutoEncoder 异常检测模型训练（可扩展版）
支持动态特征维度，自动适配输入大小
"""

import numpy as np
import json
import os
from typing import Tuple, Dict
from feature_extractor import ConfigurableFeatureExtractor

# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class AutoEncoder:
    """
    可扩展的 AutoEncoder 异常检测模型
    输入维度由特征配置自动决定
    """

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def __init__(self, input_size: int, hidden_sizes: list = None):
        # 条件判断：根据当前输入或运行状态选择处理分支。
        if hidden_sizes is None:
            # 自动计算隐藏层大小
            hidden_sizes = [input_size // 2, input_size // 4, input_size // 8]

        self.input_size = input_size
        self.hidden_sizes = hidden_sizes

        # 初始化权重
        self.weights = []
        self.biases = []

        sizes = [input_size] + hidden_sizes + [input_size]
        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i in range(len(sizes) - 1):
            scale = np.sqrt(2.0 / sizes[i])
            self.weights.append(np.random.randn(sizes[i], sizes[i+1]) * scale)
            self.biases.append(np.zeros(sizes[i+1]))

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def relu(self, x):
        return np.maximum(0, x)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def relu_derivative(self, x):
        return (x > 0).astype(float)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def sigmoid(self, x):
        x = np.clip(x, -500, 500)
        return 1 / (1 + np.exp(-x))

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def forward(self, X):
        """前向传播"""
        self.activations = [X]
        self.z_values = []

        current = X
        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i, (w, b) in enumerate(zip(self.weights, self.biases)):
            z = current @ w + b
            self.z_values.append(z)

            # 条件判断：根据当前输入或运行状态选择处理分支。
            if i < len(self.weights) - 1:
                current = self.relu(z)
            else:
                current = self.sigmoid(z)

            self.activations.append(current)

        return current

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def backward(self, X, output, learning_rate=0.01):
        """反向传播"""
        m = X.shape[0]
        deltas = [output - X]

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i in range(len(self.weights) - 1, 0, -1):
            delta = deltas[-1] @ self.weights[i].T * self.relu_derivative(self.z_values[i-1])
            deltas.append(delta)

        deltas.reverse()

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for i in range(len(self.weights)):
            self.weights[i] -= learning_rate * (self.activations[i].T @ deltas[i]) / m
            self.biases[i] -= learning_rate * np.mean(deltas[i], axis=0)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def train(self, X: np.ndarray, epochs: int = 100, learning_rate: float = 0.01,
              batch_size: int = 32) -> Dict:
        """训练模型"""
        history = {'loss': []}

        # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
        for epoch in range(epochs):
            indices = np.random.permutation(len(X))
            total_loss = 0

            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for start in range(0, len(X), batch_size):
                batch_idx = indices[start:start+batch_size]
                batch_x = X[batch_idx]

                output = self.forward(batch_x)
                loss = np.mean((output - batch_x) ** 2)
                total_loss += loss * len(batch_x)

                self.backward(batch_x, output, learning_rate)

                # 梯度裁剪
                for i in range(len(self.weights)):
                    self.weights[i] = np.clip(self.weights[i], -1, 1)
                    self.biases[i] = np.clip(self.biases[i], -1, 1)

            avg_loss = total_loss / len(X)
            history['loss'].append(avg_loss)

            # 条件判断：根据当前输入或运行状态选择处理分支。
            if (epoch + 1) % 20 == 0:
                print(f"Epoch [{epoch+1}/{epochs}] Loss: {avg_loss:.6f}")

        return history

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def compute_threshold(self, X: np.ndarray, percentile: float = 95):
        """计算异常阈值"""
        output = self.forward(X)
        mse = np.mean((output - X) ** 2, axis=1)
        self.threshold = np.percentile(mse, percentile)
        print(f"异常阈值 ({percentile}th percentile): {self.threshold:.6f}")
        return self.threshold

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def predict(self, X: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """预测异常"""
        output = self.forward(X)
        mse = np.mean((output - X) ** 2, axis=1)
        return mse, (mse > self.threshold).astype(np.int32)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def export_onnx(self, onnx_path: str):
        """导出 ONNX 格式"""
        # 异常边界：隔离可能失败的解析、I/O 或外部服务调用。
        try:
            import onnx
            from onnx import helper, TensorProto

            X = helper.make_tensor_value_info('input', TensorProto.FLOAT, [None, self.input_size])
            Y = helper.make_tensor_value_info('output', TensorProto.FLOAT, [None, self.input_size])

            nodes = []
            initializers = []

            # 循环处理：逐项处理数据，并在循环条件变化时及时结束。
            for i, (w, b) in enumerate(zip(self.weights, self.biases)):
                w_name = f'encoder_w{i}'
                b_name = f'encoder_b{i}'
                initializers.append(helper.make_tensor(w_name, TensorProto.FLOAT, w.shape, w.flatten().tolist()))
                initializers.append(helper.make_tensor(b_name, TensorProto.FLOAT, b.shape, b.flatten().tolist()))

                # 条件判断：根据当前输入或运行状态选择处理分支。
                if i < len(self.weights) - 1:
                    matmul_node = helper.make_node('MatMul', inputs=[f'layer{i}' if i > 0 else 'input', w_name], outputs=[f'z{i}'])
                    add_node = helper.make_node('Add', inputs=[f'z{i}', b_name], outputs=[f'a{i}'])
                    relu_node = helper.make_node('Relu', inputs=[f'a{i}'], outputs=[f'layer{i+1}'])
                    nodes.extend([matmul_node, add_node, relu_node])
                else:
                    matmul_node = helper.make_node('MatMul', inputs=[f'layer{i}' if i > 0 else 'input', w_name], outputs=[f'z{i}'])
                    add_node = helper.make_node('Add', inputs=[f'z{i}', b_name], outputs=[f'a{i}'])
                    sigmoid_node = helper.make_node('Sigmoid', inputs=[f'a{i}'], outputs=['output'])
                    nodes.extend([matmul_node, add_node, sigmoid_node])

            graph = helper.make_graph(nodes, 'network_autoencoder', [X], [Y], initializers)
            model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 11)])

            onnx.save(model, onnx_path)
            print(f"ONNX 模型已导出到: {onnx_path}")

        # 异常收尾：将错误转换为可观察结果，并确保资源得到释放。
        except ImportError:
            print("警告: 未安装 onnx 库，跳过 ONNX 导出")

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def save(self, model_path: str):
        """保存模型"""
        np.savez(model_path,
                 weights=[w.tobytes() for w in self.weights],
                 biases=[b.tobytes() for b in self.biases],
                 input_size=self.input_size,
                 hidden_sizes=self.hidden_sizes,
                 threshold=self.threshold)
        print(f"模型已保存到: {model_path}")


# 类说明：组织相关状态和操作，实例成员的生命周期与线程安全由调用方遵守。
class AutoEncoderTrainer:
    """训练器封装"""

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def __init__(self, config_path: str = None):
        self.extractor = ConfigurableFeatureExtractor(config_path)
        self.input_size = self.extractor.get_input_size()
        self.model = AutoEncoder(input_size=self.input_size)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def train(self, X_train: np.ndarray, X_val: np.ndarray,
              epochs: int = 100, batch_size: int = 32) -> Dict:
        """训练模型"""
        X_train_flat = X_train.reshape(X_train.shape[0], -1)
        X_val_flat = X_val.reshape(X_val.shape[0], -1)

        print(f"输入维度: {self.input_size}")
        print(f"训练集: {X_train_flat.shape}")
        print(f"验证集: {X_val_flat.shape}")

        history = self.model.train(X_train_flat, epochs=epochs, batch_size=batch_size)
        self.model.compute_threshold(X_train_flat)

        return history

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def evaluate(self, X: np.ndarray, labels: np.ndarray) -> Dict:
        """评估模型"""
        X_flat = X.reshape(X.shape[0], -1)
        anomaly_scores, predictions = self.model.predict(X_flat)

        tp = np.sum((predictions == 1) & (labels == 1))
        fp = np.sum((predictions == 1) & (labels == 0))
        tn = np.sum((predictions == 0) & (labels == 0))
        fn = np.sum((predictions == 0) & (labels == 1))

        accuracy = (tp + tn) / len(labels)
        precision = tp / (tp + fp) if (tp + fp) > 0 else 0
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

        metrics = {
            'accuracy': float(accuracy),
            'precision': float(precision),
            'recall': float(recall),
            'f1': float(f1),
            'threshold': float(self.model.threshold),
            'input_size': self.input_size,
            'feature_dim': self.extractor.get_feature_dim(),
            'window_size': self.extractor.get_window_size()
        }

        print(f"\n评估结果:")
        print(f"  输入维度: {self.input_size}")
        print(f"  准确率: {accuracy:.4f}")
        print(f"  精确率: {precision:.4f}")
        print(f"  召回率: {recall:.4f}")
        print(f"  F1: {f1:.4f}")

        return metrics

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def export_onnx(self, onnx_path: str):
        """导出 ONNX"""
        self.model.export_onnx(onnx_path)

    # 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
    def save(self, model_path: str):
        """保存模型"""
        self.model.save(model_path)


# 条件判断：根据当前输入或运行状态选择处理分支。
if __name__ == '__main__':
    # 使用配置加载数据
    data_dir = '../data/synthetic'
    X = np.load(f'{data_dir}/features.npy')
    labels = np.load(f'{data_dir}/labels.npy')

    # 划分数据集
    split = int(len(X) * 0.8)
    X_train, X_test = X[:split], X[split:]
    labels_train, labels_test = labels[:split], labels[split:]

    X_train_normal = X_train[labels_train == 0]

    print(f"训练集: {X_train_normal.shape} (仅正常数据)")
    print(f"测试集: {X_test.shape}")

    # 创建训练器
    trainer = AutoEncoderTrainer()

    # 训练
    print("\n开始训练...")
    history = trainer.train(X_train_normal, X_train_normal, epochs=100, batch_size=32)

    # 评估
    print("\n评估模型...")
    metrics = trainer.evaluate(X_test, labels_test)

    # 导出 ONNX
    output_dir = '../models/autoencoder'
    os.makedirs(output_dir, exist_ok=True)
    trainer.export_onnx(f'{output_dir}/network_autoencoder.onnx')
    trainer.save(f'{output_dir}/network_autoencoder.npz')

    # 保存评估结果
    with open(f'{output_dir}/evaluation_metrics.json', 'w') as f:
        json.dump(metrics, f, indent=2)

    print(f"\n模型和评估结果已保存到 {output_dir}")
