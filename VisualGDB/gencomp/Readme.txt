This tool generates 3 files:
- compemu.cpp
- compstbl.cpp
- comptbl.h

To use it, compile it for the target system (e.g. Raspberry), then copy it there together
with the "compiler" subdirectory and the "table68k" file.

Execute the "gencomp" tool:

gencomp > compemu.cpp

, then copy back the generated files:
- compemu.cpp
- compstbl.cpp
- comptbl.h

into the jit/compiler directory