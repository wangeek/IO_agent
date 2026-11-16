#!/usr/bin/env node
/*
 * sophon-bridge — 把 Sophon dongle 的 MCP(HTTP)接成 stdio MCP server。
 *
 * ★ 为什么是 Node ★
 *   WorkBuddy 的 `mcpServers[].runtime` **只支持 { "type": "node" }**
 *   (官方文档原文:「runtime.type 目前仅支持 "node"」;版本表里 python 只挂在
 *    `CliConfig.runtime.type` 名下;四个真实上架的连接器全部用 node)。
 *   而 MCP 方案的承诺是「用户无需额外安装」⇒ 只有 npx + 托管 node 这一条
 *   零安装路径。用 Python 就得让用户自己装 Python,违背承诺。
 *
 * 设计原则:透明代理,不翻译语义
 *   本脚本**不重新实现**任何工具逻辑,只把 stdin 来的 JSON-RPC 原样 POST 给
 *   设备的 /mcp,把回包原样写回 stdout。设备端契约(工具集 / schema / isError)
 *   永远是唯一真源,桥不会随固件演进漂移(哑设备原则:只搬运,不解读)。
 *
 * 用法
 *   npx sophon-bridge                      # mDNS 发现设备
 *   npx sophon-bridge --name sophon-dongle
 *   npx sophon-bridge --endpoint http://172.17.21.231:8080/mcp
 *   npx sophon-bridge --psk mysecret
 */

'use strict';

const dgram = require('dgram');
const http = require('http');
const https = require('https');
const os = require('os');
const readline = require('readline');
const { URL } = require('url');

const MDNS_ADDR = '224.0.0.251';
const MDNS_PORT = 5353;
const DEFAULT_SERVICE = '_ioagent._tcp.local';
const DEFAULT_NAME = 'sophon-dongle';
const DEFAULT_PORT = 8080;
const HTTP_TIMEOUT_MS = 30000;

// 发现失败后的冷却期:避免"设备没插"时每条调用都白等一次 mDNS 扫描。
const FAIL_COOLDOWN_MS = 10000;

// ------------------------------------------------------------------ DNS 编解码

