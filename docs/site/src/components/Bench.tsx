import React, {createContext, useContext, useMemo, useState} from 'react';
import {useColorMode} from '@docusaurus/theme-common';
import {
  Chart as ChartJS,
  BarElement,
  CategoryScale,
  LinearScale,
  Tooltip,
} from 'chart.js';
import {Bar} from 'react-chartjs-2';
import raw from '@site/src/generated/bench.json';
import styles from './Bench.module.css';

ChartJS.register(BarElement, CategoryScale, LinearScale, Tooltip);

// The shape of the files the benchmark suite writes (benchmarks/suite.py, schema 1); scripts/prepare.py copies the published ones into bench.json.
type Round = {rps: number; us_per_req: number; p50_ms: number; p99_ms: number; p999_ms: number; errors: number};
type Row = {
  suite: string;
  runtime: string;
  server: string;
  variant: string;
  io_threads: number | null;
  unit: string;
  failed?: string;
  rps?: number;
  rps_sd_pct?: number;
  rps_min?: number;
  rps_max?: number;
  us_per_req?: number;
  p50_ms?: number;
  p99_ms?: number;
  p999_ms?: number;
  errors?: number;
  rounds?: Round[];
};
type Python = {
  label: string;
  version: string;
  gil: boolean;
  libs: Record<string, string>;
  gil_after_import?: Record<string, boolean>;
};
type Idle = {server: string; connected: number; failed: number; rss_before_kib: number; rss_after_kib: number; kib_per_connection: number};
type Hardware = {
  cpu?: string;
  vendor?: string;
  arch?: string;
  sockets?: number;
  physical_cores?: number;
  logical_cpus?: number;
  max_mhz?: number;
  l2_cache?: string;
  l3_cache?: string;
  memory_gib?: number;
  swap_gib?: number;
  os?: string;
  kernel?: string;
  wsl?: boolean;
  hypervisor?: string;
  model?: string;
  is_laptop?: boolean;
  on_battery?: boolean;
};
type Run = {
  id: string;
  meta: {
    date: string;
    purpose: string;
    hardware?: Hardware;
    server_cpus: string;
    load_cpus: string;
    governor: string;
    tree: string;
    compiler: string;
    note: string;
    pythons: Python[];
    settings: {conc: number; seconds: number; rounds: number; client_conc: number};
  };
  results: Row[];
  idle: Idle[];
};

const runs = (raw as unknown as {runs: Run[]}).runs;

const COLORS: Record<string, string> = {
  httpp: '#2f6fed',
  uvicorn: '#7a8599',
  fastapi: '#14b8a6',
  aiohttp: '#f59e0b',
  drogon: '#ef4444',
  websockets: '#a855f7',
  requests: '#84cc16',
  httpx: '#ec4899',
  'httpx AsyncClient': '#db2777',
};

const METRICS = {
  rps: {label: 'requests per second', better: 'higher is better', higher: true},
  p50_ms: {label: 'latency p50 (ms)', better: 'lower is better', higher: false},
  p99_ms: {label: 'latency p99 (ms)', better: 'lower is better', higher: false},
  us_per_req: {label: 'CPU per request (µs)', better: 'lower is better', higher: false},
} as const;
type Metric = keyof typeof METRICS;

const nf = new Intl.NumberFormat('en-US', {maximumFractionDigits: 1});

export function runtimeLabel(rt: string): string {
  if (rt === 'cpp') return 'C++';
  const m = /^py(\d)(\d+)(t?)$/.exec(rt);
  return m ? `Python ${m[1]}.${m[2]}${m[3]}` : rt;
}

const Ctx = createContext<Run | null>(null);

// Wraps a page: picks the run (when several are published) and shows a note when there is none.
export function BenchProvider({children}: {children: React.ReactNode}) {
  const [id, setId] = useState(runs[0]?.id);
  const run = runs.find((r) => r.id === id) ?? runs[0];
  // Without a run the children still render (their headings stay), and every chart in them renders nothing.
  return (
    <Ctx.Provider value={run ?? null}>
      {!run && (
        <p className={styles.empty}>
          No benchmark results are published yet. Run the suite (<code>benchmarks/README.md</code>) and commit the <code>.json</code> it writes to{' '}
          <code>benchmarks/results/</code>.
        </p>
      )}
      {run && runs.length > 1 && (
        <p>
          Run:{' '}
          <select className={styles.select} value={run.id} onChange={(e) => setId(e.target.value)}>
            {runs.map((r) => (
              <option key={r.id} value={r.id}>
                {r.meta.date.slice(0, 10)} {r.meta.hardware?.cpu ?? ''}{r.meta.purpose === 'smoke' ? ' (smoke test)' : ''}
              </option>
            ))}
          </select>
        </p>
      )}
      {children}
    </Ctx.Provider>
  );
}

function useRun(): Run | null {
  return useContext(Ctx);
}

