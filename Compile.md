
--------------------------------------------
vse\src\          所有 .cpp / .h（程序的正文）
vse\Makefile      编译清单（make 读它）
vse\vse.rc        exe 的图标/版本资源定义
vse\icon.ico      图标本体（vse.rc 引用它）
--------------------------------------------


```
cd .\vse
make
```


进阶：make 不可用时，手动一条命令编出（仅在 MinGW/msys 终端里有效）
```
    windres --input vse.rc --output build/vse_res.o --output-format=coff
    g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE ^
        -mwindows -municode src\*.cpp build\vse_res.o -o bin\VideoEditor.exe ^
        -municode -mwindows -ld3d11 -ldxgi -ld3dcompiler -lole32 -loleaut32 ^
        -luuid -lshell32 -luser32 -lgdi32 -lwindowscodecs -lwinmm -lwinhttp ^
        -lmfplat -lmfreadwrite -lmfuuid -ldwmapi
  （cmd 不认 src\*.cpp 这种通配，得用 msys 终端；平时还是用 make 最省事。）
```

