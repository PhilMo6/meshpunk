"""python -m unittest discover -s website/tests

Covers md2html.py, the Markdown-to-page renderer: the subset the docs use
must come out as the HTML the site styles, and a shipped page's in-page
links must have a heading to land on.
"""

import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import md2html  # noqa: E402

DOCS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")


def body(md):
    return md2html.render(md)[2]


class Inline(unittest.TestCase):
    def test_code_span_protects_its_contents(self):
        self.assertEqual(md2html.inline("use `a **b** <c>` here"),
                         "use <code>a **b** &lt;c&gt;</code> here")

    def test_bold_and_link(self):
        self.assertEqual(md2html.inline("**Contents:** [Overview](#overview)"),
                         '<strong>Contents:</strong> <a href="#overview">Overview</a>')

    def test_escapes_html_and_backslashes(self):
        self.assertEqual(md2html.inline(r"a \_b\_ & <tag>"), "a _b_ &amp; &lt;tag&gt;")

    def test_plain_strips_markup(self):
        self.assertEqual(md2html.plain("**Bold** `code` [text](url)"), "Bold code text")


class Blocks(unittest.TestCase):
    def test_headings_get_github_style_ids(self):
        out = body("# Meshpunk Guide\n\n## Before you start\n\n### Speed on the S3 boards\n")
        self.assertIn('<h1 id="meshpunk-guide">Meshpunk Guide</h1>', out)
        self.assertIn('<h2 id="before-you-start">Before you start</h2>', out)
        self.assertIn('<h3 id="speed-on-the-s3-boards">Speed on the S3 boards</h3>', out)

    def test_paragraph_joins_lines_and_bold_line_is_a_run_in(self):
        out = body("# T\n\nfirst line\nsecond line\n\n**What you need**\n")
        self.assertIn("<p>first line second line</p>", out)
        self.assertIn('<p class="run-in"><strong>What you need</strong></p>', out)

    def test_lists(self):
        out = body("# T\n\n- one\n- [ ] todo\n- [x] done\n\n1. first\n2. second\n")
        self.assertIn('<ul>\n<li>one</li>\n<li class="task">todo</li>\n<li class="task done">done</li>\n</ul>', out)
        self.assertIn("<ol>\n<li>first</li>\n<li>second</li>\n</ol>", out)

    def test_table(self):
        out = body("# T\n\n| Code | Meaning |\n| --- | --- |\n| `0x80` | Shift |\n")
        self.assertIn("<thead><tr><th>Code</th><th>Meaning</th></tr></thead>", out)
        self.assertIn("<tr><td><code>0x80</code></td><td>Shift</td></tr>", out)
        self.assertEqual(out.count('<div class="table-wrap">'), 1)

    def test_fenced_code_is_escaped_and_tagged(self):
        out = body("# T\n\n```c\nint *p = a < b ? &x : 0; // **not bold**\n```\n")
        self.assertIn('<pre><code class="language-c">int *p = a &lt; b ? &amp;x : 0; // **not bold**</code></pre>', out)

    def test_title_and_description(self):
        title, desc, _ = md2html.render("# My Page\n\nThe **first** paragraph.\n\nSecond.\n")
        self.assertEqual(title, "My Page")
        self.assertEqual(desc, "The first paragraph.")

    def test_missing_title_fails(self):
        with self.assertRaises(ValueError):
            md2html.render("just text\n")


class Pages(unittest.TestCase):
    def test_renders_docs_dir_into_slug_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            docs = os.path.join(tmp, "docs")
            os.makedirs(docs)
            with open(os.path.join(docs, "my-page.md"), "w", encoding="utf-8") as f:
                f.write("# My Page\n\nHello.\n")
            out = os.path.join(tmp, "out")
            self.assertEqual(md2html.render_docs(docs, out), ["my-page"])
            with open(os.path.join(out, "my-page", "index.html"), encoding="utf-8") as f:
                page = f.read()
            self.assertIn("<title>My Page &mdash; MeshPunk</title>", page)
            self.assertIn('<meta name="description" content="Hello.">', page)
            self.assertIn('href="../css/site.css"', page)
            self.assertIn('<a href="../flash/">Flash</a>', page)

    def test_bad_file_name_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            docs = os.path.join(tmp, "docs")
            os.makedirs(docs)
            with open(os.path.join(docs, "Bad Name.md"), "w", encoding="utf-8") as f:
                f.write("# T\n")
            with self.assertRaises(ValueError):
                md2html.render_docs(docs, os.path.join(tmp, "out"))

    def test_no_docs_dir_is_fine(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(md2html.render_docs(os.path.join(tmp, "missing"), tmp), [])


class ShippedDocs(unittest.TestCase):
    def test_in_page_links_resolve(self):
        for name in sorted(os.listdir(DOCS)):
            if not name.endswith(".md"):
                continue
            with open(os.path.join(DOCS, name), encoding="utf-8") as f:
                _, _, out = md2html.render(f.read())
            ids = set(re.findall(r' id="([^"]+)"', out))
            for anchor in re.findall(r'href="#([^"]+)"', out):
                self.assertIn(anchor, ids, "%s: broken in-page link #%s" % (name, anchor))

    def test_developer_guide_is_shipped(self):
        self.assertTrue(os.path.isfile(os.path.join(DOCS, "elf-app-developer-guide.md")))
