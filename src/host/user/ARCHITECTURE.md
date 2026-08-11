# src/host/user Architecture

`src/host/user/` implements user identity and sync behavior.

This code handles credential behavior and user-facing sync orchestration. It may
call DB stores, but should keep schema details inside `src/host/db/`.
