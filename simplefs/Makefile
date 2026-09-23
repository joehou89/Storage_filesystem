# 获取当前 Makefile 所在目录的绝对路径
PWD := $(shell pwd)

obj-m := simplefs.o
# 注意：这里需要加上 source/ 路径，告诉 Kbuild 依赖的 .o 文件在哪
simplefs-objs := source/simple.o

# 编译选项
ccflags-y := -DSIMPLEFS_DEBUG
# 添加头文件搜索路径，$(src) 指向当前 Makefile 所在目录
ccflags-y += -I$(src)/source

all: ko mkfs-simplefs

ko:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# 修正用户态程序的编译规则（原图中的 mkfs-simplefs_SOURCES 并非有效规则）
mkfs-simplefs: source/mkfs-simplefs.c source/simple.h
	gcc -I source source/mkfs-simplefs.c -o mkfs-simplefs


clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm mkfs-simplefs
