#!/usr/bin/env python3

# 模块职责：本文件实现文件名所对应的日志、数据处理或网络诊断功能。
# 维护提示：扩展公共函数或命令行入口时，应说明输入、输出、异常、外部依赖和降级路径，
# 并保持既有 CLI/API 行为不变。

"""
阿里百炼API测试脚本
测试API调用是否正常工作
"""

import os
from openai import OpenAI

# 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
def test_dashscope_api():
    """测试阿里百炼API"""
    # 异常边界：隔离可能失败的解析、I/O 或外部服务调用。
    try:
        # 设置API密钥
        api_key = "YOUR_DASHSCOPE_API_KEY_HERE"  # 请替换为您的阿里百炼API密钥
        os.environ["DASHSCOPE_API_KEY"] = api_key
        
        # 按照官方文档初始化客户端
        client = OpenAI(
            api_key=os.getenv("DASHSCOPE_API_KEY"),
            base_url="https://dashscope.aliyuncs.com/compatible-mode/v1",
        )
        
        print("✅ 阿里百炼客户端初始化成功")
        
        # 测试简单调用
        print("🔄 测试API调用...")
        completion = client.chat.completions.create(
            model="qwen-plus",
            messages=[
                {"role": "system", "content": "你是一个专业的网络问题诊断专家。"},
                {"role": "user", "content": "请简单分析一下RTT延迟15ms的网络状况。"}
            ],
            extra_body={"enable_thinking": False}
        )
        
        result = completion.choices[0].message.content
        print("✅ API调用成功!")
        print(f"📋 返回结果:\n{result}")
        
        return True
        
    # 异常收尾：将错误转换为可观察结果，并确保资源得到释放。
    except Exception as e:
        print(f"❌ API调用失败: {e}")
        return False

# 条件判断：根据当前输入或运行状态选择处理分支。
if __name__ == "__main__":
    print("🧪 阿里百炼API测试")
    print("=" * 40)
    
    success = test_dashscope_api()
    
    # 条件判断：根据当前输入或运行状态选择处理分支。
    if success:
        print("\n🎉 API测试成功，可以正常使用!")
    else:
        print("\n❌ API测试失败，请检查配置")
