# ADR-0050: Serve the reading half as static pages

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The site has two halves. The instrument is an application: it fetches a
trajectory, decodes it in a Worker, and draws sixty frames a second. The other
half is the argument, and it is a document. It states what the project claims,
what each claim was measured against, and the command that produces the figure
again.

Those two things want opposite treatment. The instrument's cost is justified by
what it does. A page of prose that cannot be read until a bundle has been
fetched, parsed and executed is paying that cost for nothing, and it is paying it
in exactly the situation where a reader is least patient: arriving from a link,
on a connection that is not the one the site was built on.

The client is already a Vite build published to GitHub Pages. There is no server
and no framework for pages.

## Decision

The method pages are static HTML under `web/method/`, each an entry of the same
Vite build, each carrying a stylesheet and no script.

Written as HTML rather than rendered from components. React is in the build for
the instrument, and pre-rendering these pages through it would mean either
shipping the runtime to hydrate prose that never changes, or adding a
render-to-string step whose only output is markup that could have been written.
What is published is then what was written, and "this page costs no JavaScript to
read" is a property of the file rather than a claim about the build.

Written rather than generated from the Markdown in `docs/`. The reports are the
reference and are longer than a page on a site should be; the site's prose is
shorter, arranged as an argument rather than as a specification, and links back
to the report for the full tables. Generating one from the other would mean
either publishing the reference at length or teaching a generator which
paragraphs to drop. ADR-0044 made the same choice for the validation report and
for the same reason.

Sharing the design system rather than resembling it. The pages import the same
`tokens.css`, set the same three faces, and carry the same masthead at the same
height with the same hairline under it. The paper palette is the instrument's
inverted, not a second palette: `--paper-ink` and its two steps are measured
against `--paper` the way the ink ramp is measured against `--chrome`. The one
value that changes rather than inverts is the focus ring, which is `--brass-dim`
here because full brass is 2.4:1 against paper and a non-text indicator has to
hold three to one.

Every figure names the file it came from, in the markup:

```html
<span class="num" data-source="docs/performance.md">95.68</span>
```

`web/tests/method/figures.test.ts` opens each named file and requires the figure
to be in it. A table states its source once and every figure cell in it is
checked. Where the page sets a figure differently from the way the report writes
it, which is usually a power of ten, it also states what the report says.

## Consequences

The masthead's markup is repeated on each page rather than defined once. That is
the cost of the pages being what they are, it is fifteen lines of static markup
with no figure in it, and the alternative is a template step that exists to avoid
copying a nav bar.

Nothing in the client imports a method page, so a page that was renamed would
otherwise leave a dead link behind and still build. Every link the instrument
makes into the reading half is named in `web/src/method/links.ts`, and a test
requires each one to be a page that exists and an entry of the build.

`npm run budget` fails if any built method page carries a script tag. The pages
have no way of acquiring one by accident today; the assertion is there because
the way a site starts shipping a bundle to a document is a change to how it is
built rather than a change to the document.

The reading half is five more entries in the build and one more stylesheet, which
a reader of the instrument never fetches and a reader of a method page fetches
instead of the bundle.
