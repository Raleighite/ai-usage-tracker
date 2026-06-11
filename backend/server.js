const express = require('express');
const { createProxyMiddleware } = require('http-proxy-middleware');
const { execSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const PORT = process.env.PORT || 8040;
const API_TARGET = process.env.API_TARGET || 'http://192.168.0.231:8089';
// Workspace file server — runs on the Mac Mini so QA-hosted frontend
// can proxy workspace reads without needing local filesystem access.
const WORKSPACE_SERVER = process.env.WORKSPACE_SERVER || null;
const SECRETS_PATH = process.env.SECRETS_PATH || '/Users/macmini/.openclaw/workspace/tools/agent-hq-secrets';
const SECRETS_SERVER = process.env.SECRETS_SERVER || null;

const app = express();
app.use(express.json());

// Available models for the selector
const MODELS = [
  { id: 'anthropic/claude-3-5-haiku-20241022', label: 'Claude 3.5 Haiku', tier: 'fast' },
  { id: 'anthropic/claude-sonnet-4-6',         label: 'Claude Sonnet 4.6', tier: 'balanced' },
  { id: 'anthropic/claude-opus-4-5',           label: 'Claude Opus 4.5', tier: 'powerful' },
  { id: 'google/gemini-2.5-flash-preview',     label: 'Gemini 2.5 Flash', tier: 'fast' },
  { id: 'google/gemini-2.5-pro-preview',       label: 'Gemini 2.5 Pro', tier: 'powerful' },
  { id: 'ollama/qwen3-coder:8b',               label: 'Qwen3 Coder 8B (local)', tier: 'local' },
  { id: 'ollama/gemma3:12b',                   label: 'Gemma3 12B (local)', tier: 'local' },
  { id: 'ollama/qwen3:8b',                     label: 'Qwen3 8B (local)', tier: 'local' },
  { id: 'default',                             label: 'Default (reset)', tier: 'reset' },
];

// GET /api/models — return available model list
app.get('/api/models', (req, res) => res.json(MODELS));

// GET /api/config/model — read configured model from openclaw.json
app.get('/api/config/model', (req, res) => {
  const configPath = process.env.OPENCLAW_CONFIG ||
    require('path').join(require('os').homedir(), '.openclaw', 'openclaw.json');
  try {
    const cfg = JSON.parse(require('fs').readFileSync(configPath, 'utf8'));
    const modelCfg = cfg?.agents?.defaults?.model || {};
    res.json({
      primary: modelCfg.primary || null,
      fallbacks: modelCfg.fallbacks || [],
    });
  } catch (e) {
    res.status(500).json({ error: 'Failed to read openclaw config', detail: e.message });
  }
});

// GET /api/agents/:id/model — read the model config for a specific agent
app.get('/api/agents/:id/model', async (req, res) => {
  const { id } = req.params;
  try {
    // Fetch agent list to get its configPath
    const agents = JSON.parse(execSync(`curl -s ${API_TARGET}/api/agents`, { timeout: 5000 }));
    const agent = agents.find(a => a.id === id);
    if (!agent) return res.status(404).json({ error: `Agent '${id}' not found` });

    const configPath = agent.configPath || process.env.OPENCLAW_CONFIG ||
      require('path').join(require('os').homedir(), '.openclaw', 'openclaw.json');

    const cfg = JSON.parse(require('fs').readFileSync(configPath, 'utf8'));
    // Check for agent-specific model override first, fall back to defaults
    const agentCfg = cfg?.agents?.[id]?.model || cfg?.agents?.defaults?.model || {};
    res.json({
      primary: agentCfg.primary || null,
      fallbacks: agentCfg.fallbacks || [],
      source: cfg?.agents?.[id]?.model ? 'agent-specific' : 'defaults',
    });
  } catch (e) {
    res.status(500).json({ error: 'Failed to read agent model config', detail: e.message });
  }
});

// POST /api/agents/:id/model — change model for an agent session
app.post('/api/agents/:id/model', async (req, res) => {
  const { id } = req.params;
  const { model } = req.body;
  if (!model) return res.status(400).json({ error: 'model required' });

  // Look up the agent's session label and configPath from the backend
  let agent;
  try {
    const agents = JSON.parse(execSync(`curl -s ${API_TARGET}/api/agents`, { timeout: 5000 }));
    agent = agents.find(a => a.id === id);
    if (!agent) return res.status(404).json({ error: `Agent '${id}' not found` });
    if (!agent.sessionLabel) return res.status(400).json({ error: `Agent '${id}' has no sessionLabel` });
  } catch (e) {
    return res.status(502).json({ error: 'Failed to fetch agent list', detail: e.message });
  }

  // Update openclaw.json directly with the model override for this agent's session
  // Also attempt to send /model command via openclaw CLI if available
  try {
    const modelArg = model === 'default' ? 'default' : model;
    const ocPath = process.env.OPENCLAW_BIN || 'openclaw';
    const sessionArg = agent.sessionLabel;
    const cmd = `${ocPath} agent --agent "${id}" --message "/model ${modelArg}" --local 2>&1 || true`;
    let out = '';
    try {
      out = execSync(cmd, { timeout: 15000, encoding: 'utf8' });
    } catch (cmdErr) {
      out = cmdErr.stderr || cmdErr.message || 'CLI unavailable';
    }
    res.json({ ok: true, agent: id, model, session: sessionArg, output: out.trim() });
  } catch (e) {
    res.status(500).json({ error: 'Failed to send model command', detail: e.message });
  }
});

// Workspace file browser — proxy to workspace-server.js on the Mac Mini
// when WORKSPACE_SERVER is set; fall back to local reads otherwise.
if (WORKSPACE_SERVER) {
  const workspaceProxy = createProxyMiddleware({
    target: WORKSPACE_SERVER,
    changeOrigin: true,
    pathFilter: '/api/workspace',
    on: {
      error(err, req, res) {
        console.error(`Workspace proxy error: ${err.message}`);
        if (res.writeHead) {
          res.writeHead(502, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Workspace server unreachable', detail: err.message }));
        }
      }
    }
  });
  app.use(workspaceProxy);
  console.log(`Workspace API → ${WORKSPACE_SERVER} (proxy)`);
} else {
  // Local filesystem reads (only works when frontend runs on the same host as agent workspaces)
  const BINARY_EXTS = new Set(['.png','.jpg','.jpeg','.gif','.webp','.ico','.svg',
    '.pdf','.zip','.tar','.gz','.bz2','.7z','.wasm','.db','.sqlite','.bin']);
  const MAX_FILE_SIZE = 512 * 1024;

  async function resolveAgentWorkspace(agentId) {
    const agents = JSON.parse(execSync(`curl -s ${API_TARGET}/api/agents`, { timeout: 5000 }));
    const agent = agents.find(a => a.id === agentId);
    if (!agent) throw Object.assign(new Error(`Agent '${agentId}' not found`), { status: 404 });
    if (!agent.workspace) throw Object.assign(new Error(`No workspace configured for '${agentId}'`), { status: 400 });
    return agent.workspace;
  }

  function safePath(base, rel) {
    const resolved = path.resolve(base, rel || '');
    if (!resolved.startsWith(path.resolve(base))) throw Object.assign(new Error('Access denied'), { status: 403 });
    return resolved;
  }

  app.get('/api/workspace/:agentId/tree', async (req, res) => {
    try {
      const base = await resolveAgentWorkspace(req.params.agentId);
      const maxDepth = Math.min(parseInt(req.query.depth || '3', 10), 5);
      const showHidden = req.query.hidden === '1';
      function buildTree(dir, depth) {
        if (depth > maxDepth) return [];
        let entries;
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return []; }
        return entries
          .filter(e => showHidden || !e.name.startsWith('.'))
          .map(e => {
            const fullPath = path.join(dir, e.name);
            const relPath = path.relative(base, fullPath);
            const stat = (() => { try { return fs.statSync(fullPath); } catch { return null; } })();
            const node = {
              name: e.name, path: relPath,
              type: e.isDirectory() ? 'dir' : 'file',
              ext: e.isFile() ? path.extname(e.name).toLowerCase() : null,
              size: stat && e.isFile() ? stat.size : null,
              mtime: stat ? stat.mtimeMs : null,
            };
            if (e.isDirectory()) node.children = buildTree(fullPath, depth + 1);
            return node;
          })
          .sort((a, b) => { if (a.type !== b.type) return a.type === 'dir' ? -1 : 1; return a.name.localeCompare(b.name); });
      }
      res.json({ base, entries: buildTree(base, 1) });
    } catch (e) { res.status(e.status || 500).json({ error: e.message }); }
  });

  function lsHandler(fixedDir) {
    return async (req, res) => {
      try {
        const base = await resolveAgentWorkspace(req.params.agentId);
        const target = safePath(base, fixedDir !== null ? fixedDir : (req.query.dir || ''));
        if (!fs.existsSync(target)) return res.status(404).json({ error: 'Path not found' });
        if (!fs.statSync(target).isDirectory()) return res.status(400).json({ error: 'Not a directory' });
        const entries = fs.readdirSync(target, { withFileTypes: true })
          .filter(e => !e.name.startsWith('.') || req.query.hidden === '1')
          .map(e => {
            const fp = path.join(target, e.name);
            const s = (() => { try { return fs.statSync(fp); } catch { return null; } })();
            return { name: e.name, type: e.isDirectory() ? 'dir' : 'file', ext: e.isFile() ? path.extname(e.name).toLowerCase() : null, size: s && e.isFile() ? s.size : null, mtime: s ? s.mtimeMs : null };
          })
          .sort((a, b) => { if (a.type !== b.type) return a.type === 'dir' ? -1 : 1; return a.name.localeCompare(b.name); });
        res.json({ base, dir: fixedDir !== null ? fixedDir : (req.query.dir || ''), entries });
      } catch (e) { res.status(e.status || 500).json({ error: e.message }); }
    };
  }
  app.get('/api/workspace/:agentId', lsHandler(''));
  app.get('/api/workspace/:agentId/ls', lsHandler(null));

  app.get('/api/workspace/:agentId/file', async (req, res) => {
    try {
      const base = await resolveAgentWorkspace(req.params.agentId);
      const filePath = safePath(base, req.query.path || '');
      if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'File not found' });
      const stat = fs.statSync(filePath);
      if (!stat.isFile()) return res.status(400).json({ error: 'Not a file' });
      const ext = path.extname(filePath).toLowerCase();
      if (BINARY_EXTS.has(ext)) return res.status(415).json({ error: 'Binary file — preview not supported' });
      if (stat.size > MAX_FILE_SIZE) {
        const buf = Buffer.alloc(MAX_FILE_SIZE);
        const fd = fs.openSync(filePath, 'r');
        fs.readSync(fd, buf, 0, MAX_FILE_SIZE, 0);
        fs.closeSync(fd);
        return res.json({ content: buf.toString('utf8'), truncated: true, size: stat.size, path: req.query.path });
      }
      res.json({ content: fs.readFileSync(filePath, 'utf8'), truncated: false, size: stat.size, path: req.query.path });
    } catch (e) { res.status(e.status || 500).json({ error: e.message }); }
  });

  app.get('/api/workspace/:agentId/download', async (req, res) => {
    try {
      const base = await resolveAgentWorkspace(req.params.agentId);
      const filePath = safePath(base, req.query.path || '');
      if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'File not found' });
      const stat = fs.statSync(filePath);
      if (!stat.isFile()) return res.status(400).json({ error: 'Not a file' });
      res.setHeader('Content-Disposition', `attachment; filename="${path.basename(filePath)}"`);
      res.setHeader('Content-Length', stat.size);
      fs.createReadStream(filePath).pipe(res);
    } catch (e) { res.status(e.status || 500).json({ error: e.message }); }
  });

  console.log('Workspace API → local filesystem');
}

