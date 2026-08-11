# branding Architecture

`branding/` owns checked-in application identity assets.

These files are consumed by packaging, desktop integration, and Qt resources.
They should remain passive assets; runtime behavior belongs in `src/`, and
installer logic belongs in `deploy/`.
