import {
  ArrowRight,
  BookOpen,
  Check,
  Copy,
  Cpu,
  Database,
  FileCode2,
  Gauge,
  Layers,
  Lock,
  Network,
  // Sparkles,
  Terminal,
  Zap,
} from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { CodeBlock } from "../components/CodeBlock";
import { GitHubIcon } from "../components/GitHubIcon";
import { ThemeSwitcher } from "../components/ThemeSwitcher";
import { GITHUB_URL } from "../data/docsData";

const CODE_TABS = [
  {
    id: "routing",
    label: "Two-Layer Routing",
    filename: "src/modules/users/users.router.ts",
    code: `import { createLampRegistry, createLampRouter, createErrorHandler, s } from "lamp-app";
import { localUsersRouter } from "./local_users/local_users.router.ts";

const usersRegistry = createLampRegistry({
  onError: createErrorHandler({ log: true }),
}).add("get", {
  input: s.object({ id: s.string({ minLength: 1 }) }),
  output: s.object({ id: s.string(), name: s.string() }),
  cache: { ttl: 60 },
  handler: ({ input }) => ({ id: input.id, name: "Ada Lovelace" }),
});

// Resolves to "app.users.get" and "app.users.local_users.*"
export const usersRouter = createLampRouter({
  description: "Users route module",
  target: "users",
  registry: [usersRegistry],
  nesting: [localUsersRouter],
});`,
  },
  {
    id: "parallel",
    label: "C++ Parallelism & Files",
    filename: "src/modules/compute/compute.registry.ts",
    code: `import { createLampRegistry, s } from "lamp-app";

export const computeRegistry = createLampRegistry()
  .add("parallel", {
    input: s.object({ items: s.array(s.number()) }),
    output: s.object({ results: s.array(s.number()) }),
    handler: async ({ input, ctx }) => ({
      results: await ctx.parallel(input.items, (n) => (n * 2654435761) >>> 0),
    }),
  })
  .add("upload", {
    input: s.object({ file: s.file({ maxSizeBytes: 5 * 1024 * 1024 }) }),
    output: s.object({ name: s.string(), bytes: s.number() }),
    handler: ({ input }) => ({
      name: input.file.name,
      bytes: input.file.data.byteLength,
    }),
  });`,
  },
  {
    id: "bootstrap",
    label: "App & Origin Policy",
    filename: "src/app.ts",
    code: `import { createLampApp } from "lamp-app";
import { rootRouter } from "./router.ts";

const app = createLampApp({
  transport: "lmp1-binary",
  serialization: "lcb1",
  execution: "parallel-dynamic",
  log: true,
  cache: { enabled: true, defaultTtl: 60, maxTtlLimitSeconds: 300 },
  allowOrigin: (expose, callback) => {
    expose(["http://localhost:5173"]);
    callback();
    return true;
  },
});

app.registerRouter(rootRouter);
await app.start();`,
  },
  {
    id: "client",
    label: "Node & Browser Client",
    filename: "client.ts",
    code: `import { createLampClient } from "lamp-client/browser";

const client = createLampClient({ url: "ws://127.0.0.1:3000" });

const manifest = await client.manifest();
const { data, meta } = await client.callWithMeta("app.users.get", { id: "u_1" });
console.log(data, { cacheHit: meta.cacheHit, responseBytes: meta.responseBytes });`,
  },
] as const;

interface BenchmarkMetric {
  readonly label: string;
  readonly expressLabel: string;
  readonly lampLabel: string;
  readonly expressPct: number;
  readonly lampPct: number;
  readonly badge: string;
  readonly note: string;
}

const SUITE_A_METRICS: readonly BenchmarkMetric[] = [
  {
    label: "MongoDB CRUD Throughput (200 ops/run)",
    expressLabel: "Express: 291.6 RPS (3.369 ms)",
    lampLabel: "Lamp: 350.2 RPS (2.898 ms)",
    expressPct: 83,
    lampPct: 100,
    badge: "1.17x faster",
    note: "Higher is better",
  },
  {
    label: "Cached Read Latency (Wire Cache Hit)",
    expressLabel: "Express DB: 2.582 ms",
    lampLabel: "Lamp Wire Cache: 0.590 ms",
    expressPct: 100,
    lampPct: 23,
    badge: "3.91x faster",
    note: "Lower is better",
  },
  {
    label: "Wire Bandwidth (125 CRUD ops/run)",
    expressLabel: "Express JSON: 70,565 B",
    lampLabel: "Lamp LCB1 Binary: 31,065 B",
    expressPct: 100,
    lampPct: 44,
    badge: "56% saved (2.27x less)",
    note: "Lower is better",
  },
];