// Secrets — proxy to workspace-server on Mac Mini when SECRETS_SERVER is set;
// fall back to local filesystem reads otherwise.
if (SECRETS_SERVER) {
  const secretsProxy = createProxyMiddleware({
    target: SECRETS_SERVER,
    changeOrigin: true,
    pathFilter: '/api/secrets',
    on: {
      error(err, req, res) {
        console.error(`Secrets proxy error: ${err.message}`);
        if (res.writeHead) {
          res.writeHead(502, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Secrets server unreachable', detail: err.message }));
        }
      }
    }
  });
  app.use(secretsProxy);
  console.log(`Secrets API → ${SECRETS_SERVER} (proxy)`);
} else {
  // Local filesystem reads (only works when running on same host as secrets)
  // GET /api/secrets — list secrets
  app.get('/api/secrets', async (req, res) => {
    try {
      if (!fs.existsSync(SECRETS_PATH)) return res.json({ keys: [] });
      const files = fs.readdirSync(SECRETS_PATH).filter(f => !f.startsWith('.'));
      const keys = files.map(f => {
        const filePath = path.join(SECRETS_PATH, f);
        const stat = fs.statSync(filePath);
        return { name: f, hasValue: stat.size > 0, created: stat.mtime };
      });
      res.json({ keys });
    } catch (e) {
      res.status(500).json({ error: 'Failed to read secrets', detail: e.message });
    }
  });

  // Sync all agent-hq secrets into every workspace-*/secrets.env and the root secrets.env.
  // Only manages keys that exist in SECRETS_PATH — never removes manually-set keys.
  function syncSecretsToWorkspaces() {
    const ocDir = path.dirname(SECRETS_PATH.includes('workspace') ? SECRETS_PATH.split('/workspace')[0] + '/workspace' : SECRETS_PATH);
    const rootOcDir = '/Users/macmini/.openclaw';
    try {
      if (!fs.existsSync(SECRETS_PATH)) return;
      const secretFiles = fs.readdirSync(SECRETS_PATH).filter(f => !f.startsWith('.'));
      const secrets = {};
      for (const f of secretFiles) {
        try { secrets[f] = fs.readFileSync(path.join(SECRETS_PATH, f), 'utf8').trim(); } catch {}
      }
      if (!Object.keys(secrets).length) return;

      // Collect target env files: all workspace-*/secrets.env + root secrets.env
      const targets = [];
      try {
        fs.readdirSync(rootOcDir)
          .filter(d => d.startsWith('workspace-'))
          .forEach(d => targets.push(path.join(rootOcDir, d, 'secrets.env')));
      } catch {}
      targets.push(path.join(rootOcDir, 'secrets.env'));

      for (const envPath of targets) {
        try {
          let existing = '';
          try { existing = fs.readFileSync(envPath, 'utf8'); } catch {}

          const lines = existing.length ? existing.split('\n') : [
            '# secrets.env — managed by Agent HQ Secrets Manager',
            '# DO NOT COMMIT THIS FILE',
            ''
          ];

          const updatedKeys = new Set();
          const updatedLines = lines.map(line => {
            const m = line.match(/^([A-Za-z_][A-Za-z0-9_]*)=/);
            if (m && secrets[m[1]] !== undefined) {
              updatedKeys.add(m[1]);
              return `${m[1]}="${secrets[m[1]]}"`;
            }
            return line;
          });

          for (const [key, val] of Object.entries(secrets)) {
            if (!updatedKeys.has(key)) updatedLines.push(`${key}="${val}"`);
          }

          fs.writeFileSync(envPath, updatedLines.join('\n'), { mode: 0o600 });
        } catch (e) {
          console.error(`syncSecretsToWorkspaces: failed to update ${envPath}: ${e.message}`);
        }
      }
      console.log(`Secrets synced to ${targets.length} env file(s)`);
    } catch (e) {
      console.error('syncSecretsToWorkspaces error:', e.message);
    }
  }

  // POST /api/secrets — create a new secret
  app.post('/api/secrets', async (req, res) => {
    try {
      const { name, value } = req.body;
      if (!name || !value) return res.status(400).json({ error: 'name and value required' });
      if (!fs.existsSync(SECRETS_PATH)) fs.mkdirSync(SECRETS_PATH, { recursive: true });
      fs.writeFileSync(path.join(SECRETS_PATH, name), value, { mode: 0o600 });
      syncSecretsToWorkspaces();
      res.json({ ok: true, id: name });
    } catch (e) {
      res.status(500).json({ error: 'Failed to create secret', detail: e.message });
    }
  });

  // DELETE /api/secrets/:id — delete a secret
  app.delete('/api/secrets/:id', async (req, res) => {
    try {
      const filePath = path.join(SECRETS_PATH, req.params.id);
      if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'Secret not found' }); 
      fs.unlinkSync(filePath);
      syncSecretsToWorkspaces();
      res.json({ ok: true });
    } catch (e) {
      res.status(500).json({ error: 'Failed to delete secret', detail: e.message });
    }
  });

  console.log('Secrets API → local filesystem');
}

