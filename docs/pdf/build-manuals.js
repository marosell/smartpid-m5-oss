#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const repoRoot = path.resolve(__dirname, "..", "..");
const docsDir = path.join(repoRoot, "docs");
const outDir = path.join(docsDir, "pdf", "build");
const cssPath = path.join(docsDir, "pdf", "manual.css");
const chromePath = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const chromeProfileDir = "/private/tmp/proofpro-manual-chrome-profile";

const manuals = [
  {
    markdown: path.join(docsDir, "PROOFPRO_USER_MANUAL.md"),
    html: path.join(outDir, "PROOFPRO_USER_MANUAL.html"),
    pdf: path.join(docsDir, "PROOFPRO_USER_MANUAL.pdf"),
    title: "ProofPro Distilling Controller User Manual",
  },
  {
    markdown: path.join(docsDir, "PROOFPRO_USER_MANUAL-technicalrecovery.md"),
    html: path.join(outDir, "PROOFPRO_USER_MANUAL-technicalrecovery.html"),
    pdf: path.join(docsDir, "PROOFPRO_USER_MANUAL-technicalrecovery.pdf"),
    title: "ProofPro Technical Recovery Companion",
  },
  {
    markdown: path.join(docsDir, "TEST_PROTOCOL.md"),
    html: path.join(outDir, "TEST_PROTOCOL.html"),
    pdf: path.join(docsDir, "TEST_PROTOCOL.pdf"),
    title: "ProofPro Bench Test Protocol",
  },
];

function escapeHtml(value) {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function inlineMarkdown(value) {
  let s = escapeHtml(value);
  s = s.replace(/`([^`]+)`/g, "<code>$1</code>");
  s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  s = s.replace(/\*([^*]+)\*/g, "<em>$1</em>");
  return s;
}

function isTableStart(lines, index) {
  return (
    index + 1 < lines.length &&
    /^\|.*\|$/.test(lines[index].trim()) &&
    /^\|[\s:-]+\|[\s|:-]+$/.test(lines[index + 1].trim())
  );
}

function splitTableRow(line) {
  return line
    .trim()
    .replace(/^\|/, "")
    .replace(/\|$/, "")
    .split("|")
    .map((cell) => cell.trim());
}

function renderTable(lines, start) {
  const header = splitTableRow(lines[start]);
  let i = start + 2;
  const rows = [];
  while (i < lines.length && /^\|.*\|$/.test(lines[i].trim())) {
    rows.push(splitTableRow(lines[i]));
    i += 1;
  }

  const head = header.map((cell) => `<th>${inlineMarkdown(cell)}</th>`).join("");
  const body = rows
    .map((row) => `<tr>${row.map((cell) => `<td>${inlineMarkdown(cell)}</td>`).join("")}</tr>`)
    .join("\n");
  return {
    html: `<table>\n<thead><tr>${head}</tr></thead>\n<tbody>\n${body}\n</tbody>\n</table>`,
    next: i,
  };
}

function renderList(lines, start, ordered) {
  const tag = ordered ? "ol" : "ul";
  const re = ordered ? /^\s*\d+\.\s+(.*)$/ : /^\s*-\s+(.*)$/;
  let i = start;
  const items = [];
  while (i < lines.length) {
    const match = lines[i].match(re);
    if (!match) break;
    let text = match[1];
    i += 1;
    while (i < lines.length && /^\s{2,}\S/.test(lines[i]) && !/^\s*(-|\d+\.)\s+/.test(lines[i])) {
      text += " " + lines[i].trim();
      i += 1;
    }
    items.push(`<li>${inlineMarkdown(text)}</li>`);
  }
  return { html: `<${tag}>\n${items.join("\n")}\n</${tag}>`, next: i };
}

function renderParagraph(lines, start) {
  let i = start;
  const parts = [];
  while (
    i < lines.length &&
    lines[i].trim() !== "" &&
    !/^#{1,6}\s+/.test(lines[i]) &&
    !/^```/.test(lines[i]) &&
    !/^\s*-\s+/.test(lines[i]) &&
    !/^\s*\d+\.\s+/.test(lines[i]) &&
    !isTableStart(lines, i)
  ) {
    parts.push(lines[i].trim());
    i += 1;
  }
  const text = parts.join(" ");
  const className = /^Figure\s+\d+\./.test(text) || /^Placeholder:/.test(text)
    ? ' class="figure-placeholder"'
    : "";
  return { html: `<p${className}>${inlineMarkdown(text)}</p>`, next: i };
}