function encodeName(name) {
  const parts = [];
  for (const label of name.replace(/\.$/, '').split('.')) {
    const raw = Buffer.from(label, 'ascii');
    parts.push(Buffer.from([raw.length]), raw);
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

/**
 * 解析一个 DNS 名字,返回 [名字, **名字之后的下一字节偏移**]。
 *
 * ★ 压缩指针这里必须"先前进 2 字节再记 end,然后才跳" ★
 * 写成 `offset = ptr; end = offset` 会让 end 指向跳转目标,后续资源记录
 * 整体错位、解析结果永远为空 —— 这个 bug 我在 Python 版里踩过一次:
 * 抓包能看到应答,但解出 0 条记录。
 */
function parseName(data, offset) {
  const labels = [];
  let jumped = false;
  let end = offset;
  let hops = 0;
  for (;;) {
    if (offset >= data.length) throw new Error('name overruns message');
    const len = data[offset];
    if (len === 0) {
      offset += 1;
      if (!jumped) end = offset;
      break;
    }
    if ((len & 0xc0) === 0xc0) {
      const ptr = ((len & 0x3f) << 8) | data[offset + 1];
      offset += 2;
      if (!jumped) end = offset;      // ← 指针之后的地址,不是跳转目标
      offset = ptr;
      jumped = true;
      if (++hops > 8) throw new Error('compression loop');
      continue;
    }
    offset += 1;
    labels.push(data.slice(offset, offset + len).toString('ascii'));
    offset += len;
    if (!jumped) end = offset;
  }
  return [labels.join('.'), end];
}

function buildQuery(qname, qtype) {
  const header = Buffer.alloc(12);
  header.writeUInt16BE(1, 4);              // qdcount = 1
  const tail = Buffer.alloc(4);
  tail.writeUInt16BE(qtype, 0);
  tail.writeUInt16BE(1, 2);                // class IN
  return Buffer.concat([header, encodeName(qname), tail]);
}

/**
 * 解析一份 mDNS 报文,返回 [{owner, type, value}]。
 *
 * ★ 必须把 answer / **authority** / additional 三段都读 ★
 * 本项目的 lwIP responder 回的包是 an=0、记录全在 authority(ns)里
 * (实测抓包:qd=2 an=0 ns=2 ar=0)。只遍历 an 的话永远是 0 条。
 */
function parseRecords(data) {
  if (data.length < 12) return [];
  const qd = data.readUInt16BE(4);
  const an = data.readUInt16BE(6);
  const ns = data.readUInt16BE(8);
  const ar = data.readUInt16BE(10);

  let offset = 12;
  try {
    for (let i = 0; i < qd; i++) {
      [, offset] = parseName(data, offset);
      offset += 4;
    }
  } catch (e) {
    return [];
  }

  const out = [];
  for (let i = 0; i < an + ns + ar; i++) {
    try {
      const [owner, next] = parseName(data, offset);
      offset = next;
      const rtype = data.readUInt16BE(offset);
      const rdlength = data.readUInt16BE(offset + 8);
      offset += 10;
      const rdataAt = offset;
      offset += rdlength;

      if (rtype === 1 && rdlength === 4) {
        out.push({ owner, type: 1, value: `${data[rdataAt]}.${data[rdataAt + 1]}.${data[rdataAt + 2]}.${data[rdataAt + 3]}` });
      } else if (rtype === 33 && rdlength >= 7) {
        const port = data.readUInt16BE(rdataAt + 4);
        const [target] = parseName(data, rdataAt + 6);
        out.push({ owner, type: 33, value: { target, port } });
      }
    } catch (e) {
      break;                                // 报文截断:用已解析到的
    }
  }
  return out;
}

// ------------------------------------------------------------------ mDNS 发现

function localIPv4() {
  const ips = [];
  const ifaces = os.networkInterfaces();
  for (const name of Object.keys(ifaces)) {
    for (const ni of ifaces[name] || []) {
      if (ni.family === 'IPv4' && !ni.internal) ips.push(ni.address);
    }
  }
  return ips;
}

/**
 * 找设备。返回 {ip, port, instance} 或 null。
 *
 * ★ 这个写法是踩出来的,别简化 ★
 *   ① mcast socket **绑 5353** 并**逐个本机 IP 入组**(用 0.0.0.0 入组在多网卡
 *      机器上收不到);查询也要**从它发**,应答才回得到。
 *   ② 要发**多种查询**:device.local 的 A/ANY + service 的 PTR/SRV/ANY。
 *      只发一种时,本项目的 lwIP responder 常常不回。
 *   ③ 两个 socket 都要读(组播应答与单播应答可能落在不同 socket)。
 */
function discover(name, waitMs) {
  return new Promise((resolve) => {
    const service = process.env.SOPHON_SERVICE || DEFAULT_SERVICE;
    const queries = [
      [`${name}.local`, 1],
      [`${name}.local`, 255],
      [service, 12],
      [service, 33],
      [service, 255],
    ];

    let mcast = null;
    let unicast = null;
    let timer = null;
    let done = false;
    let srvTarget = null;
    let port = null;
    let instance = null;
    const ipByName = new Map();

    const finish = (result) => {
      if (done) return;
      done = true;
      if (timer) clearInterval(timer);
      for (const s of [mcast, unicast]) {
        if (s) { try { s.close(); } catch (e) { /* 已关 */ } }
      }
      resolve(result);
    };

    const onMessage = (msg) => {
      for (const rec of parseRecords(msg)) {
        if (rec.type === 1) {
          ipByName.set(rec.owner.toLowerCase(), rec.value);
        } else if (rec.type === 33) {
          srvTarget = rec.value.target;
          port = rec.value.port;
          instance = rec.owner;
        }
      }
      if (srvTarget && ipByName.has(srvTarget.toLowerCase())) {
        return finish({ ip: ipByName.get(srvTarget.toLowerCase()), port: port || DEFAULT_PORT, instance });
      }
      if (port && ipByName.size === 1) {
        return finish({ ip: [...ipByName.values()][0], port, instance });
      }
    };

    try {
      mcast = dgram.createSocket({ type: 'udp4', reuseAddr: true });
      mcast.on('message', onMessage);
      mcast.on('error', () => { /* 抢不到 5353 就只靠 unicast */ });
      mcast.bind(MDNS_PORT, () => {
        for (const ip of localIPv4()) {
          try {
            mcast.addMembership(MDNS_ADDR, ip);
            break;                            // 入组成功一个就够
          } catch (e) { /* 换下一个网卡 */ }
        }
        try {
          mcast.setMulticastTTL(255);
          mcast.setMulticastLoopback(true);
        } catch (e) { /* 忽略 */ }
      });

      unicast = dgram.createSocket({ type: 'udp4', reuseAddr: true });
      unicast.on('message', onMessage);
      unicast.on('error', () => { /* 忽略 */ });
      unicast.bind(0);

      // 等两个 socket 都 bind 完再发;固定 120ms 足够,不值得为它加状态机
      setTimeout(() => {
        if (done) return;
        const senders = [mcast, unicast].filter((s) => s);
        const send = () => {
          if (done) return;
          for (const [qname, qtype] of queries) {
            const pkt = buildQuery(qname, qtype);
            for (const s of senders) {
              try { s.send(pkt, 0, pkt.length, MDNS_PORT, MDNS_ADDR); } catch (e) { /* 忽略 */ }
            }
          }
        };
        send();
        const deadline = Date.now() + waitMs;
        timer = setInterval(() => {
          if (Date.now() > deadline) finish(null);
          else send();
        }, 1000);
        if (timer.unref) timer.unref();
        setTimeout(() => finish(null), waitMs + 200).unref();
      }, 120);
    } catch (e) {
      finish(null);
    }
  });
}

// ------------------------------------------------------------------ 设备客户端

class Device {
  constructor({ endpoint, name, psk }) {
    this.endpoint = endpoint || null;
    this.name = name || DEFAULT_NAME;
    this.psk = psk || null;
    this.resolved = null;
    this.failedAt = 0;
  }

  async resolve(force) {
    if (this.resolved && !force) return this.resolved;
    if (this.endpoint) {
      this.resolved = this.endpoint;
      return this.resolved;
    }

    const now = Date.now();
    if (!force && this.failedAt && now - this.failedAt < FAIL_COOLDOWN_MS) {
      throw new Error(`no Sophon device named '${this.name}' found over mDNS `
        + '(retrying in a few seconds; is the dongle powered and on the same network?)');
    }

    const found = await discover(this.name, 6000);
    if (!found) {
      this.failedAt = now;
      throw new Error(`no Sophon device named '${this.name}' found over mDNS `
        + '(is the dongle powered and on the same network?)');
    }
    this.resolved = `http://${found.ip}:${found.port}/mcp`;
    this.failedAt = 0;
    return this.resolved;
  }

  /** POST 一段 JSON-RPC,返回**原始响应文本**(可能为空)。 */
  async post(payload) {
    const body = Buffer.from(JSON.stringify(payload), 'utf8');
    for (let attempt = 1; attempt <= 2; attempt++) {
      const url = await this.resolve(attempt === 2);
      try {
        return await this._request(url, body);
      } catch (e) {
        if (attempt === 2) throw e;
        this.resolved = null;              // IP 可能变了,强制重发现再试
      }
    }
    return '';
  }

  _request(urlStr, body) {
    return new Promise((resolve, reject) => {
      const u = new URL(urlStr);
      const mod = u.protocol === 'https:' ? https : http;
      const headers = {
        'Content-Type': 'application/json',
        'Content-Length': body.length,
        // MCP Streamable HTTP 要求客户端声明两种都接受
        Accept: 'application/json, text/event-stream',
      };
      if (this.psk) headers.Authorization = `Bearer ${this.psk}`;

      const req = mod.request(u, { method: 'POST', headers, timeout: HTTP_TIMEOUT_MS }, (res) => {
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
      });
      req.on('timeout', () => req.destroy(new Error('request timed out')));
      req.on('error', reject);
      req.end(body);
    });
  }
}

/** 设备若回 text/event-stream,把每条 data: 取出(每行一条 JSON)。 */
function unwrapSse(text) {
  const t = text.replace(/^\s+/, '');
  if (!t.startsWith('event:') && !t.startsWith('data:')) return text;
  return t.split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.startsWith('data:'))
    .map((l) => l.slice(5).trim())
    .join('\n');
}

// ------------------------------------------------------------------ 主循环

function parseArgs(argv) {
  const out = {
    endpoint: process.env.SOPHON_ENDPOINT || null,
    name: process.env.SOPHON_NAME || DEFAULT_NAME,
    psk: process.env.SOPHON_PSK || null,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--endpoint') out.endpoint = argv[++i];
    else if (a === '--name') out.name = argv[++i];
    else if (a === '--psk') out.psk = argv[++i];
    else if (a === '--version') out.version = true;
    else if (a === '--help' || a === '-h') out.help = true;
  }
  return out;
}

function out(line) {
  process.stdout.write(line + '\n');
}

async function handleLine(dev, text) {
  let payload;
  try {
    payload = JSON.parse(text);
  } catch (e) {
    out(JSON.stringify({
      jsonrpc: '2.0', id: null,
      error: { code: -32700, message: `parse error: ${e.message}` },
    }));
    return;
  }

  let raw;
  try {
    raw = await dev.post(payload);
  } catch (e) {
    if (payload.id === undefined) return;        // 通知没有回包义务
    out(JSON.stringify({
      jsonrpc: '2.0', id: payload.id,
      error: { code: -32603, message: `device unreachable: ${e.message}` },
    }));
    return;
  }

  const body = unwrapSse(raw).trim();
  if (!body) return;                             // 202 / 通知:无回包
  for (const chunk of body.split(/\r?\n/)) {
    const c = chunk.trim();
    if (c) out(c);
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.version) {
    // 用同目录的 package.json:通过 npx / 全局安装后,脚本与 package.json
    // 始终在同一层,用 '../package.json' 会找不到(踩过一次)。
    let v = '0.0.0';
    try {
      v = require('./package.json').version;
    } catch (e) { /* 打包方式特殊时退化为占位 */ }
    process.stdout.write(v + '\n');
    return 0;
  }
  if (args.help) {
    process.stderr.write(
      'sophon-bridge — expose a Sophon dongle as a stdio MCP server\n'
      + '  --endpoint URL   skip mDNS and use this endpoint\n'
      + '  --name NAME      device name (default sophon-dongle)\n'
      + '  --psk KEY        device key, sent as a Bearer token\n');
    return 0;
  }

  const dev = new Device(args);

  // 端点解析:成功就先打印,失败**不退出**。
  // 为什么:stdio 客户端看到"子进程退出"时无法区分「没插」「名字错」「网不通」;
  // 继续跑则每条调用都带回具体错误,设备一插上下一次调用就可用,不用重连。
  try {
    process.stderr.write(`sophon_bridge: device at ${await dev.resolve()}\n`);
  } catch (e) {
    process.stderr.write(`sophon_bridge: ${e.message}\n`);
    process.stderr.write('sophon_bridge: starting anyway -- each call will report '
      + 'the failure until the device is reachable\n');
  }

  const rl = readline.createInterface({ input: process.stdin, terminal: false });
  let chain = Promise.resolve();
  rl.on('line', (line) => {
    const text = line.trim();
    if (!text) return;
    chain = chain.then(() => handleLine(dev, text));   // 串行化,避免响应乱序
  });

  await new Promise((resolve) => {
    rl.on('close', resolve);
    process.stdin.on('end', resolve);
  });
  await chain;
  return 0;
}

main().then((code) => process.exit(code)).catch((e) => {
  process.stderr.write(`sophon_bridge: fatal: ${e && e.message}\n`);
  process.exit(2);
});
