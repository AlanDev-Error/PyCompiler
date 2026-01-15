#PyCC
A Lightweight Python Compiler made in C.

Usage (on Windows): compiler --build <inputfile>.py <out>.exe
Usage (on Linux): pycc --build <script>.py <out_binary>

##How to use?

On Windows: Locate your python's installation folder, Place the "compiler.exe" executable in the root folder where the "Lib" folder is located.
Add the installation folder in the PATH if not added yet. Use the .exe and compile!
Or compile your own using any compiler.

On Linux: 
Just use the ./pycc executable provided

I appreciate contributions, Contribute to PyCompiler or PyCC by forking and adding a contribute.md with account name and date!

#NOTE

Static executables cant be made, and errors in programs lead to this kind of thing(?):
Bugs can be reported on issues

On linux:
`Failed to execute embedded Python code
Bootloader: no embedded payload or run failed (code 13)
Usage to build: ./tests --build <script.py> <out_binary>`
