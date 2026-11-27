import {
  ArrowLeft,
  ArrowRight,
  BookOpen,
  ChevronRight,
  Menu,
  Search,
  Sparkles,
  X,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { Link, Navigate, useParams } from "react-router-dom";
import { CodeBlock } from "../components/CodeBlock";
import { GitHubIcon } from "../components/GitHubIcon";
import { ThemeSwitcher } from "../components/ThemeSwitcher";
import { DOC_GROUPS, DOC_TOPICS, GITHUB_URL } from "../data/docsData";

export function DocsPage() {
  const { topic } = useParams<{ topic: string }>();
  const [query, setQuery] = useState("");
  const [selectedSubId, setSelectedSubId] = useState<string>("");
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false);
  const contentRef = useRef<HTMLDivElement | null>(null);

  const currentIndex = DOC_TOPICS.findIndex((t) => t.id === topic);
  const activeTopic = currentIndex >= 0 ? DOC_TOPICS[currentIndex] : null;

  useEffect(() => {
    if (contentRef.current) {
      contentRef.current.scrollTo({ top: 0, behavior: "auto" });
    }
  }, [topic]);

  const filteredTopics = useMemo(() => {
    const q = query.trim().toLowerCase();
    if (!q) return DOC_TOPICS;
    return DOC_TOPICS.filter(
      (t) =>
        t.title.toLowerCase().includes(q) ||
        t.summary.toLowerCase().includes(q) ||
        t.subtopics.some((s) => s.title.toLowerCase().includes(q))
    );
  }, [query]);

  if (!topic || !activeTopic) {
    return <Navigate to={`/docs/${DOC_TOPICS[0].id}`} replace />;
  }

  const activeSubId =
    selectedSubId && activeTopic.subtopics.some((s) => s.id === selectedSubId)
      ? selectedSubId
      : (activeTopic.subtopics[0]?.id ?? "");

  const prevTopic = currentIndex > 0 ? DOC_TOPICS[currentIndex - 1] : null;
  const nextTopic =
    currentIndex < DOC_TOPICS.length - 1 ? DOC_TOPICS[currentIndex + 1] : null;

  const scrollToSubtopic = (subId: string) => {
    setSelectedSubId(subId);
    const el = document.getElementById(`sub-${subId}`);
    if (el && contentRef.current) {
      const containerTop = contentRef.current.getBoundingClientRect().top;
      const elTop = el.getBoundingClientRect().top;
      contentRef.current.scrollBy({
        top: elTop - containerTop - 24,
        behavior: "smooth",
      });
    }
  };

  const handleContentScroll = () => {
    const container = contentRef.current;
    if (!container) return;
    const containerTop = container.getBoundingClientRect().top;
    let current = activeTopic.subtopics[0]?.id ?? "";
    for (const sub of activeTopic.subtopics) {
      const el = document.getElementById(`sub-${sub.id}`);
      if (el && el.getBoundingClientRect().top - containerTop <= 120) {
        current = sub.id;
      }
    }
    setSelectedSubId(current);
  };

  return (
    <div className="docs-shell">
      <div className="docs-topbar">
        <div className="docs-topbar-left">
          <button
            type="button"
            className="mobile-menu-btn"
            onClick={() => setMobileMenuOpen((v) => !v)}
            aria-label="Toggle docs navigation"
          >
            {mobileMenuOpen ? <X size={18} /> : <Menu size={18} />}
          </button>
          <Link to="/" className="brand-link">
            <span className="brand-mark">
              <Sparkles size={16} />
            </span>
            <span>Lamp</span>
            <span className="version-pill">v0.4.1</span>
          </Link>
          <span className="topbar-divider">/</span>
          <span className="topbar-section">
            <BookOpen size={15} /> Docs
          </span>
        </div>

        <div className="docs-topbar-right">
          <div className="docs-search">
            <Search size={14} />
            <input
              type="search"
              placeholder="Filter topics..."
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              aria-label="Filter documentation topics"
            />
          </div>
          <a
            href={GITHUB_URL}
            target="_blank"
            rel="noreferrer"
            className="icon-link-btn"
          >
            <GitHubIcon size={16} />
            <span>GitHub</span>
          </a>
          <ThemeSwitcher />
        </div>
      </div>

      <div className="docs-body">
        {/* Left: Primary Sidebar Menu */}
        <aside className={`docs-primary-sidebar ${mobileMenuOpen ? "open" : ""}`}>
          <nav aria-label="Documentation topics">
            {DOC_GROUPS.map((group) => {
              const items = filteredTopics.filter((t) => t.group === group);
              if (items.length === 0) return null;
              return (
                <div key={group} className="sidebar-group">
                  <div className="sidebar-group-title">{group}</div>
                  {items.map((item) => {
                    const active = item.id === activeTopic.id;
                    return (
                      <Link
                        key={item.id}
                        to={`/docs/${item.id}`}
                        className={`sidebar-link ${active ? "active" : ""}`}
                        onClick={() => setMobileMenuOpen(false)}
                      >
                        <span>{item.title}</span>
                        {active && <ChevronRight size={14} />}
                      </Link>
                    );
                  })}
                </div>
              );
            })}
          </nav>
        </aside>

        {/* Middle: Scrollable Main Content */}
        <main
          ref={contentRef}
          onScroll={handleContentScroll}
          className="docs-content-scroll"
        >
          <article className="docs-article">
            <div className="article-breadcrumb">
              <span>{activeTopic.group}</span>
              <ChevronRight size={14} />
              <span>{activeTopic.title}</span>
            </div>
            <h1>{activeTopic.title}</h1>
            <p className="article-lead">{activeTopic.summary}</p>

            {/* Mobile inline TOC */}
            {activeTopic.subtopics.length > 1 && (
              <div className="mobile-toc">
                <div className="sub-sidebar-title">On This Page</div>
                <div className="mobile-toc-pills">
                  {activeTopic.subtopics.map((sub) => (
                    <button
                      key={sub.id}
                      type="button"
                      className={`mobile-toc-btn ${activeSubId === sub.id ? "active" : ""}`}
                      onClick={() => scrollToSubtopic(sub.id)}
                    >
                      {sub.title}
                    </button>
                  ))}
                </div>
              </div>
            )}

            {activeTopic.subtopics.map((sub) => (
              <section
                key={sub.id}
                id={`sub-${sub.id}`}
                className="doc-subtopic"
              >
                <h2>{sub.title}</h2>
                {sub.paragraphs.map((p, i) => (
                  <p key={i}>{p}</p>
                ))}

                {sub.bullets && (
                  <ul className="doc-bullets">
                    {sub.bullets.map((b, i) => (
                      <li key={i}>{b}</li>
                    ))}
                  </ul>
                )}

                {sub.code && (
                  <CodeBlock
                    code={sub.code.content}
                    filename={sub.code.filename}
                    language={sub.code.language}
                  />
                )}

                {sub.configTable && (
                  <div className="config-table-wrap">
                    <div className="config-table-title">
                      {sub.configTable.title}
                    </div>
                    <table className="config-table">
                      <thead>
                        <tr>
                          <th>Option</th>
                          <th>Type</th>
                          <th>Default</th>
                          <th>Impact &amp; Accepted Values</th>
                        </tr>
                      </thead>
                      <tbody>
                        {sub.configTable.rows.map((row) => (
                          <tr key={row.option}>
                            <td>
                              <code>{row.option}</code>
                            </td>
                            <td>
                              <code className="type-code">{row.type}</code>
                            </td>
                            <td>
                              <code>{row.defaultVal}</code>
                            </td>
                            <td>
                              <div>{row.impact}</div>
                              {row.values && row.values.length > 0 && (
                                <ul className="config-values-list">
                                  {row.values.map((v) => (
                                    <li key={v.value}>
                                      <code>{v.value}</code> — {v.behavior}
                                    </li>
                                  ))}
                                </ul>
                              )}
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                )}

                {sub.callout && (
                  <div className={`doc-callout ${sub.callout.kind}`}>
                    <strong>{sub.callout.title}</strong>
                    <p>{sub.callout.body}</p>
                  </div>
                )}
              </section>
            ))}

            <footer className="docs-pager">
              {prevTopic ? (
                <Link to={`/docs/${prevTopic.id}`} className="pager-card prev">
                  <span className="pager-dir">
                    <ArrowLeft size={14} /> Previous
                  </span>
                  <strong>{prevTopic.title}</strong>
                </Link>
              ) : (
                <div />
              )}
              {nextTopic && (
                <Link to={`/docs/${nextTopic.id}`} className="pager-card next">
                  <span className="pager-dir">
                    Next <ArrowRight size={14} />
                  </span>
                  <strong>{nextTopic.title}</strong>
                </Link>
              )}
            </footer>
          </article>
        </main>

        {/* Right: Sub-menu (On This Page) */}
        <aside className="docs-sub-sidebar" aria-label="Topic sub-menu">
          <div className="sub-sidebar-title">On This Page</div>
          <div className="sub-sidebar-topic">{activeTopic.title}</div>
          <ul className="sub-sidebar-list">
            {activeTopic.subtopics.map((sub) => (
              <li key={sub.id}>
                <button
                  type="button"
                  className={`sub-link ${activeSubId === sub.id ? "active" : ""}`}
                  onClick={() => scrollToSubtopic(sub.id)}
                >
                  {sub.title}
                </button>
              </li>
            ))}
          </ul>
        </aside>
      </div>
    </div>
  );
}
