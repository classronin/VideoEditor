# 原生 Win32 + D3D11 + ffmpeg 子进程，MinGW GCC 构建。
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -mwindows -municode
LDFLAGS  := -municode -mwindows
# -ldwmapi 是深色标题栏（main.cpp 的 DwmSetWindowAttribute）带来的。两只都是 OS 自带，
# 无第三方依赖、无新 DLL。
LIBS     := -ld3d11 -ldxgi -ld3dcompiler -lole32 -loleaut32 -luuid -lshell32 -luser32 -lgdi32 -lwindowscodecs -lwinmm -lwinhttp -lmfplat -lmfreadwrite -lmfuuid -ldwmapi

SRC_DIR  := src
BIN_DIR  := bin
OBJ_DIR  := build
EXE      := $(BIN_DIR)/VideoEditor.exe

SRC := $(wildcard $(SRC_DIR)/*.cpp)
OBJ := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
# exe 的图标（vse.rc 里的 1 ICON = icon.ico）。没有它 Windows 就发那张默认的应用图标。
RC := vse.rc
RES := $(OBJ_DIR)/vse_res.o

.PHONY: all clean run

all: $(EXE)

$(EXE): $(OBJ) $(RES) | $(BIN_DIR)
	$(CXX) $(OBJ) $(RES) -o $@ $(LDFLAGS) $(LIBS)

$(RES): $(RC) icon.ico | $(OBJ_DIR)
	windres --input $< --output $@ --output-format=coff

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

run: all
	$(EXE) &

# 只删自己产出的文件：bin/ 里还放着随程序分发的 ffmpeg.exe / ffprobe.exe。
clean:
	rm -rf $(OBJ_DIR) $(EXE)

-include $(DEP)