function renderMarkdown(markdown) {
  const lines = markdown.replace(/\r\n/g, "\n").split("\n");
  const out = [];
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];
    const trimmed = line.trim();

    if (trimmed === "") {
      i += 1;
      continue;
    }

    if (/^```/.test(trimmed)) {
      const language = trimmed.slice(3).trim();
      i += 1;
      const code = [];
      while (i < lines.length && !/^```/.test(lines[i].trim())) {
        code.push(lines[i]);
        i += 1;
      }
      if (i < lines.length) i += 1;
      const text = code.join("\n");
      const className = /^Figure\s+\d+\./.test(text.trim()) ? ' class="figure-placeholder"' : "";
      out.push(`<pre${className}><code${language ? ` class="language-${escapeHtml(language)}"` : ""}>${escapeHtml(text)}</code></pre>`);
      continue;
    }

    const heading = line.match(/^(#{1,6})\s+(.*)$/);
    if (heading) {
      const level = heading[1].length;
      out.push(`<h${level}>${inlineMarkdown(heading[2])}</h${level}>`);
      i += 1;
      continue;
    }

    if (isTableStart(lines, i)) {
      const rendered = renderTable(lines, i);
      out.push(rendered.html);
      i = rendered.next;
      continue;
    }

    if (/^\s*-\s+/.test(line)) {
      const rendered = renderList(lines, i, false);
      out.push(rendered.html);
      i = rendered.next;
      continue;
    }

    if (/^\s*\d+\.\s+/.test(line)) {
      const rendered = renderList(lines, i, true);
      out.push(rendered.html);
      i = rendered.next;
      continue;
    }

    const rendered = renderParagraph(lines, i);
    out.push(rendered.html);
    i = rendered.next;
  }

  return out.join("\n\n");
}

function titlePage(markdown, title) {
  const rawLines = markdown.split(/\r?\n/);
  const lines = rawLines.filter((line) => line.trim() !== "");
  const h1RawIndex = rawLines.findIndex((line) => line.startsWith("# "));
  const h1 = rawLines[h1RawIndex]?.replace(/^#\s+/, "") || title;
  const subtitleLines = [];
  for (let i = h1RawIndex + 1; i < rawLines.length; i += 1) {
    const line = rawLines[i].trim();
    if (line === "") continue;
    if (/^(Firmware|Draft date):/.test(line) || /^MQTT\b/.test(line)) break;
    subtitleLines.push(line);
  }
  const subtitle = subtitleLines.join(" ");
  const meta = lines
    .filter((line) => /^(Firmware|Draft date):/.test(line) || /^MQTT\b/.test(line))
    .map((line) => `<div>${inlineMarkdown(line)}</div>`)
    .join("\n");
  return `
<section class="title-page">
  <div class="eyebrow">SmartPID M5 PRO Custom Firmware</div>
  <h1>${inlineMarkdown(h1)}</h1>
  <div class="subtitle">${inlineMarkdown(subtitle)}</div>
  <div class="meta">${meta}</div>
</section>`;
}

function stripTitleBlock(markdown) {
  const lines = markdown.split(/\r?\n/);
  let i = 0;
  let seenDraft = false;
  while (i < lines.length) {
    if (/^Draft date:/.test(lines[i])) {
      seenDraft = true;
      i += 1;
      break;
    }
    i += 1;
  }
  return seenDraft ? lines.slice(i).join("\n") : markdown;
}

function renderDocument(manual) {
  const markdown = fs.readFileSync(manual.markdown, "utf8");
  const css = fs.readFileSync(cssPath, "utf8");
  const body = `${titlePage(markdown, manual.title)}\n${renderMarkdown(stripTitleBlock(markdown))}`;
  return `<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>${escapeHtml(manual.title)}</title>
  <style>${css}</style>
</head>
<body>
  <main class="manual">
${body}
  </main>
</body>
</html>
`;
}

function runChrome(htmlPath, pdfPath) {
  if (!fs.existsSync(chromePath)) {
    throw new Error(`Chrome not found at ${chromePath}`);
  }
  fs.mkdirSync(chromeProfileDir, { recursive: true });
  if (fs.existsSync(pdfPath)) fs.rmSync(pdfPath);
  const result = spawnSync(
    chromePath,
    [
      "--headless",
      "--disable-gpu",
      "--disable-background-networking",
      "--disable-component-update",
      "--disable-default-apps",
      "--disable-extensions",
      "--disable-sync",
      "--no-first-run",
      "--no-default-browser-check",
      "--no-pdf-header-footer",
      `--user-data-dir=${chromeProfileDir}`,
      `--print-to-pdf=${pdfPath}`,
      `file://${htmlPath}`,
    ],
    { encoding: "utf8", timeout: 15000, killSignal: "SIGTERM" }
  );
  const pdfExists = fs.existsSync(pdfPath) && fs.statSync(pdfPath).size > 0;
  if (result.error && result.error.code === "ETIMEDOUT" && pdfExists) {
    return;
  }
  if (result.status !== 0 && !pdfExists) {
    throw new Error(`Chrome PDF render failed for ${htmlPath}\n${result.stderr}\n${result.stdout}`);
  }
}

fs.mkdirSync(outDir, { recursive: true });

for (const manual of manuals) {
  const html = renderDocument(manual);
  fs.writeFileSync(manual.html, html);
  runChrome(manual.html, manual.pdf);
  console.log(`Wrote ${path.relative(repoRoot, manual.html)}`);
  console.log(`Wrote ${path.relative(repoRoot, manual.pdf)}`);
}
