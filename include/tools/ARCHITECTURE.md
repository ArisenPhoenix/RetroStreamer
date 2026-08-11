# include/tools Architecture

`include/tools/` defines reusable app wrappers for command-line tools.

Tool headers should adapt host/client/common libraries to executable entry
points without becoming owners of session, protocol, or platform behavior.