const SUITE_B_METRICS: readonly BenchmarkMetric[] = [
  {
    label: "Pure Runtime Throughput (750 ops/run)",
    expressLabel: "Express: 1,091.2 RPS (0.884 ms)",
    lampLabel: "C++ Lamp: 1,482.0 RPS (0.676 ms)",
    expressPct: 74,
    lampPct: 100,
    badge: "1.36x higher RPS",
    note: "Higher is better",
  },
  {
    label: "CPU Time (500 reads/run)",
    expressLabel: "Express: 278.13 ms",
    lampLabel: "C++ Wire Cache: 63.28 ms",
    expressPct: 100,
    lampPct: 23,
    badge: "4.4x less CPU",
    note: "Lower is better",
  },
  {
    label: "Burst Multithreading Wall Time",
    expressLabel: "Express Single-Thread: 1,333.73 ms",
    lampLabel: "Lamp DynamicWorkerPool: 344.75 ms",
    expressPct: 100,
    lampPct: 26,
    badge: "3.87x faster",
    note: "Lower is better",
  },
  {
    label: "Concurrent Light Read p95 Under Burst",
    expressLabel: "Express Blocked Event Loop: 1,314.24 ms",
    lampLabel: "Lamp Isolated Pool: 1.325 ms",
    expressPct: 100,
    lampPct: 2,
    badge: "991.5x lower p95",
    note: "Lower is better",
  },
];

const STACK_LABELS = [
  "Hero",
  "Why Lamp",
  "Code",
  "Benchmarks",
  "Architecture",
];

