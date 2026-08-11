# Android ui/games Architecture

`ui/games/` owns Android game browsing and selection UI pieces.

It should consume catalog state from the view model and avoid owning network or
protocol behavior directly.
