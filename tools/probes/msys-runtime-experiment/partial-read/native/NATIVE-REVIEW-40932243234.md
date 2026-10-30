# Native partial-read component review — run 40932243234

Exact remote commit `430ab882b9e47e02841fef96816183d81c38cb3d`, job
`123653415094`; published tree matches local `10bdb7a1e8d41578c8b08bf1df3a887fb356655a`.
Terminal **failure**, solely the fixed trial step. No rerun occurred. The four
stock positive-copy failures remain failures; overall red is not a candidate
failure, and candidate success does not turn the overall result green.

## Provenance and gates

Artifact12925066001, ZIP1,640,786bytes, SHA256
`85fd839c93a2a66dd67e0b1dbd3a7702261ec5af14da681f51424152d2a1bb8d`.
Archive and all105 build plus53 runtime manifest entries rehashed locally;
33,232,350 inventoried bytes, collection complete, no required missing/rejected
files. Evidence: `/workspace/shared/partial-read-40932243234-evidence/artifact`.
Both source builds, actual linked recorder/errno gates, strict native reader and
controller compiles, imports and isolated staging passed. Staged recorded DLL
hashes match linked reports:

- stock `fb94615a85d4f9252b2388645c52092067468876ad4489d616381db838e75469`
- stock+0007 (`copied`) `f7e9d647c5bbbfba0004050ad1e8d08a83bdfefba7f6ba9b287c4c3ee535503c`
- unchanged original `d3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717`

These are recorded native binary identities: binaries are deliberately absent
from the artifact. Source/text/trace hashes were directly rehashed. Both exact
instrumented console hashes and trial-plan bytes match the locked local inputs.
No prior retirement patch, shipped-runtime pin, or default was changed.

## Every fixed row

First read is count / errno. Positive-count errno is descriptive, not a success
requirement. Every row has strict `RESIZE_OBSERVED` and expected_return_match=true.

| Order | Case | Variant | First read | Final exact hex | Byte / component | Reader exit |
|---|---|---|---|---|---|---|
| 1 | partial-zero | stock | -1 / 4 | absent (alarm) | FAIL / FAIL | 124 |
| 1 | partial-zero | copied | 1 / 0 | 6162 | PASS / PASS | 0 |
| 1 | positive-prefix | stock | -1 / 4 | absent (alarm) | FAIL / FAIL | 124 |
| 1 | positive-prefix | copied | 1 / 0 | 616263 | PASS / PASS | 0 |
| 1 | zero-copy | stock | -1 / 4 | 62 | PASS / PASS | 0 |
| 1 | zero-copy | copied | -1 / 4 | 62 | PASS / PASS | 0 |
| 2 | partial-zero | copied | 1 / 0 | 6162 | PASS / PASS | 0 |
| 2 | partial-zero | stock | -1 / 4 | absent (alarm) | FAIL / FAIL | 124 |
| 2 | positive-prefix | copied | 1 / 0 | 616263 | PASS / PASS | 0 |
| 2 | positive-prefix | stock | -1 / 4 | absent (alarm) | FAIL / FAIL | 124 |
| 2 | zero-copy | copied | -1 / 4 | 62 | PASS / PASS | 0 |
| 2 | zero-copy | stock | -1 / 4 | 62 | PASS / PASS | 0 |

Candidate6/6 byte/component pass: all4 positive-copy reads report the already
copied1 and then exact complete ab/abc; both zero-copy controls still return
-1/EINTR before the exact b tail. Stock positive cases fail4/4: trace copied1,
then -1/EINTR, eventually alarm124 with no final exact-hex record. Stock zero-copy
controls pass2/2, preserving the intended EINTR behavior. No stock tail bytes are
invented from count-only logs.

All12 outer processes were reaped without timeout, output truncation or cleanup
error; native queue residue0. All12 strict resize numeric chains are complete,
valid/finalized and nonoverflowing. The actual local chain is changed resize
(WINDOW_BUFFER_SIZE_EVENT4), signal28 and matched handler, input_winch, then the
recorded read return. All12 rows were independently reparsed locally with the
exact runner and predicate; all recorded fields matched. Legacy mouse/master
coverage remains **INCONCLUSIVE on all12**, never relabelled or relaxed.

## What this establishes — and does not

This is native component evidence for the narrow positive-copy return change
under this fixed non-SA_RESTART input_winch resize schedule, with successful
zero-copy preservation controls in both orders. It directly improves the
previously observed partial-read failure without extra bytes, sleeps, retries,
queue manipulation or disabled resize. General async-signal safety and
production qualification remain false.

It does not establish Settings leading-ESC causality; does not qualify the
separate refined-retirement patch; does not cover the intended mouse/master
rewrite predicate, WAIT_SIGNALED ingress, SA_RESTART cases, copied prefixes>1,
canonical variants, cancellation, generic errors or competing-reader behavior.
The corresponding predeclared semantic regressions in ../QUALIFICATION.md are
the next bounded work before a wider application/promotion decision. Retain
this failing-control run as evidence rather than rerun unchanged to seek green.

Exact results.json SHA256: `3d2e0afc381444d7dc81e835f478e79e90ee7389ffe6d65346dac0c2cbecb392`.
