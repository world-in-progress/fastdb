# Historical C-Two Call-DB Plan

This file is retained only as a pointer to superseded FastDB-side planning work. C-Two-specific FastDB call-db planning, CRM bridge derivation, and TypeScript helper generation now live in the C-Two repository, where the CRM contract, route identity, relay behavior, scheduler policy, and memory lease semantics are defined.

FastDB now treats that integration as an external consumer boundary. This repository owns generic storage engines, schema export, binary buffers, Python feature declarations, the generic `fdb codegen --ts` path, and the `fastdb4ts` runtime primitives that other systems can compose with.
