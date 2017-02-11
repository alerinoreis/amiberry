This project generates the "cpudefs.cpp" file, by reading from the "table68k" file.

To use it, follow these steps:
1. Compile the build68k.cpp project
2. Copy the resulting executable and the "table68k" files on the target system (e.g. Raspberry)
3. Run the resulting executable on your target device (e.g. Raspberry) as follows:

build68k > cpudefs.cpp < table68k

4. Take the cpudefs.cpp file and copy it back to your main "src" folder