// The machine and the software of the run, so that nobody reads the numbers without them.
export function BenchMeta() {
  const run = useRun();
  if (!run) return null;
  const m = run.meta;
  const hw = m.hardware ?? {};
  const reenabled = m.pythons.flatMap((p) =>
    p.gil ? [] : Object.entries(p.gil_after_import ?? {}).filter(([, on]) => on).map(([lib]) => `${lib} (Python ${p.label})`),
  );
  return (
    <>
      {hw.on_battery && <p className={styles.warn}>The machine was running on battery: expect low and noisy numbers.</p>}
      {m.purpose === 'smoke' && <p className={styles.warn}>This is a smoke test of the benchmark machinery, not a measurement. Do not read the numbers.</p>}
      <table className={styles.meta}>
        <tbody>
          <tr><th>Date</th><td>{m.date.slice(0, 16).replace('T', ' ')}</td></tr>
          {hw.model && <tr><th>Machine</th><td>{hw.model}{hw.is_laptop ? ' (laptop)' : ''}</td></tr>}
          <tr>
            <th>CPU</th>
            <td>
              {hw.cpu ?? 'unknown'}
              {hw.physical_cores ? `, ${hw.physical_cores} cores / ${hw.logical_cpus} threads` : hw.logical_cpus ? `, ${hw.logical_cpus} threads` : ''}
              {hw.max_mhz ? `, up to ${nf.format(hw.max_mhz / 1000)} GHz` : ''}
              {hw.l3_cache ? `, L3 ${hw.l3_cache}` : ''}
            </td>
          </tr>
          {hw.memory_gib !== undefined && <tr><th>Memory</th><td>{nf.format(hw.memory_gib)} GiB</td></tr>}
          <tr>
            <th>System</th>
            <td>
              {hw.os ?? 'Linux'}, kernel {hw.kernel ?? '?'}{hw.wsl ? ', WSL' : ''}{hw.hypervisor ? `, virtual machine (${hw.hypervisor})` : ''}, governor {m.governor}
            </td>
          </tr>
          <tr><th>Pinning</th><td>server on CPUs {m.server_cpus}, load generator on CPUs {m.load_cpus}</td></tr>
          {m.compiler && <tr><th>C++</th><td>{m.compiler}</td></tr>}
          {m.pythons.map((p) => (
            <tr key={p.label}>
              <th>Python {p.label}</th>
              <td>
                {p.version}, GIL {p.gil ? 'on' : 'off'}; {Object.entries(p.libs).map(([n, v]) => `${n} ${v}`).join(', ')}
              </td>
            </tr>
          ))}
          <tr><th>Method</th><td>{m.settings.rounds} interleaved rounds of {m.settings.seconds} s, median shown, {m.settings.conc} connections</td></tr>
          <tr><th>Source tree</th><td><code>{m.tree}</code></td></tr>
        </tbody>
      </table>
      {reenabled.length > 0 && (
        <p className={styles.warn}>The GIL came back on a free-threaded interpreter because of: {reenabled.join(', ')}. Read those rows with that in mind.</p>
      )}
    </>
  );
}

function value(r: Row, metric: Metric): number {
  return (r[metric] as number | undefined) ?? 0;
}

type ChartProps = {suite: string; title: string; what?: string; metric?: Metric};

