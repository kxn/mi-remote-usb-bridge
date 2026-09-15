# Reference sources and offline development

The repository includes the WCH SDK subset required to build firmware under
`firmware/wch/vendor/`; no SDK download is needed for an ordinary build.
[SDK lock](sdk-lock.json) records its upstream revision and binary hashes.

The full offline research collection remains local and is excluded from Git:
`documents/`, `sources/`, `archives/`, `text/`, `packages/`, and comparison snapshots.
It contains specifications and source archives with independent redistribution terms.

- [sources.json](sources.json): pinned commits and explicit source URLs.
- [toolchain-links.json](toolchain-links.json): stable MounRiver resource IDs/resolver URLs.
- `python tools/archive_references.py`: download the collection by exact URL.
- `python tools/archive_references.py --verify`: check local downloaded originals.
- `python tools/index_references.py`: index downloaded documents (requires the document extraction dependencies noted in the script).

No search engine is required. Downloaded programs are not executed by the archive
script. Some original URLs may require access permission or stop working; the local
cache is therefore preserved. Full reference/audio comparison tests requiring this
cache are separate from the repository's self-contained regression suite.