// GET /api/tools/status?url=http://host:port/ — server-side health probe
app.get('/api/tools/status', async (req, res) => {
  const { url } = req.query;
  if (!url) return res.status(400).json({ error: 'url required' });
  // Only allow local network targets
  let parsed;
  try { parsed = new URL(url); } catch { return res.status(400).json({ error: 'invalid url' }); }
  const allowed = /^(localhost|127\.0\.0\.1|192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.)/.test(parsed.hostname);
  if (!allowed) return res.status(403).json({ error: 'non-local target denied' });

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 3000);
  try {
    const r = await fetch(url, { method: 'HEAD', signal: controller.signal, redirect: 'manual' });
    clearTimeout(timer);
    res.json({ ok: true, status: r.status });
  } catch (e) {
    clearTimeout(timer);
    res.json({ ok: false, error: e.name === 'AbortError' ? 'timeout' : e.message });
  }
});

// PostHog Analytics Proxy — reads POSTHOG_PERSONAL_KEY from Agent HQ secrets
// Mounted BEFORE apiProxy so it takes priority over the catch-all /api route
app.use('/api/analytics/posthog', async (req, res) => {
  try {
    const keyPath = path.join(SECRETS_PATH, 'POSTHOG_PERSONAL_KEY');
    if (!fs.existsSync(keyPath)) {
      return res.status(503).json({ error: 'POSTHOG_PERSONAL_KEY not configured in Agent HQ secrets' });
    }
    const personalKey = fs.readFileSync(keyPath, 'utf8').trim();
    // req.url is relative to mount point and includes query string
    const url = `https://us.posthog.com/api${req.url}`;
    const fetchOpts = {
      method: req.method,
      headers: {
        Authorization: `Bearer ${personalKey}`,
        'Content-Type': 'application/json',
      },
    };
    if (req.method !== 'GET' && req.method !== 'HEAD' && req.body && Object.keys(req.body).length) {
      fetchOpts.body = JSON.stringify(req.body);
    }
    const r = await fetch(url, fetchOpts);
    const text = await r.text();
    let data;
    try { data = JSON.parse(text); } catch { data = { raw: text }; }
    res.status(r.status).json(data);
  } catch (e) {
    res.status(500).json({ error: 'PostHog proxy error', detail: e.message });
  }
});

