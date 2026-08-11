# shared Architecture

`shared/` owns checked-in runtime data used by multiple targets.

Data here should be stable, declarative, and loaded by host/client/mobile code as
needed. Generated artifacts and local caches should not be placed here.
