# ---------------------------------------------------------------------------
# S4：OPA REST API 自身的 token 认证（与 policy.rego 的业务鉴权数据相互独立，
# 且必须是独立文件——一个 .rego 文件只能有一个 package 声明）。
#
# compose 以 --authentication=token --authorization=basic 启动：token 方案
# 只把 Authorization: Bearer 的值解析进 input.identity（无头时缺省），
# 真正的共享密钥校验由本包完成——basic 授权求值 data.system.authz.allow。
# 期望密钥来自容器环境变量 OPA_TOKEN（dev_lite init 生成、应用客户端经
# IOAP_OPA_TOKEN 携带同一值）。object.get 兜底空串，保证无头请求得到
# false → 401，而不是 undefined → 500。
#
# 已在 OPA 1.8.0-static 实测：无头 401 / 错误 token 401 / 正确 token 200。
# ---------------------------------------------------------------------------
package system.authz

import rego.v1

allow := object.get(input, "identity", "") == opa.runtime().env.OPA_TOKEN