// QuestCoins device children proxy — reads QUESTCOINS_DEVICE_TOKEN from Agent HQ secrets
app.get('/api/analytics/questcoins-children', async (req, res) => {
  try {
    const tokenPath = path.join(SECRETS_PATH, 'QUESTCOINS_DEVICE_TOKEN');
    if (!fs.existsSync(tokenPath)) {
      return res.status(503).json({ error: 'QUESTCOINS_DEVICE_TOKEN not configured', children: [] });
    }
    const token = fs.readFileSync(tokenPath, 'utf8').trim();
    const r = await fetch('https://questcoins.gg/api/device/children', {
      headers: { Authorization: `Bearer ${token}` },
    });
    if (!r.ok) {
      return res.status(r.status).json({ error: 'questcoins.gg API error', children: [] });
    }
    const data = await r.json();
    res.json(data);
  } catch (e) {
    res.status(500).json({ error: 'QuestCoins proxy error', detail: e.message, children: [] });
  }
});

// GET /api/usage/summary — ESP32-optimized summary of agent spend
// Pre-filters /api/usage so the device doesn't need to parse 30-day arrays
// or do model-string filtering on-chip.
app.get('/api/usage/summary', async (req, res) => {
  try {
    const r = await fetch(`${API_TARGET}/api/usage`);
    if (!r.ok) return res.status(502).json({ error: 'upstream /api/usage failed', status: r.status });
    const data = await r.json();

    // Per-tier token/cost aggregation from perSession[]
    const tiers = { sonnet: { tokens: 0, cost: 0 }, haiku: { tokens: 0, cost: 0 }, opus: { tokens: 0, cost: 0 }, other: { tokens: 0, cost: 0 } };
    for (const s of (data.perSession || [])) {
      const m = (s.model || '').toLowerCase();
      const tier = m.includes('sonnet') ? 'sonnet' : m.includes('haiku') ? 'haiku' : m.includes('opus') ? 'opus' : 'other';
      tiers[tier].tokens += s.tokens || 0;
      tiers[tier].cost   += s.cost   || 0;
    }

    // Last 7 days from days[] array (already sorted oldest-first)
    const week = (data.days || []).slice(-7).map(d => ({ date: d.label, cost: d.cost, tokens: d.tokens }));

    res.json({
      todayCost:       data.todayCost      || 0,
      todayTokens:     data.todayTokens    || 0,
      totalCost:       data.totalCost      || 0,
      totalTokens:     data.totalTokens    || 0,
      openrouterDaily: data.billing?.openrouter?.daily || 0,
      openrouterTotal: data.billing?.openrouter?.total || 0,
      byTier: tiers,
      week,
    });
  } catch (e) {
    res.status(500).json({ error: 'usage summary failed', detail: e.message });
  }
});

