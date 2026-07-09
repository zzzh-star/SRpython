@echo off
echo Building force_decoder.dll...
gcc -shared -o force_decoder.dll force_wrapper.c app_ForceOut.c -Wl,--out-implib,libforce_decoder.a
echo Done.
