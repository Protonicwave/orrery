# ADR-0043: Generate the site with the tool that generates the reference

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

Phase 14 asks for two things that are usually built by two different tools: a
generated API reference, which comes from the comments in the public headers,
and a documentation site, which is the prose the repository already holds. There
are about twenty Markdown documents by now, the two reports, the file format
specifications, the visualisation and bindings guides, and forty-four
architecture decision records, and they are written to be read on a code host as
well as on a site.

The question is not which static site generator is best in general. It is how
many tools should stand between a change to this repository and the site that
change produces.

## Decision

Doxygen generates both, from `docs/Doxyfile`, run from the repository root. Its
input is `include/` and the Markdown under `docs/`; the front page is
`docs/index.md`, written for the site rather than borrowed from the README; and
heading anchors are spelled the way a code host spells them, so a link written
for GitHub resolves in both places.

Warnings fail the run. Continuous integration builds the site on every pull
request and publishes it from `main` to GitHub Pages.

## Alternatives considered

**A site generator beside Doxygen, MkDocs or Docusaurus or Hugo.** The
conventional arrangement, and it produces a better-looking site than this one.
It also means two tools, two configurations, two ways for a build to break and a
navigation structure maintained in a third place, all so that a reader following
a claim from the validation report into the class that implements it can change
site halfway. The prose in this repository is already about the code; splitting
the two apart to present them is the wrong seam.

**Sphinx with Breathe.** The strongest option on the list, and the usual answer
for a C++ project that also ships Python. It still runs Doxygen underneath, to
produce the XML Breathe reads, so it is this decision plus a Python toolchain and
a second markup language. Worth revisiting if the Python bindings ever grow
documentation of their own that is more than a page.

**Doxygen with a third-party theme.** Doxygen Awesome is a single stylesheet and
would improve the appearance considerably. It is a pinned dependency on somebody
else's CSS for a project whose documentation is text and tables, and this project
has been careful about what it pins and why (ADR-0002). The default appearance is
plain rather than bad.

**No site: leave the Markdown to be read on GitHub.** This is what the project
did for thirteen phases and it works, but it produces no API reference at all,
and a reference is the one document that cannot be written by hand and kept
true.

## Consequences

The site looks like Doxygen, because it is. The navigation is Doxygen's tree
view and the search is Doxygen's search.

The output directory is one level deep, `build/html`, because Doxygen creates
the last component of that path and no more: a two-level path works on a machine
that has built the project and fails in a fresh clone, which is exactly the
machine continuous integration runs on. It fails by exiting without printing
anything, which is worth knowing about the tool.

A page's URL is derived from its path, so the validation report is at
`md_docs_2validation.html` rather than at anything a person would type. Those
names are stable across machines, which needed `STRIP_FROM_PATH` to include the
repository root: without it a page's URL contained the home directory of
whoever generated the site.

The Markdown in this repository is now compiled rather than only rendered. A
dead cross-reference is a failed build, which is the point, and it means a
document that links to a file outside the site's input has to say so in words
instead. The one place that bit was the record template, whose status line
linked to a file called `NNNN-title.md` that does not exist and should not; it
now names the superseding record in prose.

There is no version number on the site. The version has one home, the `project()`
call in `CMakeLists.txt`, which is what `core::version()` reports and what a
wheel is tagged with, and a second copy in the Doxyfile would eventually
disagree with it.

The site is built from a Doxygen release pinned by version and checked by hash
rather than from the distribution's package, because the GitHub-style heading
anchors this configuration depends on arrived after the version Ubuntu ships.
