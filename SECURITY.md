# security

this is a file-format decoder that people will point at untrusted `.mmf` files
off the internet, so parser robustness is the real security surface here.

the parser is written to be defensive: every chunk length is clamped to the
actual buffer, the huffman inflate has node and depth guards, and the event
decoders carry iteration caps so a crafted or truncated file cannot hang or read
out of bounds. it degrades to "not a valid SMAF file" rather than crashing.

if you find an input that crashes, hangs, or reads out of bounds, please report
it: open an issue with the file (or a link) and a short description, or email
**akustikrausch@gmail.com** for anything you would rather not post publicly.

this is interoperability research, not an audited production security stack. treat
it accordingly, and fuzzing PRs are very welcome.
