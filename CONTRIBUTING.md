# contributing

pull requests welcome, especially on the roadmap items (embedded adpcm voices is
the big one). a few house rules so the repo stays clean:

- **keep it dependency-free.** the whole point is that `smaf` links into anything
  with zero third-party baggage. no new libraries.
- **keep it original work.** the format knowledge comes from the public SMAF spec
  and from watching real files. never copy code from a GPL/AGPL source (that would
  poison the Apache license for everyone), and never add the yamaha sample rom or
  any yamaha sdk material. write your own.
- **keep the voice as-is.** lowercase, plain, no em-dashes in comments or docs.
- **authorship:** commits are by their author. no AI-attribution / `Co-Authored-By`
  trailers, please.
- **test what you touch.** the engine is deliberately easy to exercise: render a
  handful of real `.mmf` files before and after your change and confirm nothing
  that played went silent.

if you have a `.mmf` that plays wrong, an issue with the file attached (or a link)
is genuinely useful. the format has corners that only show up on real tunes.
