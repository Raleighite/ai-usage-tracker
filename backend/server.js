/**
 * AI Usage Tracker — Backend Server
 *
 * Serves Claude subscription quota and (optionally) Codex session usage
 * to the TTGO T-Display ESP32 firmware over HTTP on port 8030.
 *
 * Environment variables:
 *   PORT                    HTTP port (default: 8030)
 *   CLAUDE_OAUTH_TOKEN      Claude OAuth access token (required)
 *   CODEX_TRAJECTORY_DIR    Path to OpenClaw agents dir (optional; enables /api/codex/usage)
 */

'use strict';

const express = require('express');
const https   = require('https');
const fs      = require('fs');
const path    = require('path');
const os      = require('os');

const app  = express();
const PORT = parseInt(process.env.PORT || '8030', 10);

// ── Token resolution ──────────────────────────────────────────────────────────

function getClaudeOAuthToken() {
  if (process.env.CLAUDE_OAUTH_TOKEN) return process.env.CLAUDE_OAUTH_TOKEN;

  const credPath = path.join(os.homedir(), '.claude', '.credentials.json');
  if (fs.existsSync(credPath)) {
    try {
      const creds = JSON.parse(fs.readFileSync(credPath, 'utf8'));
      const token = creds?.claudeAiOauth?.accessToken;
      if (token) return token;
    } catch (_) {}
  }

  // macOS keychain (Claude Code stores credentials here)
  if (process.platform === 'darwin') {
    try {
      const { execSync } = require('child_process');
      const raw = execSync('security find-generic-password -s "Claude Code-credentials" -w', {
        timeout: 5000, stdio: ['ignore', 'pipe', 'ignore'],
      }).toString().trim();
      const creds = JSON.parse(raw);
      const token = creds?.claudeAiOauth?.accessToken;
      if (token) return token;
    } catch (_) {}
  }

  throw new Error(
    'No Claude OAuth token found. Set CLAUDE_OAUTH_TOKEN env var, ' +
    'or run `claude login` to write ~/.claude/.credentials.json.'
  );
}

// ── Helpers ───────────────────────────────────────────────────────────────────

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
  return { reset_at: ts, resets_in: fmtResetMs(diffMs > 0 ? diffMs : 0) };
}

// ── Claude usage ──────────────────────────────────────────────────────────────

let _claudeCache = null;
let _claudeCacheTime = 0;
const CLAUDE_TTL = 60_000;

function fetchClaudeOAuthUsage(token) {
  return new Promise((resolve, reject) => {
    const req = https.request({
      hostname: 'api.anthropic.com',
      path: '/api/oauth/usage',
      method: 'GET',
      headers: {
        Authorization: `Bearer ${token}`,
        'anthropic-beta': 'oauth-2025-04-20',
        'User-Agent': 'ai-usage-tracker-backend/1.0',
      },
    }, (res) => {
      let body = '';
      res.on('data', c => body += c);
      res.on('end', () => {
        if (res.statusCode !== 200)
          return reject(new Error(`Anthropic ${res.statusCode}: ${body}`));
        try { resolve(JSON.parse(body)); }
        catch (e) { reject(new Error('Bad JSON from Anthropic')); }
      });
    });
    req.on('error', reject);
    req.end();
  });
}

function normalizeClaudeUsage(raw) {
  let pct5h = null, pct7d = null, resetAt = null, resetsIn = null;
  let sonnet7dPct = null, sonnet7dResetAt = null, sonnet7dResetsIn = null;
  let total7dResetAt = null, total7dResetsIn = null;

  if (raw.five_hour?.utilization !== undefined) {
    pct5h    = raw.five_hour.utilization;
    pct7d    = raw.seven_day?.utilization ?? 0;
    const sr = resetInfo(raw.five_hour.resets_at);
    resetAt  = sr.reset_at;
    resetsIn = sr.resets_in;
    const snr = resetInfo(raw.seven_day_sonnet?.resets_at);
    sonnet7dPct      = raw.seven_day_sonnet?.utilization ?? null;
    sonnet7dResetAt  = snr.reset_at;
    sonnet7dResetsIn = snr.resets_in;
    const tr = resetInfo(raw.seven_day?.resets_at);
    total7dResetAt  = tr.reset_at;
    total7dResetsIn = tr.resets_in;
  } else if (raw.session?.percent !== undefined) {
    pct5h    = Math.round(raw.session.percent);
    pct7d    = Math.round(raw.weekly?.percent ?? 0);
    resetAt  = raw.weekly?.resetAt ?? null;
    resetsIn = fmtResetMs(raw.session.resetMs);
  } else if (raw.usage_5h?.utilization !== undefined) {
    pct5h   = Math.round(raw.usage_5h.utilization * 100);
    pct7d   = Math.round((raw.usage_7d?.utilization ?? 0) * 100);
    resetAt = raw.usage_5h.reset_at ?? null;
    if (resetAt) {
      const diffMs = new Date(resetAt) - Date.now();
      resetsIn = fmtResetMs(diffMs > 0 ? diffMs : 0);
    }
  } else {
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
  if (_claudeCache && now - _claudeCacheTime < CLAUDE_TTL)
    return res.json({ ..._claudeCache, cached: true });
  try {
    const token  = getClaudeOAuthToken();
    const raw    = await fetchClaudeOAuthUsage(token);
    const result = normalizeClaudeUsage(raw);
    _claudeCache     = result;
    _claudeCacheTime = now;
    res.json({ ...result, cached: false });
  } catch (e) {
    console.error('[claude] fetch failed:', e.message);
    res.status(500).json({ error: 'claude usage fetch failed', detail: e.message });
  }
});

