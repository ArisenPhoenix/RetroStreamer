# include/host/user Architecture

`include/host/user/` defines user identity behavior.

This bucket owns credentials, username-sensitive behavior, and user-facing sync
operations. Storage-backed user control tables live in `host/db/`; sync behavior
that chooses when to move data between devices belongs here.