// GET /api/claude/usage — Claude session & weekly usage % for T-Display-S3
// Reads OAuth token from CLAUDE_OAUTH_TOKEN env var or ~/.claude/.credentials.json
// Returns: { claude5h_pct, claude7d_pct, reset_at, cached }
const https = require('https');
const os = require('os');

let _claudeUsageCache = null;
let _claudeUsageCacheTime = 0;
let _claudeUsageLastGood = null;   // last successful result, returned stale on error
let _claudeUsageBackoffUntil = 0;  // don't retry Anthropic until this timestamp
const CLAUDE_USAGE_CACHE_TTL = 60 * 1000;  // 60s normal TTL
const CLAUDE_USAGE_BACKOFF_TTL = 3 * 60 * 1000; // 3 min backoff after error

function getClaudeOAuthToken() {
  // 1. Env var override
  if (process.env.CLAUDE_OAUTH_TOKEN) return process.env.CLAUDE_OAUTH_TOKEN;

  // 2. Claude Code credentials file
  const credPath = require('path').join(os.homedir(), '.claude', '.credentials.json');
  if (require('fs').existsSync(credPath)) {
    try {
      const creds = JSON.parse(require('fs').readFileSync(credPath, 'utf8'));
      const token = creds?.claudeAiOauth?.accessToken;
      if (token) return token;
    } catch (_) {}
  }

  // 3. macOS Keychain (Claude Code stores credentials here on macOS)
  try {
    const { execSync } = require('child_process');
    const raw = execSync('security find-generic-password -s "Claude Code-credentials" -w', {
      timeout: 5000, stdio: ['ignore', 'pipe', 'ignore'],
    }).toString().trim();
    const creds = JSON.parse(raw);
    const token = creds?.claudeAiOauth?.accessToken;
    if (token) return token;
  } catch (_) {}

  throw new Error(
    'No Claude OAuth token found. Tried: CLAUDE_OAUTH_TOKEN env var, ' +
    '~/.claude/.credentials.json, macOS Keychain (Claude Code-credentials). ' +
    'Run `claude login` to authenticate.'
  );
}

