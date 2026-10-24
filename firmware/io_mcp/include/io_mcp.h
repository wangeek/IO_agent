/*
 * io_mcp — 设备端 MCP server(Streamable HTTP + JSON-RPC 2.0)
 *
 * 端点:
 *   POST /mcp        JSON-RPC 2.0
 *   GET  /blob/<id>  观察帧原字节
 *
 * 工具:status | ls | read | capture | input | update
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** MCP server 的 HTTP 端口(also used for the blob endpoint and mDNS)。 */
#define IO_MCP_PORT 8080

/** 启动 MCP server(HTTP)。 */
esp_err_t io_mcp_start(void);

#ifdef __cplusplus
}
#endif
