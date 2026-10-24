#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""io_mcp_client -- 访问 Sophon 设备端 MCP server 的最小客户端。

设备是**服务端**(Streamable HTTP + JSON-RPC 2.0),agent 主动连过去。
本模块把"怎么调"这件事收在一处,各探针/脚本只关心"调什么"。

★ 两个必踩的坑,这里已经堵住 ★
  1. **代理**:本机有 http_proxy(Clash Verge 在 127.0.0.1:7897)。裸 urlopen
     会把设备请求交给代理,代理回 **502 Bad Gateway** —— 看起来像设备死了,
     其实不是。所有请求都走空 ProxyHandler。
  2. **取结果**:设备现在同时给 `content`(内容块数组)和 `structuredContent`
     (结构化结果)。要读结构化字段就取 structuredContent —— 那是规范里的
     契约字段,而且文本块里**不含** base64 图片体。

用法:
    from io_mcp_client import McpClient
    c = McpClient("172.17.21.225")
    print(c.call("status"))
    print(c.call("ls"))
    print(c.call("read", {"name": "AGENT.SH"}))
"""

import json
import os
import urllib.error
import urllib.request

# 长动作(注入类)会跑 ~10 s,客户端必须比它活得久,否则报假失败。
TIMEOUT = 60


class McpError(RuntimeError):
    """设备回了 JSON-RPC error(协议层错误,不是工具执行错误)。"""

    def __init__(self, code, message):
        super().__init__("JSON-RPC error %s: %s" % (code, message))
        self.code = code
        self.message = message


def _opener():
    """空代理 + 清掉环境里的代理变量,两头都堵住。"""
    for var in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                "ALL_PROXY", "all_proxy"):
        os.environ.pop(var, None)
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


class McpClient:
    """设备端 MCP server 的最小客户端。"""

    def __init__(self, host, port=8080, name=None, timeout=TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.url = "http://%s:%d/mcp" % (host, port)
        self.base = "http://%s:%d" % (host, port)
        # no_proxy 也要设:某些库即使给了空 ProxyHandler 仍会读环境变量
        os.environ["no_proxy"] = host + "," + os.environ.get("no_proxy", "")
        os.environ["NO_PROXY"] = os.environ["no_proxy"]
        self._rpc_id = 0

    # ---- 底层 ----

    def _post(self, body):
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(self.url, data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with _opener().open(req, timeout=self.timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def rpc(self, method, params=None):
        """发一个 JSON-RPC 请求,返回解析后的信封(成功或失败都返回)。"""
        self._rpc_id += 1
        body = {"jsonrpc": "2.0", "id": self._rpc_id, "method": method}
        if params is not None:
            body["params"] = params
        _, raw = self._post(body)
        try:
            return json.loads(raw)
        except ValueError:
            return {"_raw": raw}

    def get(self, path):
        """裸 GET(用于 /ping 与 /files/<name>)。"""
        req = urllib.request.Request(self.base + path, method="GET")
        try:
            with _opener().open(req, timeout=self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    # ---- 语义层 ----

    def initialize(self, version=None):
        params = {} if version is None else {"protocolVersion": version}
        env = self.rpc("initialize", params)
        return env.get("result", env)

    def list_tools(self):
        """返回工具清单(每个一项带 name/title/description/inputSchema/outputSchema)。"""
        env = self.rpc("tools/list")
        return env.get("result", {}).get("tools", [])

    def call(self, name, arguments=None):
        """调一个工具,返回**结构化结果**。

        协议错误(未知工具等)抛 McpError;工具执行错误(读不到文件等)返回
        带 ok=False / isError 的字典 —— 两种失败不要混为一谈。
        """
        env = self.rpc("tools/call", {"name": name, "arguments": arguments or {}})
        if "error" in env:
            err = env["error"]
            raise McpError(err.get("code"), err.get("message", ""))
        result = env.get("result", {})
        sc = result.get("structuredContent")
        if isinstance(sc, dict):
            return sc
        if result.get("content"):
            text = result["content"][0].get("text", "")
            try:
                return json.loads(text)
            except ValueError:
                return {"raw": text}
        return result

    def call_raw(self, name, arguments=None):
        """调一个工具并返回**完整信封**(要 content 数组/图片块时用这个)。"""
        return self.rpc("tools/call", {"name": name, "arguments": arguments or {}})


def image_block(envelope):
    """从完整信封里取出第一个 image 内容块(base64 + mimeType),没有就 None。"""
    result = envelope.get("result", {}) if isinstance(envelope, dict) else {}
    for item in result.get("content", []) or []:
        if item.get("type") == "image":
            return {"data": item.get("data", ""), "mimeType": item.get("mimeType", "")}
    return None