function fetchClaudeOAuthUsage(token) {
  return new Promise((resolve, reject) => {
    const req = https.request({
      hostname: 'api.anthropic.com',
      path: '/api/oauth/usage',
      method: 'GET',
      headers: {
        Authorization: `Bearer ${token}`,
        'anthropic-beta': 'oauth-2025-04-20',
        'User-Agent': 'agent-hq-frontend/1.0',
      },
    }, (res) => {
      let body = '';
      res.on('data', c => body += c);
      res.on('end', () => {
        if (res.statusCode !== 200) return reject(new Error(`Anthropic ${res.statusCode}: ${body}`));
        try { resolve(JSON.parse(body)); } catch (e) { reject(new Error('Bad JSON from Anthropic')); }
      });
    });
    req.on('error', reject);
    req.end();
  });
}

function fmtResetMs(ms) {
  if (!ms || ms <= 0) return null;
  const totalMin = Math.round(ms / 60000);
  const h = Math.floor(totalMin / 60);
  const m = totalMin % 60;
  return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

function resetInfo(ts) {
  if (!ts) return { reset_at: null, resets_in: null };
  const diffMs = new Date(ts) - Date.now();
  return {
    reset_at: ts,
    resets_in: fmtResetMs(diffMs > 0 ? diffMs : 0),
  };
}

function normalizeClaudeUsage(raw) {
  // Handle known response shapes — field names are undocumented and may change
  let pct5h = null, pct7d = null, resetAt = null, resetsIn = null;
  let sonnet7dPct = null, sonnet7dResetAt = null, sonnet7dResetsIn = null;
  let total7dResetAt = null, total7dResetsIn = null;

  if (raw.five_hour?.utilization !== undefined) {
    // Shape C (live): { five_hour: { utilization, resets_at }, seven_day: { utilization, resets_at }, ... }
    // utilization is already an integer percentage (3 = 3%)
    pct5h   = raw.five_hour.utilization;
    pct7d   = raw.seven_day?.utilization ?? 0;
    const sessionReset = resetInfo(raw.five_hour.resets_at);
    resetAt = sessionReset.reset_at;
    resetsIn = sessionReset.resets_in;

    const sonnetReset = resetInfo(raw.seven_day_sonnet?.resets_at);
    sonnet7dPct = raw.seven_day_sonnet?.utilization ?? null;
    sonnet7dResetAt = sonnetReset.reset_at;
    sonnet7dResetsIn = sonnetReset.resets_in;

    const totalReset = resetInfo(raw.seven_day?.resets_at);
    total7dResetAt = totalReset.reset_at;
    total7dResetsIn = totalReset.resets_in;
  } else if (raw.session?.percent !== undefined) {
    // Shape A: { session: { percent, resetMs }, weekly: { percent, resetAt } }
    pct5h    = Math.round(raw.session.percent);
    pct7d    = Math.round(raw.weekly?.percent ?? 0);
    resetAt  = raw.weekly?.resetAt ?? null;
    resetsIn = fmtResetMs(raw.session.resetMs);
  } else if (raw.usage_5h?.utilization !== undefined) {
    // Shape B: { usage_5h: { utilization, reset_at }, usage_7d: { utilization } }
    pct5h   = Math.round(raw.usage_5h.utilization * 100);
    pct7d   = Math.round((raw.usage_7d?.utilization ?? 0) * 100);
    resetAt = raw.usage_5h.reset_at ?? null;
    if (resetAt) {
      const diffMs = new Date(resetAt) - Date.now();
      resetsIn = fmtResetMs(diffMs > 0 ? diffMs : 0);
    }
  } else {
    // Unknown shape — pass raw through for debugging
    return { raw, normalized: false };
  }

  return {
    claude5h_pct: pct5h,
    claude7d_pct: pct7d,
    reset_at: resetAt,
    resets_in: resetsIn,
    sonnet7d_pct: sonnet7dPct,
    sonnet7d_reset_at: sonnet7dResetAt,
    sonnet7d_resets_in: sonnet7dResetsIn,
    total7d_pct: pct7d,
    total7d_reset_at: total7dResetAt,
    total7d_resets_in: total7dResetsIn,
    normalized: true,
  };
}

app.get('/api/claude/usage', async (req, res) => {
  const now = Date.now();
  if (_claudeUsageCache && now - _claudeUsageCacheTime < CLAUDE_USAGE_CACHE_TTL) {
    return res.json({ ..._claudeUsageCache, cached: true });
  }
  // During backoff window, return stale data rather than hammering Anthropic
  if (now < _claudeUsageBackoffUntil) {
    if (_claudeUsageLastGood) return res.json({ ..._claudeUsageLastGood, cached: true, stale: true });
    return res.status(503).json({ error: 'claude usage rate limited, retrying soon' });
  }
  try {
    const token = getClaudeOAuthToken();
    const raw   = await fetchClaudeOAuthUsage(token);
    const result = normalizeClaudeUsage(raw);
    _claudeUsageCache     = result;
    _claudeUsageCacheTime = now;
    _claudeUsageLastGood  = result;
    _claudeUsageBackoffUntil = 0;
    res.json({ ...result, cached: false });
  } catch (e) {
    // On any error: engage backoff so we don't spam Anthropic, return last known good if available
    _claudeUsageBackoffUntil = now + CLAUDE_USAGE_BACKOFF_TTL;
    if (_claudeUsageLastGood) return res.json({ ..._claudeUsageLastGood, cached: true, stale: true });
    res.status(500).json({ error: 'claude usage fetch failed', detail: e.message });
  }
});

// GET /api/codex/usage — local OpenClaw ChatGPT Codex session usage rollup
// Reads OpenClaw trajectory telemetry and returns compact ESP32-friendly totals.
let _codexUsageCache = null;
let _codexUsageCacheTime = 0;
const CODEX_USAGE_CACHE_TTL = 60 * 1000;
const CODEX_SESSIONS_ROOT = process.env.OPENCLAW_AGENTS_DIR ||
  path.join(os.homedir(), '.openclaw', 'agents');

function walkTrajectoryFiles(root, cutoffMs, out = []) {
  if (!fs.existsSync(root)) return out;
  for (const ent of fs.readdirSync(root, { withFileTypes: true })) {
    const p = path.join(root, ent.name);
    if (ent.isDirectory()) {
      if (ent.name === 'node_modules') continue;
      walkTrajectoryFiles(p, cutoffMs, out);
      continue;
    }
    if (!ent.isFile() || !ent.name.endsWith('.trajectory.jsonl')) continue;
    const st = fs.statSync(p);
    if (st.mtimeMs >= cutoffMs) out.push(p);
  }
  return out;
}

function emptyCodexBucket() {
  return { turns: 0, sessions: 0, tokens: 0, input: 0, output: 0, cacheRead: 0 };
}

function addCodexUsage(bucket, usage) {
  bucket.turns += 1;
  bucket.tokens += usage.total || 0;
  bucket.input += usage.input || 0;
  bucket.output += usage.output || 0;
  bucket.cacheRead += usage.cacheRead || 0;
}

function collectCodexUsage() {
  const now = Date.now();
  const fiveHourMs = 5 * 60 * 60 * 1000;
  const cutoff = now - 7 * 24 * 60 * 60 * 1000;
  const buckets = {
    five_hour: emptyCodexBucket(),
    twenty_four_hour: emptyCodexBucket(),
    seven_day: emptyCodexBucket(),
  };
  const seen = {
    five_hour: new Set(),
    twenty_four_hour: new Set(),
    seven_day: new Set(),
  };
  let last_turn_at = null;
  let first_five_hour_turn_ms = null;

  for (const file of walkTrajectoryFiles(CODEX_SESSIONS_ROOT, cutoff)) {
    const lines = fs.readFileSync(file, 'utf8').split('\n');
    for (const line of lines) {
      if (!line.includes('"type":"model.completed"')) continue;
      let evt;
      try { evt = JSON.parse(line); } catch (_) { continue; }
      if (evt.type !== 'model.completed') continue;
      const provider = evt.provider || evt.data?.provider || '';
      const modelApi = evt.modelApi || evt.data?.modelApi || '';
      if (!provider.includes('openai-codex') && !modelApi.includes('openai-codex')) continue;

      const ts = Date.parse(evt.ts || '');
      if (!Number.isFinite(ts) || ts < cutoff) continue;
      const usage = evt.usage || evt.data?.usage || {};
      const sessionId = evt.sessionId || evt.data?.sessionId || file;
      if (!last_turn_at || ts > Date.parse(last_turn_at)) last_turn_at = evt.ts;

      addCodexUsage(buckets.seven_day, usage);
      seen.seven_day.add(sessionId);
      if (ts >= now - 24 * 60 * 60 * 1000) {
        addCodexUsage(buckets.twenty_four_hour, usage);
        seen.twenty_four_hour.add(sessionId);
      }
      if (ts >= now - fiveHourMs) {
        addCodexUsage(buckets.five_hour, usage);
        seen.five_hour.add(sessionId);
        if (first_five_hour_turn_ms === null || ts < first_five_hour_turn_ms) {
          first_five_hour_turn_ms = ts;
        }
      }
    }
  }

  for (const key of Object.keys(buckets)) buckets[key].sessions = seen[key].size;

  const codex5hResetMs = first_five_hour_turn_ms === null
    ? 0
    : Math.max(0, first_five_hour_turn_ms + fiveHourMs - now);
  const codex5hResetAt = first_five_hour_turn_ms === null
    ? null
    : new Date(first_five_hour_turn_ms + fiveHourMs).toISOString();
  const codex5hPct = first_five_hour_turn_ms === null
    ? 0
    : Math.max(0, Math.min(100, Math.round(((now - first_five_hour_turn_ms) / fiveHourMs) * 100)));

  return {
    codex5h_pct: codex5hPct,
    codex5h_reset_at: codex5hResetAt,
    codex5h_resets_in: fmtResetMs(codex5hResetMs) || 'now',
    codex5h_turns: buckets.five_hour.turns,
    codex5h_sessions: buckets.five_hour.sessions,
    codex5h_tokens: buckets.five_hour.tokens,
    codex24h_turns: buckets.twenty_four_hour.turns,
    codex24h_sessions: buckets.twenty_four_hour.sessions,
    codex24h_tokens: buckets.twenty_four_hour.tokens,
    codex7d_turns: buckets.seven_day.turns,
    codex7d_sessions: buckets.seven_day.sessions,
    codex7d_tokens: buckets.seven_day.tokens,
    last_turn_at,
    normalized: true,
  };
}

app.get('/api/codex/usage', (req, res) => {
  const now = Date.now();
  if (_codexUsageCache && now - _codexUsageCacheTime < CODEX_USAGE_CACHE_TTL) {
    return res.json({ ..._codexUsageCache, cached: true });
  }
  try {
    const result = collectCodexUsage();
    _codexUsageCache = result;
    _codexUsageCacheTime = now;
    res.json({ ...result, cached: false });
  } catch (e) {
    res.status(500).json({ error: 'codex usage rollup failed', detail: e.message });
  }
});

// Proxy /api/* to the Agent HQ backend — avoids CORS entirely
// Mount at app level with pathFilter so the /api prefix is preserved
const apiProxy = createProxyMiddleware({
  target: API_TARGET,
  changeOrigin: true,
  pathFilter: '/api',
  on: {
    error(err, req, res) {
      console.error(`Proxy error: ${err.message}`);
      if (res.writeHead) {
        res.writeHead(502, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'API backend unreachable', detail: err.message }));
      }
    }
  }
});
app.use(apiProxy);

// Explicit download endpoint for archive files in /public
app.get('/download/:filename', (req, res) => {
  const filename = path.basename(req.params.filename); // no path traversal
  const filePath = path.join(__dirname, 'public', filename);
  if (!fs.existsSync(filePath)) return res.status(404).json({ error: 'File not found' });
  const stat = fs.statSync(filePath);
  res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);
  res.setHeader('Content-Type', 'application/octet-stream');
  res.setHeader('Content-Length', stat.size);
  fs.createReadStream(filePath).pipe(res);
});

// Force download for archive files served from /public
app.get(/\.(tar\.gz|zip|tar|gz|bz2|7z)$/, (req, res, next) => {
  res.setHeader('Content-Disposition', `attachment; filename="${path.basename(req.path)}"`);
  next();
});

// Serve static files from /public
app.use(express.static(path.join(__dirname, 'public')));

// SPA fallback — serve index.html for unmatched routes (skip file extensions)
app.get(/^\/?(?!api|public|\.).*$/, (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Agent HQ frontend → http://0.0.0.0:${PORT}`);
  console.log(`API proxy → ${API_TARGET}`);
});
