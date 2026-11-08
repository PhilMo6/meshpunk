"""
md2html.py -- renders website/docs/*.md into pages of the site.

Each docs/<slug>.md becomes <out>/<slug>/index.html, wrapped in the same
header, nav and footer as the hand-written pages. The first "# " heading
is the page title and the first paragraph after it is the meta description.

Markdown subset (everything the docs use; anything else comes out as
HTML-escaped text):

  # .. ###          headings, given GitHub-style ids for in-page links
  blank-line paragraphs; a paragraph that is one **bold** phrase is a run-in
  - item            bullet list      - [ ] item / - [x] item   task list
  1. item           numbered list
  | a | b |         pipe table; the second row must be the --- separator
  ```lang ... ```   fenced code block
  `code`  **bold**  [text](url)   and backslash-escaped punctuation

Stdlib only, like build_site.py. A malformed page (no title, bad file
name) raises ValueError so the site build fails instead of deploying it.
"""

import html
import os
import re

SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
FENCE_RE = re.compile(r"^```\s*([A-Za-z0-9_+-]*)\s*$")
BULLET_RE = re.compile(r"^- (.*)$")
TASK_RE = re.compile(r"^- \[([ xX])\] (.*)$")
NUMBER_RE = re.compile(r"^\d+\. (.*)$")
TABLE_SEP_RE = re.compile(r"^\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
INLINE_RE = re.compile(
    r"(`[^`]+`)"                       # 1 code span
    r"|\*\*(.+?)\*\*"                  # 2 bold
    r"|\[([^\]]+)\]\(([^)\s]+)\)"      # 3,4 link text, url
    r"|\\([\\`*_{}\[\]()#+\-.!|<>])"   # 5 escaped punctuation
)

NAV = [
    ("", "Home"),
    ("flash/", "Flash"),
    ("elf-app-developer-guide/", "Developer guide"),
    ("https://github.com/PhilMo6/meshpunk", "GitHub"),
    ("https://discord.gg/cmJKV5gEdP", "Discord"),
]

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; form-action 'none'">
<title>{title} &mdash; MeshPunk</title>
<meta name="description" content="{description}">
<link rel="icon" href="{root}img/meshpunk-icon.svg" type="image/svg+xml">
<link rel="stylesheet" href="{root}css/site.css">
</head>
<body>

<header class="site-header">
  <div class="wrap">
    <a class="brand" href="{root}"><img src="{root}img/meshpunk-icon.svg" alt="" width="30" height="30">MeshPunk</a>
    <nav class="site-nav" aria-label="Site">
{nav}
    </nav>
  </div>
</header>

<main>
  <div class="wrap">
    <article class="article">
{body}
    </article>
  </div>
</main>

<footer class="site-footer">
  <div class="wrap">
    <span>MeshPunk &mdash; LVGL with Lua for LoRa handhelds</span>
    <span><a href="https://github.com/PhilMo6/meshpunk">Stable repo</a> &middot; <a href="https://github.com/PhilMo6/meshpunk-dev">Dev repo</a> &middot; <a href="https://github.com/PhilMo6/meshpunk-apps">App Library</a> &middot; <a href="https://discord.gg/cmJKV5gEdP">Discord</a></span>
  </div>
</footer>

</body>
</html>
"""


def esc(text):
    """HTML-escape text for element content."""
    return html.escape(text, quote=False)


def attr(text):
    """HTML-escape text for an attribute value."""
    return html.escape(text, quote=True)


def slugify(text):
    """GitHub-style heading id: lower case, punctuation dropped, spaces to dashes."""
    plain = re.sub(r"[`*\[\]()]", "", text)
    plain = re.sub(r"[^\w\s-]", "", plain.lower())
    return re.sub(r"\s+", "-", plain.strip())


def plain(text):
    """Inline markdown reduced to its text, for titles and descriptions."""
    out = []
    pos = 0
    for m in INLINE_RE.finditer(text):
        out.append(text[pos:m.start()])
        if m.group(1):
            out.append(m.group(1)[1:-1])
        elif m.group(2) is not None:
            out.append(plain(m.group(2)))
        elif m.group(3) is not None:
            out.append(plain(m.group(3)))
        else:
            out.append(m.group(5))
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


def inline(text):
    """Inline markdown to HTML: code spans, bold, links, escapes."""
    out = []
    pos = 0
    for m in INLINE_RE.finditer(text):
        out.append(esc(text[pos:m.start()]))
        if m.group(1):
            out.append("<code>%s</code>" % esc(m.group(1)[1:-1]))
        elif m.group(2) is not None:
            out.append("<strong>%s</strong>" % inline(m.group(2)))
        elif m.group(3) is not None:
            out.append('<a href="%s">%s</a>' % (attr(m.group(4)), inline(m.group(3))))
        else:
            out.append(esc(m.group(5)))
        pos = m.end()
    out.append(esc(text[pos:]))
    return "".join(out)


def table_cells(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip() for c in line.split("|")]


def render_table(lines):
    head = table_cells(lines[0])
    rows = [table_cells(l) for l in lines[2:]]
    out = ['<div class="table-wrap"><table>']
    out.append("<thead><tr>" + "".join("<th>%s</th>" % inline(c) for c in head) + "</tr></thead>")
    if rows:
        out.append("<tbody>")
        for r in rows:
            r = r + [""] * (len(head) - len(r))
            out.append("<tr>" + "".join("<td>%s</td>" % inline(c) for c in r) + "</tr>")
        out.append("</tbody>")
    out.append("</table></div>")
    return "\n".join(out)


def render(md):
    """Markdown text -> (title, description, body_html)."""
    lines = md.replace("\r\n", "\n").split("\n")
    out = []
    title = None
    description = None
    para = []
    i = 0

    def flush_para():
        nonlocal description
        if not para:
            return
        text = " ".join(s.strip() for s in para)
        para.clear()
        if title and description is None:
            description = plain(text)
        m = re.match(r"^\*\*(.+)\*\*$", text)
        if m and "**" not in m.group(1):
            out.append('<p class="run-in"><strong>%s</strong></p>' % inline(m.group(1)))
        else:
            out.append("<p>%s</p>" % inline(text))

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        m = FENCE_RE.match(stripped)
        if m:
            flush_para()
            lang = m.group(1)
            i += 1
            code = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1  # closing fence
            cls = ' class="language-%s"' % attr(lang) if lang else ""
            out.append("<pre><code%s>%s</code></pre>" % (cls, esc("\n".join(code))))
            continue

        m = HEADING_RE.match(stripped)
        if m:
            flush_para()
            level = len(m.group(1))
            text = m.group(2)
            if level == 1 and title is None:
                title = plain(text)
            out.append('<h%d id="%s">%s</h%d>' % (level, attr(slugify(text)), inline(text), level))
            i += 1
            continue

        if stripped.startswith("|") and i + 1 < len(lines) and TABLE_SEP_RE.match(lines[i + 1].strip()):
            flush_para()
            block = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                block.append(lines[i])
                i += 1
            out.append(render_table(block))
            continue

        if BULLET_RE.match(stripped) or NUMBER_RE.match(stripped):
            flush_para()
            ordered = bool(NUMBER_RE.match(stripped))
            tag = "ol" if ordered else "ul"
            items = []
            while i < len(lines):
                s = lines[i].strip()
                t = TASK_RE.match(s)
                b = NUMBER_RE.match(s) if ordered else BULLET_RE.match(s)
                if not ordered and t:
                    done = t.group(1).lower() == "x"
                    items.append('<li class="task%s">%s</li>' % (" done" if done else "", inline(t.group(2))))
                elif b:
                    items.append("<li>%s</li>" % inline(b.group(1)))
                else:
                    break
                i += 1
            out.append("<%s>\n%s\n</%s>" % (tag, "\n".join(items), tag))
            continue

        if not stripped:
            flush_para()
            i += 1
            continue

        para.append(line)
        i += 1

    flush_para()
    if title is None:
        raise ValueError("no '# Title' heading")
    return title, description or title, "\n".join(out)


def nav_html(root, current_slug):
    out = []
    for href, label in NAV:
        target = href if href.startswith("http") else root + href
        cur = ' aria-current="page"' if href.rstrip("/") == current_slug else ""
        out.append('      <a href="%s"%s>%s</a>' % (attr(target), cur, esc(label)))
    return "\n".join(out)


def page(slug, md):
    """A docs page as a complete HTML document (served at /<slug>/index.html)."""
    title, description, body = render(md)
    root = "../"
    return PAGE.format(
        title=esc(title),
        description=attr(description[:160]),
        root=root,
        nav=nav_html(root, slug),
        body=body,
    )


def render_docs(docs_dir, out_dir):
    """Render every docs_dir/*.md into out_dir/<slug>/index.html. Returns the slugs."""
    slugs = []
    if not os.path.isdir(docs_dir):
        return slugs
    for name in sorted(os.listdir(docs_dir)):
        if not name.endswith(".md"):
            continue
        slug = name[:-3]
        if not SLUG_RE.match(slug):
            raise ValueError("docs/%s: file name must be lower-case letters, digits and dashes" % name)
        with open(os.path.join(docs_dir, name), encoding="utf-8") as f:
            md = f.read()
        try:
            doc = page(slug, md)
        except ValueError as e:
            raise ValueError("docs/%s: %s" % (name, e))
        target = os.path.join(out_dir, slug)
        os.makedirs(target, exist_ok=True)
        with open(os.path.join(target, "index.html"), "w", encoding="utf-8", newline="\n") as f:
            f.write(doc)
        slugs.append(slug)
    return slugs
