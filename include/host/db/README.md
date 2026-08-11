# include/host/db

Host database headers live here.

This directory owns persistence APIs for catalog metadata, active sessions,
Cadence session tracking, and user control mappings.

Business logic that merely syncs or interprets persisted values should usually
live closer to the feature area that uses it.
