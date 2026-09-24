# RFC 8785 JCS fixtures

These fixtures are derived from RFC 8785, *JSON Canonicalization Scheme
(JCS)*, published June 2020:

- Sections 3.2.2 and 3.2.4 provide the `rfc8785-values` parsed-value sample
  and its canonical UTF-8 bytes.
- Section 3.2.3 provides the UTF-16 property-ordering sample exercised by the
  C++ test.
- Appendix B, Table 1 provides the finite IEEE 754 binary64 number corpus and
  the required ECMAScript-compatible encodings. The C++ test also covers the
  `1e+30` example from Section 3.2.2 and the `1e-7` fixed/exponent boundary.

Authoritative source: <https://www.rfc-editor.org/rfc/rfc8785.html>

The `.input.json` files preserve the human-readable source vectors for the
strict parser composition test added in Task 4. Task 2 deliberately constructs
the equivalent `JsonValue` trees directly and does not add a semantic JSON
parser in test code.

Expected canonical bytes are stored as hexadecimal. This keeps fixture-file
line endings and the final fixture newline from accidentally becoming part of
the identity bytes.