// One chart for one part of the suite (hello, json, cpu, latency, ws, client), with the filters that make sense for its rows.
export function BenchChart({suite, title, what, metric: first}: ChartProps) {
  const run = useRun();
  const {colorMode} = useColorMode();
  const rows = useMemo(() => (run ? run.results.filter((r) => r.suite === suite) : []), [run, suite]);
  const good = rows.filter((r) => !r.failed && r.rps !== undefined);
  const runtimes = Array.from(new Set(good.map((r) => r.runtime)));
  const threads = Array.from(new Set(good.map((r) => r.io_threads).filter((x): x is number => x !== null))).sort((a, b) => a - b);
  const [metric, setMetric] = useState<Metric>(first ?? (suite === 'latency' ? 'p50_ms' : 'rps'));
  const [rt, setRt] = useState<string>('all');
  const [io, setIo] = useState<number | 'all'>(threads.includes(4) ? 4 : (threads[0] ?? 'all'));

  if (!run || good.length === 0) return null;
  const shown = good
    .filter((r) => rt === 'all' || r.runtime === rt)
    .filter((r) => io === 'all' || r.io_threads === null || r.io_threads === io)
    .sort((a, b) => (METRICS[metric].higher ? value(b, metric) - value(a, metric) : value(a, metric) - value(b, metric)));
  const label = (r: Row) => `${runtimeLabel(r.runtime)} · ${r.variant}${r.io_threads !== null ? ` (io ${r.io_threads})` : ''}`;
  const dark = colorMode === 'dark';
  const text = dark ? '#d6d9e0' : '#333';
  const grid = dark ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.08)';
  const families = Array.from(new Set(shown.map((r) => r.server)));
  const failed = rows.filter((r) => r.failed).length;

  return (
    <div className={styles.box}>
      <h3 className={styles.title}>{title}</h3>
      {what && <p className={styles.what}>{what}</p>}
      <div className={styles.controls}>
        <span className={styles.group}>
          {(Object.keys(METRICS) as Metric[]).map((k) => (
            <button key={k} type="button" className={styles.chip} aria-pressed={metric === k} onClick={() => setMetric(k)}>
              {METRICS[k].label}
            </button>
          ))}
        </span>
        {runtimes.length > 1 && (
          <span className={styles.group}>
            {['all', ...runtimes].map((x) => (
              <button key={x} type="button" className={styles.chip} aria-pressed={rt === x} onClick={() => setRt(x)}>
                {x === 'all' ? 'all runtimes' : runtimeLabel(x)}
              </button>
            ))}
          </span>
        )}
        {threads.length > 1 && (
          <span className={styles.group}>
            I/O threads:
            {([...threads, 'all'] as (number | 'all')[]).map((x) => (
              <button key={x} type="button" className={styles.chip} aria-pressed={io === x} onClick={() => setIo(x)}>
                {x}
              </button>
            ))}
          </span>
        )}
      </div>
      <div className={styles.legend}>
        {families.map((f) => (
          <span key={f}>
            <span className={styles.swatch} style={{background: COLORS[f] ?? '#9ca3af'}} />
            {f}
          </span>
        ))}
        <span>({METRICS[metric].better})</span>
      </div>
      <div style={{height: shown.length * 26 + 70}}>
        <Bar
          data={{
            labels: shown.map(label),
            datasets: [{data: shown.map((r) => value(r, metric)), backgroundColor: shown.map((r) => COLORS[r.server] ?? '#9ca3af'), borderRadius: 3}],
          }}
          options={{
            indexAxis: 'y',
            maintainAspectRatio: false,
            animation: false,
            plugins: {
              tooltip: {
                callbacks: {
                  label: (c) => {
                    const r = shown[c.dataIndex];
                    const extra = metric === 'rps' && r.rps_min !== undefined ? ` [${nf.format(r.rps_min)} to ${nf.format(r.rps_max ?? 0)}], sd ${nf.format(r.rps_sd_pct ?? 0)} %` : '';
                    return `${nf.format(value(r, metric))} ${r.unit === 'msg/s' && metric === 'rps' ? 'msg/s' : ''}${extra}`;
                  },
                },
              },
            },
            scales: {
              x: {title: {display: true, text: METRICS[metric].label, color: text}, ticks: {color: text}, grid: {color: grid}, beginAtZero: true},
              y: {ticks: {color: text, font: {size: 11}}, grid: {display: false}},
            },
          }}
        />
      </div>
      <details className={styles.details}>
        <summary>Numbers</summary>
        <table>
          <thead>
            <tr><th>variant</th><th className={styles.num}>{rows[0]?.unit ?? 'req/s'}</th><th className={styles.num}>sd %</th><th className={styles.num}>p50 ms</th><th className={styles.num}>p99 ms</th><th className={styles.num}>µs CPU</th><th className={styles.num}>errors</th></tr>
          </thead>
          <tbody>
            {shown.map((r) => (
              <tr key={label(r)}>
                <td>{label(r)}</td>
                <td className={styles.num}>{nf.format(r.rps ?? 0)}</td>
                <td className={styles.num}>{nf.format(r.rps_sd_pct ?? 0)}</td>
                <td className={styles.num}>{nf.format(r.p50_ms ?? 0)}</td>
                <td className={styles.num}>{nf.format(r.p99_ms ?? 0)}</td>
                <td className={styles.num}>{nf.format(r.us_per_req ?? 0)}</td>
                <td className={styles.num}>{r.errors ?? 0}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>
      {failed > 0 && <p className={styles.what}>{failed} variant(s) failed to run and are not shown.</p>}
    </div>
  );
}

// Memory of the server with N idle WebSocket connections.
export function BenchIdle() {
  const run = useRun();
  const {colorMode} = useColorMode();
  if (!run || run.idle.length === 0) return null;
  const rows = [...run.idle].sort((a, b) => a.kib_per_connection - b.kib_per_connection);
  const dark = colorMode === 'dark';
  const text = dark ? '#d6d9e0' : '#333';
  const family = (s: string) => Object.keys(COLORS).find((k) => s.toLowerCase().includes(k)) ?? '';
  return (
    <div className={styles.box}>
      <h3 className={styles.title}>Memory with idle WebSocket connections</h3>
      <p className={styles.what}>{nf.format(rows[0].connected)} connections opened and left alone; the server's memory growth per connection (one run, not a median).</p>
      <div style={{height: rows.length * 26 + 70}}>
        <Bar
          data={{
            labels: rows.map((r) => r.server),
            datasets: [{data: rows.map((r) => r.kib_per_connection), backgroundColor: rows.map((r) => COLORS[family(r.server)] ?? '#9ca3af'), borderRadius: 3}],
          }}
          options={{
            indexAxis: 'y',
            maintainAspectRatio: false,
            animation: false,
            scales: {
              x: {title: {display: true, text: 'KiB per connection (lower is better)', color: text}, ticks: {color: text}, beginAtZero: true},
              y: {ticks: {color: text, font: {size: 11}}, grid: {display: false}},
            },
          }}
        />
      </div>
    </div>
  );
}