export function LandingPage() {
  const [copiedCmd, setCopiedCmd] = useState(false);
  const [activeTab, setActiveTab] = useState<(typeof CODE_TABS)[number]["id"]>("routing");
  const [benchSuite, setBenchSuite] = useState<"A" | "B">("B");
  const [activeStack, setActiveStack] = useState(0);
  const slotRefs = useRef<(HTMLDivElement | null)[]>([]);

  const heroCmd = "npx create-lamp-app@latest my-app";

  const handleCopyHeroCmd = async () => {
    await navigator.clipboard.writeText(heroCmd);
    setCopiedCmd(true);
    setTimeout(() => setCopiedCmd(false), 1800);
  };

  useEffect(() => {
    const onScroll = () => {
      const mid = window.scrollY + window.innerHeight * 0.4;
      let current = 0;
      slotRefs.current.forEach((el, idx) => {
        if (el && el.offsetTop <= mid) {
          current = idx;
        }
      });
      setActiveStack(current);
    };
    window.addEventListener("scroll", onScroll, { passive: true });
    return () => window.removeEventListener("scroll", onScroll);
  }, []);

  const jumpToStack = (index: number) => {
    const el = slotRefs.current[index];
    if (el) {
      window.scrollTo({
        top: el.offsetTop,
        behavior: "smooth",
      });
    }
  };

  const currentTab = CODE_TABS.find((t) => t.id === activeTab) ?? CODE_TABS[0];
  const metrics = benchSuite === "A" ? SUITE_A_METRICS : SUITE_B_METRICS;

  return (
    <div className="landing-root">
      {/* Floating minimal controls (No separate header) */}
      <div className="floating-controls">
        <Link to="/docs/concept" className="float-pill">
          <BookOpen size={14} />
          <span>Docs</span>
        </Link>
        <a
          href={GITHUB_URL}
          target="_blank"
          rel="noreferrer"
          className="float-pill"
        >
          <GitHubIcon size={14} />
          <span>GitHub</span>
        </a>
        <ThemeSwitcher />
      </div>

      {/* Vertical Section Stack Indicator */}
      <nav className="stack-dots" aria-label="Section stack navigation">
        {STACK_LABELS.map((label, idx) => (
          <button
            key={label}
            type="button"
            className={`stack-dot ${activeStack === idx ? "active" : ""}`}
            onClick={() => jumpToStack(idx)}
            title={label}
            aria-label={`Jump to ${label}`}
          >
            <span className="dot-label">{label}</span>
            <i />
          </button>
        ))}
      </nav>

      {/* Gentle Section Stack Container */}
      <div className="section-stack">
        {/* STACK 1: Broad Hero Section */}
        <div
          className="stack-slot"
          ref={(el) => {
            slotRefs.current[0] = el;
          }}
        >
          <section className="stack-card hero-card" style={{ zIndex: 1 }}>
            <div className="hero-bg" aria-hidden="true">
              <div className="hero-orb orb-1" />
              <div className="hero-orb orb-2" />
              <div className="hero-grid" />
            </div>

            <div className="hero-inner">
              {/* <div className="hero-badge">
                <Sparkles size={14} />
                <span>Lamp Framework v0.4.1 · C++20 + Embedded V8 + TypeScript</span>
              </div> */}

              <h1 className="hero-title">
                Drop REST.
                <br />
                <span>Run Binary Operations at Native Speed.</span>
              </h1>

              <p className="hero-subtitle">
                Lamp pairs a modular TypeScript <strong>Router + Registry</strong> SDK with a
                pure-binary <code>LMP1 / LCB1</code> C++20 runtime, elastic{" "}
                <code>DynamicWorkerPool</code> multithreading, and zero-V8 wire caching.
              </p>

              <div className="hero-cmd-box">
                <Terminal size={16} className="cmd-icon" />
                <code>{heroCmd}</code>
                <button
                  type="button"
                  className="cmd-copy-btn"
                  onClick={() => void handleCopyHeroCmd()}
                >
                  {copiedCmd ? <Check size={14} /> : <Copy size={14} />}
                  <span>{copiedCmd ? "Copied!" : "Copy"}</span>
                </button>
              </div>

              <div className="hero-actions">
                <Link to="/docs/concept" className="btn-primary">
                  <span>Read Documentation</span>
                  <ArrowRight size={16} />
                </Link>
                <a
                  href={GITHUB_URL}
                  target="_blank"
                  rel="noreferrer"
                  className="btn-secondary"
                >
                  <GitHubIcon size={16} />
                  <span>View on GitHub</span>
                </a>
              </div>
            </div>
          </section>
        </div>

        {/* STACK 2: Why Lamp Framework Is Best */}
        <div
          className="stack-slot"
          ref={(el) => {
            slotRefs.current[1] = el;
          }}
        >
          <section className="stack-card feature-card-section" style={{ zIndex: 2 }}>
            <div className="section-inner">
              <div className="section-eyebrow">Why Lamp</div>
              <h2 className="section-heading">
                Engineered where Node.js REST hits a wall.
              </h2>
              <div className="features-grid">
                <div className="feat-item">
                  <div className="feat-icon"><Zap size={20} /></div>
                  <h3>Pure-Binary LMP1 / LCB1</h3>
                  <p>
                    14-byte multiplexed frames with string-interned binary payloads cut wire
                    bandwidth by <strong>56%–63%</strong> vs JSON REST.
                  </p>
                </div>
                <div className="feat-item">
                  <div className="feat-icon"><Layers size={20} /></div>
                  <h3>Two-Layer Router &amp; Registry</h3>
                  <p>
                    Separate request structure (<code>createLampRouter</code>) from business
                    logic (<code>createLampRegistry</code>) with arbitrary nesting.
                  </p>
                </div>
                <div className="feat-item">
                  <div className="feat-icon"><Cpu size={20} /></div>
                  <h3>True Multi-Core V8 (ctx.parallel)</h3>
                  <p>
                    Distribute CPU work across C++ <code>DynamicWorkerPool</code> isolates so
                    concurrent reads stay at <strong>1.32 ms p95</strong>.
                  </p>
                </div>
                <div className="feat-item">
                  <div className="feat-icon"><Database size={20} /></div>
                  <h3>Full-Cycle Wire Cache</h3>
                  <p>
                    Cache hits return pre-serialized <code>LCB1</code> bytes directly from C++ in{" "}
                    <strong>0.59 ms</strong>—never touching V8.
                  </p>
                </div>
                <div className="feat-item">
                  <div className="feat-icon"><FileCode2 size={20} /></div>
                  <h3>Native Binary s.file()</h3>
                  <p>
                    Stream <code>Uint8Array</code> and Web <code>File</code> payloads directly in
                    binary frames with zero base64 bloat.
                  </p>
                </div>
                <div className="feat-item">
                  <div className="feat-icon"><Lock size={20} /></div>
                  <h3>Scoped Errors &amp; Origin Policy</h3>
                  <p>
                    Attach <code>createErrorHandler</code> at registry or route level, with native
                    C++ <code>allowOrigin</code> and RBAC enforcement.
                  </p>
                </div>
              </div>
            </div>
          </section>
        </div>

        {/* STACK 3: Interactive Code Showcase */}
        <div
          className="stack-slot"
          ref={(el) => {
            slotRefs.current[2] = el;
          }}
        >
          <section className="stack-card code-card-section" style={{ zIndex: 3 }}>
            <div className="section-inner">
              <div className="section-eyebrow">Developer Experience</div>
              <h2 className="section-heading">
                TypeScript contracts. Native C++20 execution.
              </h2>

              <div className="code-tabs">
                {CODE_TABS.map((tab) => (
                  <button
                    key={tab.id}
                    type="button"
                    className={`code-tab-btn ${activeTab === tab.id ? "active" : ""}`}
                    onClick={() => setActiveTab(tab.id)}
                  >
                    {tab.label}
                  </button>
                ))}
              </div>

              <div className="showcase-code-wrap">
                <CodeBlock
                  code={currentTab.code}
                  filename={currentTab.filename}
                  language="ts"
                />
              </div>
            </div>
          </section>
        </div>

        {/* STACK 4: Express REST vs Lamp Benchmark Comparison */}
        <div
          className="stack-slot"
          ref={(el) => {
            slotRefs.current[3] = el;
          }}
        >
          <section className="stack-card bench-card-section" style={{ zIndex: 4 }}>
            <div className="section-inner">
              <div className="bench-header">
                <div>
                  <div className="section-eyebrow">
                    <Gauge size={14} /> 20-Run Averaged Empirical Benchmark
                  </div>
                  <h2 className="section-heading">Lamp vs Express.js REST</h2>
                </div>

                <div className="bench-toggle">
                  <button
                    type="button"
                    className={benchSuite === "B" ? "active" : ""}
                    onClick={() => setBenchSuite("B")}
                  >
                    Suite B: Pure Runtime (C++ vs Express)
                  </button>
                  <button
                    type="button"
                    className={benchSuite === "A" ? "active" : ""}
                    onClick={() => setBenchSuite("A")}
                  >
                    Suite A: MongoDB CRUD Parity
                  </button>
                </div>
              </div>

              <div className="bench-grid">
                {metrics.map((m) => (
                  <div key={m.label} className="bench-card">
                    <div className="bench-card-top">
                      <h3>{m.label}</h3>
                      <span className="bench-badge">{m.badge}</span>
                    </div>
                    <div className="bench-bars">
                      <div className="bar-row">
                        <div className="bar-meta">
                          <span>{m.expressLabel}</span>
                        </div>
                        <div className="bar-track">
                          <div
                            className="bar-fill express"
                            style={{ width: `${m.expressPct}%` }}
                          />
                        </div>
                      </div>
                      <div className="bar-row">
                        <div className="bar-meta">
                          <strong>{m.lampLabel}</strong>
                        </div>
                        <div className="bar-track">
                          <div
                            className="bar-fill lamp"
                            style={{ width: `${m.lampPct}%` }}
                          />
                        </div>
                      </div>
                    </div>
                    <div className="bench-note">{m.note}</div>
                  </div>
                ))}
              </div>
            </div>
          </section>
        </div>

        {/* STACK 5: Architecture & Quick Start */}
        <div
          className="stack-slot stack-slot-last"
          ref={(el) => {
            slotRefs.current[4] = el;
          }}
        >
          <section className="stack-card arch-card-section" style={{ zIndex: 5 }}>
            <div className="section-inner">
              <div className="section-eyebrow">
                <Network size={14} /> End-to-End Architecture
              </div>
              <h2 className="section-heading">
                From CLI scaffold to production binary bundle.
              </h2>

              <div className="pipeline-row">
                <div className="pipe-node">
                  <span>1. Scaffold</span>
                  <code>npx create-lamp-app my-app</code>
                </div>
                <ArrowRight size={18} className="pipe-arrow" />
                <div className="pipe-node">
                  <span>2. Add Modules</span>
                  <code>npx lamp add users/local_users</code>
                </div>
                <ArrowRight size={18} className="pipe-arrow" />
                <div className="pipe-node">
                  <span>3. Watch Mode</span>
                  <code>npm run dev (--watch)</code>
                </div>
                <ArrowRight size={18} className="pipe-arrow" />
                <div className="pipe-node">
                  <span>4. Build &amp; Serve</span>
                  <code>npm run build && npm start</code>
                </div>
              </div>

              <div className="final-cta-card">
                <div>
                  <h3>Ready to build with Lamp?</h3>
                  <p>
                    Dive into the complete documentation covering App Config, Routing,
                    Registries, Error Handlers, Wire Caching, Parallelism, and Client
                    Integration.
                  </p>
                </div>
                <div className="final-cta-actions">
                  <Link to="/docs/concept" className="btn-primary">
                    <span>Explore Docs</span>
                    <ArrowRight size={16} />
                  </Link>
                  <Link to="/docs/quick-start" className="btn-secondary">
                    <span>Quick Start</span>
                  </Link>
                  <a
                    href={GITHUB_URL}
                    target="_blank"
                    rel="noreferrer"
                    className="btn-secondary"
                  >
                    <GitHubIcon size={16} />
                    <span>GitHub</span>
                  </a>
                </div>
              </div>

              <footer className="site-footer">
                <div className="footer-left">
                  <strong>Lamp Framework</strong> · <span>v0.4.1</span> ·{" "}
                  <span>MIT License</span>
                </div>
                <div className="footer-links">
                  <Link to="/docs/concept">Docs</Link>
                  <Link to="/docs/quick-start">Quick Start</Link>
                  <Link to="/docs/client-integration">Client SDK</Link>
                  <a href={GITHUB_URL} target="_blank" rel="noreferrer">
                    GitHub
                  </a>
                </div>
              </footer>
            </div>
          </section>
        </div>
      </div>
    </div>
  );
}
