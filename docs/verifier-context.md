# Verifier context (M2.1)

The internal regex verifier accepts a non-owning `detail::VerifierContext`. It is not part of the stable C ABI (or the public C++ search surface).

## Coordinates and ownership

All offsets are source **bytes**, not Unicode code points. Every interval is half-open, `[begin,end)`. `source[0]` corresponds to absolute `source_begin`, and `source_end` must equal `source_begin + source.size()`. The caller owns the backing storage and keeps it alive through the verification call.

* `record_begin/end` are the logical record bounds. A separator, and the CRLF terminator when CRLF policy is enabled, are outside the record.
* candidate_begin/end is the planned half-open range of start offsets to try. The end may be record_end + 1 to include an empty match at the record end. This range limits start attempts only; it does not narrow bytes visible to an attempt. M2.3 additionally passes absolute bounded-region coordinates for its narrow guarded subset (currently at most one capture and at least two mandatory literal segments).
* `left_context_available` and `right_context_available` state whether context beyond the logical record is available for lookaround/word checks. They are authoritative, as are separator and CRLF policy.

The verifier validates the context before execution. Anchors use source and record bounds as appropriate; word boundaries and lookaround inspect source bytes while respecting record/context availability. Captures and matches are returned in absolute source-byte coordinates.

## Roadmap ownership

M2.1 defines and threads this context. M2.2 adds conservative width/context metadata, and M2.3 consumes only proven finite-width metadata for guarded bounded-region execution. The default full-record/full-file path remains authoritative for unsupported patterns and boundary-sensitive modes.
