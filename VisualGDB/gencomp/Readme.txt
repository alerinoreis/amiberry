This tool generates 3 files:
- compemu.cpp
- compstbl.cpp
- comptbl.h

To use it, compile it for the target system (e.g. Raspberry), then copy it there together
with the "jit" subdirectory and the "table68k" file.

Execute the "gencomp" tool, then copy back the generated files:
- jit/compemu.cpp
- jit/compstbl.cpp
- jit/comptbl.h