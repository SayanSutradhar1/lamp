import { Check, Copy } from "lucide-react";
import { useState, type ReactNode } from "react";

interface CodeBlockProps {
  readonly code: string;
  readonly filename?: string;
  readonly language?: string;
}

const KEYWORDS = new Set([
  "import",
  "from",
  "export",
  "const",
  "let",
  "async",
  "await",
  "return",
  "if",
  "else",
  "throw",
  "new",
  "try",
  "catch",
  "finally",
  "interface",
  "type",
  "true",
  "false",
  "null",
  "undefined",
]);

function highlightLine(line: string): ReactNode[] {
  const tokens: ReactNode[] = [];
  const tokenRe =
    /(\/\/.*$|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`|\b\d+(?:_\d+)*(?:\.\d+)?\b|\b[a-zA-Z_$][a-zA-Z0-9_$]*\b)/g;
  let lastIndex = 0;
  let match: RegExpExecArray | null;
  let key = 0;

  while ((match = tokenRe.exec(line)) !== null) {
    if (match.index > lastIndex) {
      tokens.push(line.slice(lastIndex, match.index));
    }
    const word = match[0];
    if (word.startsWith("//")) {
      tokens.push(
        <span key={key++} className="tok-comment">
          {word}
        </span>
      );
    } else if (word.startsWith('"') || word.startsWith("'") || word.startsWith("`")) {
      tokens.push(
        <span key={key++} className="tok-string">
          {word}
        </span>
      );
    } else if (/^\d/.test(word)) {
      tokens.push(
        <span key={key++} className="tok-number">
          {word}
        </span>
      );
    } else if (KEYWORDS.has(word)) {
      tokens.push(
        <span key={key++} className="tok-keyword">
          {word}
        </span>
      );
    } else if (/^[A-Z]/.test(word)) {
      tokens.push(
        <span key={key++} className="tok-type">
          {word}
        </span>
      );
    } else {
      tokens.push(word);
    }
    lastIndex = tokenRe.lastIndex;
  }

  if (lastIndex < line.length) {
    tokens.push(line.slice(lastIndex));
  }
  return tokens;
}

export function CodeBlock({ code, filename, language = "ts" }: CodeBlockProps) {
  const [copied, setCopied] = useState(false);

  const handleCopy = async () => {
    await navigator.clipboard.writeText(code);
    setCopied(true);
    setTimeout(() => setCopied(false), 1600);
  };

  const lines = code.trimEnd().split("\n");

  return (
    <div className="code-block">
      <div className="code-header">
        <div className="code-meta">
          <span className="code-dots" aria-hidden="true">
            <i />
            <i />
            <i />
          </span>
          {filename && <span className="code-filename">{filename}</span>}
          <span className="code-lang">{language}</span>
        </div>
        <button type="button" className="copy-btn" onClick={() => void handleCopy()}>
          {copied ? <Check size={14} /> : <Copy size={14} />}
          <span>{copied ? "Copied" : "Copy"}</span>
        </button>
      </div>
      <pre className="code-pre">
        <code>
          {lines.map((line, idx) => (
            <div key={idx} className="code-line">
              <span className="line-no">{idx + 1}</span>
              <span className="line-text">
                {language === "bash" || language === "powershell"
                  ? line
                  : highlightLine(line)}
              </span>
            </div>
          ))}
        </code>
      </pre>
    </div>
  );
}