// ── Codex usage (optional) ───────────────────────────────────────────────────

const CODEX_DIR = process.env.CODEX_TRAJECTORY_DIR || '';

let _codexCache = null;
let _codexCacheTime = 0;
const CODEX_TTL = 60_000;

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
    if (fs.statSync(p).mtimeMs >= cutoffMs) out.push(p);
  }
  return out;
}

function collectCodexUsage(root) {
  const now        = Date.now();
  const fiveHourMs = 5 * 60 * 60 * 1000;
  const cutoff     = now - 7 * 24 * 60 * 60 * 1000;
  const mk = () => ({ turns: 0, sessions: 0, tokens: 0 });
  const buckets = { five_hour: mk(), twenty_four_hour: mk(), seven_day: mk() };
  const seen = { five_hour: new Set(), twenty_four_hour: new Set(), seven_day: new Set() };
  let last_turn_at = null;
  let first_five_hour_turn_ms = null;

  for (const file of walkTrajectoryFiles(root, cutoff)) {
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
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
      const sid   = evt.sessionId || evt.data?.sessionId || file;
      if (!last_turn_at || ts > Date.parse(last_turn_at)) last_turn_at = evt.ts;

      const add = (b, s) => { b.turns++; b.tokens += usage.total || 0; s.add(sid); };
      add(buckets.seven_day, seen.seven_day);
      if (ts >= now - 24 * 60 * 60 * 1000) add(buckets.twenty_four_hour, seen.twenty_four_hour);
      if (ts >= now - fiveHourMs) {
        add(buckets.five_hour, seen.five_hour);
        if (first_five_hour_turn_ms === null || ts < first_five_hour_turn_ms)
          first_five_hour_turn_ms = ts;
      }
    }
  }
  for (const k of Object.keys(buckets)) buckets[k].sessions = seen[k].size;

  const resetMs = first_five_hour_turn_ms === null
    ? 0 : Math.max(0, first_five_hour_turn_ms + fiveHourMs - now);
  const pct = first_five_hour_turn_ms === null
    ? 0 : Math.min(100, Math.round(((now - first_five_hour_turn_ms) / fiveHourMs) * 100));

  return {
    codex5h_pct: pct,
    codex5h_reset_at: first_five_hour_turn_ms === null
      ? null : new Date(first_five_hour_turn_ms + fiveHourMs).toISOString(),
    codex5h_resets_in: fmtResetMs(resetMs) || 'now',
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
  if (!CODEX_DIR)
    return res.status(404).json({ error: 'CODEX_TRAJECTORY_DIR not configured' });
  const now = Date.now();
  if (_codexCache && now - _codexCacheTime < CODEX_TTL)
    return res.json({ ..._codexCache, cached: true });
  try {
    const result = collectCodexUsage(CODEX_DIR);
    _codexCache     = result;
    _codexCacheTime = now;
    res.json({ ...result, cached: false });
  } catch (e) {
    console.error('[codex] rollup failed:', e.message);
    res.status(500).json({ error: 'codex usage rollup failed', detail: e.message });
  }
});

// ── Health check ──────────────────────────────────────────────────────────────

app.get('/health', (_, res) => res.json({ ok: true, ts: new Date().toISOString() }));

// ── Start ─────────────────────────────────────────────────────────────────────

app.listen(PORT, '0.0.0.0', () => {
  console.log(`AI Usage Tracker backend listening on :${PORT}`);
  console.log(`  Claude:  http://localhost:${PORT}/api/claude/usage`);
  console.log(`  Codex:   ${CODEX_DIR ? `http://localhost:${PORT}/api/codex/usage` : 'disabled (set CODEX_TRAJECTORY_DIR)'}`);
});
